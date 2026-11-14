# API Reference

[The API pages](../../api/) document the public C++ interface of the SDK, the classes, functions and macros declared in the headers under `include/rex`. Doxygen generates them from those headers and their documentation comments. These guide pages explain how the parts fit together and link a class or macro to its API page on its first mention.

The API pages cover the macros that `rexglue codegen` writes into the precompiled header of a project, such as `DEFINE_REX_FUNC` and the load and store macros `REX_LOAD_U32` and `REX_STORE_U32`. Names in a `detail` namespace and macros that start with `REX_DETAIL_` are internal and are left out.

## Where to Start

A program is a subclass of one of two classes from `rexapp`:

- `rex::app::TitleApp` in `rex/app/title_app.h` runs a game. `rexglue init` writes a `TitleApp` subclass into `src/main.cpp`.
- `rex::app::App` in `rex/app/app.h` is the base class, for a tool that runs no game.

See [App Lifecycle](../app/app-lifecycle.md) for the steps and hooks of both classes.

Code that is not part of the app class, such as a hook or a module, reaches the running program through the global access macros:

- `REX_INTERFACE(T)` from `rex/module.h` returns the interface `T` of a loaded module, or null. See [Finding an Interface](../app/modules.md#finding-an-interface).
- `REX_RUNTIME()` from `rex/kernel/title_runtime.h` returns the `TitleRuntime` of the loaded game, or null when no game is loaded.
- `REX_GUEST_MEMORY()` from the same header returns its guest memory, or null.
- `REX_SETTINGS()` from `rex/settings_registry.h` returns the settings registry.
- `REX_LOGGER()` from `rex/logging/logger.h` returns the logger.
- `REX_PATHS()` from `rex/path_settings.h` returns the `PathSettings`.

Hook sources include `rex/ppc/hook.h`. See [Hooks](../codegen/hooks.md) for function hooks and detours.

## Kernel Services

`TitleRuntime` gives access to the kernel services of the loaded game, such as the filesystem, the object table and the content manager. Each accessor returns a reference that stays valid from `Load` until `Unload`. See `TitleRuntime` in the API pages for every accessor, [Runtime Lifecycle](../runtime/lifecycle.md) for `Load` and `Unload`, and [Kernel Services](../runtime/kernel.md) for what the services do.
