# Errors

SDK functions that can fail return their error as a value, not as an exception. Modules are separate libraries, and an exception must not cross from one to another, so every fallible call returns a `rex::Result` from `rex/result.h`.

## Result

`Result<T>` is `std::expected<T, Error>`. It holds either the value of type `T` or an `Error`. `Result<void>` holds no value on success. Test a result with `if (result)`, read the value with `*result` and the error with `result.error()`.

The header provides two helpers to build results:

- `Ok()` returns a success with no value, and `Ok(value)` a success with a value.
- `Err(error)` returns a failure. `Err(category, message)` and `Err(category, message, code)` build the `Error` in place.

Three macros return early from a function that itself returns a `Result`:

- `REX_TRY(decl, expr)` evaluates `expr`, returns its error on failure, and otherwise moves its value into the declaration `decl`.
- `REX_TRY_VOID(expr)` returns the error of `expr` on failure, and discards any value.
- `REX_ERROR_IF(cond, error)` returns `error` when `cond` is true.

```cpp
rex::Result<void> LoadAndLaunch(rex::kernel::TitleRuntime& runtime) {
  REX_TRY_VOID(runtime.Load());
  return runtime.Launch();
}
```

## Error

An `Error` holds three fields:

- `category`, an `ErrorCategory` that says what type of failure it is.
- `message`, the error text.
- `code`, a code from the platform or a library, or the value of an error enum. It is 0 when there is none.

`GetDescription` returns `Success` for the category `kNoError`, and otherwise the message, with the code appended when it is not 0. `Error::FromErrno` builds an error from an `errno` value or another platform code.

The categories are `kIO`, `kMemory`, `kFormat`, `kCrypto`, `kCompression`, `kRuntime`, `kPlatform`, `kConfig`, `kValidation`, `kNotFound`, `kNotImplemented` and `kModule`. A default constructed `Error` has the category `kNoError`, which `IsSuccess` reports as true.

### Error Enums

Two parts of the SDK need finer codes than a category. Each declares an error enum, and a `BuildError` function that puts the enum value in `Error::code`. `CodeOf` reads the enum value back from an error.

| Enum | Header | Category of the error |
|---|---|---|
| `FileSystemError` | `rex/filesystem/file_system_error.h` | `kNotFound` for `kNotFound`, `kPathNotFound` and `kNoMount`, `kFormat` for `kCorrupt`, `kNotImplemented` for `kUnsupported`, and `kIO` for the others. |
| `HIDError` | `rex/xam/input.h` | Always `kPlatform`. |

```cpp
if (auto opened = fs.Open(path, options); !opened &&
    rex::filesystem::CodeOf(opened.error()) == rex::filesystem::FileSystemError::kNoMount) {
  // The path is on no mount.
}
```
