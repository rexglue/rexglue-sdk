/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_LAZY_PUBLICATION_H_
#define XENIA_GPU_METAL_METAL_LAZY_PUBLICATION_H_

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "fmt/format.h"
#include <rex/types.h>
#include <rex/logging/log_category.h>
#include <gpu/xenos/command_processor.h>
#include "rex/graphics/metal/metal_tiled_publish_run.h"
#include "rex/graphics/texture_address.h"

namespace rex {
namespace graphics {
namespace metal {

// Copies the 32-bit words of [src, src + length) that differ from `pre` into
// dst and returns the bytes copied. A memexport writes whole words, and only
// the elements it exports; the rest of its range in the shared buffer is what
// the buffer held when the export was armed, which RAM may since have
// overwritten with newer CPU data. Battlefield 3 keeps its job queues in the
// block its pixel shader exports into, and whole-page copies rolled the
// queues back (a request completed twice, call through a null callback).
inline uint64_t CopyChangedWords(uint8_t* dst, const uint8_t* src,
                                 const uint8_t* pre, uint32_t length) {
  uint64_t copied = 0;
  uint32_t i = 0;
  for (; i + 8 <= length; i += 8) {
    uint64_t s8, p8;
    std::memcpy(&s8, src + i, 8);
    std::memcpy(&p8, pre + i, 8);
    if (s8 == p8) {
      continue;
    }
    for (uint32_t w = i; w < i + 8; w += 4) {
      if (std::memcmp(src + w, pre + w, 4)) {
        std::memcpy(dst + w, src + w, 4);
        copied += 4;
      }
    }
  }
  for (; i < length; ++i) {
    if (src[i] != pre[i]) {
      dst[i] = src[i];
      ++copied;
    }
  }
  return copied;
}

// Stage 2 of optimize-metal-gpu-resident-resolve-coherence: demand-driven RAM
// publication of GPU-resident resolve output.
//
// A GPU-current PendingWriteback owes guest RAM its bytes. Instead of copying
// them at every swap / fence packet, the command processor ARMS the record
// here: every 4 KiB guest page of the record's tiled extent is mapped to the
// record (page -> owners) and the physical heap protects the pages no-access
// with a read watch. The first CPU touch of a page faults, the physical heap
// calls the read callback, and this table copies the OWNING records' bytes for
// the touched pages from the shared buffer (CPU-visible, already at their
// guest tiled addresses) into guest RAM, on the faulting thread, after those
// records' submissions have completed. Pages the CPU never touches are never
// copied.
//
// A page can have several owners (two resolves sharing a page, an export
// overlapping a resolve). The shared buffer is the single source of the bytes,
// so an owner only contributes COVERAGE (which bytes are GPU-written and may
// be copied); the fault copies the union of the owners' coverage after waiting
// for the newest owning submission. Bytes outside every owner's coverage are
// never copied: the shared buffer may hold stale content there.
//
// Thread model: the GPU thread arms and retires; any guest thread may fault.
// All table state is behind `mutex_`. The copy itself reads the shared
// buffer contents (stable pointer) and writes RAM through the physical alias,
// which the watch does not protect.
class MetalLazyPublication {
 public:
  static bool PartialPublishEnabled() {
    static const bool s_enabled = [] {
      const char* v = std::getenv("XE_METAL_LAZY_PARTIAL");
      return !(v && v[0] == '0' && !v[1]);
    }();
    return s_enabled;
  }
  // XE_METAL_LAZY_BULK=0: an access over many pages copies them one by one
  // (the previous behavior).
  static bool BulkPublishEnabled() {
    static const bool s_enabled = [] {
      const char* v = std::getenv("XE_METAL_LAZY_BULK");
      return !(v && v[0] == '0' && !v[1]);
    }();
    return s_enabled;
  }
  // XE_METAL_LAZY_CLAIM=0: an access copies what it found before it waited,
  // whoever published the page meanwhile (the previous behavior).
  static bool ClaimEnabled() {
    static const bool s_enabled = [] {
      const char* v = std::getenv("XE_METAL_LAZY_CLAIM");
      return !(v && v[0] == '0' && !v[1]);
    }();
    return s_enabled;
  }
  // A fault touches one host page (four of these on a 16 KB host).
  static constexpr size_t kBulkPages = 16;
  static constexpr uint32_t kPageShift = 12;
  static constexpr uint32_t kPageSize = 1u << kPageShift;

  // Geometry of a GPU-current record, copied so a fault never touches the
  // command processor's pending list.
  struct Owner {
    uint64_t authority_sequence = 0;
    uint64_t submission = 0;  // 0 = untagged, never fault-publishable.
    uint32_t guest_addr = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bpp = 0;
    uint32_t dest_pitch_aligned = 0;  // texels; 0 = align32(width)
    uint32_t dest_offset_x = 0;
    uint32_t dest_offset_y = 0;
    uint32_t dest_extent_bytes = 0;
    uint32_t range_bytes = 0;
    // Memexport output: a plain linear range in the shared buffer.
    bool linear = false;
    // Linear only: the shared buffer's [guest_addr, guest_addr + range_bytes)
    // before the export ran. When set, a copy moves only the words the GPU
    // changed (CopyChangedWords) instead of the whole covered range.
    std::shared_ptr<const std::vector<uint8_t>> pre_image;
  };

  struct Stats {
    uint64_t armed_records = 0;
    uint64_t armed_pages = 0;
    uint64_t faults = 0;
    uint64_t fault_pages_copied = 0;
    uint64_t fault_bytes_copied = 0;
    uint64_t stale_reads = 0;  // owner not yet committed at fault time
    // Stale pages whose older, completed owners were still published (the
    // read sees the newest finished producer, as on hardware).
    uint64_t stale_partial_pages = 0;
    uint64_t retired_records = 0;
    // Owners that left their page while an access waited for them. Replaced
    // by a newer record that still owns the page: copied (superseded).
    // Published by a drain or another access, no owner left: not copied
    // again (already_published), or, with XE_METAL_LAZY_CLAIM=0, copied a
    // second time (raced).
    uint64_t superseded_copies = 0;
    uint64_t already_published = 0;
    uint64_t raced_copies = 0;
    // Accesses that waited for another access to finish copying their pages.
    uint64_t in_flight_waits = 0;
  };

  // Arms `owner`: records page ownership for [guest_addr, guest_addr +
  // range_bytes) and returns the page range so the caller can enable the read
  // watch. A linear owner that covers a whole page replaces that page's older
  // owners (their coverage is fully superseded); otherwise owners accumulate.
  void Arm(const Owner& owner, uint32_t* out_first_page,
           uint32_t* out_page_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t first = owner.guest_addr >> kPageShift;
    uint32_t last = (owner.guest_addr + owner.range_bytes - 1) >> kPageShift;
    std::vector<uint32_t>& owned = owner_pages_[owner.authority_sequence];
    owned.reserve(owned.size() + (last - first + 1));
    for (uint32_t page = first; page <= last; ++page) {
      std::vector<Owner>& owners = PageOwnersForWrite(page);
      if (owner.linear && owner.guest_addr <= (page << kPageShift) &&
          owner.guest_addr + owner.range_bytes >= ((page + 1) << kPageShift)) {
        for (const Owner& old : owners) {
          ForgetPage(old.authority_sequence, page);
        }
        owners.clear();
      } else {
        // Re-arming the same record (watch re-enable) must not duplicate it.
        bool present = false;
        for (const Owner& old : owners) {
          if (old.authority_sequence == owner.authority_sequence) {
            present = true;
            break;
          }
        }
        if (present) {
          continue;
        }
      }
      owners.push_back(owner);
      owned.push_back(page);
    }
    ++stats_.armed_records;
    stats_.armed_pages += last - first + 1;
    if (out_first_page) {
      *out_first_page = first;
    }
    if (out_page_count) {
      *out_page_count = last - first + 1;
    }
  }

  // Drops every page still owned by `authority_sequence` (the record was
  // published eagerly or superseded). Returns the number of pages dropped.
  uint32_t Retire(uint64_t authority_sequence) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = owner_pages_.find(authority_sequence);
    if (it == owner_pages_.end()) {
      return 0;
    }
    uint32_t dropped = 0;
    for (uint32_t page : it->second) {
      dropped += RemoveOwnerFromPage(page, authority_sequence) ? 1u : 0u;
    }
    owner_pages_.erase(it);
    if (dropped) {
      ++stats_.retired_records;
    }
    return dropped;
  }

  // Drops ownership of every page inside [phys_start, phys_start + length):
  // the contents are irrelevant now (allocation, release, host overwrite).
  uint32_t DropRange(uint32_t phys_start, uint32_t length) {
    if (!length) {
      return 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t first = phys_start >> kPageShift;
    uint32_t last = (phys_start + length - 1) >> kPageShift;
    uint32_t dropped = 0;
    for (uint32_t page = first; page <= last; ++page) {
      std::vector<Owner>* owners = FindPageOwners(page);
      if (!owners) {
        continue;
      }
      for (const Owner& old : *owners) {
        ForgetPage(old.authority_sequence, page);
      }
      ErasePage(page);
      ++dropped;
    }
    return dropped;
  }

  // Appends the page-granular ranges `authority_sequence` still owns
  // (coalesced, ascending). An eager drain of an armed record copies only
  // these: its other pages were published on first touch or discarded by the
  // host, and RAM there may already hold newer CPU data.
  void OwnedRangesOf(uint64_t authority_sequence,
                     std::vector<std::pair<uint32_t, uint32_t>>* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = owner_pages_.find(authority_sequence);
    if (it == owner_pages_.end()) {
      return;
    }
    std::vector<uint32_t> pages = it->second;
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    for (uint32_t page : pages) {
      const std::vector<Owner>* owners = FindPageOwners(page);
      bool held = false;
      if (owners) {
        for (const Owner& o : *owners) {
          if (o.authority_sequence == authority_sequence) {
            held = true;
            break;
          }
        }
      }
      if (!held) {
        continue;
      }
      uint32_t start = page << kPageShift;
      if (!out->empty() && out->back().first + out->back().second == start) {
        out->back().second += kPageSize;
      } else {
        out->emplace_back(start, kPageSize);
      }
    }
  }

  bool Owns(uint64_t authority_sequence) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = owner_pages_.find(authority_sequence);
    return it != owner_pages_.end() && !it->second.empty();
  }

  // Appends the page-granular sub-ranges of [phys_start, phys_start + length)
  // that some armed owner still holds (GPU output not yet published to RAM),
  // coalesced and clipped to the query. Pages the table does not own have
  // been published (RAM holds the GPU bytes) or were never GPU-written.
  void OwnedRanges(uint32_t phys_start, uint32_t length,
                   std::vector<std::pair<uint32_t, uint32_t>>* out) const {
    if (!length) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t end = uint64_t(phys_start) + length;
    uint32_t first = phys_start >> kPageShift;
    uint32_t last = uint32_t((end - 1) >> kPageShift);
    uint64_t run_start = 0, run_end = 0;
    for (uint32_t page = first; page <= last; ++page) {
      const std::vector<Owner>* owners = FindPageOwners(page);
      if (!owners || owners->empty()) {
        continue;
      }
      uint64_t p0 = std::max<uint64_t>(uint64_t(page) << kPageShift,
                                       phys_start);
      uint64_t p1 = std::min<uint64_t>(uint64_t(page + 1) << kPageShift, end);
      if (run_end && p0 == run_end) {
        run_end = p1;
      } else {
        if (run_end) {
          out->emplace_back(uint32_t(run_start), uint32_t(run_end - run_start));
        }
        run_start = p0;
        run_end = p1;
      }
    }
    if (run_end) {
      out->emplace_back(uint32_t(run_start), uint32_t(run_end - run_start));
    }
  }

  // Whether any page of [phys_start, phys_start + length) still has an owner.
  bool OwnsAny(uint32_t phys_start, uint32_t length) const {
    if (!length) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t first = phys_start >> kPageShift;
    uint32_t last =
        uint32_t((uint64_t(phys_start) + length - 1) >> kPageShift);
    for (uint32_t page = first; page <= last; ++page) {
      const std::vector<Owner>* owners = FindPageOwners(page);
      if (owners && !owners->empty()) {
        return true;
      }
    }
    return false;
  }

  // Returns the oldest armed owner registered before `sequence` whose byte
  // range overlaps [phys_start, phys_start + length), or 0. Such an owner's
  // first-access copy would land over a newer record's bytes published to RAM
  // in that range (memexport/resolve ordering census).
  uint64_t OldestOwnerBefore(uint32_t phys_start, uint32_t length,
                             uint64_t sequence) const {
    if (!length) {
      return 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t oldest = 0;
    uint64_t end = uint64_t(phys_start) + length;
    uint32_t first = phys_start >> kPageShift;
    uint32_t last = uint32_t((end - 1) >> kPageShift);
    for (uint32_t page = first; page <= last; ++page) {
      const std::vector<Owner>* owners = FindPageOwners(page);
      if (!owners) {
        continue;
      }
      for (const Owner& owner : *owners) {
        if (owner.authority_sequence >= sequence ||
            (oldest && owner.authority_sequence >= oldest)) {
          continue;
        }
        uint64_t owner_end = uint64_t(owner.guest_addr) + owner.range_bytes;
        if (owner.guest_addr < end && phys_start < owner_end) {
          oldest = owner.authority_sequence;
        }
      }
    }
    return oldest;
  }

  // Fault path. `wait_for_submission(submission)` returns true once the
  // owner's submission has completed on the GPU (false if it cannot be
  // waited for, e.g. not yet committed). `shared_contents` is the shared
  // buffer's CPU pointer (guest physical offset == buffer offset);
  // `translate_physical(addr)` returns the RAM pointer for a guest physical
  // address. Copies only the touched pages, and within them only the owners'
  // coverage. A page whose owners cannot all be waited for stays armed (the
  // caller re-arms the watch) and is counted as a stale read.
  //
  // Returns the number of pages that remain unpublished in the range.
  uint32_t OnAccess(uint32_t phys_start, uint32_t length,
                    const std::function<bool(uint64_t)>& wait_for_submission,
                    const uint8_t* shared_contents,
                    const std::function<uint8_t*(uint32_t)>& translate_physical) {
    if (!length) {
      return 0;
    }
    uint32_t first = phys_start >> kPageShift;
    uint32_t last = (phys_start + length - 1) >> kPageShift;
    const bool claim = ClaimEnabled();
    // Snapshot the owners under the lock; copy without it.
    std::vector<std::pair<uint32_t, std::vector<Owner>>> touched;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      ++stats_.faults;
      if (claim) {
        // Pages another access is copying still have their owners, and RAM
        // does not have all their bytes yet. Let it finish: this access then
        // finds them published.
        WaitForInFlight(lock, first, last);
      }
      for (uint32_t page = first; page <= last; ++page) {
        const std::vector<Owner>* owners = FindPageOwners(page);
        if (owners && !owners->empty()) {
          touched.emplace_back(page, *owners);
        }
      }
    }
    if (touched.empty()) {
      return 0;
    }
    // One answer per submission for the whole access: a range over many pages
    // of one record waits once, and a submission that cannot be waited for is
    // not waited for again page after page.
    std::vector<std::pair<uint64_t, bool>> waited;
    auto wait_once = [&](uint64_t submission) {
      for (const auto& w : waited) {
        if (w.first == submission) {
          return w.second;
        }
      }
      bool completed = wait_for_submission(submission);
      waited.emplace_back(submission, completed);
      return completed;
    };
    // Owners are in arm (producer) order. Publish the completed prefix:
    // a newer owner that cannot be waited for is still uncommitted, so the
    // GPU has not run it and the shared buffer holds exactly the completed
    // owners' results -- what hardware would show a CPU read issued before
    // the newer producer executes. The page stays armed for the rest.
    std::vector<uint32_t> done_counts(touched.size(), 0);
    uint32_t max_done = 0;
    for (size_t t = 0; t < touched.size(); ++t) {
      const std::vector<Owner>& owners = touched[t].second;
      uint32_t done = 0;
      for (const Owner& owner : owners) {
        if (!owner.submission || !wait_once(owner.submission)) {
          break;
        }
        ++done;
      }
      // XE_METAL_LAZY_PARTIAL=0: all-or-nothing (the previous behavior).
      if (done != owners.size() && !PartialPublishEnabled()) {
        done = 0;
      }
      done_counts[t] = done;
      max_done = std::max(max_done, done);
    }
    // The waits take up to milliseconds. A drain or another access may have
    // published a page meanwhile and let the guest write to it: copying the
    // page again would put the GPU's bytes back over that write (Rise of the
    // Tomb Raider: 10,000 pages in a three-minute route came back from the
    // wait already published; Crysis 3: two threads on one page every few
    // frames). Look at the table again, drop what is published, and hold the
    // pages that are left until their bytes are in RAM, so that a second
    // access to one of them waits instead of copying it too.
    std::vector<uint32_t> snapshot_sizes(touched.size(), 0);
    std::vector<uint32_t> snapshot_done = done_counts;
    std::vector<uint32_t> claimed;
    for (size_t t = 0; t < touched.size(); ++t) {
      snapshot_sizes[t] = uint32_t(touched[t].second.size());
    }
    if (claim) {
      std::unique_lock<std::mutex> lock(mutex_);
      WaitForInFlight(lock, first, last);
      max_done = 0;
      for (size_t t = 0; t < touched.size(); ++t) {
        const uint32_t page = touched[t].first;
        std::vector<Owner>& owners = touched[t].second;
        const std::vector<Owner>* now = FindPageOwners(page);
        uint32_t kept = 0;
        for (uint32_t i = 0; i < done_counts[t]; ++i) {
          bool present = false;
          if (now) {
            for (const Owner& o : *now) {
              if (o.authority_sequence == owners[i].authority_sequence) {
                present = true;
                break;
              }
            }
          }
          if (!present) {
            if (!now || now->empty()) {
              const uint64_t n = ++stats_.already_published;
              if (n <= 4 || (n & 16383) == 0) {
                REXLOG_INFO("MetalLazyPublication: page {:08X} of seq={} was "
                       "published during the wait, not copied again (#{}, "
                       "{} superseded copies, {} in-flight waits)",
                       page << kPageShift, owners[i].authority_sequence, n,
                       stats_.superseded_copies, stats_.in_flight_waits);
              }
              continue;
            }
            // A newer record replaced this one and still owns the page (a
            // resolve to the same destination every frame: Just Cause 2
            // thousands of times a run, LEGO Indiana Jones' arena at every
            // boot). The page stays armed for it; this access still gets
            // the record it waited for.
            ++stats_.superseded_copies;
          }
          if (kept != i) {
            owners[kept] = owners[i];
          }
          ++kept;
        }
        owners.erase(owners.begin() + kept, owners.end());
        done_counts[t] = kept;
        max_done = std::max(max_done, kept);
        if (kept) {
          claimed.push_back(page);
        }
      }
      if (!claimed.empty()) {
        // A thread parked by a guest suspend in the middle of the copy would
        // keep its pages held, and the suspender may be the next to touch
        // them.
        rex::threading::BeginSuspendDeferral();
        in_flight_.push_back(&claimed);
      }
    }
    std::vector<uint64_t> page_bytes(touched.size(), 0);
    uint64_t bulk_bytes = 0;
    if (touched.size() < kBulkPages || !BulkPublishEnabled()) {
      for (size_t t = 0; t < touched.size(); ++t) {
        for (uint32_t i = 0; i < done_counts[t]; ++i) {
          page_bytes[t] += CopyPage(touched[t].second[i], touched[t].first,
                                    shared_contents, translate_physical);
        }
      }
    } else {
      // A range of pages (a guest protection change over an arena, a large
      // host read): CopyPage walks a whole 32-row tile band for every page,
      // ~40 us each for a 1280-wide target -- 70 ms for one 7 MB range of
      // LEGO Indiana Jones' frame targets. Walk every record once instead.
      // Round k copies the k-th owner of each page, so a page's owners still
      // land in producer order.
      struct Group {
        const Owner* owner;
        std::vector<uint32_t> pages;
      };
      for (uint32_t k = 0; k < max_done; ++k) {
        std::vector<Group> groups;
        std::unordered_map<uint64_t, size_t> group_of;
        for (size_t t = 0; t < touched.size(); ++t) {
          if (done_counts[t] <= k) {
            continue;
          }
          const Owner& owner = touched[t].second[k];
          auto found = group_of.find(owner.authority_sequence);
          if (found == group_of.end()) {
            found =
                group_of.emplace(owner.authority_sequence, groups.size()).first;
            groups.push_back({&owner, {}});
          }
          // `touched` is in ascending page order, and so is each group.
          groups[found->second].pages.push_back(touched[t].first);
        }
        std::vector<uint64_t> selected;
        for (const Group& group : groups) {
          uint32_t first_page = group.pages.front();
          uint32_t last_page = group.pages.back();
          selected.assign(((last_page - first_page) >> 6) + 1, 0);
          for (uint32_t page : group.pages) {
            uint32_t bit = page - first_page;
            selected[bit >> 6] |= uint64_t(1) << (bit & 63);
          }
          bulk_bytes += CopyPages(*group.owner, first_page, last_page,
                                  selected.data(), shared_contents,
                                  translate_physical);
        }
      }
    }
    uint32_t remaining = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stats_.fault_bytes_copied += bulk_bytes;
      for (size_t t = 0; t < touched.size(); ++t) {
        const uint32_t page = touched[t].first;
        const std::vector<Owner>& owners = touched[t].second;
        const uint32_t done = done_counts[t];
        for (uint32_t i = 0; i < done; ++i) {
          if (!RemoveOwnerFromPage(page, owners[i].authority_sequence) &&
              !claim) {
            // XE_METAL_LAZY_CLAIM=0: the owner left the page between the
            // snapshot and here, and the copy above was made all the same.
            const std::vector<Owner>* left = FindPageOwners(page);
            if (left && !left->empty()) {
              ++stats_.superseded_copies;
            } else {
              const uint64_t n = ++stats_.raced_copies;
              if (n <= 8 || (n & 1023) == 0) {
                REXLOG_WARN("MetalLazyRace #{}: page {:08X} of seq={} was "
                       "published by a drain or a second access while this "
                       "access copied it ({} superseded copies so far)",
                       n, page << kPageShift, owners[i].authority_sequence,
                       stats_.superseded_copies);
              }
            }
          }
          ForgetPage(owners[i].authority_sequence, page);
        }
        if (done) {
          ++stats_.fault_pages_copied;
          stats_.fault_bytes_copied += page_bytes[t];
        }
        if (snapshot_done[t] != snapshot_sizes[t]) {
          ++stats_.stale_reads;
          if (snapshot_done[t]) {
            ++stats_.stale_partial_pages;
          }
          ++remaining;
        }
      }
      if (!claimed.empty()) {
        in_flight_.erase(
            std::find(in_flight_.begin(), in_flight_.end(), &claimed));
      }
    }
    if (!claimed.empty()) {
      in_flight_done_.notify_all();
      rex::threading::EndSuspendDeferral();
    }
    return remaining;
  }

  Stats stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
  }

  // The owners of `page` in arm order (diagnostics).
  std::vector<Owner> PageOwners(uint32_t page) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::vector<Owner>* owners = FindPageOwners(page);
    return owners ? *owners : std::vector<Owner>();
  }

  size_t armed_page_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (FlatPagesEnabled() ? flat_armed_pages_ : 0) + pages_.size();
  }

 private:
  // Removes `authority_sequence` from `page`'s owner list; erases the page
  // when no owner remains. Returns true when the owner was present.
  bool RemoveOwnerFromPage(uint32_t page, uint64_t authority_sequence) {
    std::vector<Owner>* found = FindPageOwners(page);
    if (!found) {
      return false;
    }
    std::vector<Owner>& owners = *found;
    bool removed = false;
    for (size_t i = 0; i < owners.size();) {
      if (owners[i].authority_sequence == authority_sequence) {
        owners.erase(owners.begin() + i);
        removed = true;
      } else {
        ++i;
      }
    }
    if (owners.empty()) {
      if (FlatPagesEnabled() && page < kFlatPageCount) {
        // Found non-empty above, emptied by the loop: the page leaves.
        --flat_armed_pages_;
      } else {
        pages_.erase(page);
      }
    }
    return removed;
  }

  // Drops `page` from the owner's page index (the owner no longer covers it).
  void ForgetPage(uint64_t authority_sequence, uint32_t page) {
    auto it = owner_pages_.find(authority_sequence);
    if (it == owner_pages_.end()) {
      return;
    }
    std::vector<uint32_t>& pages = it->second;
    for (size_t i = 0; i < pages.size(); ++i) {
      if (pages[i] == page) {
        pages[i] = pages.back();
        pages.pop_back();
        break;
      }
    }
    if (pages.empty()) {
      owner_pages_.erase(it);
    }
  }

  // Copies the owner's tiled runs that fall inside `page` from the shared
  // buffer into RAM. Same walk as the eager publication: rows of the resolve
  // rectangle, runs of contiguous tiled texels, clipped to dest_extent_bytes.
  // Only the 32-row tile bands whose bytes can reach the page are walked: a
  // band's tiled addresses lie within its pitch*32*bpp span, plus the 4 KiB
  // micro-tile shuffle on either side, so the others are skipped whole.
  static uint64_t CopyPage(
      const Owner& o, uint32_t page, const uint8_t* shared_contents,
      const std::function<uint8_t*(uint32_t)>& translate_physical) {
    const uint32_t page_start = page << kPageShift;
    const uint32_t page_end = page_start + kPageSize;
    uint8_t* ram = translate_physical(o.guest_addr);
    if (!ram || !shared_contents) {
      return 0;
    }
    if (o.linear) {
      uint32_t c0 = std::max(o.guest_addr, page_start);
      uint32_t c1 = std::min(o.guest_addr + o.range_bytes, page_end);
      if (c0 >= c1) {
        return 0;
      }
      if (o.pre_image) {
        if (GuestRamWatchOverlaps(c0, c1 - c0)) {
          // Diagnostic: which words this publication moves, and RAM's value.
          const uint8_t* pre = o.pre_image->data() + (c0 - o.guest_addr);
          const uint8_t* src = shared_contents + c0;
          const uint8_t* dst = ram + (c0 - o.guest_addr);
          std::string text;
          uint32_t n = 0;
          for (uint32_t w = 0; w + 4 <= c1 - c0; w += 4) {
            if (std::memcmp(src + w, pre + w, 4)) {
              if (++n <= 6) {
                uint32_t sv, pv, rv;
                std::memcpy(&sv, src + w, 4);
                std::memcpy(&pv, pre + w, 4);
                std::memcpy(&rv, dst + w, 4);
                text += fmt::format(" {:08X}:pre={:08X},gpu={:08X},ram={:08X}",
                                    c0 + w, rex::byte_swap(pv),
                                    rex::byte_swap(sv), rex::byte_swap(rv));
              }
            }
          }
          if (n) {
            REXLOG_WARN("GuestRamWatch merge seq={} page {:08X}: {} words{}",
                   o.authority_sequence, c0, n, text);
          }
        }
        return CopyChangedWords(ram + (c0 - o.guest_addr), shared_contents + c0,
                                o.pre_image->data() + (c0 - o.guest_addr),
                                c1 - c0);
      }
      std::memcpy(ram + (c0 - o.guest_addr),
                  shared_contents + c0, c1 - c0);
      return c1 - c0;
    }
    const uint64_t selected = 1;
    return CopyTiledPages(o, page, page, &selected, shared_contents, ram);
  }

  // CopyPage for the pages of [first_page, last_page] whose bit is set in
  // `selected` (bit i = first_page + i), in one walk of the record.
  static uint64_t CopyPages(
      const Owner& o, uint32_t first_page, uint32_t last_page,
      const uint64_t* selected, const uint8_t* shared_contents,
      const std::function<uint8_t*(uint32_t)>& translate_physical) {
    if (o.linear) {
      uint64_t copied = 0;
      for (uint32_t page = first_page; page <= last_page; ++page) {
        uint32_t bit = page - first_page;
        if (selected[bit >> 6] & (uint64_t(1) << (bit & 63))) {
          copied += CopyPage(o, page, shared_contents, translate_physical);
        }
      }
      return copied;
    }
    uint8_t* ram = translate_physical(o.guest_addr);
    if (!ram || !shared_contents) {
      return 0;
    }
    return CopyTiledPages(o, first_page, last_page, selected, shared_contents,
                          ram);
  }

  static uint64_t CopyTiledPages(const Owner& o, uint32_t first_page,
                                 uint32_t last_page, const uint64_t* selected,
                                 const uint8_t* shared_contents, uint8_t* ram) {
    if (!o.bpp) {
      return 0;
    }
    const uint8_t* src_base = shared_contents + o.guest_addr;
    const uint32_t pitch_aligned =
        o.dest_pitch_aligned ? o.dest_pitch_aligned : ((o.width + 31u) & ~31u);
    uint32_t bl2 = 0;
    while ((1u << bl2) < o.bpp) {
      ++bl2;
    }
    // Same run geometry as the eager publication walk.
    const uint32_t run_texels = TiledPublishRunTexels(o.bpp);
    const uint64_t band_bytes = uint64_t(pitch_aligned) * 32u * o.bpp;
    // The pages' span, with slack applied symmetrically.
    const uint64_t span_lo = (uint64_t(first_page) << kPageShift) + 8192u;
    const uint64_t span_hi = (uint64_t(last_page + 1) << kPageShift) + 8192u;
    uint64_t copied = 0;
    for (uint32_t y = 0; y < o.height;) {
      const uint32_t tiled_y = o.dest_offset_y + y;
      const uint32_t band = tiled_y >> 5;
      const uint64_t band_start = uint64_t(o.guest_addr) + band * band_bytes;
      const uint64_t band_end = band_start + band_bytes;
      // Band span [band_start - 4 KiB, band_end + 4 KiB) vs the pages, both
      // shifted by 8 KiB so the comparison never underflows.
      if (band_end + 4096u + 8192u <= span_lo ||
          band_start + 8192u - 4096u >= span_hi) {
        // Skip to the first row of the next band.
        const uint32_t next_tiled_y = (band + 1u) << 5;
        y = next_tiled_y - o.dest_offset_y;
        continue;
      }
      for (uint32_t x = 0; x < o.width;) {
        uint32_t tiled_x = o.dest_offset_x + x;
        uint32_t run =
            std::min(run_texels - (tiled_x & (run_texels - 1)), o.width - x);
        uint32_t off = uint32_t(texture_address::Tiled2D(
            int32_t(o.dest_offset_x + x), int32_t(tiled_y),
            pitch_aligned, int32_t(bl2)));
        uint32_t run_bytes = run * o.bpp;
        if (o.dest_extent_bytes && off + run_bytes > o.dest_extent_bytes) {
          uint32_t fit =
              o.dest_extent_bytes > off ? (o.dest_extent_bytes - off) / o.bpp
                                        : 0u;
          run_bytes = fit * o.bpp;
        }
        if (run_bytes) {
          const uint64_t abs0 = uint64_t(o.guest_addr) + off;
          const uint64_t abs1 = abs0 + run_bytes;
          const uint64_t run_first_page = abs0 >> kPageShift;
          const uint64_t run_last_page = (abs1 - 1) >> kPageShift;
          for (uint64_t page = std::max<uint64_t>(run_first_page, first_page);
               page <= run_last_page && page <= last_page; ++page) {
            const uint64_t bit = page - first_page;
            if (!(selected[bit >> 6] & (uint64_t(1) << (bit & 63)))) {
              continue;
            }
            const uint64_t c0 = std::max(abs0, page << kPageShift);
            const uint64_t c1 = std::min(abs1, (page + 1) << kPageShift);
            if (c0 < c1) {
              std::memcpy(ram + (c0 - o.guest_addr),
                          src_base + (c0 - o.guest_addr), size_t(c1 - c0));
              copied += c1 - c0;
            }
          }
        }
        x += run;
      }
      ++y;
    }
    return copied;
  }

  // Page owner storage. Default: a flat array indexed by page whose emptied
  // lists keep their capacity -- arming and retiring a large resolve no
  // longer hashes, allocates and frees once per 4 KB page (Red Dead
  // gameplay: ~2% of the command-processor thread). A page "exists" exactly
  // when its list is non-empty, as with the map, which a page also leaves
  // when its last owner goes. XE_METAL_LAZY_FLAT_PAGES=0 restores the map.
  static bool FlatPagesEnabled() {
    static const bool s_enabled = [] {
      const char* v = std::getenv("XE_METAL_LAZY_FLAT_PAGES");
      return !(v && v[0] == '0' && !v[1]);
    }();
    return s_enabled;
  }
  static constexpr uint32_t kFlatPageCount = uint32_t(1) << (29 - kPageShift);
  std::vector<Owner>* FindPageOwners(uint32_t page) {
    if (FlatPagesEnabled() && page < kFlatPageCount) {
      if (page >= flat_pages_.size() || flat_pages_[page].empty()) {
        return nullptr;
      }
      return &flat_pages_[page];
    }
    auto it = pages_.find(page);
    return it == pages_.end() ? nullptr : &it->second;
  }
  const std::vector<Owner>* FindPageOwners(uint32_t page) const {
    return const_cast<MetalLazyPublication*>(this)->FindPageOwners(page);
  }
  // The caller appends at least one owner (Arm), so a flat page that was
  // empty becomes armed here.
  std::vector<Owner>& PageOwnersForWrite(uint32_t page) {
    if (FlatPagesEnabled() && page < kFlatPageCount) {
      if (flat_pages_.empty()) {
        flat_pages_.resize(kFlatPageCount);
      }
      std::vector<Owner>& owners = flat_pages_[page];
      if (owners.empty()) {
        ++flat_armed_pages_;
      }
      return owners;
    }
    return pages_[page];
  }
  void ErasePage(uint32_t page) {
    if (FlatPagesEnabled() && page < kFlatPageCount) {
      if (page >= flat_pages_.size()) {
        return;
      }
      if (!flat_pages_[page].empty()) {
        --flat_armed_pages_;
      }
      flat_pages_[page].clear();
      return;
    }
    pages_.erase(page);
  }

  // Blocks while another access holds a page of [first, last]. The holder
  // only copies memory, so a caller inside the global critical region can
  // wait for it.
  void WaitForInFlight(std::unique_lock<std::mutex>& lock, uint32_t first,
                       uint32_t last) {
    auto held = [&] {
      for (const std::vector<uint32_t>* pages : in_flight_) {
        // Ascending, like the snapshot they were taken from.
        auto it = std::lower_bound(pages->begin(), pages->end(), first);
        if (it != pages->end() && *it <= last) {
          return true;
        }
      }
      return false;
    };
    if (in_flight_.empty() || !held()) {
      return;
    }
    ++stats_.in_flight_waits;
    in_flight_done_.wait(lock, [&] { return !held(); });
  }

  mutable std::mutex mutex_;
  // The pages each access in its copy holds (a vector on that access's
  // stack, registered from its second look at the table until its owners
  // are removed).
  std::vector<const std::vector<uint32_t>*> in_flight_;
  std::condition_variable in_flight_done_;
  std::vector<std::vector<Owner>> flat_pages_;
  size_t flat_armed_pages_ = 0;
  std::unordered_map<uint32_t, std::vector<Owner>> pages_;
  // authority_sequence -> pages it still owns (O(1) retire).
  std::unordered_map<uint64_t, std::vector<uint32_t>> owner_pages_;
  Stats stats_;
};

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_LAZY_PUBLICATION_H_
