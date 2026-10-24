# Shim List

## Shims

The shim registry in `rex/kernel/shim.h` lists 69 shims in seven groups. See [Shims](../runtime/shims.md) for how to enable a shim.

### `file`

These shims read and write files through the filesystem module.

| Name | Description |
|---|---|
| `CreateFileA` | Opens or creates a file. |
| `ReadFile` | Reads from an open file handle. |
| `WriteFile` | Writes to an open file handle. |
| `SetFilePointer` | Moves the file position with a 32 bit distance. |
| `GetFileSize` | Returns the low 32 bits of a file size. |
| `GetFileSizeEx` | Returns the 64 bit file size. |
| `SetEndOfFile` | Truncates a file at the current position. |
| `FlushFileBuffers` | Returns success without flushing the file. |
| `DeleteFileA` | Deletes a file. |
| `CloseHandle` | Closes a file or search handle. |
| `FindFirstFileA` | Begins a directory search. |
| `FindNextFileA` | Returns the next directory search result. |
| `FindClose` | Ends a directory search. |
| `CreateDirectoryA` | Creates a directory. |
| `MoveFileA` | Renames a file or directory. |
| `SetFileAttributesA` | Returns success without setting attributes. |
| `GetFileAttributesA` | Returns file attributes. |
| `GetFileAttributesExA` | Returns extended file attribute data. |
| `SetFilePointerEx` | Moves the file position with a 64 bit distance. |
| `SetFileTime` | Returns success without setting the timestamps. |
| `CompareFileTime` | Compares two file times. |
| `CopyFileA` | Copies a file. |
| `RemoveDirectoryA` | Removes an empty directory. |
| `GetFileType` | Returns the type of a file handle. |

### `heap`

These shims allocate from the shim heap, which uses the o1heap allocator. The manifest must list all four together.

| Name | Description |
|---|---|
| `RtlAllocateHeap` | Allocates memory, honors the zero flag. |
| `RtlFreeHeap` | Frees an allocation. |
| `RtlSizeHeap` | Returns the requested size of an allocation. |
| `RtlReAllocateHeap` | Resizes an allocation in place or by copy. |

### `memory`

These shims copy and fill memory.

| Name | Description |
|---|---|
| `memcpy` | Copies memory. |
| `memmove` | Copies overlapping memory. |
| `memset` | Fills memory. |
| `memchr` | Finds a byte in memory. |
| `XMemCpy` | Copies memory. |
| `XMemSet` | Fills memory. |
| `XMemSet128` | Fills memory in blocks of 128 bytes. |
| `memset_vmx` | Fills memory through the vector path. |
| `memcpy_s` | Copies memory with a size check. |
| `memmove_s` | Copies overlapping memory with a size check. |

### `string`

These shims run the C, Win32 and UTF-16 string routines.

| Name | Description |
|---|---|
| `strncmp` | Compares two strings up to a length. |
| `strncpy` | Copies a string up to a length. |
| `strchr` | Finds a character in a string. |
| `strstr` | Finds a substring. |
| `strrchr` | Finds the last occurrence of a character. |
| `strtok` | Splits a string into tokens. |
| `_stricmp` | Compares two strings ignoring case. |
| `strcpy_s` | Copies a string with a size check. |
| `lstrlenA` | Returns the length of a string. |
| `lstrcpyA` | Copies a string. |
| `lstrcpynA` | Copies a string up to a length. |
| `lstrcatA` | Appends a string. |
| `lstrcmpiA` | Compares two strings ignoring case. |
| `wcslen` | Returns the length of a UTF-16 string. |
| `wcscmp` | Compares two UTF-16 strings. |
| `wcsncmp` | Compares two UTF-16 strings up to a length. |
| `wcscoll` | Compares two UTF-16 strings by collation. |
| `wcschr` | Finds a character in a UTF-16 string. |
| `wcsrchr` | Finds the last occurrence in a UTF-16 string. |
| `wcscpy` | Copies a UTF-16 string. |
| `wcsncpy` | Copies a UTF-16 string up to a length. |
| `wcsncpy_s` | Copies a UTF-16 string with a size check. |
| `wcsstr` | Finds a UTF-16 substring. |

### `math`

These shims compute floating point math and convert text to numbers.

| Name | Description |
|---|---|
| `atan2` | Returns the arc tangent of y over x. |
| `atof` | Parses a floating point number, independent of the locale. |

### `debug`

This shim writes debug output to the log.

| Name | Description |
|---|---|
| `DebugPrint` | Logs a formatted debug message at `debug` level, converted from Shift-JIS to UTF-8. |

### `threading`

These shims run guest fibers on host fibers.

| Name | Description |
|---|---|
| `ConvertThreadToFiber` | Turns the calling thread into a fiber. |
| `ConvertFiberToThread` | Turns the calling fiber back into a thread. |
| `CreateFiber` | Creates a fiber. |
| `DeleteFiber` | Deletes a fiber. |
| `SwitchToFiber` | Switches to another fiber. |
