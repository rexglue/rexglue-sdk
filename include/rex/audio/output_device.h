/**
 * @file        rex/audio/output_device.h
 *
 * @brief       The sound output device: which ones there are, which one plays.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <string>
#include <vector>

namespace rex::audio {

/// The sound output devices connected now, by the names the system gives
/// them (Linux: the PipeWire or PulseAudio sink's description; Windows: the
/// endpoint's name in Sound settings). A name already in the list gets " #2",
/// " #3" ... Setting `audio_device` to one of these plays the sound there; an
/// empty `audio_device` follows the system's default device. The setting is
/// live, kept in the settings file like any other, and when its device is
/// unplugged the sound moves to the system default until it is back.
/// Empty before audio starts. Safe to call from any thread.
std::vector<std::string> ListOutputDevices();

/// The name of the device the sound plays on now ("" before audio starts).
/// Safe to call from any thread.
std::string CurrentOutputDevice();

}  // namespace rex::audio
