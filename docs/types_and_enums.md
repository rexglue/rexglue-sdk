# Type / enum header emission

Optional codegen step that materialises producer-supplied
`[[types]]` / `[[enums]]` config arrays into two header files
alongside the recompiler's `.cpp` output:

```
<out_directory_path>/mappings_generated/types.h
<out_directory_path>/mappings_generated/enums.h
```

The headers are pure declarations — they pull in `<cstddef>` /
`<cstdint>` and nothing else. Each producer type becomes an opaque
`struct` of the right size, guarded by `static_assert(sizeof(...) ==
size)`, with a sibling `<Type>_offsets` namespace whose members are
`inline constexpr size_t` field offsets. Each producer enum becomes
a strongly-typed `enum class` with a fixed-width underlying type.

## When to use this

A mapping file is the right tool when the host substrate (audio /
graphics / kernel HAL code) has to address PPC-side data with
hand-coded byte offsets:

```cpp
// kernel shim: clear the alerted byte on a thread
*((uint8_t*)thread + 0x6D) = 0;
```

With type headers emitted from the same producer the recompiler
already trusts for function names, the same shim becomes:

```cpp
*((uint8_t*)thread + _KTHREAD_offsets::Alerted) = 0;
```

Self-documenting, drift-detectable (the constant fails the build if
the producer ever changes), zero-overhead at runtime.

This feature is *not* required for codegen and is a no-op unless the
config supplies `[[types]]` and/or `[[enums]]` arrays.

## Schema

`[[types]]` and `[[enums]]` are top-level arrays-of-tables, parsed
from any included config file with the same merge semantics as
`[[switch_tables]]` and `[[invalid_instructions]]`. Place them in
the project's main config or in a separate file pulled in via
`includes`.

### `[[types]]`

| Field | Type | Required | Use |
|-------|------|----------|-----|
| `name` | string | yes | Display name (`_KTHREAD`, `Vehicle::State`). Sanitized into a C++ identifier. Anonymous tags (`<unnamed-tag>`) are skipped. |
| `size` | uint32 | yes (effectively) | sizeof, in bytes. `0` skips the static_assert and uses `_opaque[0x1]`. |
| `kind` | string | no | `"struct"` / `"union"` / `"class"`. Header comment only; emitted as `struct` regardless (opaque storage). Defaults to `"struct"`. |
| `unique_name` | string | no | Producer's mangled / unique name. Header comment only. |
| `[[types.fields]]` | array | no | Per-field offset entries. |

Per `[[types.fields]]` entry:

| Field | Type | Required | Use |
|-------|------|----------|-----|
| `name` | string | yes | Sanitized to a C++ identifier; reserved-word names (`register`, `not`, ...) are prefixed with `f_`. |
| `offset` | uint32 | yes | Bytes from the start of the parent type. |
| `type_expr` | string | no | Raw producer type expression. Preserved verbatim and emitted as a comment beside the offset constant. *Not parsed* — translating producer type expressions into self-consistent C++ is intentionally out of scope. |

### `[[enums]]`

| Field | Type | Required | Use |
|-------|------|----------|-----|
| `name` | string | yes | Display name. Sanitized; anonymous tags skipped. |
| `underlying` | string | no | Producer's integer type (`int`, `unsigned long`, `char`, `unsigned char`). Defaults to `int`. Translated to a fixed-width type at emit time (see "Underlying-type mapping"). |
| `unique_name` | string | no | Header comment only. |
| `[[enums.values]]` | array | yes (non-empty for non-empty enums) | Per-enumerator entries. |

Per `[[enums.values]]` entry:

| Field | Type | Required | Use |
|-------|------|----------|-----|
| `name` | string | yes | Sanitized to a C++ identifier. |
| `value` | int64 | yes | Signed-widened — fits both signed and unsigned underlyings. Narrowing happens at the `enum class` boundary. |

## Sanitization

Type / enum / nested-name sanitizer (separate from the function-name
sanitizer):

1. Anonymous producer names (anything starting with `<`, e.g.
   `<unnamed-tag>`) are skipped — they cannot be surfaced at file
   scope without a synthesized stable identifier, and the producer
   doesn't give us one.
2. `::` collapses to `__` (preserves the scope marker).
3. Each remaining non-`[A-Za-z0-9_]` byte becomes `_`.
4. Empty / leading-digit results are dropped (counts as
   "anonymous or unsanitizable").

There is intentionally **no address suffix** — types and enums are
name-keyed, not address-keyed. When two distinct producer entries
sanitize to the same identifier, the first wins; subsequent
collisions are dropped silently (logged at `[debug]`).

Field names use a stricter sanitizer (no `::` handling) plus a C++
keyword guard: a sanitized field name that collides with a reserved
word gets an `f_` prefix. Enum value names skip the guard because
producer enumerators are conventionally upper-case and collisions
don't arise in practice.

## Layout rules

### Types

```cpp
// <raw_name> (kind=<kind>, unique_name=<unique>)
struct <sanitized_name> {
    unsigned char _opaque[0x<size>];
};
static_assert(sizeof(<sanitized_name>) == 0x<size>, "<name> size mismatch with producer");
namespace <sanitized_name>_offsets {
    inline constexpr size_t <FieldA> = 0x<off>; // <raw_field_type>
    // ...
}
```

The opaque-buffer choice is what makes this a tractable feature
rather than a multi-month type-system project. Consumers that need
named-field access cast through the offset constants:

```cpp
auto* tlsData = *(void**)((uint8_t*)thread + _KTHREAD_offsets::TlsData);
```

`size == 0` skips the `static_assert` and the buffer becomes
`unsigned char _opaque[0x1]` (`sizeof(struct)` is always >= 1, so a
zero static_assert would always fail). Affects forward-declared
types where the producer doesn't carry a definition.

Anonymous unions and bitfields are flattened in the offsets
namespace: multiple fields at the same `offset` produce one entry
per unique sanitized name (first occurrence wins). The header is
documenting layout, not reproducing union semantics.

### Enums

```cpp
// <raw_name> (underlying=<u>, unique_name=<unique>)
enum class <sanitized_name> : <fixed_width_underlying> {
    VALUE_A = 0,
    VALUE_B = 1,
};
```

Underlying-type mapping:

| Producer string | C++ underlying |
|----------------|----------------|
| `char` | `int8_t` |
| `unsigned char` | `uint8_t` |
| `short` | `int16_t` |
| `unsigned short` | `uint16_t` |
| `int` | `int32_t` |
| `unsigned int` | `uint32_t` |
| `long` | `int32_t` |
| `unsigned long` | `uint32_t` |
| `__int64` | `int64_t` |
| `unsigned __int64` | `uint64_t` |
| `bool` | `bool` |
| anything else | `int32_t` (MSVC default) |

The Xbox 360 ABI maps `long` to 32 bits, matching MSVC PPC.

`enum class` is preferred over plain `enum` so values must be
qualified at use sites (`VehicleClass::Sedan`); this dodges the
collision class where two producer enums share value names.

Values within a single enum dedupe on sanitized name (first wins)
because two enumerators with the same name would be a hard C++
error.

## Example

```toml
# project.toml
project_name = "myproject"
file_path = "default.xex"
out_directory_path = "generated"

includes = ["pdb_types.toml"]

# pdb_types.toml
[[types]]
name        = "Vehicle"
unique_name = ".?AVVehicle@@"
kind        = "class"
size        = 0x80

[[types.fields]]
name      = "rpm"
type_expr = "float"
offset    = 0x10

[[types.fields]]
name      = "wheels"
type_expr = "Wheel[4]"
offset    = 0x20

[[enums]]
name        = "VehicleClass"
underlying  = "int"

[[enums.values]]
name  = "Sedan"
value = 0

[[enums.values]]
name  = "SUV"
value = 1
```

Running `rexglue codegen project.toml` writes
`generated/mappings_generated/types.h` and
`generated/mappings_generated/enums.h` alongside the recompiled
`.cpp` files, and logs:

```
[info] [codegen] Loaded 1 type definitions for header emission
[info] [codegen] Loaded 1 enum definitions for header emission
[info] [codegen] [type-headers] wrote generated/mappings_generated/types.h (1 types)
[info] [codegen] [type-headers] wrote generated/mappings_generated/enums.h (1 enums)
```

## Out of scope (future work)

- **Named-field emission.** A future opt-in mode that parses
  producer type expressions, topologically sorts UDTs, and emits
  real `struct Foo { _BAR baz; };` definitions. Substantial
  parser + ordering work; tracked separately. Until landed, host
  code uses the `<Type>_offsets` namespace.
- **Static-assert per field offset.** Possible to emit
  `static_assert(<Type>_offsets::Foo == 0xN)` for each field, but
  redundant with the `inline constexpr` value itself.
- **Unique-name as identifier fallback.** Use the producer's
  `unique_name` (`U_KTHREAD@@`, `T0x471`) to rescue anonymous
  types. The resulting identifiers are usually useless without
  surrounding parent context; defer until host code shows it
  actually wants them.

## Compatibility

- Default behaviour is unchanged when neither `[[types]]` nor
  `[[enums]]` is configured. The emit step is a no-op.
- Generated headers live in `mappings_generated/` so consumers
  that don't `#include` them are unaffected.
- The sanitized-identifier scheme is stable: same input config
  produces the same headers byte-for-byte. Re-runs are idempotent.
