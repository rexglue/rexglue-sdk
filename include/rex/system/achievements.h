/**
 * @file        rex/system/achievements.h
 * @brief       Convenience facade over the live AchievementManager for hooks.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>

#include <rex/system/achievement_store.h>

namespace rex::system {

// Free-function facade over the active KernelState's AchievementManager,
// intended for guest hooks (REX_HOOK) and recomp app code that just want to
// fire achievement calls without reaching through kernel_state()->achievements()
// every time.
//
// Every call is null-safe: if no runtime/kernel is live yet, it is a no-op that
// returns false. For advanced use -- listing, callbacks, persistence control --
// reach the manager directly via rex::system::kernel_state()->achievements().

// Defines a new achievement or overrides an existing one with the same ID.
// Call before unlocking it, e.g. from TitleApp::OnTitleLaunch(). IDs must be
// non-zero; to avoid colliding with title (XDBF) achievement IDs, use a high
// range (e.g. 0x10000+) for recomp-authored achievements.
bool RegisterAchievement(AchievementInfo info);

// Unlocks an achievement by ID. Shows the unlock toast by default. Safe to call
// from any thread, including guest hook threads. No-op if the ID was never
// registered. Returns true only on a first-time unlock (false if already
// unlocked, unknown, or no runtime is live).
bool UnlockAchievement(uint32_t id, bool show_toast = true);

// True if the achievement is currently unlocked.
bool IsAchievementUnlocked(uint32_t id);

}  // namespace rex::system
