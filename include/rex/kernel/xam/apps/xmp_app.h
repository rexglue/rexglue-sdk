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

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/macros.h>
#include <rex/runtime.h>
#include <rex/runtime/xam/app_manager.h>
#include <rex/platform/lock_order.h>

namespace rex {
namespace kernel {
namespace xam {
namespace apps {

/**
 * Xmp app 0xFA, which serves the music player state and title playlists.
 */
class REX_RUNTIME_API XmpApp : public runtime::xam::App {
 public:
  /** Playback state reported to the title. */
  enum class State : uint32_t {
    kIdle = 0,    /**< Nothing is playing. */
    kPlaying = 1, /**< A song is playing. */
    kPaused = 2,  /**< Playback is paused. */
  };
  /** Which side controls playback. */
  enum class PlaybackClient : uint32_t {
    kSystem = 0, /**< The system music player. */
    kTitle = 1,  /**< The title. */
  };
  /** Playback order set by the guest through XMPSetPlaybackBehavior. */
  enum class PlaybackMode : uint32_t {
    kUnknown = 0, /**< Value 0, XMP_PLAYBACKMODE_INORDER in the XDK. */
  };
  /** Repeat mode set by the guest through XMPSetPlaybackBehavior. */
  enum class RepeatMode : uint32_t {
    kUnknown = 0, /**< Value 0, XMP_REPEATMODE_PLAYLIST in the XDK. */
  };
  /** A song added to a title playlist. */
  struct Song {
    /** Audio container format. */
    enum class Format : uint32_t {
      kWma = 0, /**< Windows Media Audio. */
      kMp3 = 1, /**< MP3. */
    };

    uint32_t handle;             /**< Handle for the song. */
    std::u16string file_path;    /**< Path of the song added to the playlist. */
    std::u16string name;         /**< Song title. */
    std::u16string artist;       /**< Artist name. */
    std::u16string album;        /**< Album name. */
    std::u16string album_artist; /**< Album artist name. */
    std::u16string genre;        /**< Genre name. */
    uint32_t track_number;       /**< Track number on the album. */
    uint32_t duration_ms;        /**< Length in milliseconds. */
    Format format;               /**< Audio container format. */
  };
  /** A playlist created by the guest. */
  struct Playlist {
    uint32_t handle;                          /**< Handle for the playlist. */
    std::u16string name;                      /**< Playlist name. */
    uint32_t flags;                           /**< Flags passed at creation. */
    std::vector<std::unique_ptr<Song>> songs; /**< Songs in play order. */
  };

  /**
   * @param runtime The runtime the app serves.
   */
  explicit XmpApp(Runtime& runtime);

  /**
   * Writes the playback state.
   *
   * @param status_ptr Guest address that receives the State value.
   * @return X_E_SUCCESS.
   */
  X_HRESULT XMPGetStatus(uint32_t status_ptr);

  /**
   * Creates a playlist from songs in guest memory.
   *
   * @param songs_ptr Guest address of the song descriptors, or 0 for an empty playlist.
   * @param song_count Number of song descriptors.
   * @param playlist_name_ptr Guest address of the playlist name, used only for logging.
   * @param playlist_name The playlist name.
   * @param flags Playlist flags.
   * @param out_song_handles Guest address of an array that receives the song handles, or 0 to skip.
   * @param out_playlist_handle Guest address that receives the playlist handle, or 0 to skip.
   * @return X_E_SUCCESS.
   */
  X_HRESULT XMPCreateTitlePlaylist(uint32_t songs_ptr, uint32_t song_count,
                                   uint32_t playlist_name_ptr, const std::u16string& playlist_name,
                                   uint32_t flags, uint32_t out_song_handles,
                                   uint32_t out_playlist_handle);
  /**
   * Deletes a playlist and stops playback when it was the active one.
   *
   * @param playlist_handle The playlist handle.
   * @return X_E_SUCCESS, or X_E_NOTFOUND when the handle is unknown.
   */
  X_HRESULT XMPDeleteTitlePlaylist(uint32_t playlist_handle);
  /**
   * Starts playing a playlist from its first song.
   *
   * @param playlist_handle The playlist handle.
   * @param song_handle Handle of the song to start with, unused.
   * @return X_E_SUCCESS, or X_E_NOTFOUND when the handle is unknown.
   */
  X_HRESULT XMPPlayTitlePlaylist(uint32_t playlist_handle, uint32_t song_handle);
  /**
   * Resumes paused playback.
   *
   * @return X_E_SUCCESS.
   */
  X_HRESULT XMPContinue();
  /**
   * Stops playback and clears the active playlist.
   *
   * @param unk Must be 0.
   * @return X_E_SUCCESS.
   */
  X_HRESULT XMPStop(uint32_t unk);
  /**
   * Pauses playback when a song is playing.
   *
   * @return X_E_SUCCESS.
   */
  X_HRESULT XMPPause();
  /**
   * Advances to the next song of the active playlist, wrapping at the end.
   *
   * @return X_E_SUCCESS, or X_E_NOTFOUND when no playlist is active.
   */
  X_HRESULT XMPNext();
  /**
   * Moves to the previous song of the active playlist, wrapping at the start.
   *
   * @return X_E_SUCCESS, or X_E_NOTFOUND when no playlist is active.
   */
  X_HRESULT XMPPrevious();

  /**
   * Handles 1 message sent to the Xmp app.
   *
   * @param message The message identifier.
   * @param buffer_ptr Guest address of the message buffer.
   * @param buffer_length Size of the message buffer in bytes.
   * @return The result code returned to the title.
   */
  X_HRESULT DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                uint32_t buffer_length) override;

 private:
  static const uint32_t kMsgStateChanged = 0x0A000001;
  static const uint32_t kMsgPlaybackBehaviorChanged = 0x0A000002;
  static const uint32_t kMsgPlaybackControllerChanged = 0x0A000003;

  void OnStateChanged();

  State state_;
  PlaybackClient playback_client_;
  PlaybackMode playback_mode_;
  RepeatMode repeat_mode_;
  uint32_t unknown_flags_;
  float volume_;
  Playlist* active_playlist_;
  int active_song_index_;

  rex::platform::RankedMutex mutex_{rex::platform::LockRank::Leaf};
  std::unordered_map<u32, std::unique_ptr<Playlist>> playlists_;
  uint32_t next_playlist_handle_;
  uint32_t next_song_handle_;
};

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
