# Storage Paths

This page lists where the SDK stores the saves, content, profile data and shader caches of a game on the host. See [Folders and Storage](../filesystem/folders-and-storage.md) for the settings that choose the folders.

## Saves, Content and Profiles

These paths are under the folder in `app.user_root`. The parts in angle brackets are filled in for each item:

- `<XUID>` is the XUID of the profile, as 16 uppercase hexadecimal digits, such as `B13EBABEBABEBABE`.
- `<TitleID>` is the title ID of the game, as 8 uppercase hexadecimal digits.
- `<ContentType>` is the content type, as 8 uppercase hexadecimal digits, such as `00000001` for a saved game.
- `<ProfileName>` is the name of the profile.
- `<SettingID>` is the ID of the profile setting, as 8 uppercase hexadecimal digits.

| Item | Path under `app.user_root` |
|---|---|
| Content package | `<XUID>/<TitleID>/<ContentType>/` and the package file name |
| Package header | `<XUID>/<TitleID>/Headers/<ContentType>/` and the package file name with `.header` added |
| Package thumbnail | `__thumbnail.png` inside a package folder |
| Saved game | `<XUID>/<TitleID>/00000001/` and the package file name |
| Marketplace content | `0000000000000000/<TitleID>/00000002/` and the package file name |
| Profile setting | `<TitleID>/profile/<ProfileName>/<SettingID>` |
| Achievement state | `achievements/<TitleID>.toml` |
| Console overlay history | `console_history.txt` |

For example, a saved game named `save0` of the default profile, for a game with the title ID `12345678`, is at `B13EBABEBABEBABE/12345678/00000001/save0` under `app.user_root`.

## Shader Cache

These paths are under the folder in `app.cache_root`. The variant is the render target path of the driver, the method that it uses to emulate the render targets of the Xbox 360 GPU. Each method keeps its own file. Direct3D 12 uses `rov` (rasterizer ordered views) or `rtv` (render target views), and Vulkan uses `fsi` (fragment shader interlock) or `fbo` (framebuffer objects). The settings `gpu.d3d12.render_target_path` and `gpu.vulkan.render_target_path` choose the method. When the setting is empty, Direct3D 12 uses `rov` on an Intel adapter that supports it and `rtv` otherwise, and Vulkan uses `fbo`. A method that the device cannot run falls back to `rtv` or `fbo`.

| Item | Path under `app.cache_root` |
|---|---|
| Direct3D 12 pipeline descriptions | `shaders/shareable/<TitleID>.<variant>.d3d12.xpso` |
| Vulkan pipeline descriptions | `shaders/shareable/<TitleID>.<variant>.vk.xpso` |
| Guest shaders | `shaders/shareable/<TitleID>.xsh` |

See [Shader Cache](../filesystem/folders-and-storage.md#shader-cache) for when the drivers write these files.
