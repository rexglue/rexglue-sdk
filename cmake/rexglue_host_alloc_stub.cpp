#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define REX_HOST_ALLOC_SANITIZED 1
#endif
#endif

#ifndef REX_HOST_ALLOC_SANITIZED

#include <cstddef>
#include <new>

#include <rex/memory/host.h>

using rex::memory::detail::DeleteBlock;
using rex::memory::detail::Kind;
using rex::memory::detail::NewBlock;

void* operator new(std::size_t size) {
  return NewBlock(size, 0, Kind::kScalar, true);
}

void* operator new[](std::size_t size) {
  return NewBlock(size, 0, Kind::kArray, true);
}

void* operator new(std::size_t size, std::align_val_t align) {
  return NewBlock(size, static_cast<std::size_t>(align), Kind::kScalar, true);
}

void* operator new[](std::size_t size, std::align_val_t align) {
  return NewBlock(size, static_cast<std::size_t>(align), Kind::kArray, true);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return NewBlock(size, 0, Kind::kScalar, false);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return NewBlock(size, 0, Kind::kArray, false);
}

void* operator new(std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
  return NewBlock(size, static_cast<std::size_t>(align), Kind::kScalar, false);
}

void* operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
  return NewBlock(size, static_cast<std::size_t>(align), Kind::kArray, false);
}

void operator delete(void* ptr) noexcept {
  DeleteBlock(ptr, 0, Kind::kScalar);
}

void operator delete[](void* ptr) noexcept {
  DeleteBlock(ptr, 0, Kind::kArray);
}

void operator delete(void* ptr, std::size_t size) noexcept {
  DeleteBlock(ptr, size, Kind::kScalar);
}

void operator delete[](void* ptr, std::size_t size) noexcept {
  DeleteBlock(ptr, size, Kind::kArray);
}

void operator delete(void* ptr, std::align_val_t) noexcept {
  DeleteBlock(ptr, 0, Kind::kScalar);
}

void operator delete[](void* ptr, std::align_val_t) noexcept {
  DeleteBlock(ptr, 0, Kind::kArray);
}

void operator delete(void* ptr, std::size_t size, std::align_val_t) noexcept {
  DeleteBlock(ptr, size, Kind::kScalar);
}

void operator delete[](void* ptr, std::size_t size, std::align_val_t) noexcept {
  DeleteBlock(ptr, size, Kind::kArray);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
  DeleteBlock(ptr, 0, Kind::kScalar);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
  DeleteBlock(ptr, 0, Kind::kArray);
}

void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
  DeleteBlock(ptr, 0, Kind::kScalar);
}

void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
  DeleteBlock(ptr, 0, Kind::kArray);
}

#endif
