#pragma once

#include <functional>

namespace rex {

#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L
template <typename Signature>
using move_only_function = std::move_only_function<Signature>;
#else
template <typename Signature>
using move_only_function = std::function<Signature>;
#endif

}  // namespace rex
