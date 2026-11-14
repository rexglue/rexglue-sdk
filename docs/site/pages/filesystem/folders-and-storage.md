# Folders and Storage

A program keeps its files in host folders, and a setting chooses each folder. The `app.*` settings hold the folders of the program itself, and the `title.*` settings hold the folders of the game files. The [filesystem](filesystem.md) then mounts the game folders under guest paths such as `game:`.

## App Folders

The `app.*` settings are [`kStartupOnly`](../app/settings.md#traits), so they are fixed once the systems start.

| Setting | Default | Holds |
|---|---|---|
| `app.root` | The [app folder](#app-folder) | The files of the program. |
| `app.config_file` | `{app.root}/{app.name}.toml` | The config file. |
| `app.log_root` | `{app.root}/logs` | The log files. |
| `app.user_root` | `{app.root}/user` | Saves, content packages and profile data. |
| `app.cache_root` | Empty | The shader and pipeline caches. Empty keeps no cache. |

A default in braces refers to another setting and takes its value. Because the config file, the logs and the user folder default to paths under `{app.root}`, changing `app.root` moves all three. `app.name` holds the name that the program passes to the `App` constructor, and it is never saved to the config file.

`App` declares `app.root` and `app.log_root`. `PathSettings`, which `REX_PATHS()` returns, declares `app.config_file`, `app.user_root` and `app.cache_root`.

### App Folder

The app folder is the folder of the executable. The app uses the user config folder instead when it cannot write to the executable folder, or when it runs as a packaged app. The user config folder is a folder named after the app:

| Platform | User config folder | Packaged when |
|---|---|---|
| Windows | Under `%APPDATA%` | Never |
| macOS | Under `~/Library/Application Support` | It runs from an app bundle |
| Linux | Under `$XDG_CONFIG_HOME`, or `~/.config` when that is unset | It runs as an AppImage |

When there is no user config folder, such as with no home folder, the app folder stays the executable folder.

### Single Instance

`app.single_instance` holds the name of a lock that only one running copy of the program can hold. A second copy with the same lock name fails to start. Empty means no lock.

## Game Folders

The `title.*` settings are [`kUntilLaunch`](../app/settings.md#traits), so they are fixed when the game launches.

| Setting | Command line option | Default | Holds |
|---|---|---|---|
| `title.game_root` | `--game_data_root` | Empty | The game files, mounted as `game:`. |
| `title.game_writable` | None | `false` | Whether `game:` is writable. |
| `title.update_root` | `--update_data_root` | Empty | The title update files, mounted as `update:`. |
| `title.metadata_root` | `--metadata_root` | `{app.root}/metadata` | The achievement metadata. |
| `title.entry` | None | `default.xex` | The executable under `game:` that `TitleRuntime::Load` runs. |
| `title.imports` | None | Empty | Other files under `game:` that the executable can import from, separated by `\|`. |

`TitleApp::LaunchTitle` fails when `title.game_root` is empty or is not a folder. A relative `title.game_root` or `title.update_root` is relative to the working directory.

### Metadata

The runtime reads the achievement metadata from a file named `achievements.toml`. When `title.metadata_root` is set in any layer, the runtime looks for the file only in that folder. While the setting keeps its default, the runtime ignores the default value and tries three places in order:

1. The `metadata` folder in `title.game_root`.
2. A `metadata` folder next to `title.game_root`.
3. `title.game_root` itself.

## Saves and Content

XAM stores the saves, the downloaded content and the profile data of the game under `app.user_root`. See [Storage Paths](../appendix/storage-paths.md) for the path of each item.

A content package is a save or a piece of downloaded content. The path of a package holds three IDs, written in uppercase hexadecimal:

- The XUID, the 16 digit ID of the profile that the package belongs to.
- The title ID of the game, 8 digits.
- The content type, 8 digits, such as `00000001` for a saved game.

The default profile has the name `User` and the XUID `B13EBABEBABEBABE`.

When a folder exists whose name differs from a content package or achievement path only in case, the SDK uses that folder. A relative `app.user_root` is relative to the working directory for content packages and profile data. Achievement state and the console overlay history use the path as written.

### Content Mounts

When the game opens a content package, XAM mounts it in the filesystem and links the root name that the game chose, such as `save:`, to the mount. The first package opened is mounted at `\Device\Content\1`, the second at `\Device\Content\2`, and the number counts up with each open.

- A package stored as a folder is a writable host directory mount.
- A package stored as a file is a read only STFS mount.

## Shader Cache

The Xenos graphics drivers keep the guest shaders and the pipeline descriptions of a game under `app.cache_root`, one set of files for each title ID, and translate them again when the next run starts. With an empty `app.cache_root`, or with `gpu.xenos.store_shaders` off, they keep none.

The pipeline file name holds a variant, the render target path that the driver uses, so each path keeps its own file. The Direct3D 12 variant is `rov` or `rtv`, and the Vulkan variant is `fsi` or `fbo`. See [Storage Paths](../appendix/storage-paths.md#shader-cache) for the file names.

`gpu.xenos.dump_shaders` sets a separate folder where the driver writes the shaders it translates, for debugging.
