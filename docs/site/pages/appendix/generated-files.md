# Generated Files

These are the files that `rexglue codegen` writes into the output folder of each binary, for the project `foobar` from [Getting Started](../getting-started.md). The prefix `foobar` is `[project].name`. See [Output Files](../codegen/generated-code.md#output-files) for the prefix and the templates, and `rex::ppc::PPCImage` for the fields of `PPCImageConfig`.

| File | Purpose |
|---|---|
| `foobar_pch.h` | The precompiled header. Defines the image constants, the configuration defines and the macros that generated code uses. |
| `foobar_funcs.h` | Declares every function with `DECLARE_REX_FUNC`, imports included. |
| `foobar_init.h` | Includes `foobar_pch.h` and `foobar_funcs.h`. |
| `foobar_init.cpp` | Defines `PPCImageConfig`, the function mappings and the call usage table. |
| `foobar_register.cpp` | Defines a function for each import from another recompiled module. Also defines `foobar_RegisterFunctions` for the entrypoint, or `ReXRegisterModule` and `ReXGetModuleImage` for a DLL module. |
| `foobar_recomp.0.cpp`, `foobar_recomp.1.cpp` and up | Define the function bodies, ordered by address. |
| `foobar_funcs.0.h`, `foobar_funcs.1.h` and up | Each declares the functions that the `.cpp` file of the same number references. |
| `sources.cmake` | Sets `GENERATED_SOURCES` to the `.cpp` files to compile. |
| `codegen.stamp` | Holds the input fingerprint and the list of output files. |
| `codegen.partition.json` | Maps each function to the number of the file that holds it. |
| `codegen.mmio.json` | Lists the loads and stores that reach or might reach a register block, by how codegen handles each. |

The entrypoint folder also holds these files:

| File | Purpose |
|---|---|
| `codegen.d` | Lists the inputs of the run, so the build reruns codegen when one changes. |
| `codegen.build.stamp` | The output file of the rule in `codegen.d`. Holds the fingerprint of each binary. |
| `module_registry.cpp` | Registers the DLL modules with the runtime. Written only when the manifest lists `[[modules]]`. |
| `dll_targets.cmake` | Defines a shared library target for each DLL module. Written only when the manifest lists `[[modules]]`. |
