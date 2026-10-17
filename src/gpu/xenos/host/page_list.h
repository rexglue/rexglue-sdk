/**
 * @file        gpu/xenos/host/page_list.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/types.h>

namespace rex::gpu {

/**
 * Moves the submitted pages the GPU has finished with to the end of the writable list.
 *
 * @tparam Page A page with last_submission_index and next.
 * @param submitted_first The first submitted page.
 * @param submitted_last The last submitted page.
 * @param writable_first The first writable page.
 * @param writable_last The last writable page.
 * @param completed_submission_index The newest submission the GPU has completed.
 */
template <typename Page>
void ReclaimPages(Page*& submitted_first, Page*& submitted_last, Page*& writable_first,
                  Page*& writable_last, u64 completed_submission_index) {
  while (submitted_first) {
    if (submitted_first->last_submission_index > completed_submission_index) {
      break;
    }
    if (writable_last) {
      writable_last->next = submitted_first;
    } else {
      writable_first = submitted_first;
    }
    writable_last = submitted_first;
    submitted_first = submitted_first->next;
    writable_last->next = nullptr;
  }
  if (!submitted_first) {
    submitted_last = nullptr;
  }
}

/**
 * Makes every submitted page writable and resets the submission index of all writable pages, for
 * a submission timeline that restarts.
 *
 * @tparam Page A page with last_submission_index and next.
 * @param submitted_first The first submitted page.
 * @param submitted_last The last submitted page.
 * @param writable_first The first writable page.
 * @param writable_last The last writable page.
 */
template <typename Page>
void ResetPageTimeline(Page*& submitted_first, Page*& submitted_last, Page*& writable_first,
                       Page*& writable_last) {
  if (writable_last) {
    writable_last->next = submitted_first;
  } else {
    writable_first = submitted_first;
  }
  writable_last = submitted_last;
  submitted_first = nullptr;
  submitted_last = nullptr;

  Page* page = writable_first;
  while (page) {
    page->last_submission_index = 0;
    page = page->next;
  }
}

}  // namespace rex::gpu
