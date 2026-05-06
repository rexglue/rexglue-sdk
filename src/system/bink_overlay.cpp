#include "rex/system/bink_overlay.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "rex/logging.h"

#if REX_HAS_FFMPEG
#include <dlfcn.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
}
#include <SDL3/SDL_audio.h>
#endif

namespace rex::system {

namespace {

#if REX_HAS_FFMPEG

// Resolve every FFmpeg call we use through a private dlopen. The static
// libavcodec we link for XMA defines the same symbol names (avcodec_alloc_context3,
// avcodec_find_decoder, etc.); without RTLD_LOCAL the dynamic Fedora
// libavcodec would shadow our static codec list and break game audio.
struct FFmpegSyms {
  void* avformat_handle = nullptr;
  void* avcodec_handle = nullptr;
  void* avutil_handle = nullptr;
  void* swscale_handle = nullptr;

  // libavformat
  int (*avformat_open_input)(AVFormatContext**, const char*, const AVInputFormat*,
                             AVDictionary**) = nullptr;
  int (*avformat_find_stream_info)(AVFormatContext*, AVDictionary**) = nullptr;
  void (*avformat_close_input)(AVFormatContext**) = nullptr;
  int (*av_read_frame)(AVFormatContext*, AVPacket*) = nullptr;

  // libavcodec
  const AVCodec* (*avcodec_find_decoder)(enum AVCodecID) = nullptr;
  AVCodecContext* (*avcodec_alloc_context3)(const AVCodec*) = nullptr;
  void (*avcodec_free_context)(AVCodecContext**) = nullptr;
  int (*avcodec_parameters_to_context)(AVCodecContext*, const AVCodecParameters*) = nullptr;
  int (*avcodec_open2)(AVCodecContext*, const AVCodec*, AVDictionary**) = nullptr;
  int (*avcodec_send_packet)(AVCodecContext*, const AVPacket*) = nullptr;
  int (*avcodec_receive_frame)(AVCodecContext*, AVFrame*) = nullptr;
  AVPacket* (*av_packet_alloc)() = nullptr;
  void (*av_packet_free)(AVPacket**) = nullptr;
  void (*av_packet_unref)(AVPacket*) = nullptr;
  AVFrame* (*av_frame_alloc)() = nullptr;
  void (*av_frame_free)(AVFrame**) = nullptr;
  void (*av_frame_unref)(AVFrame*) = nullptr;

  // libavutil
  int64_t (*av_rescale_q)(int64_t, AVRational, AVRational) = nullptr;

  // libswscale
  SwsContext* (*sws_getContext)(int, int, enum AVPixelFormat, int, int,
                                enum AVPixelFormat, int, SwsFilter*, SwsFilter*,
                                const double*) = nullptr;
  void (*sws_freeContext)(SwsContext*) = nullptr;
  int (*sws_scale)(SwsContext*, const uint8_t* const[], const int[], int, int,
                   uint8_t* const[], const int[]) = nullptr;

  bool ready = false;
};

FFmpegSyms& syms() {
  static FFmpegSyms s;
  static std::once_flag init_flag;
  std::call_once(init_flag, [&] {
    auto open_lib = [](const char* primary, const char* fallback) -> void* {
      void* h = dlopen(primary, RTLD_LAZY | RTLD_LOCAL);
      if (!h && fallback) h = dlopen(fallback, RTLD_LAZY | RTLD_LOCAL);
      return h;
    };
    s.avutil_handle = open_lib("libavutil.so.60", "libavutil.so");
    s.swscale_handle = open_lib("libswscale.so.9", "libswscale.so");
    s.avcodec_handle = open_lib("libavcodec.so.62", "libavcodec.so");
    s.avformat_handle = open_lib("libavformat.so.62", "libavformat.so");
    if (!s.avutil_handle || !s.swscale_handle || !s.avcodec_handle ||
        !s.avformat_handle) {
      REXLOG_WARN(
          "[bink-overlay] dlopen failed: avformat={} avcodec={} avutil={} swscale={}",
          static_cast<void*>(s.avformat_handle),
          static_cast<void*>(s.avcodec_handle),
          static_cast<void*>(s.avutil_handle),
          static_cast<void*>(s.swscale_handle));
      return;
    }

#define LOAD(handle, member, name) \
    s.member = reinterpret_cast<decltype(s.member)>(dlsym(handle, name));    \
    if (!s.member) { REXLOG_WARN("[bink-overlay] dlsym " name " failed"); }

    LOAD(s.avformat_handle, avformat_open_input,         "avformat_open_input");
    LOAD(s.avformat_handle, avformat_find_stream_info,   "avformat_find_stream_info");
    LOAD(s.avformat_handle, avformat_close_input,        "avformat_close_input");
    LOAD(s.avformat_handle, av_read_frame,               "av_read_frame");

    LOAD(s.avcodec_handle, avcodec_find_decoder,           "avcodec_find_decoder");
    LOAD(s.avcodec_handle, avcodec_alloc_context3,         "avcodec_alloc_context3");
    LOAD(s.avcodec_handle, avcodec_free_context,           "avcodec_free_context");
    LOAD(s.avcodec_handle, avcodec_parameters_to_context,  "avcodec_parameters_to_context");
    LOAD(s.avcodec_handle, avcodec_open2,                  "avcodec_open2");
    LOAD(s.avcodec_handle, avcodec_send_packet,            "avcodec_send_packet");
    LOAD(s.avcodec_handle, avcodec_receive_frame,          "avcodec_receive_frame");
    LOAD(s.avcodec_handle, av_packet_alloc,                "av_packet_alloc");
    LOAD(s.avcodec_handle, av_packet_free,                 "av_packet_free");
    LOAD(s.avcodec_handle, av_packet_unref,                "av_packet_unref");
    LOAD(s.avcodec_handle, av_frame_alloc,                 "av_frame_alloc");
    LOAD(s.avcodec_handle, av_frame_free,                  "av_frame_free");
    LOAD(s.avcodec_handle, av_frame_unref,                 "av_frame_unref");

    LOAD(s.avutil_handle, av_rescale_q, "av_rescale_q");

    LOAD(s.swscale_handle, sws_getContext,  "sws_getContext");
    LOAD(s.swscale_handle, sws_freeContext, "sws_freeContext");
    LOAD(s.swscale_handle, sws_scale,       "sws_scale");
#undef LOAD

    s.ready = s.avformat_open_input && s.avformat_find_stream_info &&
              s.avformat_close_input && s.av_read_frame &&
              s.avcodec_find_decoder && s.avcodec_alloc_context3 &&
              s.avcodec_free_context && s.avcodec_parameters_to_context &&
              s.avcodec_open2 && s.avcodec_send_packet &&
              s.avcodec_receive_frame && s.av_packet_alloc &&
              s.av_packet_free && s.av_packet_unref && s.av_frame_alloc &&
              s.av_frame_free && s.av_frame_unref && s.av_rescale_q &&
              s.sws_getContext && s.sws_freeContext && s.sws_scale;
    if (s.ready) {
      REXLOG_INFO("[bink-overlay] FFmpeg loaded; cinematics will composite");
    } else {
      REXLOG_WARN("[bink-overlay] missing FFmpeg symbols, overlay disabled");
    }
  });
  return s;
}

class BikDecoder {
 public:
  explicit BikDecoder(std::string path)
      : path_(std::move(path)) {
    thread_ = std::thread(&BikDecoder::Run, this);
  }

  ~BikDecoder() {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    if (sws_) syms().sws_freeContext(sws_);
    if (codec_ctx_) syms().avcodec_free_context(&codec_ctx_);
    if (audio_codec_ctx_) syms().avcodec_free_context(&audio_codec_ctx_);
    if (fmt_ctx_) syms().avformat_close_input(&fmt_ctx_);
    if (audio_stream_) {
      SDL_DestroyAudioStream(audio_stream_);
      audio_stream_ = nullptr;
    }
  }

  bool LatestFrame(std::vector<uint8_t>& dst, uint32_t& w, uint32_t& h,
                   int64_t& presentation_us) {
    std::lock_guard lock(mutex_);
    if (!has_frame_) return false;
    dst = frame_rgba_;
    w = width_;
    h = height_;
    presentation_us = frame_presentation_us_;
    return true;
  }

  bool finished() const { return finished_.load(std::memory_order_acquire); }

 private:
  void Run() {
    auto& f = syms();
    if (!f.ready) {
      finished_.store(true, std::memory_order_release);
      return;
    }
    if (f.avformat_open_input(&fmt_ctx_, path_.c_str(), nullptr, nullptr) < 0) {
      REXLOG_WARN("[bink] avformat_open_input failed: {}", path_);
      finished_.store(true, std::memory_order_release);
      return;
    }
    if (f.avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
      REXLOG_WARN("[bink] avformat_find_stream_info failed: {}", path_);
      finished_.store(true, std::memory_order_release);
      return;
    }
    int video_stream_idx = -1;
    int audio_stream_idx = -1;
    for (unsigned i = 0; i < fmt_ctx_->nb_streams; ++i) {
      auto type = fmt_ctx_->streams[i]->codecpar->codec_type;
      if (type == AVMEDIA_TYPE_VIDEO && video_stream_idx < 0) {
        video_stream_idx = static_cast<int>(i);
      } else if (type == AVMEDIA_TYPE_AUDIO && audio_stream_idx < 0) {
        audio_stream_idx = static_cast<int>(i);
      }
    }
    if (video_stream_idx < 0) {
      finished_.store(true, std::memory_order_release);
      return;
    }
    AVStream* stream = fmt_ctx_->streams[video_stream_idx];
    const AVCodec* codec = f.avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
      REXLOG_WARN("[bink] no decoder for codec_id {}", int(stream->codecpar->codec_id));
      finished_.store(true, std::memory_order_release);
      return;
    }
    codec_ctx_ = f.avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
      finished_.store(true, std::memory_order_release);
      return;
    }
    f.avcodec_parameters_to_context(codec_ctx_, stream->codecpar);
    if (f.avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
      finished_.store(true, std::memory_order_release);
      return;
    }
    width_ = static_cast<uint32_t>(codec_ctx_->width);
    height_ = static_cast<uint32_t>(codec_ctx_->height);
    REXLOG_INFO("[bink] decoding {} ({}x{})", path_, width_, height_);

    // Set up Bink audio decoder + dedicated SDL audio stream. SDL3 mixes
    // multiple streams on the default playback device, so we ride alongside
    // the XMA driver's stream without conflict. Decoded frames are float
    // planar; we interleave manually before pushing to SDL.
    AVStream* audio_stream = nullptr;
    AVRational audio_tb{};
    // Open the Bink audio decoder. SDL3 mixes multiple SDL_AudioStreams on
    // the default device transparently, so the Bink track rides alongside
    // the XMA driver's stream without conflict. Decoded frames are usually
    // float planar (binkaudio_dct, binkaudio_rdft); we interleave to packed
    // SDL_AUDIO_F32 by hand to avoid pulling in libswresample.
    if (audio_stream_idx >= 0) {
      audio_stream = fmt_ctx_->streams[audio_stream_idx];
      const AVCodec* a_codec = f.avcodec_find_decoder(audio_stream->codecpar->codec_id);
      if (a_codec) {
        audio_codec_ctx_ = f.avcodec_alloc_context3(a_codec);
        if (audio_codec_ctx_) {
          f.avcodec_parameters_to_context(audio_codec_ctx_, audio_stream->codecpar);
          if (f.avcodec_open2(audio_codec_ctx_, a_codec, nullptr) >= 0) {
            int channels = audio_codec_ctx_->ch_layout.nb_channels;
            if (channels <= 0) channels = 1;
            channels = std::min(channels, 2);
            int sample_rate = audio_codec_ctx_->sample_rate > 0
                                  ? audio_codec_ctx_->sample_rate
                                  : 44100;
            SDL_AudioSpec spec{};
            spec.freq = sample_rate;
            spec.format = SDL_AUDIO_F32;
            spec.channels = channels;
            audio_stream_ = SDL_OpenAudioDeviceStream(
                SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
            if (audio_stream_) {
              SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(audio_stream_);
              if (dev) SDL_ResumeAudioDevice(dev);
              audio_channels_ = channels;
              audio_tb = audio_stream->time_base;
              REXLOG_INFO("[bink] audio: codec_id={} {}Hz {}ch", int(a_codec->id),
                          sample_rate, channels);
            } else {
              REXLOG_WARN("[bink] SDL_OpenAudioDeviceStream failed: {}",
                          SDL_GetError());
              f.avcodec_free_context(&audio_codec_ctx_);
              audio_codec_ctx_ = nullptr;
            }
          } else {
            f.avcodec_free_context(&audio_codec_ctx_);
            audio_codec_ctx_ = nullptr;
          }
        }
      }
    }
    (void)audio_tb;

    AVRational tb = stream->time_base;
    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    int64_t first_pts = AV_NOPTS_VALUE;

    AVPacket* packet = f.av_packet_alloc();
    AVFrame* frame = f.av_frame_alloc();
    AVFrame* audio_frame = audio_codec_ctx_ ? f.av_frame_alloc() : nullptr;
    uint32_t video_frames_decoded = 0;
    auto last_rate_log = clock::now();

    while (!stop_.load(std::memory_order_acquire)) {
      int ret = f.av_read_frame(fmt_ctx_, packet);
      if (ret < 0) {
        f.avcodec_send_packet(codec_ctx_, nullptr);
        if (audio_codec_ctx_) f.avcodec_send_packet(audio_codec_ctx_, nullptr);
        break;
      }
      if (packet->stream_index == audio_stream_idx && audio_codec_ctx_ &&
          audio_frame && audio_stream_) {
        if (f.avcodec_send_packet(audio_codec_ctx_, packet) >= 0) {
          while (true) {
            int rcv = f.avcodec_receive_frame(audio_codec_ctx_, audio_frame);
            if (rcv == AVERROR(EAGAIN) || rcv == AVERROR_EOF) break;
            if (rcv < 0) break;
            int nb_samples = audio_frame->nb_samples;
            int channels = audio_channels_;
            if (audio_codec_ctx_->sample_fmt == AV_SAMPLE_FMT_FLTP) {
              size_t out_bytes = static_cast<size_t>(nb_samples) * channels *
                                 sizeof(float);
              if (audio_pcm_.size() < out_bytes) audio_pcm_.resize(out_bytes);
              float* dst = reinterpret_cast<float*>(audio_pcm_.data());
              for (int s = 0; s < nb_samples; ++s) {
                for (int c = 0; c < channels; ++c) {
                  const float* plane = reinterpret_cast<const float*>(
                      audio_frame->extended_data[c]);
                  dst[s * channels + c] = plane[s];
                }
              }
              SDL_PutAudioStreamData(audio_stream_, audio_pcm_.data(),
                                     static_cast<int>(out_bytes));
            } else if (audio_codec_ctx_->sample_fmt == AV_SAMPLE_FMT_FLT) {
              int bytes = nb_samples * channels * sizeof(float);
              SDL_PutAudioStreamData(audio_stream_, audio_frame->data[0], bytes);
            }
            f.av_frame_unref(audio_frame);
          }
        }
        f.av_packet_unref(packet);
        continue;
      }
      if (packet->stream_index != video_stream_idx) {
        f.av_packet_unref(packet);
        continue;
      }
      if (f.avcodec_send_packet(codec_ctx_, packet) >= 0) {
        while (true) {
          int rcv = f.avcodec_receive_frame(codec_ctx_, frame);
          if (rcv == AVERROR(EAGAIN) || rcv == AVERROR_EOF) break;
          if (rcv < 0) break;

          if (!sws_) {
            // SWS_FAST_BILINEAR is ~2× faster than SWS_BILINEAR on aarch64
            // (the BILINEAR path falls through to a slow generic loop on
            // SIMD-incomplete builds). Quality difference is invisible at
            // 1280×720 native scale where input and output dims match.
            sws_ = f.sws_getContext(width_, height_, codec_ctx_->pix_fmt, width_,
                                    height_, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR,
                                    nullptr, nullptr, nullptr);
          }
          if (!sws_) {
            f.av_frame_unref(frame);
            continue;
          }
          std::vector<uint8_t> buf(static_cast<size_t>(width_) * height_ * 4);
          uint8_t* dst[4] = {buf.data(), nullptr, nullptr, nullptr};
          int dst_linesize[4] = {static_cast<int>(width_) * 4, 0, 0, 0};
          f.sws_scale(sws_, frame->data, frame->linesize, 0, height_, dst,
                      dst_linesize);

          int64_t pts = frame->pts != AV_NOPTS_VALUE ? frame->pts
                        : frame->pkt_dts != AV_NOPTS_VALUE ? frame->pkt_dts
                                                           : 0;
          if (first_pts == AV_NOPTS_VALUE) first_pts = pts;
          int64_t pts_us = f.av_rescale_q(pts - first_pts, tb, {1, 1000000});

          auto target = start + std::chrono::microseconds(pts_us);
          auto now = clock::now();
          if (now < target - std::chrono::milliseconds(20)) {
            std::this_thread::sleep_for(target - now -
                                        std::chrono::milliseconds(5));
          }

          {
            std::lock_guard lock(mutex_);
            frame_rgba_ = std::move(buf);
            frame_presentation_us_ = pts_us;
            has_frame_ = true;
          }
          f.av_frame_unref(frame);
          ++video_frames_decoded;
          auto now2 = clock::now();
          if (now2 - last_rate_log >= std::chrono::seconds(2)) {
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now2 - last_rate_log).count();
            REXLOG_DEBUG("[bink] {} frames in {} ms ({}fps)",
                         video_frames_decoded, elapsed_ms,
                         elapsed_ms ? video_frames_decoded * 1000 / elapsed_ms : 0);
            video_frames_decoded = 0;
            last_rate_log = now2;
          }
        }
      }
      f.av_packet_unref(packet);
    }
    f.av_packet_free(&packet);
    f.av_frame_free(&frame);
    if (audio_frame) f.av_frame_free(&audio_frame);
    finished_.store(true, std::memory_order_release);
  }

  std::string path_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> finished_{false};
  AVFormatContext* fmt_ctx_ = nullptr;
  AVCodecContext* codec_ctx_ = nullptr;
  AVCodecContext* audio_codec_ctx_ = nullptr;
  SwsContext* sws_ = nullptr;
  SDL_AudioStream* audio_stream_ = nullptr;
  int audio_channels_ = 0;
  std::vector<uint8_t> audio_pcm_;
  std::mutex mutex_;
  std::vector<uint8_t> frame_rgba_;
  uint32_t width_ = 0, height_ = 0;
  int64_t frame_presentation_us_ = 0;
  bool has_frame_ = false;
};

#endif  // REX_HAS_FFMPEG

bool EndsWithIgnoreCase(std::string_view s, std::string_view suffix) {
  if (s.size() < suffix.size()) return false;
  size_t off = s.size() - suffix.size();
  for (size_t i = 0; i < suffix.size(); ++i) {
    char a = s[off + i];
    char b = suffix[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

}  // namespace

#if REX_HAS_FFMPEG
struct BinkOverlay::Impl {
  std::mutex mu;
  std::unordered_map<std::string, std::unique_ptr<BikDecoder>> decoders;
  std::string active_path;
  std::vector<uint8_t> last_rgba;
  uint32_t last_w = 0, last_h = 0;
  int64_t last_pts_us = 0;
};
#else
struct BinkOverlay::Impl {};
#endif

BinkOverlay::BinkOverlay() : impl_(std::make_unique<Impl>()) {}
BinkOverlay::~BinkOverlay() = default;

BinkOverlay* BinkOverlay::Get() {
  static BinkOverlay instance;
  return &instance;
}

void BinkOverlay::OnFileOpened(std::string_view host_path) {
#if REX_HAS_FFMPEG
  if (!EndsWithIgnoreCase(host_path, ".bik")) return;
  if (!syms().ready) return;
  // Note: overlay still composites for frontendbackground.bik (PRESS START
  // attract) — without it, the swap is solid black there because the
  // recomp's own Bink frame uploads don't reach the GPU on aarch64. The
  // PRESS START text drawn by the engine *will* be hidden under our quad,
  // but the documented mnk_mode mapping (Esc = Start) lets the user
  // advance without needing to see the prompt.
  std::string p(host_path);
  std::lock_guard lock(impl_->mu);
  auto it = impl_->decoders.find(p);
  if (it != impl_->decoders.end() && !it->second->finished()) {
    impl_->active_path = p;
    return;
  }
  REXLOG_INFO("[bink-overlay] open {}", p);
  impl_->decoders[p] = std::make_unique<BikDecoder>(p);
  impl_->active_path = p;
#else
  (void)host_path;
#endif
}

void BinkOverlay::OnFileClosed(std::string_view host_path) {
#if REX_HAS_FFMPEG
  if (!EndsWithIgnoreCase(host_path, ".bik")) return;
  std::string p(host_path);
  std::lock_guard lock(impl_->mu);
  auto it = impl_->decoders.find(p);
  if (it != impl_->decoders.end()) {
    REXLOG_INFO("[bink-overlay] close {}", p);
    impl_->decoders.erase(it);
  }
  if (impl_->active_path == p) impl_->active_path.clear();
#else
  (void)host_path;
#endif
}

bool BinkOverlay::HasActiveFmv() const {
#if REX_HAS_FFMPEG
  std::lock_guard lock(impl_->mu);
  if (impl_->active_path.empty()) return false;
  auto it = impl_->decoders.find(impl_->active_path);
  return it != impl_->decoders.end() && !it->second->finished();
#else
  return false;
#endif
}

bool BinkOverlay::AcquireCurrentFrame(BinkOverlayFrame& out_frame) {
#if REX_HAS_FFMPEG
  std::lock_guard lock(impl_->mu);
  if (impl_->active_path.empty()) return false;
  auto it = impl_->decoders.find(impl_->active_path);
  if (it == impl_->decoders.end()) return false;
  uint32_t w = 0, h = 0;
  int64_t pts_us = 0;
  if (!it->second->LatestFrame(impl_->last_rgba, w, h, pts_us)) return false;
  impl_->last_w = w;
  impl_->last_h = h;
  impl_->last_pts_us = pts_us;
  out_frame.data = impl_->last_rgba.data();
  out_frame.width = w;
  out_frame.height = h;
  out_frame.presentation_us = pts_us;
  return true;
#else
  (void)out_frame;
  return false;
#endif
}

}  // namespace rex::system
