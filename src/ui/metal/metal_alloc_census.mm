/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "rex/ui/metal/metal_alloc_census.h"

#import <Metal/Metal.h>
#import <objc/runtime.h>

#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <rex/logging/log_category.h>

namespace {

enum AllocKind : int {
  kAllocTexture,
  kAllocBuffer,
  kAllocBufferNoCopy,
  kAllocHeap,
  kAllocKindCount,
};
const char* const kAllocKindNames[kAllocKindCount] = {"texture", "buffer",
                                                      "buffer-nocopy", "heap"};

// A creation site: the three return addresses above the device call.
constexpr size_t kSiteFrames = 3;
struct AllocSite {
  int kind;
  void* frames[kSiteFrames];
  bool operator<(const AllocSite& other) const {
    if (kind != other.kind) {
      return kind < other.kind;
    }
    return std::memcmp(frames, other.frames, sizeof(frames)) < 0;
  }
};
struct AllocBucket {
  uint64_t live_bytes = 0;
  uint64_t live_count = 0;
  uint64_t created = 0;
  uint64_t largest_bytes = 0;
  std::string largest;
};

std::mutex census_mutex_;
std::map<AllocSite, AllocBucket> census_buckets_;
id<MTLDevice> census_device_ = nil;
dispatch_source_t census_timer_ = nil;
// The device's own creation methods call each other (bytes -> length); only
// the outermost call is an allocation of ours.
thread_local int census_depth_ = 0;

}  // namespace

// Rides on each resource as an associated object; its dealloc is the
// resource's.
@interface XEMetalAllocCensusTag : NSObject {
 @public
  AllocSite site_;
  uint64_t bytes_;
}
@end
@implementation XEMetalAllocCensusTag
- (void)dealloc {
  std::lock_guard<std::mutex> lock(census_mutex_);
  auto it = census_buckets_.find(site_);
  if (it != census_buckets_.end()) {
    it->second.live_bytes -= bytes_;
    it->second.live_count -= 1;
  }
}
@end

namespace {

const char kTagKey = 0;

void Track(void* raw, AllocKind kind, void* caller, uint64_t bytes,
           std::string description) {
  if (!raw) {
    return;
  }
  AllocSite site = {};
  site.kind = kind;
  site.frames[0] = caller;
  void* stack[12];
  int depth = backtrace(stack, 12);
  for (int i = 0; i < depth; ++i) {
    if (stack[i] != caller) {
      continue;
    }
    for (size_t j = 1; j < kSiteFrames && i + int(j) < depth; ++j) {
      site.frames[j] = stack[i + j];
    }
    break;
  }
  XEMetalAllocCensusTag* tag = [[XEMetalAllocCensusTag alloc] init];
  tag->site_ = site;
  tag->bytes_ = bytes;
  {
    std::lock_guard<std::mutex> lock(census_mutex_);
    AllocBucket& bucket = census_buckets_[site];
    bucket.live_bytes += bytes;
    bucket.live_count += 1;
    bucket.created += 1;
    if (bytes >= bucket.largest_bytes) {
      bucket.largest_bytes = bytes;
      bucket.largest = std::move(description);
    }
  }
  objc_setAssociatedObject((__bridge id)raw, &kTagKey, tag,
                           OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

std::string DescribeTexture(id<MTLTexture> texture) {
  char text[160];
  std::snprintf(text, sizeof(text),
                "%lux%lux%lu fmt=%lu samples=%lu mips=%lu type=%lu storage=%lu "
                "usage=0x%lx",
                (unsigned long)texture.width, (unsigned long)texture.height,
                (unsigned long)std::max(texture.depth, texture.arrayLength),
                (unsigned long)texture.pixelFormat,
                (unsigned long)texture.sampleCount,
                (unsigned long)texture.mipmapLevelCount,
                (unsigned long)texture.textureType,
                (unsigned long)texture.storageMode,
                (unsigned long)texture.usage);
  return text;
}

std::string DescribeBuffer(id<MTLBuffer> buffer) {
  char text[80];
  std::snprintf(text, sizeof(text), "%lu bytes storage=%lu",
                (unsigned long)buffer.length,
                (unsigned long)buffer.storageMode);
  return text;
}

// The originals, called through untyped pointers so ARC leaves the +1 results
// alone on the way through.
using NewFromDescriptorFn = void* (*)(void*, SEL, void*);
using NewBufferWithLengthFn = void* (*)(void*, SEL, NSUInteger, NSUInteger);
using NewBufferWithBytesFn = void* (*)(void*, SEL, const void*, NSUInteger,
                                       NSUInteger);
using NewBufferNoCopyFn = void* (*)(void*, SEL, void*, NSUInteger, NSUInteger,
                                    void*);
NewFromDescriptorFn original_new_texture_ = nullptr;
NewFromDescriptorFn original_new_heap_ = nullptr;
NewBufferWithLengthFn original_new_buffer_length_ = nullptr;
NewBufferWithBytesFn original_new_buffer_bytes_ = nullptr;
NewBufferNoCopyFn original_new_buffer_no_copy_ = nullptr;

void* CensusNewTexture(void* self, SEL cmd, void* descriptor) {
  void* caller = __builtin_return_address(0);
  ++census_depth_;
  void* raw = original_new_texture_(self, cmd, descriptor);
  if (--census_depth_ == 0 && raw) {
    id<MTLTexture> texture = (__bridge id<MTLTexture>)raw;
    Track(raw, kAllocTexture, caller, texture.allocatedSize,
          DescribeTexture(texture));
  }
  return raw;
}

void* CensusNewHeap(void* self, SEL cmd, void* descriptor) {
  void* caller = __builtin_return_address(0);
  ++census_depth_;
  void* raw = original_new_heap_(self, cmd, descriptor);
  if (--census_depth_ == 0 && raw) {
    id<MTLHeap> heap = (__bridge id<MTLHeap>)raw;
    char text[64];
    std::snprintf(text, sizeof(text), "%lu bytes storage=%lu",
                  (unsigned long)heap.size, (unsigned long)heap.storageMode);
    Track(raw, kAllocHeap, caller, heap.size, text);
  }
  return raw;
}

void* CensusNewBufferWithLength(void* self, SEL cmd, NSUInteger length,
                                NSUInteger options) {
  void* caller = __builtin_return_address(0);
  ++census_depth_;
  void* raw = original_new_buffer_length_(self, cmd, length, options);
  if (--census_depth_ == 0 && raw) {
    id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)raw;
    Track(raw, kAllocBuffer, caller, buffer.allocatedSize,
          DescribeBuffer(buffer));
  }
  return raw;
}

void* CensusNewBufferWithBytes(void* self, SEL cmd, const void* bytes,
                               NSUInteger length, NSUInteger options) {
  void* caller = __builtin_return_address(0);
  ++census_depth_;
  void* raw = original_new_buffer_bytes_(self, cmd, bytes, length, options);
  if (--census_depth_ == 0 && raw) {
    id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)raw;
    Track(raw, kAllocBuffer, caller, buffer.allocatedSize,
          DescribeBuffer(buffer));
  }
  return raw;
}

void* CensusNewBufferNoCopy(void* self, SEL cmd, void* bytes,
                            NSUInteger length, NSUInteger options,
                            void* deallocator) {
  void* caller = __builtin_return_address(0);
  ++census_depth_;
  void* raw = original_new_buffer_no_copy_(self, cmd, bytes, length, options,
                                           deallocator);
  if (--census_depth_ == 0 && raw) {
    id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)raw;
    Track(raw, kAllocBufferNoCopy, caller, buffer.length,
          DescribeBuffer(buffer));
  }
  return raw;
}

template <typename Fn>
bool Replace(Class device_class, SEL selector, Fn replacement, Fn* original) {
  Method method = class_getInstanceMethod(device_class, selector);
  if (!method) {
    return false;
  }
  *original = reinterpret_cast<Fn>(
      method_setImplementation(method, reinterpret_cast<IMP>(replacement)));
  return true;
}

std::string SymbolName(void* address) {
  if (!address) {
    return "-";
  }
  Dl_info info = {};
  // A return address: one back lands inside the calling instruction.
  if (!dladdr(static_cast<char*>(address) - 1, &info) || !info.dli_sname) {
    char text[32];
    std::snprintf(text, sizeof(text), "%p", address);
    return text;
  }
  int status = 0;
  char* demangled =
      abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
  std::string name = status == 0 && demangled ? demangled : info.dli_sname;
  std::free(demangled);
  // Arguments make the names unreadably long.
  size_t paren = name.find('(');
  if (paren != std::string::npos && paren > 0) {
    name.resize(paren);
  }
  if (name.size() > 72) {
    name.resize(72);
  }
  char offset[24];
  std::snprintf(offset, sizeof(offset), "+%td",
                static_cast<char*>(address) -
                    static_cast<char*>(info.dli_saddr));
  return name + offset;
}

void Report() {
  std::vector<std::pair<AllocSite, AllocBucket>> buckets;
  {
    std::lock_guard<std::mutex> lock(census_mutex_);
    buckets.assign(census_buckets_.begin(), census_buckets_.end());
  }
  uint64_t kind_bytes[kAllocKindCount] = {}, kind_count[kAllocKindCount] = {};
  for (const auto& entry : buckets) {
    kind_bytes[entry.first.kind] += entry.second.live_bytes;
    kind_count[entry.first.kind] += entry.second.live_count;
  }
  std::sort(buckets.begin(), buckets.end(), [](const auto& a, const auto& b) {
    return a.second.live_bytes > b.second.live_bytes;
  });
  REXLOG_INFO(
      "MetalAllocCensus device={} MB | textures {} MB in {} | buffers {} MB "
      "in {} | no-copy buffers {} MB in {} | heaps {} MB in {} | {} sites",
      uint64_t(census_device_.currentAllocatedSize) >> 20,
      kind_bytes[kAllocTexture] >> 20, kind_count[kAllocTexture],
      kind_bytes[kAllocBuffer] >> 20, kind_count[kAllocBuffer],
      kind_bytes[kAllocBufferNoCopy] >> 20, kind_count[kAllocBufferNoCopy],
      kind_bytes[kAllocHeap] >> 20, kind_count[kAllocHeap], buckets.size());
  size_t rank = 0;
  for (const auto& entry : buckets) {
    if (rank >= 24 || entry.second.live_bytes < (1u << 20)) {
      break;
    }
    REXLOG_INFO(
        "MetalAllocCensus #{} {} live={:.1f} MB n={} created={} largest=[{}] "
        "at {} < {} < {}",
        ++rank, kAllocKindNames[entry.first.kind],
        double(entry.second.live_bytes) / (1024.0 * 1024.0),
        entry.second.live_count, entry.second.created, entry.second.largest,
        SymbolName(entry.first.frames[0]), SymbolName(entry.first.frames[1]),
        SymbolName(entry.first.frames[2]));
  }
}

}  // namespace

namespace rex {
namespace ui {
namespace metal {

void MetalAllocCensusInstall(void* device) {
  const char* enabled = std::getenv("XE_METAL_ALLOC_CENSUS");
  if (!device || !enabled || enabled[0] != '1' || enabled[1]) {
    return;
  }
  static bool s_installed = false;
  if (s_installed) {
    return;
  }
  s_installed = true;
  census_device_ = (__bridge id<MTLDevice>)device;
  Class device_class = object_getClass(census_device_);
  bool all = true;
  all &= Replace(device_class, @selector(newTextureWithDescriptor:),
                 &CensusNewTexture, &original_new_texture_);
  all &= Replace(device_class, @selector(newHeapWithDescriptor:),
                 &CensusNewHeap, &original_new_heap_);
  all &= Replace(device_class, @selector(newBufferWithLength:options:),
                 &CensusNewBufferWithLength, &original_new_buffer_length_);
  all &= Replace(device_class, @selector(newBufferWithBytes:length:options:),
                 &CensusNewBufferWithBytes, &original_new_buffer_bytes_);
  all &= Replace(
      device_class,
      @selector(newBufferWithBytesNoCopy:length:options:deallocator:),
      &CensusNewBufferNoCopy, &original_new_buffer_no_copy_);
  REXLOG_INFO("MetalAllocCensus: installed on {}{}", class_getName(device_class),
         all ? "" : " (some creation methods not found)");
  census_timer_ = dispatch_source_create(
      DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
      dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
  dispatch_source_set_timer(census_timer_,
                            dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC),
                            10 * NSEC_PER_SEC, NSEC_PER_SEC);
  dispatch_source_set_event_handler(census_timer_, ^{
    Report();
  });
  dispatch_resume(census_timer_);
}

}  // namespace metal
}  // namespace ui
}  // namespace rex
