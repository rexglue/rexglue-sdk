/**
 * @file        xam/app/xam_apps.h
 * @brief       The XAM, XGI, XLiveBase and XMP apps that serve guest app messages
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/kernel/xam/apps/xam_app.h, xgi_app.h,
 *              xlivebase_app.h and xmp_app.h (Ben Vanik, 2015 to 2020).
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/xam/achievement_manager.h>
#include <rex/xam/content_manager.h>
#include <rex/xam/app_manager.h>
#include <rex/platform/lock_order.h>

namespace rex {
namespace xam {

namespace apps {

/** XAM app 0xFE, which serves content enumeration messages. */
class XAMApp : public rex::xam::XAMApplet {
 public:
  /**
   * @param title The kernel interfaces of the title the app serves.
   */
  XAMApp(const XAMKernelInterfaces& title, rex::xam::IContentManager& content);

  kernel::X_HRESULT DispatchMessageSync(u32 message, u32 buffer_ptr, u32 buffer_length) override;

 private:
  rex::xam::IContentManager& content_;
};

/** XGI app 0xFB, which serves XGI messages such as user contexts and properties. */
class XGIApp : public rex::xam::XAMApplet {
 public:
  /**
   * @param title The kernel interfaces of the title the app serves.
   * @param achievements The achievement manager of the title.
   */
  XGIApp(const XAMKernelInterfaces& title, rex::xam::IAchievementManager& achievements);

  kernel::X_HRESULT DispatchMessageSync(u32 message, u32 buffer_ptr, u32 buffer_length) override;

 private:
  rex::xam::IAchievementManager& achievements_;
};

/** XLiveBase app 0xFC, which serves online service queries such as the logon ID and NAT type. */
class XLiveBaseApp : public rex::xam::XAMApplet {
 public:
  /**
   * @param title The kernel interfaces of the title the app serves.
   */
  explicit XLiveBaseApp(const XAMKernelInterfaces& title);

  kernel::X_HRESULT DispatchMessageSync(u32 message, u32 buffer_ptr, u32 buffer_length) override;
};

/** XMP app 0xFA, which serves the music player state and title playlists. */
class XMPApp : public rex::xam::XAMApplet {
 public:
  /** Playback state reported to the title. */
  enum class State : u8 {
    kIdle = 0,
    kPlaying = 1,
    kPaused = 2,
  };
  /** Which side controls playback. */
  // NOLINTNEXTLINE(performance-enum-size): holds the title u32 unchanged
  enum class PlaybackClient : u32 {
    kSystem = 0, /**< The system music player. */
    kTitle = 1,
  };
  /** Playback order set by the guest through XMPSetPlaybackBehavior. */
  // NOLINTNEXTLINE(performance-enum-size): the title reads this u32 back
  enum class PlaybackMode : u32 {
    kUnknown = 0, /**< Value 0, XMP_PLAYBACKMODE_INORDER in the XDK. */
  };
  /** Repeat mode set by the guest through XMPSetPlaybackBehavior. */
  // NOLINTNEXTLINE(performance-enum-size): the title reads this u32 back
  enum class RepeatMode : u32 {
    kUnknown = 0, /**< Value 0, XMP_REPEATMODE_PLAYLIST in the XDK. */
  };
  /** A song added to a title playlist. */
  struct Song {
    /** Audio container format. */
    // NOLINTNEXTLINE(performance-enum-size): the title reads this u32 back
    enum class Format : u32 {
      kWma = 0, /**< Windows Media Audio. */
      kMp3 = 1,
    };

    u32 handle;
    std::u16string file_path;
    std::u16string name;
    std::u16string artist;
    std::u16string album;
    std::u16string album_artist;
    std::u16string genre;
    u32 track_number;
    u32 duration_ms;
    Format format;
  };
  /** A playlist created by the guest. */
  struct Playlist {
    u32 handle;
    std::u16string name;
    u32 flags;                                /**< Flags passed at creation. */
    std::vector<std::unique_ptr<Song>> songs; /**< Songs in play order. */
  };

  /**
   * @param title The kernel interfaces of the title the app serves.
   */
  explicit XMPApp(const XAMKernelInterfaces& title);

  /**
   * Writes the playback state.
   *
   * @param state_ptr Guest address that receives the State value.
   * @return X_E_SUCCESS.
   */
  kernel::X_HRESULT XMPGetStatus(u32 state_ptr);

  /**
   * Creates a playlist from songs in guest memory.
   *
   * @param songs_ptr Guest address of the song descriptors, or 0 for an empty playlist.
   * @param song_count Number of song descriptors.
   * @param playlist_name_ptr Guest address of the playlist name, used only for logging.
   * @param playlist_name The playlist name as a UTF-16 string.
   * @param flags Playlist flags.
   * @param out_song_handles Guest address of an array that receives the song handles, or 0 to skip.
   * @param out_playlist_handle Guest address that receives the playlist handle, or 0 to skip.
   * @return X_E_SUCCESS.
   */
  kernel::X_HRESULT XMPCreateTitlePlaylist(u32 songs_ptr, u32 song_count, u32 playlist_name_ptr,
                                           const std::u16string& playlist_name, u32 flags,
                                           u32 out_song_handles, u32 out_playlist_handle);
  /**
   * Deletes a playlist and stops playback when it was the active one.
   *
   * @return X_E_SUCCESS, or X_E_NOTFOUND when the handle is unknown.
   */
  kernel::X_HRESULT XMPDeleteTitlePlaylist(u32 playlist_handle);
  /**
   * Starts playing a playlist from its first song.
   *
   * @param song_handle Handle of the song to start with, unused.
   * @return X_E_SUCCESS, or X_E_NOTFOUND when the handle is unknown.
   */
  kernel::X_HRESULT XMPPlayTitlePlaylist(u32 playlist_handle, u32 song_handle);
  /**
   * Resumes paused playback.
   *
   * @return X_E_SUCCESS.
   */
  kernel::X_HRESULT XMPContinue();
  /**
   * Stops playback and clears the active playlist.
   *
   * @param reserved Must be 0.
   * @return X_E_SUCCESS.
   */
  kernel::X_HRESULT XMPStop(u32 reserved);
  /**
   * Pauses playback when a song is playing.
   *
   * @return X_E_SUCCESS.
   */
  kernel::X_HRESULT XMPPause();
  /**
   * Advances to the next song of the active playlist, wrapping at the end.
   *
   * @return X_E_SUCCESS, or X_E_NOTFOUND when no playlist is active.
   */
  kernel::X_HRESULT XMPNext();
  /**
   * Moves to the previous song of the active playlist, wrapping at the start.
   *
   * @return X_E_SUCCESS, or X_E_NOTFOUND when no playlist is active.
   */
  kernel::X_HRESULT XMPPrevious();

  kernel::X_HRESULT DispatchMessageSync(u32 message, u32 buffer_ptr, u32 buffer_length) override;

 private:
  static const u32 kMsgStateChanged = 0x0A000001;
  static const u32 kMsgPlaybackBehaviorChanged = 0x0A000002;
  static const u32 kMsgPlaybackControllerChanged = 0x0A000003;

  void OnStateChanged();

  State state_;
  PlaybackClient playback_client_;
  PlaybackMode playback_mode_;
  RepeatMode repeat_mode_;
  u32 unknown_flags_;
  f32 volume_;
  Playlist* active_playlist_;
  size_t active_song_index_;

  rex::platform::RankedMutex mutex_{rex::platform::LockRank::kLeaf};
  std::unordered_map<u32, std::unique_ptr<Playlist>> playlists_;
  u32 next_playlist_handle_;
  u32 next_song_handle_;
};

}  // namespace apps

}  // namespace xam
}  // namespace rex
