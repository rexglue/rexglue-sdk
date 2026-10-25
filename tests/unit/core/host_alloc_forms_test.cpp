/**
 * @file        tests/unit/core/host_alloc_forms_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/memory/host.h>

#include <cstddef>
#include <cstdint>
#include <new>

namespace {

constexpr size_t kSize = 100;
constexpr std::align_val_t kAlign{64};

struct Form {
  const char* name;
  void* (*make)();
  void (*release)(void*);
  size_t align;
};

[[gnu::noinline]] void* NewPlain() {
  return ::operator new(kSize);
}
[[gnu::noinline]] void* NewArray() {
  return ::operator new[](kSize);
}
[[gnu::noinline]] void* NewAligned() {
  return ::operator new(kSize, kAlign);
}
[[gnu::noinline]] void* NewArrayAligned() {
  return ::operator new[](kSize, kAlign);
}
[[gnu::noinline]] void* NewNothrow() {
  return ::operator new(kSize, std::nothrow);
}
[[gnu::noinline]] void* NewArrayNothrow() {
  return ::operator new[](kSize, std::nothrow);
}
[[gnu::noinline]] void* NewAlignedNothrow() {
  return ::operator new(kSize, kAlign, std::nothrow);
}
[[gnu::noinline]] void* NewArrayAlignedNothrow() {
  return ::operator new[](kSize, kAlign, std::nothrow);
}

[[gnu::noinline]] void DeletePlain(void* p) {
  ::operator delete(p);
}
[[gnu::noinline]] void DeleteArray(void* p) {
  ::operator delete[](p);
}
[[gnu::noinline]] void DeleteSized(void* p) {
  ::operator delete(p, kSize);
}
[[gnu::noinline]] void DeleteArraySized(void* p) {
  ::operator delete[](p, kSize);
}
[[gnu::noinline]] void DeleteAligned(void* p) {
  ::operator delete(p, kAlign);
}
[[gnu::noinline]] void DeleteArrayAligned(void* p) {
  ::operator delete[](p, kAlign);
}
[[gnu::noinline]] void DeleteSizedAligned(void* p) {
  ::operator delete(p, kSize, kAlign);
}
[[gnu::noinline]] void DeleteArraySizedAligned(void* p) {
  ::operator delete[](p, kSize, kAlign);
}
[[gnu::noinline]] void DeleteNothrow(void* p) {
  ::operator delete(p, std::nothrow);
}
[[gnu::noinline]] void DeleteArrayNothrow(void* p) {
  ::operator delete[](p, std::nothrow);
}
[[gnu::noinline]] void DeleteAlignedNothrow(void* p) {
  ::operator delete(p, kAlign, std::nothrow);
}
[[gnu::noinline]] void DeleteArrayAlignedNothrow(void* p) {
  ::operator delete[](p, kAlign, std::nothrow);
}

constexpr size_t kA = 0;
constexpr size_t kO = 64;

constexpr Form kForms[] = {
    {"new / delete", &NewPlain, &DeletePlain, kA},
    {"new / sized delete", &NewPlain, &DeleteSized, kA},
    {"new / nothrow delete", &NewPlain, &DeleteNothrow, kA},
    {"new[] / delete[]", &NewArray, &DeleteArray, kA},
    {"new[] / sized delete[]", &NewArray, &DeleteArraySized, kA},
    {"new[] / nothrow delete[]", &NewArray, &DeleteArrayNothrow, kA},
    {"aligned new / aligned delete", &NewAligned, &DeleteAligned, kO},
    {"aligned new / sized aligned delete", &NewAligned, &DeleteSizedAligned, kO},
    {"aligned new / aligned nothrow delete", &NewAligned, &DeleteAlignedNothrow, kO},
    {"aligned new[] / aligned delete[]", &NewArrayAligned, &DeleteArrayAligned, kO},
    {"aligned new[] / sized aligned delete[]", &NewArrayAligned, &DeleteArraySizedAligned, kO},
    {"aligned new[] / aligned nothrow delete[]", &NewArrayAligned, &DeleteArrayAlignedNothrow, kO},
    {"nothrow new / delete", &NewNothrow, &DeletePlain, kA},
    {"nothrow new / nothrow delete", &NewNothrow, &DeleteNothrow, kA},
    {"nothrow new[] / delete[]", &NewArrayNothrow, &DeleteArray, kA},
    {"nothrow new[] / nothrow delete[]", &NewArrayNothrow, &DeleteArrayNothrow, kA},
    {"aligned nothrow new / aligned delete", &NewAlignedNothrow, &DeleteAligned, kO},
    {"aligned nothrow new / aligned nothrow delete", &NewAlignedNothrow, &DeleteAlignedNothrow, kO},
    {"aligned nothrow new[] / aligned delete[]", &NewArrayAlignedNothrow, &DeleteArrayAligned, kO},
    {"aligned nothrow new[] / aligned nothrow delete[]", &NewArrayAlignedNothrow,
     &DeleteArrayAlignedNothrow, kO},
};

}  // namespace

REX_TEST_CASE("Every replaceable operator new and delete form reaches the host allocator",
              "[core][memory]") {
  if (rex::test::kThreadSanitizer) {
    SKIP("ThreadSanitizer owns operator new and delete");
  }
  for (const Form& form : kForms) {
    DYNAMIC_SECTION(form.name) {
      const auto before = rex::memory::GetHostStats();
      void* block = form.make();
      REQUIRE(block != nullptr);
      CHECK(rex::memory::HostSize(block) == kSize);
      if (form.align != 0) {
        CHECK(reinterpret_cast<uintptr_t>(block) % form.align == 0);
      }
      CHECK(rex::memory::GetHostStats().total.live_count > before.total.live_count);
      form.release(block);
      CHECK(rex::memory::GetHostStats().total.live_count == before.total.live_count);
    }
  }
}
