# File Exports

This page lists how the kernel file exports, such as `NtCreateFile` and `NtQueryDirectoryFile`, check guest paths and report results to the game. See [Filesystem](../filesystem/filesystem.md) for how the filesystem resolves a path.

## Path Rules

- `NtCreateFile`, `NtQueryFullAttributesFile` and `NtOpenSymbolicLinkObject` accept a path that starts with `\??\` and remove that prefix. `ObCreateSymbolicLink` and `ObDeleteSymbolicLink` do the same for the link name.
- A path that holds a control character, a byte outside ASCII, or any of `" + , < > |` returns `X_STATUS_OBJECT_NAME_INVALID`. So does a `*` or `?` outside a search pattern.
- In an `NtQueryDirectoryFile` search pattern, a `*` must end the pattern or come right before a `.`. A bad search pattern returns `X_STATUS_INVALID_PARAMETER`.
- A new name for a rename with `NtSetInformationFile` must not contain `;` or `=`.
- `ObCreateSymbolicLink` succeeds when the link exists.

## Open Status Codes

`NtCreateFile` returns these status codes on failure:

| Condition | Status |
|---|---|
| The object attributes are null, or the disposition is above `kOverwriteIf` | `X_STATUS_INVALID_PARAMETER` |
| The path breaks the path rules | `X_STATUS_OBJECT_NAME_INVALID` |
| The root directory handle is unknown | `X_STATUS_INVALID_HANDLE` |
| The filesystem returns `kNotFound` | `X_STATUS_OBJECT_NAME_NOT_FOUND` |
| The filesystem returns `kPathNotFound` or `kNoMount` | `X_STATUS_OBJECT_PATH_NOT_FOUND` |
| The filesystem returns `kExists` | `X_STATUS_OBJECT_NAME_COLLISION` |
| The filesystem returns `kIsDirectory` | `X_STATUS_FILE_IS_A_DIRECTORY` |
| The filesystem returns `kNotDirectory` | `X_STATUS_NOT_A_DIRECTORY` |
| The filesystem returns `kAccessDenied` | `X_STATUS_ACCESS_DENIED` |
| The filesystem returns `kSharingViolation` | `X_STATUS_SHARING_VIOLATION` |
| The filesystem returns `kInvalidPath` | `X_STATUS_OBJECT_NAME_INVALID` |
| The filesystem returns `kUnsupported` | `X_STATUS_NOT_IMPLEMENTED` |
| The filesystem returns any other code | `X_STATUS_UNSUCCESSFUL` |

## Dispositions

The disposition of an open says what happens when the path exists and when it is absent. See [Disposition](../filesystem/filesystem.md#disposition) for the open options.

| Disposition | Path exists | Path absent |
|---|---|---|
| `kSupersede` | Replace | Create |
| `kOpen` | Open | Fail with `kNotFound`, or `kPathNotFound` when a folder is missing |
| `kCreate` | Fail with `kExists` | Create |
| `kOpenIf` | Open | Create |
| `kOverwrite` | Truncate | Fail with `kNotFound`, or `kPathNotFound` when a folder is missing |
| `kOverwriteIf` | Truncate | Create |

## Open Actions

`NtCreateFile` writes the action it took into the IO status block:

| `OpenAction` | Reported when |
|---|---|
| `kSuperseded` | An entry was replaced |
| `kOpened` | An entry was opened |
| `kCreated` | An entry was created |
| `kOverwritten` | An entry was truncated |
| `kExists` | `NtCreateFile` failed because the entry exists |
| `kDoesNotExist` | `NtCreateFile` failed for any other reason |
