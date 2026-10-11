/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xam/apps/xmp_app.h>
#include <rex/logging.h>
#include <rex/runtime/xthread.h>
#include <rex/platform/thread.h>
#include "kernel/kernel_macros.h"

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::runtime;
namespace apps {
using namespace rex::runtime;

namespace {

struct XMP_SONG_IN {
  be_u32 file_path;
  be_u32 name;
  be_u32 artist;
  be_u32 album;
  be_u32 album_artist;
  be_u32 genre;
  be_u32 track_number;
  be_u32 duration_ms;
  be_u32 format;
};
REX_STATIC_ASSERT_SIZE(XMP_SONG_IN, 36);

struct XMP_SONG_INFO {
  be_u32 handle;
  u8 unknown[572];
  be_u16 name[20];
  be_u16 artist[20];
  be_u16 album[20];
  be_u16 album_artist[20];
  be_u16 genre[20];
  be_u32 track_number;
  be_u32 duration_ms;
  be_u32 format;
};
REX_STATIC_ASSERT_SIZE(XMP_SONG_INFO, 788);
static_assert(offsetof(XMP_SONG_INFO, name) == 4 + 572);
static_assert(offsetof(XMP_SONG_INFO, track_number) == 4 + 572 + 200);

}  // namespace

XmpApp::XmpApp(Runtime& runtime)
    : App(runtime, 0xFA),
      state_(State::kIdle),
      playback_client_(PlaybackClient::kTitle),
      playback_mode_(PlaybackMode::kUnknown),
      repeat_mode_(RepeatMode::kUnknown),
      unknown_flags_(0),
      volume_(1.0f),
      active_playlist_(nullptr),
      active_song_index_(0),
      next_playlist_handle_(1),
      next_song_handle_(1) {}

X_HRESULT XmpApp::XMPGetStatus(u32 state_ptr) {
  if (!KernelThread::GetCurrentThread()->main_thread()) {
    // Titles poll this in a loop, the delay keeps them from starving other threads.
    rex::runtime::KernelThread::Sleep(std::chrono::milliseconds(1));
  }

  REXLOG_TRACE(kernel, "XMPGetStatus({:08X})", state_ptr);
  *memory_->TranslateVirtual<be_u32*>(state_ptr) = static_cast<u32>(state_);
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPCreateTitlePlaylist(u32 songs_ptr, u32 song_count, u32 playlist_name_ptr,
                                         const std::u16string& playlist_name, u32 flags,
                                         u32 out_song_handles, u32 out_playlist_handle) {
  REXLOG_DEBUG(kernel,
               "XMPCreateTitlePlaylist({:08X}, {:08X}, {:08X}({}), {:08X}, {:08X}, "
               "{:08X})",
               songs_ptr, song_count, playlist_name_ptr, rex::to_utf8(playlist_name), flags,
               out_song_handles, out_playlist_handle);
  auto playlist = std::make_unique<Playlist>();
  playlist->handle = ++next_playlist_handle_;
  playlist->name = playlist_name;
  playlist->flags = flags;
  if (songs_ptr) {
    for (u32 i = 0; i < song_count; ++i) {
      auto song = std::make_unique<Song>();
      song->handle = ++next_song_handle_;
      const auto& in = memory_->TranslateVirtual<const XMP_SONG_IN*>(songs_ptr)[i];
      auto read_string = [this](u32 address) {
        return memory::load_and_swap<std::u16string>(memory_->TranslateVirtual(address));
      };
      song->file_path = read_string(in.file_path);
      song->name = read_string(in.name);
      song->artist = read_string(in.artist);
      song->album = read_string(in.album);
      song->album_artist = read_string(in.album_artist);
      song->genre = read_string(in.genre);
      song->track_number = in.track_number;
      song->duration_ms = in.duration_ms;
      song->format = static_cast<Song::Format>(u32{in.format});
      if (out_song_handles) {
        *memory_->TranslateVirtual<be_u32*>(out_song_handles + (i * 4)) = song->handle;
      }
      playlist->songs.emplace_back(std::move(song));
    }
  }
  if (out_playlist_handle) {
    *memory_->TranslateVirtual<be_u32*>(out_playlist_handle) = playlist->handle;
  }

  const u32 handle = playlist->handle;
  std::lock_guard lock(mutex_);
  playlists_.emplace(handle, std::move(playlist));
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPDeleteTitlePlaylist(u32 playlist_handle) {
  REXLOG_DEBUG(kernel, "XMPDeleteTitlePlaylist({:08X})", playlist_handle);
  std::unique_ptr<Playlist> playlist;
  {
    std::lock_guard lock(mutex_);
    auto it = playlists_.find(playlist_handle);
    if (it == playlists_.end()) {
      REX_FAIL(kernel, X_E_NOTFOUND, "Playlist {:08X} not found", playlist_handle);
    }
    playlist = std::move(it->second);
    playlists_.erase(it);
  }
  if (playlist.get() == active_playlist_) {
    XMPStop(0);
  }
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPPlayTitlePlaylist(u32 playlist_handle, u32 song_handle) {
  REXLOG_DEBUG(kernel, "XMPPlayTitlePlaylist({:08X}, {:08X})", playlist_handle, song_handle);
  Playlist* playlist = nullptr;
  {
    std::lock_guard lock(mutex_);
    auto it = playlists_.find(playlist_handle);
    if (it == playlists_.end()) {
      REX_FAIL(kernel, X_E_NOTFOUND, "Playlist {:08X} not found", playlist_handle);
    }
    playlist = it->second.get();
  }

  if (playback_client_ == PlaybackClient::kSystem) {
    REXLOG_WARN(kernel, "XMPPlayTitlePlaylist: System playback is enabled!");
    return X_E_SUCCESS;
  }

  REX_KERNEL_STUB("Playlist playback");
  active_playlist_ = playlist;
  active_song_index_ = 0;
  state_ = State::kPlaying;
  OnStateChanged();
  runtime_.notifier().BroadcastNotification(kMsgPlaybackBehaviorChanged, 1);
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPContinue() {
  REXLOG_DEBUG(kernel, "XMPContinue()");
  if (state_ == State::kPaused) {
    state_ = State::kPlaying;
  }
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPStop(u32 unk) {
  REX_ASSERT(unk == 0);
  REXLOG_DEBUG(kernel, "XMPStop({:08X})", unk);
  active_playlist_ = nullptr;
  active_song_index_ = 0;
  state_ = State::kIdle;
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPPause() {
  REXLOG_DEBUG(kernel, "XMPPause()");
  if (state_ == State::kPlaying) {
    state_ = State::kPaused;
  }
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPNext() {
  REXLOG_DEBUG(kernel, "XMPNext()");
  if (!active_playlist_ || active_playlist_->songs.empty()) {
    return X_E_NOTFOUND;
  }
  state_ = State::kPlaying;
  active_song_index_ = (active_song_index_ + 1) % active_playlist_->songs.size();
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPPrevious() {
  REXLOG_DEBUG(kernel, "XMPPrevious()");
  if (!active_playlist_ || active_playlist_->songs.empty()) {
    return X_E_NOTFOUND;
  }
  state_ = State::kPlaying;
  active_song_index_ =
      active_song_index_ ? active_song_index_ - 1 : active_playlist_->songs.size() - 1;
  OnStateChanged();
  return X_E_SUCCESS;
}

void XmpApp::OnStateChanged() {
  runtime_.notifier().BroadcastNotification(kMsgStateChanged, static_cast<u32>(state_));
}

X_HRESULT XmpApp::DispatchMessageSync(u32 message, u32 buffer_ptr, u32 buffer_length) {
  switch (message) {
    case 0x00070002: {
      REX_ASSERT(!buffer_length || buffer_length == 12);
      struct {
        be_u32 xmp_client;
        be_u32 storage_ptr;
        be_u32 song_handle;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 12);

      u32 playlist_handle = *memory_->TranslateVirtual<const be_u32*>(args->storage_ptr);
      REX_ASSERT(args->xmp_client == 0x00000002);
      return XMPPlayTitlePlaylist(playlist_handle, args->song_handle);
    }
    case 0x00070003: {
      REX_ASSERT(!buffer_length || buffer_length == 4);
      REX_ASSERT(*memory_->TranslateVirtual<const be_u32*>(buffer_ptr) == 0x00000002);
      return XMPContinue();
    }
    case 0x00070004: {
      REX_ASSERT(!buffer_length || buffer_length == 8);
      struct {
        be_u32 xmp_client;
        be_u32 unk;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 8);

      REX_ASSERT(args->xmp_client == 0x00000002);
      return XMPStop(args->unk);
    }
    case 0x00070005: {
      REX_ASSERT(!buffer_length || buffer_length == 4);
      REX_ASSERT(*memory_->TranslateVirtual<const be_u32*>(buffer_ptr) == 0x00000002);
      return XMPPause();
    }
    case 0x00070006: {
      REX_ASSERT(!buffer_length || buffer_length == 4);
      REX_ASSERT(*memory_->TranslateVirtual<const be_u32*>(buffer_ptr) == 0x00000002);
      return XMPNext();
    }
    case 0x00070007: {
      REX_ASSERT(!buffer_length || buffer_length == 4);
      REX_ASSERT(*memory_->TranslateVirtual<const be_u32*>(buffer_ptr) == 0x00000002);
      return XMPPrevious();
    }
    case 0x00070008: {
      REX_ASSERT(!buffer_length || buffer_length == 16);
      struct {
        be_u32 xmp_client;
        be_u32 playback_mode;
        be_u32 repeat_mode;
        be_u32 flags;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 16);

      REX_ASSERT(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      REXLOG_DEBUG(kernel, "XMPSetPlaybackBehavior({:08X}, {:08X}, {:08X})",
                   static_cast<u32>(args->playback_mode), static_cast<u32>(args->repeat_mode),
                   static_cast<u32>(args->flags));
      playback_mode_ = static_cast<PlaybackMode>(static_cast<u32>(args->playback_mode));
      repeat_mode_ = static_cast<RepeatMode>(static_cast<u32>(args->repeat_mode));
      unknown_flags_ = args->flags;
      runtime_.notifier().BroadcastNotification(kMsgPlaybackBehaviorChanged, 0);
      return X_E_SUCCESS;
    }
    case 0x00070009: {
      REX_ASSERT(!buffer_length || buffer_length == 8);
      struct {
        be_u32 xmp_client;
        be_u32 state_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 8);

      REX_ASSERT(args->xmp_client == 0x00000002);
      return XMPGetStatus(args->state_ptr);
    }
    case 0x0007000B: {
      REX_ASSERT(!buffer_length || buffer_length == 8);
      struct {
        be_u32 xmp_client;
        be_u32 volume_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 8);

      REX_ASSERT(args->xmp_client == 0x00000002);
      REXLOG_DEBUG(kernel, "XMPGetVolume({:08X})", static_cast<u32>(args->volume_ptr));
      *memory_->TranslateVirtual<be_f32*>(args->volume_ptr) = volume_;
      return X_E_SUCCESS;
    }
    case 0x0007000C: {
      REX_ASSERT(!buffer_length || buffer_length == 8);
      struct {
        be_u32 xmp_client;
        be_f32 value;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 8);

      REX_ASSERT(args->xmp_client == 0x00000002);
      REXLOG_DEBUG(kernel, "XMPSetVolume({:g})", static_cast<f32>(args->value));
      volume_ = args->value;
      return X_E_SUCCESS;
    }
    case 0x0007000D: {
      REX_ASSERT(!buffer_length || buffer_length == 36);
      struct {
        be_u32 xmp_client;
        be_u32 storage_ptr;
        be_u32 storage_size;
        be_u32 songs_ptr;
        be_u32 song_count;
        be_u32 playlist_name_ptr;
        be_u32 flags;
        be_u32 song_handles_ptr;
        be_u32 playlist_handle_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 36);

      *memory_->TranslateVirtual<be_u32*>(args->playlist_handle_ptr) = args->storage_ptr;
      REX_ASSERT(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      std::u16string playlist_name;
      if (!args->playlist_name_ptr) {
        playlist_name = u"";
      } else {
        playlist_name = memory::load_and_swap<std::u16string>(
            memory_->TranslateVirtual(args->playlist_name_ptr));
      }
      // storage_ptr is a XamAlloc of storage_size bytes.
      REX_ASSERT(static_cast<u32>(args->storage_size) ==
                 4 + static_cast<u32>(args->song_count) * 128);
      return XMPCreateTitlePlaylist(args->songs_ptr, args->song_count, args->playlist_name_ptr,
                                    playlist_name, args->flags, args->song_handles_ptr,
                                    args->storage_ptr);
    }
    case 0x0007000E: {
      REX_ASSERT(!buffer_length || buffer_length == 12);
      struct {
        be_u32 xmp_client;
        be_u32 unk_ptr;
        be_u32 info_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 12);

      auto* info = memory_->TranslateVirtual<XMP_SONG_INFO*>(args->info_ptr);
      REX_ASSERT(args->xmp_client == 0x00000002);
      REX_ASSERT(args->unk_ptr == 0);
      REXLOG_ERROR(kernel, "XMPGetInfo?({:08X}, {:08X})", static_cast<u32>(args->unk_ptr),
                   static_cast<u32>(args->info_ptr));
      if (!active_playlist_ || active_playlist_->songs.empty()) {
        return X_E_FAIL;
      }
      auto& song = active_playlist_->songs[active_song_index_];
      info->handle = song->handle;
      memory::store_and_swap<std::u16string>(info->name, song->name);
      memory::store_and_swap<std::u16string>(info->artist, song->artist);
      memory::store_and_swap<std::u16string>(info->album, song->album);
      memory::store_and_swap<std::u16string>(info->album_artist, song->album_artist);
      memory::store_and_swap<std::u16string>(info->genre, song->genre);
      info->track_number = song->track_number;
      info->duration_ms = song->duration_ms;
      info->format = static_cast<u32>(song->format);
      return X_E_SUCCESS;
    }
    case 0x00070013: {
      REX_ASSERT(!buffer_length || buffer_length == 8);
      struct {
        be_u32 xmp_client;
        be_u32 storage_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 8);

      u32 playlist_handle = *memory_->TranslateVirtual<const be_u32*>(args->storage_ptr);
      REX_ASSERT(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      return XMPDeleteTitlePlaylist(playlist_handle);
    }
    case 0x0007001A: {
      REX_ASSERT(!buffer_length || buffer_length == 12);
      struct {
        be_u32 xmp_client;
        be_u32 controller;
        be_u32 playback_client;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 12);

      REX_ASSERT((args->xmp_client == 0x00000002 && args->controller == 0x00000000) ||
                 (args->xmp_client == 0x00000000 && args->controller == 0x00000001));
      REXLOG_DEBUG(kernel, "XMPSetPlaybackController({:08X}, {:08X})",
                   static_cast<u32>(args->controller), static_cast<u32>(args->playback_client));

      playback_client_ = PlaybackClient(static_cast<u32>(args->playback_client));
      runtime_.notifier().BroadcastNotification(kMsgPlaybackControllerChanged,
                                                !args->playback_client);
      return X_E_SUCCESS;
    }
    case 0x0007001B: {
      REX_ASSERT(!buffer_length || buffer_length == 12);
      struct {
        be_u32 xmp_client;
        be_u32 controller_ptr;
        be_u32 locked_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 12);

      REX_ASSERT(args->xmp_client == 0x00000002);
      REXLOG_DEBUG(kernel, "XMPGetPlaybackController({:08X}, {:08X}, {:08X})",
                   static_cast<u32>(args->xmp_client), static_cast<u32>(args->controller_ptr),
                   static_cast<u32>(args->locked_ptr));
      *memory_->TranslateVirtual<be_u32*>(args->controller_ptr) = 0;
      *memory_->TranslateVirtual<be_u32*>(args->locked_ptr) = 0;

      if (!KernelThread::GetCurrentThread()->main_thread()) {
        // Titles poll this in a loop, the delay keeps them from starving other threads.
        rex::runtime::KernelThread::Sleep(std::chrono::milliseconds(10));
      }

      return X_E_SUCCESS;
    }
    case 0x00070029: {
      REX_ASSERT(!buffer_length || buffer_length == 16);
      struct {
        be_u32 xmp_client;
        be_u32 playback_mode_ptr;
        be_u32 repeat_mode_ptr;
        be_u32 unk3_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 16);

      REX_ASSERT(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      REXLOG_DEBUG(kernel, "XMPGetPlaybackBehavior({:08X}, {:08X}, {:08X})",
                   static_cast<u32>(args->playback_mode_ptr),
                   static_cast<u32>(args->repeat_mode_ptr), static_cast<u32>(args->unk3_ptr));
      if (args->playback_mode_ptr) {
        *memory_->TranslateVirtual<be_u32*>(args->playback_mode_ptr) =
            static_cast<u32>(playback_mode_);
      }
      if (args->repeat_mode_ptr) {
        *memory_->TranslateVirtual<be_u32*>(args->repeat_mode_ptr) = static_cast<u32>(repeat_mode_);
      }
      if (args->unk3_ptr) {
        *memory_->TranslateVirtual<be_u32*>(args->unk3_ptr) = unknown_flags_;
      }
      return X_E_SUCCESS;
    }
    case 0x0007002E: {
      REX_ASSERT(!buffer_length || buffer_length == 12);
      // Size query for the XamAlloc whose result is passed to message 0x0007000D.
      struct {
        be_u32 xmp_client;
        be_u32 song_count;
        be_u32 size_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 12);

      REX_ASSERT(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      *memory_->TranslateVirtual<be_u32*>(args->size_ptr) =
          4 + static_cast<u32>(args->song_count) * 128;
      return X_E_SUCCESS;
    }
    case 0x0007003D: {
      REXLOG_DEBUG(kernel, "XMPCaptureOutput(...)");
      REX_ASSERT_ALWAYS("XMP output not unimplemented");
      return X_E_FAIL;
    }
    default:
      break;
  }
  REX_FAIL(kernel, X_E_FAIL,
           "Unimplemented XMP message app={:08X}, msg={:08X}, arg1={:08X}, "
           "arg2={:08X}",
           app_id(), message, buffer_ptr, buffer_length);
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
