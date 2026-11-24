# Direct3D 11 reference tessellator

`tessellator.cpp` / `tessellator.hpp` are Microsoft's Direct3D 11 fixed-function
tessellator reference implementation (MIT license, copyright Microsoft
Corporation; header retained verbatim in both files), as vendored by Mesa at
`src/gallium/auxiliary/tessellator/`.

- Source: https://gitlab.freedesktop.org/mesa/mesa/-/raw/main/src/gallium/auxiliary/tessellator/
- Fetched: 2026-09-20; last upstream commit touching the file:
  `65c2dc5a634b6ee31979546ec5eb5212d6ffe721` (2026-01-06,
  "gallium/tessellator: fix -Wmissing-prototype issues").
- sha256 at import:
  - `tessellator.cpp` `c5c986fe2c31ccb674e6c980895d5adc93cd1d24990d9354b5e63a876355db36`
  - `tessellator.hpp` `14fcefcf31d9afbbd894e088f56b0c77f7c235f1fbe8a5043b295f7e05cecb8d`
- Local modifications: NONE to the two source files. `util/macros.h` is a
  local shim providing the one Mesa macro the file uses (`UNREACHABLE`).

Why it is here (OpenSpec change `implement-metal-host-tessellation`): the
Direct3D 11 specification requires hardware tessellators to reproduce this
implementation's domain points and connectivity exactly, so feeding it the
factors the shared hull shaders compute gives the native Metal backend the same
tessellated geometry Canary D3D12 gets from the GPU — by construction, without
a mesh/object emulation stage. Only `CHWTessellator` (the hardware stage) is
used; `CHLSLTessellator` models compiler-generated hull-shader code that the
project's canned hull shaders already perform on their own.
