# Shims

A shim is a host function of the SDK that replaces a library routine compiled into the title, such as `memcpy` or `CreateFileA`.

## What a Shim Does

A kernel export replaces a function that the title imports from the console. The title links the C library, the Win32 style file routines and the `Rtl` heap routines statically, so they are compiled into the XEX as PowerPC code like the rest of the title, and codegen recompiles them like any other function.

A shim replaces one of these routines with a host function. Codegen gives the function at the manifest address the name `rexshim_` plus the routine name, such as `rexshim_memcpy`, and emits no body for it. Every call to the routine then runs the host function of the SDK. A shim is a [function hook](../codegen/hooks.md) that the SDK provides.

Each group of shims does its work on the host:

- The `memory` and `string` shims run the host C library on the guest buffers.
- The `file` shims open, read and write files through the filesystem, so the paths of the title resolve through the [mounts](../filesystem/filesystem.md#links-and-mounts).
- The `heap` shims allocate from the [shim heap](#shim-heap).
- The `atof` shim parses numbers with the C locale, regardless of the host locale.
- The `DebugPrint` shim writes the formatted message to the `kernel` log category at `debug` level.
- The `threading` shims run each guest fiber on a host fiber with its own guest stack.

## Enable a Shim

To replace a routine, add its name and the guest address of its copy in the XEX to the [`[shim]`](../reference/manifest.md#shim) table of the manifest:

```toml
[shim]
memcpy = 0x82000400
memset = 0x82000600
CreateFileA = 0x82000800
```

Find the address of the routine in IDA or in a symbol map of the executable. [`rexglue list shim`](../reference/cli.md#list-shim) prints the names that the SDK provides, and `--group` with a group name such as `file` limits the output to one group.

## Rules

Codegen checks the `[shim]` table and fails on any of these errors:

- The name must be a shim of the SDK. An unknown name fails with "is not in the shim registry", and the error can suggest the closest registered name.
- The address must be 4 byte aligned.
- The address must be inside a binary image of the project, or codegen fails with "Manifest entries are in no binary image".
- The `heap` group must be listed whole, because its four routines share one heap. A table with some but not all four `heap` routines fails with "group is incomplete" and lists the missing routines.

## Shim Heap

The shim heap is a heap in guest memory that the `heap` shims allocate from. It exists only when the `[shim]` table lists the `heap` routines, and the runtime creates it in `TitleRuntime::Load`.

- [`runtime.heap_size_mb`](../reference/cvars.md#runtime) sets the size of the first heap segment in MiB. The default is 256 and the range is 4 to 2048.
- When the heap is full, it adds a segment of at least 64 MiB from guest memory.
- An allocation that fails after the heap grows returns a null address to the title.

## Shims That Do Nothing

`FlushFileBuffers`, `SetFileAttributesA` and `SetFileTime` return success to the title and change nothing.

## Shim List

See [Shim List](../appendix/shim-list.md) for the 69 shims in seven groups.
