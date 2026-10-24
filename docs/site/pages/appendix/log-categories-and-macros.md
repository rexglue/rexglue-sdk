# Log Categories and Macros

## Built in Categories

The SDK declares these log categories. Each one has a setting such as `log.categories.gpu` that sets its level. See [Logging](../app/logging.md#categories) for how to add your own.

| Category | Messages |
|---|---|
| `core` | The core library, module loading, settings and the app lifecycle. |
| `filesystem` | The mount table, links and file providers. |
| `kernel` | The xboxkrnl kernel exports. |
| `xam` | The XAM exports. |
| `system` | Kernel objects, guest threads, guest memory and modules, and the host threads and memory behind them. |
| `cpu` | Function dispatch and the PPC context. |
| `title` | The debug text that the title writes through `DbgPrint` and `OutputDebugString`. |
| `apu` | Guest audio, XMA decoding and the audio driver modules. |
| `hid` | Input devices and the input driver modules. |
| `ui` | Windows, presenters, overlays and ImGui. |
| `console` | The output of console overlay commands. |
| `gpu` | Xenos emulation, shader translation and the host graphics paths. |
| `codegen` | Recompiler analysis and code generation. |
| `rexglue` | The `rexglue` command line tool. |

## Category Prefixes

Each prefix forms the macros of one category, such as `REXGPU_INFO` and `REXGPU_FAIL`. See [Logging](../app/logging.md#writing-messages) for the macro forms.

| Prefix | Category |
|---|---|
| `REXLOG_` | `core` |
| `REXSYS_` | `system` |
| `REXFS_` | `filesystem` |
| `REXAPU_` | `apu` |
| `REXGPU_` | `gpu` |
| `REXHID_` | `hid` |
| `REXCPU_` | `cpu` |
| `REXUI_` | `ui` |
| `REXCON_` | `console` |

The `core` failure macros have no `LOG` in their names: `REX_FAIL`, `REX_FATAL` and `REX_CHECK`.

The prefixes of `kernel`, `title`, `xam`, `codegen` and `rexglue` are declared in private headers under `src`, not in the public headers.

## Level Letters

The `%Q` flag of `log.pattern` writes the level as one letter. See [Logging](../app/logging.md#settings) for `log.pattern`.

| Letter | Level |
|---|---|
| `T` | trace |
| `V` | dev |
| `D` | debug |
| `I` | info |
| `W` | warn |
| `E` | error |
| `C` | critical |
