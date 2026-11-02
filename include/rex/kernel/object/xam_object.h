/**
 * @file        kernel/object/xam_object.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <type_traits>

#include <rex/types.h>

namespace rex::kernel {

class IXAMObject {
 public:
  virtual ~IXAMObject() = default;

  /** Returns the tag Pinned checks before it casts. It is the kTag of the concrete class. */
  [[nodiscard]] virtual u32 GetTag() const = 0;
};

struct XAMObject {
  explicit XAMObject(std::unique_ptr<IXAMObject> body_in) : body(std::move(body_in)) {}

  std::unique_ptr<IXAMObject> body;
};

/**
 * Matches the classes the table keeps as an XAMObject.
 *
 * @tparam T A class with a kTag member.
 */
template <typename T>
concept XAMObjectClass = std::is_base_of_v<IXAMObject, T> && requires { T::kTag; };

}  // namespace rex::kernel
