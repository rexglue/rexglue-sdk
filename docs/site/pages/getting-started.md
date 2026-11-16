# Getting Started

Build and install the SDK, then create, build and run a project from an Xbox 360 executable.

The SDK's `rexglue` command line tool has two commands for projects. `rexglue init` creates a project from a XEX, the executable file format of the Xbox 360, and `rexglue codegen` recompiles the XEX into C++. The project build runs `rexglue codegen` for you, so you call `rexglue` yourself only to create the project.

## Install the Tools

=== "Windows"

    1. Install [Visual Studio 2026](https://visualstudio.microsoft.com/downloads/) with the **Desktop development with C++** workload. Under **Optional**, select **C++ Clang Compiler for Windows**.
    2. Open an x64 Developer PowerShell from a PowerShell window:

        ```
        & 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
        ```

        This is the default install folder for Visual Studio 2026 Community. The path changes with your edition and install location. See [Developer PowerShell](https://learn.microsoft.com/en-us/visualstudio/ide/reference/command-prompt-powershell?view=visualstudio#developer-powershell) in the Visual Studio docs.

        Run all commands on this page from this shell.

=== "Linux"

    1. Install Clang 20, lld 20, `g++-13` and `libstdc++-13-dev`. The released Linux packages are built against the GCC 13 libstdc++. When a project finds such a package and GCC 13 is installed next to a newer GCC, `find_package(rexglue)` makes Clang use GCC 13.
    2. Install the build dependencies. On Ubuntu 24.04 or newer:

        ```
        sudo apt install cmake ninja-build libgtk-3-dev libx11-xcb-dev libxss-dev \
          libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev libdecor-0-dev \
          libasound2-dev libpulse-dev libpipewire-0.3-dev
        ```

    The build needs no Vulkan SDK. The Vulkan headers and the Vulkan Memory Allocator are submodules of the repository.

=== "macOS"

    1. Use macOS 13.3 or newer, and install Xcode 26 or newer.
    2. Run `brew install cmake ninja`.
    3. Install the LunarG Vulkan SDK 1.4.328 or newer, and source its `setup-env.sh` in the shell you build from. macOS is the one platform that needs the SDK to build, because the install includes the Vulkan loader and MoltenVK from it. The Vulkan headers and the Vulkan Memory Allocator are submodules of the repository.

## Build and Install the SDK

Clone the repository with its submodules:

```
git clone --recursive https://github.com/rexglue/rexglue-sdk.git
cd rexglue-sdk
```

Then configure, build and install the SDK, and add its `bin` folder to `PATH` so that the shell finds `rexglue`:

=== "Windows"

    ```
    cmake --preset win-amd64
    cmake --build out/build/win-amd64 --target install
    $env:PATH = "$PWD\out\install\win-amd64\bin;$env:PATH"
    ```

=== "Linux"

    ```
    cmake --preset linux-amd64
    cmake --build out/build/linux-amd64 --target install
    export PATH="$PWD/out/install/linux-amd64/bin:$PATH"
    ```

=== "macOS"

    ```
    cmake --preset mac-arm64
    cmake --build out/build/mac-arm64 --target install
    export PATH="$PWD/out/install/mac-arm64/bin:$PATH"
    ```

    On an Intel Mac, use `mac-amd64` in place of `mac-arm64`.

Run `rexglue --help` to check the install. See [CMake](reference/cmake.md#presets) for the other presets.

## Create a Project

A project is a folder with a CMake build that turns one XEX into a native program. This example makes a folder `foobar` for the project and puts the game files in a folder `assets` inside it, with the executable at `foobar/assets/default.xex`. From inside `foobar`, run:

```
rexglue init --project-name foobar --xex-path ./assets/default.xex --project-root .
```

`rexglue init` writes these files into `foobar`:

- `foobar_manifest.toml`, the [project manifest](reference/manifest.md). It holds the path of the XEX and the settings for recompiling it.
- `CMakeLists.txt` and `CMakePresets.json`, the build. `CMakeLists.txt` finds the SDK and calls `rexglue_project`, which connects the build to codegen.
- `src/main.cpp`, which declares the program, and `src/foobar_hooks.cpp`, for your own code.

A project name starts with a letter and contains only letters, digits and `_`, because it starts the names of the generated C++ files and functions. See [`init`](reference/cli.md#init) for all options.

The presets contain no paths from your machine. CMake finds the installed SDK through the user package registry, which the install fills, or through `CMAKE_PREFIX_PATH`. When the SDK is in the registry the project needs no further setup. When it is not, `init` prints the folder and how to point CMake at it.

A project made with an earlier SDK has `generated/rexglue.cmake`. Its configure stops with an error that shows the two edits, which [Updating a Project](reference/cmake.md#updating-a-project) lists.

## Build the Project

=== "Windows"

    ```
    cmake --preset win-amd64
    cmake --build --preset win-amd64-debug
    ```

=== "Linux"

    ```
    cmake --preset linux-amd64
    cmake --build --preset linux-amd64-debug
    ```

=== "macOS"

    ```
    cmake --preset mac-arm64
    cmake --build --preset mac-arm64-debug
    ```

    On an Intel Mac, use `mac-amd64` and `mac-amd64-debug`.

The build first runs `rexglue codegen`, which writes the generated C++ to `generated/default`, and then compiles it with your code. The `generated` folder is build output that the build writes again whenever it is missing or out of date, so it needs no place in version control. The program is `foobar` in `out/build/<preset>/Debug`, next to the SDK libraries that the build copies there.

## Fix Analysis Errors

The first build of a new project can stop in codegen. Codegen finds the functions of the XEX by following calls and branches from the functions it knows at the start (see [Codegen Pipeline](codegen/codegen-pipeline.md#analysis)). It knows most functions from `.pdata`, a table of function starts in the XEX. A function that is missing from that table and is reached only through a pointer can stay unknown, for example:

- a virtual function of a class without RTTI data, since codegen finds the vtables, the tables of virtual function pointers, through the RTTI data
- a callback, passed to another function as an argument

When another function branches into such code, the branch fails the validation that codegen runs before it writes any file. The validation checks every direct call (`bl`) and branch (`b`), and reports one whose target is outside the current function and inside no function that codegen knows. The log lists each error, and the build ends with a summary. Millicode errors are explained in [Restore Exits and Millicode](codegen/codegen-pipeline.md#restore-exits-and-millicode).

```
UnresolvedCall (1):
  0x82001234 from 0x82000F00: bl 0x82001234 from 0x82000F00 has a target in no function
Failed: Validation failed: 1 unresolved calls, 0 millicode errors (after 4.2s)
```

The first address is the target and the second is the instruction that branches to it. Codegen writes no generated files while an error remains.

To fix an error, add the missing function to the `[entrypoint.functions]` table of `foobar_manifest.toml`, then build again. The table under `[entrypoint]` applies to the main XEX, and the manifest page shows the same table for a DLL module. The target of a `bl` is the start of the missing function. The target of a `b` can be inside it, so find the start in a disassembler.

```toml
[entrypoint.functions]
0x82001234 = {}
0x82004320 = { size = 0x240 }
0x82009998 = { name = "LoadConfig" }
```

- `{}` marks a function start. Codegen finds the end.
- `size` gives the length in bytes, for a function whose end codegen cannot find.
- `name` replaces the default name, which is `sub_` and the address, such as `sub_82009998`.

See [`[functions]`](reference/manifest.md#functions) for the other keys, and [Project Manifest](reference/manifest.md) for jump tables and the other tables.

## Run the Project

Start the program with the game root, the folder that contains `default.xex`. The game files are inside the project, so that folder is `assets`:

=== "Windows"

    ```
    out/build/win-amd64/Debug/foobar.exe --title.game_root=assets
    ```

=== "Linux"

    ```
    ./out/build/linux-amd64/Debug/foobar --title.game_root=assets
    ```

=== "macOS"

    ```
    ./out/build/mac-arm64/Debug/foobar --title.game_root=assets
    ```

`title.game_root` is a setting. Every setting can be given on the command line as `--name=value`, for example `--gpu.vsync=false`. `--game_data_root` is an older name for the same setting, and error messages use it. See the [Settings Reference](reference/cvars.md) for every setting.

## Troubleshooting

| Message | Fix |
|---|---|
| `ReXGlue requires the Clang compiler` | Configure with a preset, since the presets select Clang. If the build folder was configured with another compiler, delete it and configure again. |
| `ReXGlue requires Clang 20 or newer` | Install Clang 20 or newer. On Windows, update Visual Studio. |
| `ReXGlue requires AppleClang 17 or newer` | Install Xcode 26 or newer. |
| `The ReXGlue SDK package was built against libstdc++ 13, but the C++ compiler of this project ... uses libstdc++ 14` | The Linux SDK packages are built against the GCC 13 libstdc++, and a project must use the same major version, since both sides emit the same standard library classes with different layouts and the process crashes at startup. Install `g++-13` and `libstdc++-13-dev`. Or set the CMake cache variable `REXSDK_DIR` to build the SDK from source with the project's own compiler, where the check does not apply (see [SDK Discovery](reference/cmake.md#sdk-discovery)). A package built against another standard library, such as libc++, gives the same error with that library named. |
| `Could NOT find Vulkan` | Only macOS looks for the SDK. Install the LunarG Vulkan SDK 1.4.328 or newer and source its `setup-env.sh`. Older SDKs include a MoltenVK that reports less than Vulkan 1.4. |
| `Could not find a package configuration file provided by "rexglue"` | Install the SDK again, which registers it for CMake, or set the `CMAKE_PREFIX_PATH` environment variable, or pass `-DCMAKE_PREFIX_PATH=<folder>` at configure, with the SDK install folder, such as `out/install/win-amd64`. To build the SDK from source with the project, set the CMake cache variable `REXSDK_DIR` to the SDK source folder (see [SDK Discovery](reference/cmake.md#sdk-discovery)). |
| `--game_data_root was not provided` | Pass `--title.game_root` with the folder of the game files. |
| `unknown setting '<name>' from CommandLine` | Check the name in the [Settings Reference](reference/cvars.md). |
