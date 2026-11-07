/**
 * @file        xam/xam_title.h
 * @brief       The kernel interfaces of one title as XAM reads them (private)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/kernel/module.h>
#include <rex/kernel/object/notification_broadcaster.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/thread_registry.h>
#include <rex/xam/title_lifecycle.h>

namespace rex::xam {

/** What the kernel hands XAM at attach, and the per title interfaces the kernel module serves. */
struct XAMTitle : XAMKernelInterfaces {
  /**
   * Looks up the interfaces of the title the kernel is setting up, which must exist.
   *
   * @param title What the kernel handed XAM at attach.
   */
  explicit XAMTitle(const XAMKernelInterfaces& title);

  kernel::IObjectTable& objects;
  kernel::IThreadRegistry& threads;
  kernel::IModuleRegistry& modules;
  kernel::INotificationBroadcaster& notifier;
  kernel::IOverlappedDispatcher& overlapped_dispatcher;
};

}  // namespace rex::xam
