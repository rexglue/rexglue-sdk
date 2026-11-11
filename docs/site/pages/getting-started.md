# Getting Started

Build and install the SDK, then create, build and run a project from an Xbox 360 executable.

The SDK's `rexglue` command line tool has two commands for projects. `rexglue init` creates a project from a XEX, the executable file format of the Xbox 360, and `rexglue codegen` recompiles the XEX into C++. The project build runs `rexglue codegen` for you, so you call `rexglue` yourself only to create the project.

## Install the Tools

=== "Windows"

    1. Install [Visual Studio 2026](https://visualstudio.microsoft.com/downloads/) with the **Desktop development with C++** workload. Under **Optional**, select **C++ Clang Compiler for Windows**.
    2. Install the Vulkan SDK.
    3. Open an x64 Developer PowerShell from a PowerShell window:

        ```
        & 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
        ```

        This is the default install folder for Visual Studio 2026 Community. The path changes with your edition and install location. See [Developer PowerShell](https://learn.microsoft.com/en-us/visualstudio/ide/reference/command-prompt-powershell?view=visualstudio#developer-powershell) in the Visual Studio docs.

        Run all commands on this page from this shell.

=== "Linux"

    1. Install Clang 20, lld 20 and `g++-13` with `libstdc++-13-dev`. The Linux SDK packages are built against the GCC 13 libstdc++, and a project must build against the same major version. With Clang, pass `--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/13` (`aarch64-linux-gnu` on arm64) in `CMAKE_C_FLAGS` and `CMAKE_CXX_FLAGS`, since Clang otherwise picks the newest GCC it finds. To build the SDK itself against GCC 13, configure with `-DREXGLUE_GCC_INSTALL_DIR=/usr/lib/gcc/x86_64-linux-gnu/13`.
    2. Install the build dependencies. On Ubuntu 24.04 or newer:

        ```
        sudo apt install cmake ninja-build libgtk-3-dev libx11-xcb-dev libxss-dev \
          libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev libdecor-0-dev \
          libasound2-dev libpulse-dev libpipewire-0.3-dev
        ```

    3. Install the LunarG Vulkan SDK, and source its `setup-env.sh` in the shell you build from. LunarG provides no SDK for arm64, so on arm64 run `sudo apt install libvulkan-dev libvulkan-memory-allocator-dev` instead.

=== "macOS"

    1. Use macOS 13.3 or newer, and install Xcode 26 or newer.
    2. Run `brew install cmake ninja`.
    3. Install the LunarG Vulkan SDK, and source its `setup-env.sh` in the shell you build from.

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

A project is a folder with a CMake build that turns one XEX into a native program. This example starts from a folder `assets` that holds the game files, with the executable at `assets/default.xex`. From the folder that contains `assets`, run:

```
rexglue init --project-name foobar --xex-path ./assets/default.xex --project-root ./foobar
cd foobar
```

`rexglue init` writes these files into `foobar`:

- `foobar_manifest.toml`, the [project manifest](reference/manifest.md). It holds the path of the XEX and the settings for recompiling it.
- `CMakeLists.txt` and `CMakePresets.json`, the build. `CMakeLists.txt` finds the SDK and calls `rexglue_project`, which connects the build to codegen.
- `src/main.cpp`, which declares the program, and `src/foobar_hooks.cpp`, for your own code.

A project name starts with a letter and contains only letters, digits and `_`, because it starts the names of the generated C++ files and functions. See [`init`](reference/cli.md#init) for all options.

The presets that `init` writes point CMake at the installed SDK, so the project needs no further setup. Nothing under `generated` needs to be committed, because `rexglue_project` comes from the SDK package and codegen runs as part of the build.

A project made with an earlier SDK has `generated/rexglue.cmake`. See [Updating a Project](reference/cmake.md#updating-a-project) for the two edits that remove it.

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

The build first runs `rexglue codegen`, which writes the generated C++ to `generated/default`, and then compiles it with your code. The program is `foobar` in `out/build/<preset>/Debug`, next to the SDK libraries that the build copies there.

## Fix Analysis Errors

The first build of a new project can stop in codegen. Codegen finds the functions of the XEX by following calls and branches from the functions it knows at the start (see [Codegen Pipeline](codegen/codegen-pipeline.md#analysis)). It knows most functions from `.pdata`, a table of function starts in the XEX. A function that is missing from that table and is reached only through a pointer can stay unknown, for example:

- a virtual function of a class without RTTI data, whose vtable the vtable scan cannot find
- a callback, passed to another function as an argument

When another function branches into such code, the branch fails validation. Codegen checks every direct call (`bl`) and branch (`b`), and reports one whose target is outside the current function and inside no function that codegen knows. The log lists each error, and the build ends with a summary:

```
UnresolvedCall (1):
  0x82001234 from 0x82000F00: bl 0x82001234 from 0x82000F00 has a target in no function
Failed: Validation failed: 1 unresolved calls, 0 millicode errors (after 4.2s)
```

The first address is the target and the second is the instruction that branches to it. Codegen writes no generated files while an error remains.

To fix an error, add the missing function to the `[entrypoint.functions]` table of `foobar_manifest.toml`, then build again. The target of a `bl` is the start of the missing function. The target of a `b` can be inside it, so find the start in a disassembler.

```toml
[entrypoint.functions]
0x82001234 = {}
0x82004321 = { size = 0x240 }
0x82009999 = { name = "LoadConfig" }
```

- `{}` marks a function start. Codegen finds the end.
- `size` gives the length in bytes, for a function whose end codegen cannot find.
- `name` replaces the default name, which is `sub_` and the address, such as `sub_82009999`.

See [`[functions]`](reference/manifest.md#functions) for the other keys, and [Project Manifest](reference/manifest.md) for jump tables and the other tables.

## Run the Project

Start the program with the game root, the folder that contains `default.xex`. The project is inside `foobar`, so that folder is `../assets`:

=== "Windows"

    ```
    out/build/win-amd64/Debug/foobar.exe --title.game_root=../assets
    ```

=== "Linux"

    ```
    ./out/build/linux-amd64/Debug/foobar --title.game_root=../assets
    ```

=== "macOS"

    ```
    ./out/build/mac-arm64/Debug/foobar --title.game_root=../assets
    ```

`title.game_root` is a setting. Every setting can be given on the command line as `--name=value`, for example `--gpu.vsync=false`. `--game_data_root` is a short alias for `--title.game_root`. See the [Settings Reference](reference/cvars.md) for every setting.

## Troubleshooting

| Message | Fix |
|---|---|
| `ReXGlue requires the Clang compiler` | Configure with a preset, since the presets select Clang. If the build folder was configured with another compiler, delete it and configure again. |
| `ReXGlue requires Clang 20 or newer` | Install Clang 20 or newer. On Windows, update Visual Studio. |
| `ReXGlue requires AppleClang 17 or newer` | Install Xcode 26 or newer. |
| `The ReXGlue SDK package was built against libstdc++ 13, but the C++ compiler of this project ... uses libstdc++ 14` | The Linux SDK packages are built against the GCC 13 libstdc++, and a project must use the same major version, since both sides emit the same standard library classes with different layouts and the process crashes at startup. Install `g++-13` and `libstdc++-13-dev`. With Clang, also pass `--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/13` (`aarch64-linux-gnu` on arm64) in `CMAKE_C_FLAGS` and `CMAKE_CXX_FLAGS`. Or set `REXSDK_DIR` to build the SDK from source with the project's own compiler, where the check does not apply. A package built against another standard library, such as libc++, gives the same error with that library named. |
| `Could NOT find Vulkan` | Install the LunarG Vulkan SDK 1.4.313 or newer, or 1.3.275 or newer on Linux arm64. On Windows, open a new shell. On Linux and macOS, source `setup-env.sh`. |
| `Could not find a package configuration file provided by "rexglue"` | Add the SDK install folder, such as `out/install/win-amd64`, to `CMAKE_PREFIX_PATH`. To build the SDK from source with the project, set `REXSDK_DIR` to the SDK source folder. |
| `--game_data_root was not provided` | Pass `--title.game_root` with the folder of the game files. |
| `unknown setting '<name>' from CommandLine` | Check the name in the [Settings Reference](reference/cvars.md). |
