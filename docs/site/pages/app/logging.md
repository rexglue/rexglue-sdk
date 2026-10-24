# Logging

The SDK writes every log message through one process logger, `rex::Logger`, which `REX_LOGGER()` returns. The declarations are in `rex/logging/log_category.h` and `rex/logging/logger.h`. This page explains categories and levels, how to write messages, and how the logger chooses where its output goes.

A message has a category, a level and a text. The logger passes each message that its category lets through to its sinks, the outputs it writes to: standard output, the debugger on Windows, the log file and any sink your code adds.

The logger starts on the first call to `REX_LOGGER()`, so code can log before the `App` exists. Until the app applies the `log` settings, the logger writes messages at `info` and above to standard output, and on Windows to an attached debugger.

## Categories

A category is a named group of messages with its own level, such as `gpu` or `filesystem`. It lets a player raise the detail of one part of the SDK and leave the rest unchanged.

`REXLOG_CATEGORY(name, description)` declares the category `rex::log::name`. Write it at global scope in a header that every writer of the category includes. The name is a lowercase identifier, unique in the process. `REXLOG_TABLE` collects categories into a table:

```cpp
#include <rex/logging/log_category.h>

REXLOG_CATEGORY(input, "Controller polling and remapping")
REXLOG_CATEGORY(save, "Save game reads and writes")
REXLOG_TABLE(kAppLog, input, save)
```

Pass the table to `Logger::AddCategories`, and to `Logger::RemoveCategories` when your code stops:

```cpp
REX_TRY_VOID(REX_LOGGER().AddCategories(rex::log::kAppLog));
```

- `AddCategories` fails for a category name that the logger has added, and when the logger has 256 categories.
- Each added category gets a setting `log.categories.input`, named after the category, that sets its level.
- A message in a category that was never added is written at the `log.level` threshold, with a warning that the category is not registered.

See [Log Categories and Macros](../appendix/log-categories-and-macros.md) for the categories of the SDK.

## Levels

Each message has a level, and each category has a threshold level. The logger writes a message when its level is at or above the threshold of its category.

| Level | Name | Purpose |
|---|---|---|
| `kTrace` | `trace` | Detail per instruction, per packet and per loop iteration. |
| `kDev` | `dev` | ReX debug output and repeated stub calls. |
| `kDebug` | `debug` | State changes and intermediate values. |
| `kInfo` | `info` | Events that a user reads. |
| `kWarn` | `warn` | Recoverable problems, fallbacks and unsupported features. |
| `kError` | `error` | Failures that lose functionality. |
| `kCritical` | `critical` | Unrecoverable state, written right before an abort. |
| `kOff` | `off` | A threshold only. No message is written at it. |

`rex::log::ParseLevel` reads a level name in any case, and `rex::log::LevelName` returns the name of a level.

## Writing Messages

Each level has a macro that takes a category, then a `std::format` string and its arguments: `REXLOG_CAT_TRACE`, `REXLOG_CAT_DEV`, `REXLOG_CAT_DEBUG`, `REXLOG_CAT_INFO`, `REXLOG_CAT_WARN`, `REXLOG_CAT_ERROR` and `REXLOG_CAT_CRITICAL`.

```cpp
REXLOG_CAT_INFO(rex::log::save, "wrote {}", path);
REXLOG_CAT_ERROR(rex::log::input, "poll failed with {}", status);
```

The macros do not evaluate the arguments when the category does not write at that level, so a disabled trace line costs one check. Write no prefix in the text, because the `log.pattern` setting adds the time, the level, the category and the thread to every line.

`REXLOG_CAT_ONCE` writes a message only the first time a key is seen. It takes the category, the level, a `rex::log::OncePerKey` object that remembers the keys, the key, the format string and its arguments. `OncePerKey<64>` remembers the keys 0 to 63, and a key at or above 64 writes every time.

The SDK categories have shorter macros that take no category. Each prefix belongs to one category, such as `REXGPU_` for `gpu` and `REXFS_` for `filesystem`, and `REXLOG_` is the prefix of `core`:

```cpp
REXLOG_INFO("initialized");
REXGPU_DEBUG("register r{} = {:#x}", reg, value);
REXFS_WARN("{} not found", path);
```

Each prefix has the level forms from `TRACE` to `CRITICAL`, plus `WARN_ONCE` and the failure forms `FAIL`, `FATAL` and `CHECK` listed [below](#failures-and-assertions). See [Log Categories and Macros](../appendix/log-categories-and-macros.md#category-prefixes) for every prefix.

### Lines Before the Log File

The app opens the log file during `App::Init`, after it applies the settings. The logger keeps the lines written before then, so the file starts with the whole startup:

- Before the file opens, the logger keeps the newest 65536 lines and writes them to the file first.
- After the file opens, it keeps the newest 4096 lines and gives them to each sink added later.
- When `log.file` is false, it keeps no lines.

## Settings

See [Settings Reference](../reference/cvars.md#log) for every `log` setting.

- Every `log` setting except `log.level` and the `log.categories` settings is startup only.
- `log.debugger` writes to the debugger on Windows and has no effect elsewhere.
- A `log.pattern` with a flag the logger does not know is refused with a warning, and the logger keeps the default pattern.
- The log file is in `app.log_root`. Its name is `log.file_name`, or the app name when that is empty, followed by a run number, such as `mygame_007.log`. The run number is one higher than the highest number in the folder.
- An absolute `log.file_name` names the file itself, with no run number.
- When `log.folder_budget` is above 0, the logger removes the oldest runs of the same name until the files fit in that many bytes.

In `log.pattern`, `%Q` writes the level as one letter and `%j` writes the thread. See [Log Categories and Macros](../appendix/log-categories-and-macros.md#level-letters) for the letters.

### Category Levels

`log.level` is the threshold of every category that has no level of its own. `log.categories.gpu` sets the threshold of the `gpu` category, and an empty value follows `log.level`.

```
myapp --log.level=warn --log.categories.gpu=trace
```

```toml
[log]
level = "info"

[log.categories]
gpu = "debug"
```

To set a default that players can override, write at the title layer in `App::OnConfigure`:

```cpp
rex::Result<void> MyApp::OnConfigure(rex::SettingsRegistry& settings) {
  REX_TRY_VOID(settings.SetText(rex::SettingLayer::kTitle, "log.level", "debug"));
  return rex::Ok();
}
```

## Sinks

A sink is a class that implements `rex::log::ILogSink`, with a `Write` call that receives each message and a `Flush` call. The logger never calls the sinks on two threads at once. The SDK has two sinks:

- `LogFileSink` writes to a file opened with `LogFileSink::Open`, which appends to the file and creates missing folders.
- `LogCaptureSink` keeps the newest 4096 messages in memory. The console overlay reads them with `CopySince` and `GetLatestSequence`.

`Logger::AddSink` adds your own sink. The sink first receives the kept lines, then every new line. `AddSink` returns a `ScopeGuard` that removes the sink when it is destroyed. Declare the guard after the sink, so the guard is destroyed first. See [Change Callbacks](settings.md#change-callbacks) for how a `ScopeGuard` works.

```cpp
class WarnCounter final : public rex::log::ILogSink {
 public:
  void Write(const rex::log::Message& message) override {
    if (message.level >= rex::log::Level::kWarn) {
      ++count_;
    }
  }
  void Flush() override {}

 private:
  rex::u32 count_ = 0;
};

WarnCounter counter;
auto guard = REX_LOGGER().AddSink(counter);
```

`rex::log::Flush` flushes every sink. It needs no logger reference, so a crash handler can call it.

## Failures and Assertions

Three macros end a function or the process with a log message:

| Macro | Effect |
|---|---|
| `REX_CAT_FAIL(category, value, fmt, ...)` | Writes an error message and returns `value` from the enclosing function. |
| `REX_CAT_FATAL(category, fmt, ...)` | Writes a critical message, flushes every sink and aborts. |
| `REX_CAT_CHECK(category, cond, fmt, ...)` | Runs `REX_CAT_FATAL` when `cond` is false. |

The prefix forms such as `REXFS_FAIL` take the same arguments without the category. For `core` they are `REX_FAIL`, `REX_FATAL` and `REX_CHECK`.

The assertion macros in `rex/assert.h` print to standard error, not through the logger, and they do nothing in Release builds. Use a `CHECK` or `FATAL` macro for a failure that must reach the log file in every build.

| Macro | Effect |
|---|---|
| `REX_ASSERT(cond, ...)` | Aborts in Debug when `cond` is false. Release does not evaluate `cond`. |
| `REX_ASSERT_ALWAYS(...)` | Aborts in Debug when reached. Release does nothing. |
| `REX_UNHANDLED_CASE(var)` | Aborts in Debug when a switch reaches a value it does not handle. |

```cpp
rex::Result<void> Load(const std::filesystem::path& path, rex::u32 count, rex::u32 capacity) {
  REX_ASSERT(count <= capacity, "ring overflow");
  if (!std::filesystem::exists(path)) {
    REXFS_FAIL(rex::Err(rex::ErrorCategory::kIO, "missing file"), "{} is missing",
               path.string());
  }
  return rex::Ok();
}
```
