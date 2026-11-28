# Third Party

The third party code and files the SDK builds with or copies. A dependency in the install is compiled into the SDK or copied with it, and the install puts its license files in `licenses/<name>`, with the license of the SDK itself in `licenses/LICENSE`. Git pins the commit of each submodule, and `git submodule status` lists them. A license text that a vendored folder lacks is in the `licenses` folder of the repository, copied from the upstream named in the table.

The configure fails when a folder or submodule in `thirdparty` has no row in the table of the next section.

## Folders in `thirdparty`

| Name | Source | Upstream and version | License | License files | Included in | Obligation |
|---|---|---|---|---|---|---|
| libmspack | submodule | [kyz/libmspack](https://github.com/kyz/libmspack) v1.11 | LGPL-2.1-only | `libmspack/libmspack/COPYING.LIB` | `rexfilesystem` | license text, library source, relinking allowed |
| glslang | submodule | [KhronosGroup/glslang](https://github.com/KhronosGroup/glslang) `f4f1d8a` | BSD-3-Clause and others | `glslang/LICENSE.txt` | `rexgpu-xenos-vulkan` | attribution |
| FFmpeg | submodule | [wmarti/FFmpeg](https://github.com/wmarti/FFmpeg) n4.4-22-g0604b464c7 | LGPL-2.1-or-later | `FFmpeg/COPYING.LGPLv2.1`, `FFmpeg/LICENSE.md` | `rexkernel` | license text, library source, relinking allowed |
| ffmpeg-overlay | vendored | the codec, parser and filter lists of the FFmpeg configure | LGPL-2.1-or-later | as FFmpeg | `rexkernel` | as FFmpeg |
| tomlplusplus | submodule | [marzer/tomlplusplus](https://github.com/marzer/tomlplusplus) v3.4.0 | MIT | `tomlplusplus/LICENSE` | SDK libraries, `rexglue` tool | attribution |
| simde | submodule | [simd-everywhere/simde](https://github.com/simd-everywhere/simde) v0.8.2 | MIT | `simde/COPYING` | SDK libraries, installed headers | attribution |
| xxHash | submodule | [Cyan4973/xxHash](https://github.com/Cyan4973/xxHash) v0.8.3 | BSD-2-Clause | `xxHash/LICENSE` | SDK libraries, `rexglue` tool | attribution |
| catch2 | submodule | [catchorg/Catch2](https://github.com/catchorg/Catch2) v3.12.0 | BSL-1.0 | `catch2/LICENSE.txt` | tests only | none |
| imgui | submodule | [ocornut/imgui](https://github.com/ocornut/imgui) `6d910d5` | MIT | `imgui/LICENSE.txt` | `rexbase`, installed headers | attribution |
| cli11 | submodule | [CLIUtils/CLI11](https://github.com/CLIUtils/CLI11) `bfffd37` | BSD-3-Clause | `cli11/LICENSE` | `rexglue` tool | attribution |
| sdl3 | submodule | [libsdl-org/SDL](https://github.com/libsdl-org/SDL) release-3.4.14 | Zlib | `sdl3/LICENSE.txt` | SDL3 shared library, SDL3 modules | none for binaries |
| inja | submodule | [pantor/inja](https://github.com/pantor/inja) v3.5.0 | MIT | `inja/LICENSE` | `rexglue` tool | attribution |
| nlohmann-json | inside inja | [nlohmann/json](https://github.com/nlohmann/json) 3.11.3 | MIT | `inja/third_party/include/nlohmann/LICENSE.MIT` | `rexglue` tool | attribution |
| tracy | submodule | [wolfpld/tracy](https://github.com/wolfpld/tracy) v0.13.1 | BSD-3-Clause | `tracy/LICENSE` | `TracyClient` when `REXGLUE_ENABLE_TRACY` is on | attribution |
| excrypt | submodule | [emoose/ExCrypt](https://github.com/emoose/ExCrypt) `0880945` | BSD-3-Clause | `excrypt/LICENSE` | `rexfilesystem`, `rexkernel` | attribution |
| spdlog | submodule | [gabime/spdlog](https://github.com/gabime/spdlog) `79524dd` | MIT | `spdlog/LICENSE` | `rexbase` | attribution |
| tlsf | submodule | [mattconte/tlsf](https://github.com/mattconte/tlsf) `deff9ab` | BSD-3-Clause | `docs/licenses/tlsf/LICENSE` | `rexkernel`, installed library | attribution |
| VulkanMemoryAllocator | submodule | [GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) `0f1a757` | MIT | `VulkanMemoryAllocator/LICENSE.txt` | `rexgpu-xenos-vulkan` | attribution |
| Vulkan-Headers | submodule | [KhronosGroup/Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) v1.4.367 | Apache-2.0 AND MIT | `Vulkan-Headers/LICENSE.md`, `Vulkan-Headers/LICENSES/Apache-2.0.txt`, `Vulkan-Headers/LICENSES/MIT.txt` | `rexgpu-xenos-vulkan` | license text |
| disasm | vendored | the PowerPC disassembler of [GNU binutils](https://sourceware.org/binutils/), copyright years up to 2007 | GPL-2.0-or-later | `docs/licenses/disasm/COPYING` | `rexglue` tool only | GPL source of the tool |
| dxbc | vendored | [GPUOpen-Archive/common-src-ShaderUtils](https://github.com/GPUOpen-Archive/common-src-ShaderUtils) `545195d`, `DX10/DXBCChecksum.cpp` | MIT AND RSA-MD | `docs/licenses/dxbc/LICENSE`, the RSA notice in `DXBCChecksum.cpp` | `rexgpu-xenos-d3d12`, Windows | attribution, MD5 identification |
| dxc | vendored | [microsoft/DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler) `6b6f402`, two headers | NCSA | `docs/licenses/dxc/LICENSE.TXT` | `rexgpu-xenos-d3d12`, Windows | attribution |
| pe | vendored | `pe_image.h` as in Xenia | Microsoft, terms not stated | none | header only, compiled into `rexfilesystem` | none stated |
| renderdoc | vendored | [baldurk/renderdoc](https://github.com/baldurk/renderdoc) `renderdoc_app.h`, API 1.0.1 | MIT | `docs/licenses/renderdoc/LICENSE` | Xenos GPU modules | attribution |
| stb | vendored | [nothings/stb](https://github.com/nothings/stb) `stb_image.h` v2.29 | MIT OR Unlicense | `docs/licenses/stb/LICENSE` | `rexbase`, `rexui` | none under the Unlicense |

## Other Third Party Code

| Name | Upstream and version | License | License files | Included in | Obligation |
|---|---|---|---|---|---|
| Xenia | [xenia-project/xenia](https://github.com/xenia-project/xenia) | BSD-3-Clause | `LICENSE` of the SDK | SDK libraries, shaders | attribution |
| ProggyTiny | [bluescan/proggyfonts](https://github.com/bluescan/proggyfonts) `139ec08` | MIT | `docs/licenses/ProggyFonts/LICENSE` | `rexui` | attribution |
| GNU binutils | GNU binutils 2.24 with the VMX128 patch of [Xenia](https://github.com/xenia-project/xenia/tree/master/third_party/binutils), the `powerpc-none-elf` tools in `tools/binutils`, with the Cygwin 1.7.25 DLLs on Windows | GPL-3.0 | `tools/binutils/LICENSE` | build only, the PowerPC tests, not installed | GPL source |

## Files From Outside the Repository

| Name | Upstream and version | License | License files | Included in | Obligation |
|---|---|---|---|---|---|
| Vulkan loader | [KhronosGroup/Vulkan-Loader](https://github.com/KhronosGroup/Vulkan-Loader) vulkan-sdk-1.4.363.0, from the Vulkan SDK 1.4.363.1 | Apache-2.0 and others | `docs/licenses/Vulkan-Loader/LICENSE.txt`, `docs/licenses/Vulkan-Loader/LICENSES` | macOS install | license text |
| MoltenVK | [KhronosGroup/MoltenVK](https://github.com/KhronosGroup/MoltenVK) v1.4.1, from the Vulkan SDK 1.4.363.1 | Apache-2.0 | `docs/licenses/MoltenVK/LICENSE` | macOS install | license text |
| Windows SDK | the Windows SDK of the build | Microsoft SDK license | none | build only | none |
| D3DCompiler and DXC | `D3DCompiler_47.dll` and `dxcompiler.dll`, loaded from the system at run time | Microsoft | none | not copied | none |

## Remarks

- FFmpeg is built from the libavutil and libavcodec sources that `thirdparty/CMakeLists.txt` lists, with the configuration headers in the FFmpeg folder. `CONFIG_GPL`, `CONFIG_NONFREE`, `CONFIG_VERSION3` and `CONFIG_POSTPROC` are 0 in every platform header, and `FFMPEG_LICENSE` reads LGPL version 2.1 or later.
- libmspack and FFmpeg are linked statically into SDK libraries. The SDK source is public, so a user can rebuild those libraries with a changed copy, as the LGPL requires.
- catch2 is linked into the tests only.
- tlsf, renderdoc and stb hold their license text in `tlsf.h`, `renderdoc_app.h` and at the end of `stb_image.h`. The files in `licenses` are those lines, unchanged.
- The `rexglue` tool includes the PowerPC disassembler of GNU binutils (`thirdparty/disasm/ppc-dis.c`), which is licensed under the GNU General Public License version 2 or, at your option, any later version. The `rexglue` tool as a whole is distributed under GPL 2.0 or later. The SDK libraries and the runtime stay under the SDK license, and titles built with the SDK do not link this code. The corresponding source is the public repository [rexglue/rexglue-sdk](https://github.com/rexglue/rexglue-sdk) at the release tag. The install puts the license text in `bin/rexglue-COPYING.txt` and `licenses/disasm/COPYING`, and the notice in `bin/rexglue-NOTICE.txt`. The configure fails when a target other than the codegen library, the `rexglue` tool and the test executables links the disassembler or the codegen library, directly or through other targets.
- `DXBCChecksum.cpp` contains the RSA Data Security, Inc. MD5 Message Digest Algorithm, and its RSA notice stays in the file.
- dxc holds two headers of DirectXShaderCompiler. Its README names the commit and the license.
- pe carries only the Microsoft copyright line of `pe_image.h`, with no terms.
- The binutils tools in `tools/binutils` assemble the PowerPC tests at build time. Nothing of them is installed.
- The Windows SDK supplies headers and import libraries for the build. Nothing of it is copied.
- On macOS the install copies the Vulkan loader and MoltenVK from the Vulkan SDK, with their license texts. On other platforms the Vulkan SDK is a build requirement only.
