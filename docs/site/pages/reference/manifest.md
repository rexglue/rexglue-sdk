# Project Manifest

The project manifest is the TOML file that lists the binaries that `rexglue codegen` recompiles, with the settings for each. A binary is the entrypoint XEX, which runs first, or a DLL module, another XEX that the game loads while it runs. `rexglue init` writes the manifest into the project root, named after the project, such as `foobar_manifest.toml` for the project `foobar` from [Getting Started](../getting-started.md).

This page lists every section and key. See [Codegen Pipeline](../codegen/codegen-pipeline.md) for how codegen uses them, and [Finding the Manifest](cli.md#finding-the-manifest) for how codegen finds the file when no path is given.

## Layout

| Section | Meaning |
|---|---|
| `[project]` | Required. Holds the keys for the whole project. |
| `[entrypoint]` | Required. Holds the keys of the entrypoint XEX. |
| `[[modules]]` | Optional. Holds the keys of one DLL module per entry. |
| Any other top level key or table | Forms the shared layer. Its keys are binary table keys that apply to every binary. See [Includes](#includes). |

Relative paths in `file_path`, `out_directory_path`, `template_dir`, `game_root` and `hook_sources` resolve against the folder of the manifest.

## `[project]`

| Key | Type | Meaning |
|---|---|---|
| `name` | string | Required. Sets the prefix of every generated C++ file. It starts with a letter or `_` and contains only letters, digits and `_`. |
| `sdk_version` | string | The SDK version that last ran codegen on the project. Codegen writes it. See [SDK Version Stamp](#sdk-version-stamp). |
| `game_root` | string | Sets the folder that holds the game files. The entrypoint and every targeted module must be inside it. The default is the folder of the entrypoint. |
| `hook_sources` | array of strings | Lists the folders that codegen scans for mid function hooks declared in C++. See [Hook Sources](../codegen/hooks.md#hook-sources). |

## Binary Tables

`[entrypoint]`, every `[[modules]]` entry and the shared layer take the same keys, the binary table keys below. `project_name` and `guest_path` have no effect in the shared layer.

### Files and Identity

| Key | Type | Meaning |
|---|---|---|
| `file_path` | string | Required. Points to the XEX file. |
| `guest_path` | string | Required for modules. Sets the path that guest code loads the module by. It must be unique, ignoring the device, the slash direction, case, and a leading project name followed by `/assets/`. |
| `out_directory_path` | string | Sets the output folder. Each targeted binary needs its own. |
| `project_name` | string | Sets the file prefix for this binary. The default is `[project].name`. |
| `template_dir` | string | Points to a folder of templates that replace the built in templates. |
| `includes` | array of strings | Lists TOML files to merge into the table. See [Includes](#includes). |
| `is_dll` | boolean | Marks the binary as a DLL. The default is `false` for the entrypoint and `true` for modules. |

### Setjmp and Longjmp

| Key | Type | Meaning |
|---|---|---|
| `longjmp_address` | integer | Gives the address of the guest `longjmp`. Codegen writes calls to it as `ppc_longjmp`. |
| `setjmp_address` | integer | Gives the address of the guest `setjmp`. Codegen writes calls to it as `ppc_setjmp`. |

### Addresses

- Addresses in `longjmp_address`, `setjmp_address`, `[mmio]`, `[shim]` and `[functions]` must be 4 byte aligned.
- An address entry in the shared layer goes to the binary whose image contains the address. An entry in no image fails with `Manifest entries are in no binary image`.

### Register and Codegen Flags

Each flag is a boolean and defaults to `false`. See [Optimizations](../codegen/codegen-pipeline.md#optimizations) for what each one changes in the generated code.

| Key | Meaning |
|---|---|
| `skip_lr` | Removes every `ctx.lr` write and the `mflr` read. The writes are the `bl` to a guest function, `blrl`, `bctrl` and `mtlr`. A `bl` to an import always writes `ctx.lr`. |
| `skip_msr` | Removes the global lock read at `mfmsr`, and the MSR update and the global lock enter and leave at `mtmsrd`. |
| `ctr_as_local` | Keeps CTR in a local instead of `ctx.ctr`. |
| `xer_as_local` | Keeps XER in a local instead of `ctx.xer`. |
| `reserved_as_local` | Keeps the reservation register in a local instead of `ctx.reserved`. |
| `cr_as_local` | Keeps the CR fields in locals instead of `ctx.cr0` to `ctx.cr7`. |
| `non_argument_as_local` | Keeps `r0`, `r11`, `r12`, `f0` and `v32` to `v63` in locals. |
| `non_volatile_as_local` | Keeps `r14` to `r31`, `f14` to `f31`, `v14` to `v31` and `v64` to `v127` in locals, except in the functions listed under [Restore Exits](../codegen/codegen-pipeline.md#restore-exits-and-millicode). |
| `forward_stack_stores` | Makes a load through `r1` reuse the value of an earlier write in the same basic block to the same `r1` offset with the same width, instead of reading guest memory. |
| `generate_exception_handlers` | Wraps functions that have SEH scopes in exception handlers. See [Exception Scopes](../codegen/generated-code.md#exception-scopes). |

### `[analysis]`

| Key | Type | Default | Meaning |
|---|---|---|---|
| `exception_handler_funcs` | array of integers | empty | Lists the addresses of exception handler functions. |

### `[functions]`

Declares functions that analysis does not find by itself, or gives their size or name. Each key is a function address in hex, with or without `0x`, and each value is a table.

```toml
[entrypoint.functions]
0x82001234 = { size = 0x240, name = "LoadConfig" }
```

| Key | Type | Meaning |
|---|---|---|
| `name` | string | Sets the function name. The default is `sub_` and the address. |
| `size` | integer | Sets the size in bytes. Cannot be combined with `end`. |
| `end` | integer | Sets the end address, one past the last byte. Cannot be combined with `size`. |
| `parent` | integer | When not zero, makes the entry a chunk of the function at this address. |
| `share_registers` | boolean | Makes the function use the registers of its caller, for an SEH funclet. |

- A bad entry is logged and skipped.
- Overlapping entries that are not chunks fail validation.

### `[[invalid_instructions]]`

Marks a range of data inside code that codegen does not translate.

| Key | Type | Meaning |
|---|---|---|
| `data` | integer | Sets the start address. |
| `size` | integer | Sets the size in bytes. |

### `[[switch_tables]]`

Lists the targets of a jump table. An entry replaces automatic detection for that `bctr`.

| Key | Type | Meaning |
|---|---|---|
| `address` | integer | Gives the address of the `bctr` instruction. |
| `register` | integer | Gives the number of the register that holds the index. |
| `labels` | array of integers | Lists the targets in index order. An empty array is an error. |

### `[[midasm_hook]]`

Inserts a call to a host function at an instruction address. See [Manifest Hooks](../codegen/hooks.md#manifest-hooks).

| Key | Type | Meaning |
|---|---|---|
| `address` | integer | Required. Gives the instruction address. |
| `name` | string | Required. The function to call. |
| `registers` | array of strings | Lists the registers to pass, in order. See the list below. |
| `return` | boolean | Returns after the call. |
| `return_on_true` | boolean | Returns when the hook returns true. |
| `return_on_false` | boolean | Returns when the hook returns false. |
| `jump_address` | integer | Jumps to this address after the call. |
| `jump_address_on_true` | integer | Jumps to this address when the hook returns true. |
| `jump_address_on_false` | integer | Jumps to this address when the hook returns false. |
| `after_instruction` | boolean | Runs the hook after the instruction instead of before it. |

The register tokens are:

- `r0` and `r2` to `r31`, `f0` to `f31` and `v0` to `v127`
- `cr0` to `cr7`, `ctr`, `xer`, `fpscr` and `reserved`
- `ctx`, which passes the whole register state as `PPCContext&`

`r1` and any other token fail validation.

- A repeated address replaces the earlier hook.
- A return and a jump on the same condition fail validation, as does a mix of the direct form (`return`, `jump_address`) and the conditional form.

### `[vector_math]`

Selects fast or exact code for vector instructions whose host results can differ from the console. Each key takes `"fast"`, the default, or `"exact"`.

- With `"fast"`, rounding and denormal results differ from the console.
- With `"exact"`, results match the console bit for bit.
- The VMX128 form of an instruction follows the same key.
- `vlogefp` has no key and is always exact.
- Any other value logs a warning and has no effect.

```toml
[vector_math]
vmaddfp = "exact"
fres = "fast"
```

| Key | Instruction |
|---|---|
| `vmaddfp` | Vector multiply add |
| `vnmsubfp` | Vector negative multiply subtract |
| `vmsum3fp128` | Three element dot product |
| `vmsum4fp128` | Four element dot product |
| `vrefp` | Vector reciprocal estimate |
| `vrsqrtefp` | Vector reciprocal square root estimate |
| `vexptefp` | Vector base 2 exponent estimate |
| `fres` | Scalar reciprocal estimate |
| `frsqrte` | Scalar reciprocal square root estimate |

### `[shim]`

A shim is an SDK host function that replaces the copy of a library routine that is compiled into the XEX. Each key is a routine name and each value is the address of that copy, which codegen replaces with `rexshim_` and the name. [`rexglue list shim`](cli.md#list-shim) prints the names, and [Shims](../runtime/shims.md) lists them by group.

```toml
[shim]
CreateFileA = 0x82000400
```

- An unknown name fails, and the error can suggest the closest known name.
- A group of routines that depend on each other, such as the four routines of the heap group, must be listed whole. A partial group fails validation with `group is incomplete`.

### `[mmio]`

Declares memory accesses to the GPU and APU registers that codegen cannot find by itself. See [Memory Accesses](../codegen/codegen-pipeline.md#memory-accesses).

- `[mmio.gpu]` and `[mmio.apu]` each take `read` and `write` arrays of the addresses of the load and store instructions.
- An address can appear only once in all of `[mmio]`.

## Includes

`includes` merges other TOML files into a binary table, into the shared layer or into another included file. This way several binaries, or several projects, can share one set of entries.

- Paths resolve against the folder of the file that holds the key.
- An included file holds binary table keys.

### Merge Order

Each binary table is built in four steps, and a later step wins:

1. The scalar keys and flags of the shared layer, without its address entries.
2. The included files, in array order.
3. The keys of the table itself.
4. The address entries of the shared layer that are in the image of the binary. An entry that the binary has for the same address wins.

### Merge Rules

| Value kind | Merge rule |
|---|---|
| Strings and integers | The last value that is not empty or zero. A later layer cannot reset a value to empty or 0. |
| Booleans | The last value present, so `false` can override `true`. |
| `vector_math` keys | The last value present. |
| `[mmio]` | Entries add up. A repeated address in a table or its includes fails the load. An entry in the binary wins over a shared one. |
| Address sections | Entries add up, and the last entry for a repeated key wins. These are `[functions]`, `[shim]`, `[[switch_tables]]`, `[[midasm_hook]]` and `[[invalid_instructions]]`. |
| `analysis.exception_handler_funcs` | Entries add up, without duplicates. |

### Include Limits

- Includes nest to at most 32 levels.
- A file can be included more than once, but an include chain that leads back to a file that is loading fails as a circular include.

## SDK Version Stamp

After a successful run, codegen writes its SDK version into `[project].sdk_version`.

- Codegen rewrites only the `sdk_version` line, so a trailing comment on that line is lost.
- A new SDK version makes the next run regenerate every binary.

## Written by `rexglue init`

- A `[project]` table with `name`, `sdk_version`, `game_root` and `hook_sources = ["src"]`.
- An `[entrypoint]` table with `file_path`, `out_directory_path` and an empty `includes`. The output folder is `generated/` and the XEX file name without its extension, in lower case, such as `generated/default`.
- With `--scan-dll`, one `[[modules]]` entry per DLL.

See [`init`](cli.md#init) and [`init module`](cli.md#init-module).

## Errors

- An unknown key, or a key with the wrong TOML type, logs a warning and has no effect.
- A missing or invalid `[project].name`, an invalid `[project].hook_sources`, or a `[mmio]` entry that is not an integer fails the load.
- The old `[rexcrt]` table fails the load, and the error prints the matching `[shim]` text.
