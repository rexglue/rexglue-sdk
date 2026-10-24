# Hooks

A hook is host C++ code that takes the place of, or runs inside, a recompiled guest function. Hooks are how a project changes the behavior of the generated code without editing it. There are three kinds:

- A function hook replaces a whole function.
- A detour is a function hook whose body can call the original function.
- A mid function hook runs at one instruction inside a function.

`rexglue init` creates a source file for your hooks, named after the project. For the project `foobar` from [Getting Started](../getting-started.md), it is `src/foobar_hooks.cpp`.

## Function Hooks

### Weak Symbols

Every generated function has two symbols. The body is `__imp__name`, and `name` is a weak symbol that runs the body. On Windows and Linux, `name` is a weak alias of `__imp__name`. On macOS, `name` is a separate weak function that tail calls `__imp__name`, because Mach-O has no alias that another definition can replace.

All guest calls go to `name`, both direct calls and indirect calls through the [function table](generated-code.md#function-table). A strong definition of `name` in your code replaces the weak one, so every call reaches your hook, and the original stays callable as `__imp__name`.

!!! note "Hooks go in an object library"
    The linker takes an object file out of a static library only to resolve a symbol that is still undefined. The weak `name` from the generated code has defined the symbol, so the linker never takes a hook out of a static library and the hook has no effect. Build hook sources as an `OBJECT` library. The `CMakeLists.txt` that `rexglue init` writes builds `src/foobar_hooks.cpp` into the object library `foobar_common`.

### Function Hook Macros

The macros are in `rex/ppc/hook.h`. Each macro except `REX_IMPORT` defines an `extern "C"` function at namespace scope, and takes the symbol name of the function it replaces.

| Macro | Arguments and effect |
|---|---|
| `REX_HOOK` | Takes a symbol name and a host function. Calls the host function with arguments read from the guest registers and writes the result to `r3`, `f1` or `v1`. |
| `REX_HOOK_RAW` | Takes a symbol name. Opens a body with `ctx` and `base` in scope. |
| `REX_DETOUR` | Takes a symbol name, a return type and a parameter list. The body can call the [original](#detours). |
| `REX_STUB` | Takes a symbol name. Logs a stub message and leaves `r3` unchanged. |
| `REX_STUB_LOG` | Takes a symbol name and a message, and logs the message with the stub message. |
| `REX_STUB_RETURN` | Takes a symbol name and a value. Logs the value and writes it to `r3`. |
| `REX_IMPORT` | Takes a symbol, a callable name and a signature. Declares a host callable for a guest function, with internal linkage. |

```cpp
#include <rex/ppc/hook.h>

static u32 AddOne(u32 value) {
  return value + 1;
}

REX_HOOK(sub_82010000, AddOne);
```

A stub logs a warning on its first call and a Dev level message on every later call.

When the runtime stops a guest thread, such as at shutdown, it sets a stop request for it. `REX_HOOK` and `REX_DETOUR` check for a stop request before and after the host function runs, and unwind the guest thread when they find one. `REX_HOOK_RAW` does not check.

### Export Macros

A kernel export is a host function that takes the place of a function that the XEX imports from the Xbox 360 kernel, XAM or XBDM. Its symbol is `__imp__` and the export name, such as `__imp__NtCreateFile`, and the export tables of the SDK link against that symbol.

The export macros `REX_EXPORT`, `REX_EXPORT_STUB` and `REX_EXPORT_STUB_RETURN` work like `REX_HOOK`, `REX_STUB` and `REX_STUB_RETURN`, and take the `__imp__` symbol of the export:

| Macro | Arguments |
|---|---|
| `REX_EXPORT` | Takes the symbol and the host function it wraps. |
| `REX_EXPORT_STUB` | Takes the symbol. |
| `REX_EXPORT_STUB_RETURN` | Takes the symbol and the value to return in `r3`. |

```cpp
REX_EXPORT(__imp__NtCreateFile, rex::kernel::xboxkrnl::NtCreateFile_entry)
```

### Argument Translation

`REX_HOOK` reads the arguments of the host function from the guest registers by the PowerPC calling convention:

- An integer, a pointer, a `be_` value and a `MappedPtr` take the next of `r3` to `r10`, then 8 byte guest stack slots from `r1 + 0x50`.
- A float goes in the next of `f1` to `f13`, and also uses up the next of `r3` to `r10`.
- A `PPCVRegister` goes in the next of `v1` to `v13`, and uses no integer slot.

Three SDK types appear in hook signatures:

- The `be_` aliases, such as `be_u32`, hold a big endian value in guest byte order. Reading the value swaps the bytes.
- `rex::memory::MappedPtr` is a host pointer that keeps the guest address it maps.
- `rex::ppc::ByValue` wraps a structure of `be_` fields passed by value.

| Parameter type | Value |
|---|---|
| `u8` to `u64`, `i8` to `i64`, `bool`, enums | The slot, cast to the type. |
| `f32`, `f64` | The float register, or the stack slot. |
| `T*` | Null for 0, otherwise the host pointer of the guest address. |
| `MappedPtr` | Null for 0, otherwise a host pointer that keeps its guest address. |
| `be_u32` and the other `be_` aliases | The slot without a swap. Reading the value swaps it. |
| `PPCVRegister` | The vector register. |

| Result type | Result |
|---|---|
| `ByValue` over 8 bytes | Written through the hidden pointer that the guest passes in `r3`. The visible arguments move up by one slot. |
| `ByValue` of 8 bytes or less | Packed into `r3`. |
| Pointer | The guest address, in `r3`. |
| Float | `f1`. |
| Vector | `v1`. |
| Integer | `r3`, sign or zero extended to 64 bits by the signedness of the type. |

## Detours

```cpp
#include <rex/ppc/hook.h>

REX_DETOUR(ExampleFunction, u32, (u32 count)) {
    return original(count + 1) + 1;
}
```

- `original` calls `__imp__ExampleFunction` on the current register state of the thread.
- A detour that returns `void` leaves the registers as the original left them.
- A detour cannot return a structure over 8 bytes by value.
- A detour must not stay running across a guest `SwitchToFiber`, because the chain of running detours belongs to the host thread.

### Context Form

If the parameter list starts with `PPCContext& ctx`, the body can read and write every register, and calls the original as `original(ctx, args...)`.

### Debug Checks

A `rex::ppc::StackFrame` pushes space on the guest stack for the length of a C++ scope. In a debug build, a detour logs an error and asserts when:

- the body returns without running the original
- a `StackFrame` opened in the body is still open
- `r2` or `r13` changed

It warns once when:

- the parameters leave out an argument register that the function reads
- the body writes through `ctx` to a register that the image keeps in a local

## Mid Function Hooks

A mid function hook calls host code at one guest instruction, before or after it, without replacing the function. Codegen writes the call into the generated function, so it reads the hook declarations at code generation time. They come from C++ [hook sources](#hook-sources) or from [manifest entries](#manifest-hooks).

In C++, declare the body with one of three macros and attach it to an address with `REX_MIDASM_AT`. The macros are in `rex/ppc/midasm.h`, which `rex/ppc/hook.h` does not include.

| Macro | Body returns | Effect |
|---|---|---|
| `REX_MIDASM_HOOK` | `void` | The function continues. |
| `REX_MIDASM_BRANCH` | `bool` | Jumps to the target set in `REX_MIDASM_AT` when the body returns true. |
| `REX_MIDASM_RETURN_IF` | `bool` | Returns from the function when the body returns true. |
| `REX_MIDASM_AT` | | Attaches the body to the address. |

- The body macros take the body name and the parameter list in parentheses.
- `REX_MIDASM_AT` takes the body name, a literal address, `before` or `after`, and one register for each parameter.
- For a `REX_MIDASM_BRANCH` body, `REX_MIDASM_AT` also takes the jump target, a numeric literal, before the registers, as in `REX_MIDASM_AT(Skip, 0x82000100, before, 0x82000210, r3)`.

```cpp
#include <rex/ppc/midasm.h>

REX_MIDASM_HOOK(BumpCount, (u32 count, PPCRegister& result)) {
    result.u64 = count + 1;
}
REX_MIDASM_AT(BumpCount, 0x82000100, before, r3, r4);
```

Registers map to parameters by position. Here `count` receives the low 32 bits of `r3`, and `result` is a reference to `r4`.

### Parameters

A body takes at most 32 parameters. Each is one of:

- `u32`, `u64` or `f64` by value
- `PPCVRegister` by value
- an object pointer, null for 0
- a `MappedPtr`
- a non const reference to a register type or to `PPCContext`

Each register token fits these parameter types:

| Register token | Fits |
|---|---|
| `r0` to `r31` except `r1` | `u32`, `u64`, `PPCRegister&`, a pointer, a `MappedPtr` |
| `f0` to `f31` | `f64`, `PPCRegister&` |
| `v0` to `v127` | `PPCVRegister`, `PPCVRegister&` |
| `cr0` to `cr7` | `PPCCRRegister&` |
| `ctr`, `reserved` | `PPCRegister&` |
| `xer` | `PPCXERRegister&` |
| `fpscr` | `PPCFPSCRRegister&` |
| `ctx` | `PPCContext&` |

- Codegen rejects `r1`.
- The address must be a nonzero multiple of 4.
- A register that does not fit its parameter type fails to compile.

## Hook Sources

`[project].hook_sources` lists the folders, relative to the manifest, that codegen scans for `REX_MIDASM_` declarations. `rexglue init` sets it to `["src"]`.

- Codegen reads every `.cpp`, `.cc`, `.h` and `.hpp` file below those folders, without expanding macros.
- A body and its `REX_MIDASM_AT` can be in different files.

Because codegen reads the text without a preprocessor, it accepts these forms:

- address and target literals in hexadecimal, binary, octal or decimal, with digit separators and `u` and `l` suffixes
- parameter types with or without `rex::`, `std::` and a leading `const`
- `uint32_t`, `uint64_t` and `double` for `u32`, `u64` and `f64`
- the mapped aliases, such as `MappedU32` and `GuestPtrT`

For the same reason, do not put the `REX_MIDASM_` macros inside a preprocessor conditional. A header guard around the whole file is allowed. To make a hook conditional, put the condition inside the body.

- An address can have one source hook.
- A `before` and an `after` source hook at the same address collide, as do a source hook and a `[[midasm_hook]]` entry.
- Of two `[[midasm_hook]]` entries at one address, the later one replaces the earlier one.
- Other errors give the file and line of the macro.

Source hooks use C linkage, so each body name must be unique across the program.

## Manifest Hooks

A `[[midasm_hook]]` entry in the manifest declares a mid function hook with `address`, `name`, `registers`, and the return and jump keys. It needs no `hook_sources`. See [`[[midasm_hook]]`](../reference/manifest.md#midasm_hook) for the keys.

```toml
[[midasm_hook]]
address = 0x82000100
name = "BumpCount"
registers = ["r3", "r4"]
```

```cpp
void BumpCount(PPCRegister& r3, PPCRegister& r4) {
    r4.u64 = r3.u32 + 1;
}
```

These entries are errors:

- a return and a jump on the same condition, such as `return_on_true` with `jump_address_on_true`
- the direct form (`return`, `jump_address`) mixed with the conditional form
- `r1` in `registers`

### Declaration

Codegen declares the function with C++ linkage and a reference parameter for each entry of `registers`. The function returns `bool` when the entry sets a conditional return or jump key, and `void` otherwise.

| Register | Parameter type |
|---|---|
| `r0` to `r31`, `f0` to `f31`, `ctr`, `reserved` | `PPCRegister&` |
| `v0` to `v127` | `PPCVRegister&` |
| `cr0` to `cr7` | `PPCCRRegister&` |
| `xer` | `PPCXERRegister&` |
| `fpscr` | `PPCFPSCRRegister&` |
| `ctx` | `PPCContext&` |
