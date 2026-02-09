> [!CAUTION]
> This project is in early development. Expect things to not work quite right and there to be significant changes and breaking public API updates as development progresses. Contributions and feedback are welcome, but please be aware that the codebase is still evolving rapidly.

# ReXGlue
![Banner](https://github.com/rexglue/rexglue-media/blob/main/ReX_Banner.png)

**Static recompilation runtime SDK for Xbox 360**

ReXGlue converts Xbox 360 PowerPC code into portable C++ that runs natively on modern platforms.

ReXGlue is heavily rooted on the foundations of [Xenia](https://github.com/xenia-project), the Xbox 360 emulator. Rather than interpreting or JIT-compiling PPC instructions at runtime, ReXGlue takes a different path: it generates C++ source code ahead of time, an approach inspired by [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and [rexdex's recompiler](https://github.com/rexdex/recompiler).

## Quickstart

### Prerequisites (non-Visual Studio users only)

- Clang 20
- CMake
- Ninja
- [vcpkg](https://github.com/microsoft/vcpkg)
- Latest Release from [Releases](https://github.com/rexglue/rexglue-sdk/releases)

### Installing the SDK
Download the latest release and extract it to a location of your choice. This will be your `REXSDK` path. Set this as an environment variable using your preferred method.

#### Building from source

```bash
git clone --recursive https://github.com/rexglue/rexglue-sdk
cd rexglue
cmake --preset <platform>
cmake --build out/build/<platform> --target install
```

This will perform a multi-config build and put the SDK at `out/install/<platform>`. You may move this folder and then set this path on your environment variable as `REXSDK` using your preferred method (system or CMakeUserPresets.json in your target project).


### Creating a project

```bash
rexglue init --app_name [project name] --app_root [project root path]
```

This generates a project with `CMakeLists.txt`, `CMakePresets.json`, and starter source files.


Ensure the install path of the SDK is correctly set as an environment variable `REXSDK`.

### Project configuration

The `rexglue init` command generates a `<project>_config.toml` file that controls the codegen pipeline. Run codegen with:

```bash
rexglue codegen <project>_config.toml
```

#### Required fields

| Key | Description |
|-----|-------------|
| `project_name` | Name used for generated output files |
| `file_path` | Path to the Xbox 360 XEX or ELF binary |
| `out_directory_path` | Directory for generated C++ code (default: `generated`) |

#### Code generation options

These control how PPC registers and instructions are translated to C++. All default to `false`.

| Key | Description |
|-----|-------------|
| `skip_lr` | Skip link register saves/restores. Reduces generated code for leaf functions. |
| `skip_msr` | Skip MSR (Machine State Register) instructions. |
| `ctr_as_local` | Emit CTR (Count Register) as a local variable instead of reading/writing thread state. Reduces memory traffic in loops. |
| `xer_as_local` | Emit XER (Integer Exception Register) as a local variable. |
| `cr_as_local` | Emit CR (Condition Register) fields as local variables. Significant codegen improvement for branch-heavy code. |
| `reserved_as_local` | Emit reserved registers (r1, r2, r13) as local variables. |
| `non_argument_as_local` | Emit non-argument volatile registers (r11-r12) as local variables. |
| `non_volatile_as_local` | Emit non-volatile registers (r14-r31) as local variables. Only safe when the function's save/restore behavior is well understood. |

#### Special addresses

| Key | Description |
|-----|-------------|
| `setjmp_address` | Address of the `setjmp` function in the binary. Required for correct non-local jump handling. |
| `longjmp_address` | Address of the `longjmp` function in the binary. |

#### Analysis tuning (`[analysis]` section)

| Key | Default | Description |
|-----|---------|-------------|
| `max_jump_extension` | `65536` | Maximum bytes to extend a function when following jump table targets. |
| `data_region_threshold` | `16` | Consecutive invalid instructions before marking a region as embedded data. |
| `large_function_threshold` | `1048576` | Byte threshold for "large function" warnings. |
| `exception_handler_funcs` | `[]` | Array of addresses for exception handler functions beyond auto-detected ones. |

#### Manual function overrides (`[functions]` section)

Define function boundaries, names, or parent-child relationships when auto-detection gets it wrong:

```toml
[functions]
# Name a function
0x82000000 = { name = "MyFunction" }

# Explicit size (bytes)
0x82000100 = { size = 64 }

# Explicit end address (exclusive)
0x82000200 = { end = 0x82000280 }

# Discontinuous chunk belonging to a parent function
0x82000300 = { parent = 0x82000000, size = 32 }
```

Fields: `name`, `size`, `end` (mutually exclusive with `size`), `parent` (makes it a chunk of another function).

#### Switch tables (`[[switch_tables]]`)

Manually define switch/jump tables when auto-detection fails:

```toml
[[switch_tables]]
address = 0x82000000    # Address of the bctr instruction
register = 11           # GPR used as the jump index
labels = [0x82000100, 0x82000200, 0x82000300]  # Branch target addresses
```

#### Mid-asm hooks (`[[midasm_hook]]`)

Inject calls to native C++ functions at specific instruction addresses:

```toml
[[midasm_hook]]
address = 0x82000000
name = "MyHook"
registers = ["r3", "r4"]       # Registers passed to the hook function
after_instruction = false       # Insert before (false) or after (true) the instruction

# Control flow after hook (pick one):
# return = true                 # Return from the function after the hook
# jump_address = 0x82001000     # Jump to a different address after the hook

# Conditional control flow (hook returns bool):
# return_on_true = true
# return_on_false = true
# jump_address_on_true = 0x82001000
# jump_address_on_false = 0x82002000
```

#### Invalid instruction hints (`[[invalid_instructions]]`)

Mark data patterns that the disassembler misidentifies as code:

```toml
[[invalid_instructions]]
data = 0x12345678   # The raw instruction word
size = 4            # Size of the data region in bytes
```

# **Disclaimer**
ReXGlue is not affiliated with nor endorsed by Microsoft or Xbox. It is an independent project created for educational and development purposes. All trademarks and copyrights belong to their respective owners. 

This project is not intended to promote piracy nor unauthorized use of copyrighted material. Any misuse of this software to endorse or enable this type of activity is strictly prohibited.


# Credits

## ReXGlue
- [Tom (crack)](https://github.com/tomcl7) - Lead developer and project founder. Creator of the recompiler and core architecture of ReXGlue.
- [Loreaxe](https://github.com/Loreaxe) - Testing on various hardware and helping found the project.
- [Soren/Roxxsen](https://github.com/Roxxsen) - Lead project manager and git maintainer.
- [Toby](https://github.com/TbyDtch) - Graphic designer.

## Very Special Thank You:
- [Project Xenia](https://github.com/xenia-project/xenia/tree/master/src/xenia) - Their invaluable work on Xbox 360 emulation laid the groundwork for ReXGlue's development. This project (and numerous others) would not exist without their hard work and dedication.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) - For pioneering the modern static recompilation approach for Xbox 360. A lot of the codegen analysis logic and instruction translations are based on their work. Thank you!
- [rexdex's recompiler](https://github.com/rexdex/recompiler) - The OG static recompiler for Xbox 360. 
- Many others in the Xbox 360 homebrew and modding communities whose work and research have contributed to the collective knowledge that makes projects like this possible.
