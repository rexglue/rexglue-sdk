> [!CAUTION]
> This project is in early development. Expect things to not work quite right and there to be significant changes and breaking public API updates as development progresses. Contributions and feedback are welcome, but please be aware that the codebase is still evolving rapidly.

<h1 align="center">
  <br>
  <a href="https://github.com/rexglue/rexglue-sdk">
    <img src="https://github.com/rexglue/rexglue-media/blob/main/ReX_Banner.png" alt="ReXGlue banner">
  </a>
  <br>
  <br>
  <a href="https://discord.gg/CNTxwSNZfT">
    <img src="https://img.shields.io/badge/Discord-Join%20Server-5865F2?logo=discord&logoColor=white" alt="Discord">
  </a>
  <a href="https://github.com/rexglue/rexglue-sdk/actions/workflows/build-win-amd64.yaml">
    <img src="https://github.com/rexglue/rexglue-sdk/actions/workflows/build-win-amd64.yaml/badge.svg" alt="win-amd64 build status">
  </a>
  <a href="https://github.com/rexglue/rexglue-sdk/stargazers">
    <img src="https://img.shields.io/github/stars/rexglue/rexglue-sdk" alt="rexglue-sdk stargazers">
  </a>
</h1>

ReXGlue converts Xbox 360 PowerPC code into portable C++ that runs natively on modern platforms.

ReXGlue is heavily rooted on the foundations of [Xenia](https://github.com/xenia-project), the Xbox 360 emulator. Rather than interpreting or JIT-compiling PPC instructions at runtime, ReXGlue takes a different path: it generates C++ source code ahead of time, an approach inspired by [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and [rexdex's recompiler](https://github.com/rexdex/recompiler).

Latest SDK builds and releases are published on [GitHub Releases](https://github.com/rexglue/rexglue-sdk/releases). Join the [Discord server](https://discord.gg/CNTxwSNZfT) for updates and share what you have created.

## Example Projects

- [demo-iruka](https://github.com/rexglue/demo-iruka)
- [reblue](https://github.com/rexglue/reblue)

## Quickstart

### Prerequisites (non-Visual Studio users only)

- Clang 20
- CMake
- Ninja
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
rexglue init --app_name <project name> --app_root <project root path>
```

This generates a project with `CMakeLists.txt`, `CMakePresets.json`, a starter `main.cpp`, and a `<project>_config.toml` for the codegen pipeline. Ensure the SDK install path is set as the `REXSDK` environment variable before running CMake.

### Running codegen

```bash
rexglue codegen <project>_config.toml
```

For full CLI reference and config file options, see the [wiki](https://github.com/rexglue/rexglue-sdk/wiki).

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
