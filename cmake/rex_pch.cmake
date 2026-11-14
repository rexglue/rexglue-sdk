# rex_target_pch(<target> [extra headers...]) attaches a PRIVATE precompiled
# header made of a common base plus per module extras.
function (rex_target_pch target)
  target_precompile_headers(
    ${target}
    PRIVATE
    <algorithm>
    <array>
    <chrono>
    <cstddef>
    <cstdint>
    <cstring>
    <expected>
    <filesystem>
    <format>
    <functional>
    <memory>
    <mutex>
    <optional>
    <span>
    <string>
    <string_view>
    <utility>
    <vector>
    <rex/platform.h>
    <rex/types.h>
    <rex/assert.h>
    <rex/logging/log_category.h>
    <rex/module.h>
    <rex/result.h>
    <rex/setting.h>
    <rex/utility.h>
    ${ARGN}
  )
endfunction ()
