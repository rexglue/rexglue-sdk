#include "alloc_module.h"

#include <rex/platform.h>

#if defined(_WIN32)
#define REX_TEST_ALLOC_EXPORT extern "C" __declspec(dllexport)
#else
#define REX_TEST_ALLOC_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

std::vector<int>* MakeVector(size_t count) {
  return new std::vector<int>(count, 7);
}

void FillString(std::string& out, size_t size) {
  out.assign(size, 'x');
}

void DeleteInt(int* value) {
  delete value;
}

void DeleteArray(int* values) {
  delete[] values;
}

rex::test::OverAligned* MakeOverAligned() {
  return new rex::test::OverAligned();
}

void DeleteOverAligned(rex::test::OverAligned* value) {
  delete value;
}

int* MakeArray(size_t count) {
  return new int[count];
}

}  // namespace

REX_TEST_ALLOC_EXPORT const rex::test::AllocEntries* rexmod_alloc_entries() {
  static const rex::test::AllocEntries kEntries = {
      &MakeVector,      &FillString,        &DeleteInt, &DeleteArray,
      &MakeOverAligned, &DeleteOverAligned, &MakeArray,
  };
  return &kEntries;
}
