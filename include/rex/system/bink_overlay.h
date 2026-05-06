#ifndef REX_SYSTEM_BINK_OVERLAY_H_
#define REX_SYSTEM_BINK_OVERLAY_H_

// In-game cinematic overlay.
//
// The recompiled Bink runtime decodes .bik files inside the guest binary, but
// the decoded frames don't always reach the swap chain on aarch64 hosts where
// SIGSEGV-based texture-cache invalidation isn't reliable. To get cinematics
// rendering without an external mpv side-channel, we hook NtCreateFile, open
// the same .bik file via system libavformat / libavcodec in parallel, and
// composite the decoded RGB frames into the swap chain just before present.
//
// Activated only when the build links against system FFmpeg (linked at SDK
// build time via REX_HAS_FFMPEG). When unavailable the helpers are no-ops so
// callers don't need to guard.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace rex::system {

struct BinkOverlayFrame {
  // RGBA8 row-major pixels, size = width * height * 4.
  const uint8_t* data = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  // Monotonic-clock-microseconds timestamp this frame should be displayed.
  int64_t presentation_us = 0;
};

class BinkOverlay {
 public:
  static BinkOverlay* Get();

  BinkOverlay();
  ~BinkOverlay();

  // Called from NtCreateFile when the guest opens a path. If it ends in
  // .bik (case-insensitive) and FFmpeg is available, kicks off a decode
  // thread for that file. Multiple concurrent files are supported (only
  // the most recently opened is rendered).
  void OnFileOpened(std::string_view host_path);

  // Called when the guest closes the file. Stops the decoder.
  void OnFileClosed(std::string_view host_path);

  // True if there is at least one active decoder with frames available.
  bool HasActiveFmv() const;

  // Fetches the current frame to display (the most recent decoded frame
  // whose presentation time has passed). Returns false when no decoder is
  // active or no frame is ready yet. The pointer remains valid until the
  // next call into BinkOverlay on this thread.
  bool AcquireCurrentFrame(BinkOverlayFrame& out_frame);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rex::system

#endif  // REX_SYSTEM_BINK_OVERLAY_H_
