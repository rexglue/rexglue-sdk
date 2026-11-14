# Storage Paths

This page lists where the SDK stores the saves, content, profile data and shader caches of a game on the host. See [Folders and Storage](../filesystem/folders-and-storage.md) for the settings that choose the folders.

## Saves, Content and Profiles

These paths are under the folder in `app.user_root`. The parts in angle brackets are filled in for each item:

- `<XUID>` is the XUID of the profile, as 16 uppercase hexadecimal digits, such as `B13EBABEBABEBABE`.
- `<TitleID>` is the title ID of the game, as 8 uppercase hexadecimal digits.
- `<ContentType>` is the content type, as 8 uppercase hexadecimal digits, such as `00000001` for a saved game.

| Item | Path under `app.user_root` |
|---|---|
| Content package | `<XUID>/<TitleID>/<ContentType>/` and the package file name |
| Package header | `<XUID>/<TitleID>/Headers/<ContentType>/` and the package file name with `.header` added |
| Package thumbnail | `__thumbnail.png` inside a package folder |
| Saved game | A content package of type `00000001` |
| Marketplace content | A content package of type `00000002`, with the XUID `0000000000000000` |
| Profile setting | `<TitleID>/profile/` and the profile name, then the setting ID as 8 hexadecimal digits |
| Achievement state | `achievements/<TitleID>.toml` |
| Console overlay history | `console_history.txt` |

For example, a saved game named `save0` of the default profile, for a game with the title ID `12345678`, is at `B13EBABEBABEBABE/12345678/00000001/save0` under `app.user_root`.

## Shader Cache

These paths are under the folder in `app.cache_root`. The variant is the render target path of the driver, `rov` or `rtv` for Direct3D 12 and `fsi` or `fbo` for Vulkan.

| Item | Path under `app.cache_root` |
|---|---|
| Direct3D 12 pipeline descriptions | `shaders/shareable/<TitleID>.<variant>.d3d12.xpso` |
| Vulkan pipeline descriptions | `shaders/shareable/<TitleID>.<variant>.vk.xpso` |
| Guest shaders | `shaders/shareable/<TitleID>.xsh` |

See [Shader Cache](../filesystem/folders-and-storage.md#shader-cache) for when the drivers write these files.
