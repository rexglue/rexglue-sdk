"""Build only the isolated fixture from an existing Release Ninja build.

No configure, install, or writes to the input build tree are performed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--install", type=Path, required=True)
    parser.add_argument("--baseline-exe", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--default-off", action="store_true")
    args = parser.parse_args()
    if not args.output.is_absolute():
        raise RuntimeError("--output must be absolute before path resolution")
    sdk, source, output = (p.resolve() for p in (args.sdk, args.source, args.output))
    if not output.is_absolute() or output.exists():
        raise RuntimeError("Use a fresh absolute task-local output directory")
    output.mkdir()
    temp = output / "temp"
    temp.mkdir()
    env = os.environ.copy()
    env["TEMP"] = env["TMP"] = str(temp)
    build = args.build.resolve()
    install = args.install.resolve()
    if args.default_off and not args.baseline_exe:
        raise RuntimeError("--default-off requires an existing --baseline-exe")
    ninja = build / "CMakeFiles/impl-Release.ninja"
    text = ninja.read_text()
    edges = {}
    for match in re.finditer(r"^build (.+)\n((?:  .*\n)*)", text, re.M):
        variables = dict(re.findall(r"^  (\w+) = (.*)$", match[2], re.M))
        edges[match[1]] = variables

    def compile_edge(relative):
        name = f"src/graphics/CMakeFiles/rexgpu-xenos.dir/Release/{relative}.obj: "
        return next((edge, values) for edge, values in edges.items() if edge.startswith(name))

    def compile_flags(relative):
        _, variables = compile_edge(relative)
        flags = shlex.split(variables["DEFINES"] + " " + variables["FLAGS"] + " " + variables["INCLUDES"])
        # Each translation unit keeps its own Release flags, without a normal PCH.
        for option in ("-include-pch", "-include"):
            while option in flags:
                at = flags.index(option)
                if at == 0 or flags[at - 1] != "-Xclang" or flags[at + 1] != "-Xclang":
                    raise RuntimeError("Unexpected PCH command syntax")
                del flags[at - 1:at + 3]
        if not args.default_off:
            flags += ["-DREX_INTERNAL_D3D12_TEXTURE_FIXTURE=1", "-DREX_INTERNAL_D3D12_TEXTURE_FIXTURE_MODULE=1"]
        # Fixture receipt headers and public headers from --source take precedence.
        return ["-I" + str(source / "include"), "-I" + str(source / "src"),
                "-I" + str(source), *flags]

    flags = compile_flags("d3d12/texture_cache.cpp")
    manifest = {"sdk_revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=sdk, text=True).strip(),
                "source_revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip(),
                "source_status": subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=normal"], cwd=source, text=True),
                "build": str(build), "install": str(install),
                "default_off": args.default_off, "temp": str(temp), "inputs": [], "commands": []}

    def input_file(path, role):
        path = Path(path).resolve()
        manifest["inputs"].append({"path": str(path), "role": role, "bytes": path.stat().st_size, "sha256": digest(path)})

    def command(label, argv):
        manifest["commands"].append({"label": label, "argv": [str(v) for v in argv]})
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2))
        with (output / (label + ".log")).open("w") as log:
            result = subprocess.run([str(v) for v in argv], cwd=build, env=env, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            print((output / (label + ".log")).read_text())
            raise RuntimeError(f"{label} failed ({result.returncode})")

    input_file(ninja, "ninja-metadata")
    input_file(args.clang, "compiler")
    replacements = {}
    for relative in ("pipeline/texture/cache.cpp", "shared_memory.cpp",
                     "d3d12/command_processor.cpp", "d3d12/texture_cache.cpp"):
        cpp = source / "src/graphics" / relative
        label = relative.replace("/", "__").removesuffix(".cpp")
        obj = output / (label + ".obj")
        input_file(cpp, "compiled-source")
        command(label, [args.clang, *compile_flags(relative), "-c", cpp, "-o", obj])
        replacements[f"src/graphics/CMakeFiles/rexgpu-xenos.dir/Release/{relative}.obj"] = obj
    objects = []
    link_edge, link = next((edge, values) for edge, values in edges.items()
                           if ": CXX_SHARED_LIBRARY_LINKER__rexgpu-xenos_Release " in edge)
    for value in link_edge.split(": CXX_SHARED_LIBRARY_LINKER__rexgpu-xenos_Release ", 1)[1].split(" | ", 1)[0].split():
        if value in replacements:
            objects.append(replacements[value])
        else:
            obj = build / value
            input_file(obj, "reused-plugin-object")
            # Retain the exact maintained source associated with each reused object.
            matching = next(edge for edge in edges if edge.startswith(value + ": "))
            cpp = matching.split(" ", 2)[2].split(" |", 1)[0].replace("$:", ":")
            input_file(cpp, "reused-object-source")
            objects.append(obj)
    fixture = source / "tests/internal_d3d12_texture_fixture"
    for path in fixture.iterdir():
        if path.is_file():
            input_file(path, "fixture-source")
    if not args.default_off:
        command("controller", [args.clang, *flags, "-c", fixture / "controller.cpp", "-o", output / "controller.obj"])
        objects.append(output / "controller.obj")
    libraries = shlex.split(link["LINK_LIBRARIES"])
    for lib in libraries:
        if not lib.startswith("-"):
            input_file(lib, "plugin-link-library")
    command("plugin-link", [args.clang, "-nostartfiles", "-nostdlib", *shlex.split(link["LANGUAGE_COMPILE_FLAGS"]),
             *shlex.split(link["LINK_FLAGS"]), "-o", output / "rexgpu-xenos.dll", "-Xlinker", "/MANIFEST:EMBED",
             "-Xlinker", "/implib:" + str(output / "rexgpu-xenos.lib"), "-Xlinker", "/pdb:" + str(output / "rexgpu-xenos.pdb"),
             *objects, *libraries])
    if args.default_off:
        input_file(args.baseline_exe, "existing-baseline-executable")
        (output / "baseline.exe").write_bytes(args.baseline_exe.read_bytes())
    else:
        build_caller(args, flags, link, source, output, install, input_file, command)
    runtime = install / "bin/rexruntime.dll"
    input_file(runtime, "runtime-module")
    (output / "rexruntime.dll").write_bytes(runtime.read_bytes())
    manifest["outputs"] = [{"path": str(path), "sha256": digest(path), "bytes": path.stat().st_size}
                           for path in output.iterdir() if path.is_file() and path.suffix in (".exe", ".dll", ".obj", ".lib")]
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2))
    print(f"Built isolated {'default-off' if args.default_off else 'fixture'} module and caller: {output}")


def build_caller(args, flags, link, source, output, install, input_file, command):
    fixture = source / "tests/internal_d3d12_texture_fixture"
    caller_flags = [v for v in flags if not v.startswith("-DREX_INTERNAL") and not v.startswith("-Drexgpu_xenos_EXPORTS")]
    command("caller", [args.clang, *caller_flags, "-c", fixture / "caller.cpp", "-o", output / "caller.obj"])
    caller_libraries = [install / ("lib/" + lib) for lib in ("rexruntime.lib", "spdlog.lib", "fmt.lib", "xxhash.lib", "SDL3-static.lib")]
    for lib in caller_libraries:
        input_file(lib, "caller-link-library")
    system = ["-l" + lib for lib in ("d3d12", "kernel32", "user32", "gdi32", "winmm", "imm32", "ole32", "oleaut32",
                                   "version", "uuid", "advapi32", "setupapi", "shell32", "hid", "dinput8", "dxgi", "dxguid",
                                   "ws2_32", "winspool", "comdlg32", "oldnames")]
    command("caller-link", [args.clang, "-nostartfiles", "-nostdlib", *shlex.split(link["LANGUAGE_COMPILE_FLAGS"]),
             "-fuse-ld=lld-link", "-Xlinker", "/subsystem:console", output / "caller.obj", *caller_libraries,
             *system, "-o", output / "texture_fixture.exe"])


if __name__ == "__main__":
    main()
