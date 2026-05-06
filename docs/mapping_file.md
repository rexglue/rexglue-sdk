# Mapping file

A mapping file is an optional TOML that overrides the default
`sub_XXXXXXXX` symbol names ReXGlue emits for recompiled functions.
The recompiler ships zero mapping data; producers (such as a tool that
extracts publics from a Microsoft PDB) supply the file out-of-band.
This is a *pure rename pass*: no analysis or codegen behaviour
changes, the only effect is friendlier symbol names in the generated
`.cpp` and the resulting `.o` symbol table.

A mapping file is the right tool when:

- You have legitimate access to a PDB (or equivalent) for a build
  whose function addresses overlap with the binary you are
  recompiling, and you want generated symbols that look like
  `Vehicle__GetRPM_82043110` instead of `sub_82043110`.
- You want recompiled output that diff-able, profileable, or
  debuggable against the original code without manually annotating
  every function.

A mapping file is *not* required for codegen and not consulted unless
configured.

## Schema

ReXGlue reads a single top-level array, `[[function]]`. Per entry,
two fields are consumed:

| Field     | Type     | Required | Use                                  |
|-----------|----------|----------|--------------------------------------|
| `address` | uint32   | yes      | Function start address (image VA).   |
| `display` | string   | yes      | Compact human name. Sanitized into a C++ identifier; see below. |

Any other fields (e.g. `name`, `demangled`, `size`, `source_file`, or
sibling arrays such as `[[type]]` / `[[enum]]`) are ignored. Producers
are free to emit them; future passes may consume them but the rename
pass will not.

A minimal example:

```toml
[[function]]
address = 0x82043110
display = "Vehicle::GetRPM"

[[function]]
address = 0x82001234
display = "operator new"

[[function]]
address = 0x820ABCDE
display = "~Vehicle"
```

A real-world example, abridged from a PDB extractor's output:

```toml
[[function]]
address    = 0x823E0000
size       = 0x40                                 # ignored by the rename pass
name       = "?GetCharacterIndex@VectorFont@@UBAH_W@Z"  # ignored
demangled  = "virtual int VectorFont::GetCharacterIndex(wchar_t) const"  # ignored
display    = "VectorFont::GetCharacterIndex"
```

## Sanitization

`display` is a human shortname (`Vehicle::GetRPM`), not a valid C++
identifier and not unique across overload sets or template
instantiations. ReXGlue applies a deterministic two-pass transform
plus an unconditional address suffix:

1. Replace every `::` with `__`. The scope boundary survives as a
   readable marker.
2. Replace every remaining byte not in `[A-Za-z0-9_]` with `_`.
3. If the result is empty or starts with a digit, fall back to
   `sub_<UPPER_HEX_ADDR>` (the default for unmapped functions). This
   keeps grep behaviour consistent across mapped and unmapped sites.
4. Otherwise, append `_<UPPER_HEX_ADDR>` and emit.

Worked examples:

| `display`              | `address`    | Output                            |
|------------------------|-------------:|-----------------------------------|
| `Vehicle::GetRPM`      | `0x82043110` | `Vehicle__GetRPM_82043110`        |
| `operator new`         | `0x82001234` | `operator_new_82001234`           |
| `Foo<int>::bar(float)` | `0x82005678` | `Foo_int___bar_float__82005678`   |
| `~Vehicle`             | `0x820ABCDE` | `_Vehicle_820ABCDE`               |
| `` (empty)             | `0x82000000` | `sub_82000000`                    |
| `42mystery`            | `0x82000004` | `sub_82000004`                    |

The unconditional address suffix is a deliberate choice over
collision-suffixed naming. It guarantees uniqueness without a
collision-detection pass, makes names regex-round-trippable to the
source PDB, and rules out a class of bugs where a later mapping
update silently changes which name a given address resolves to.

## Activation

Two equivalent mechanisms, with the CLI flag taking precedence:

| Mechanism | Resolution | Notes |
|---|---|---|
| `mapping_file_path = "..."` (top-level TOML key) | Relative paths resolve against the directory of the file that introduced the key. Absolute paths pass through. | Same scoping as `includes`. An included config can supply this and the parent does not need to know about it. |
| `--mapping <path>` (CLI flag) | Resolved against the current working directory. Absolute paths pass through. | Last occurrence wins. Replaces (does not merge) any value loaded from TOML. |

When neither is set, no mapping is loaded and the rename pass is a
no-op.

## Behaviour

The rename pass runs once per codegen invocation, after analyze and
before code emission. Its log line summarises five counters:

```
[mapping] renamed N function symbol(s) from mapping TOML
  (X intra-function skipped, Y missing,
   Z import-preserved, W helper-preserved)
```

| Counter | Meaning |
|---|---|
| renamed | Mapping address matched a `FunctionNode` entry; its name was overridden. |
| intra-function skipped | Mapping address falls inside a discovered function but is not its entry. ReXGlue's analyzer represents these as labels within a parent, not as separate functions, so the rename pass cannot give them an external symbol. See "Limitations". |
| missing | Mapping address was not discovered as code in this binary. Typical cause: the mapping was built from a sibling binary whose function set is not a subset. |
| import-preserved | Mapping address resolved to an `IMPORT` function; the kernel-supplied import name (`__imp__<symbol>`) was kept. |
| helper-preserved | Mapping address resolved to a register-save / restore helper; the ABI-mandated `__restgprlr_*` / `__savevmx_*` name was kept. |

A mapping covering a strict subset of the binary's functions is the
expected case. Uncovered functions retain their default
`sub_XXXXXXXX` names. A mapping built from a sibling-game PDB that
does not perfectly match the target binary is also tolerated:
incorrect entries simply attach the wrong name to the wrong address;
the recompiled C++ remains structurally valid.

## Limitations

The pass renames *function entries*. It does not rename labels inside
a function. ReXGlue's analyzer treats discontinuous chunks of a
parent function as labels rather than separate functions, so a
mapping entry whose `address` falls in the middle of a function is
counted under "intra-function skipped" — no symbol is renamed for
it. PDB-derived mappings that include both function entries and
intra-function chunks will therefore see a rename count substantially
lower than the entry count. Producing top-level symbols for
intra-function entries is out of scope for this feature.

The pass does not validate that mapping entries match the actual
binary (function start alignment, size, etc.). That responsibility
belongs to the producing tool.
