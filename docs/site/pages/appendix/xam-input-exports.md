# XAM Input Exports

This page lists the results of the XAM input exports, which a game calls to read controllers. The exports read the four users of `IInputSystem`. See [Input](../devices/input.md#xam-input-exports) for the connection changes and the UI block.

## User and Device Selection

- User index `0xFF` selects user 0. So does the any user flag in the query flags, except for `XamInputSetState`, which takes no flags, and the keystroke exports, where the flag takes a keystroke from any user.
- The low byte of the query flags selects device types. A state or capabilities query with device type bits set but without the gamepad bit returns `X_ERROR_DEVICE_NOT_CONNECTED`. A keystroke query takes controller keys with the gamepad bit and keyboard text with the keyboard bit.
- A user above 3, or a user with no device, returns `X_ERROR_DEVICE_NOT_CONNECTED`.

## Results

| Export | Result |
|---|---|
| `XamInputGetState` | Writes the state of the user, when the state pointer is not null. |
| `XamInputGetCapabilities`, `XamInputGetCapabilitiesEx` | Write the capabilities of the user's device. A null pointer returns `X_ERROR_BAD_ARGUMENTS`. |
| `XamInputSetState` | Sets the vibration of the user. A null pointer returns `X_ERROR_BAD_ARGUMENTS`. |
| `XamInputGetKeystroke` | Takes the next keystroke of the user, like `XamInputGetKeystrokeEx` with the user index in a local. A null pointer returns `X_ERROR_BAD_ARGUMENTS`, and a connected user with no keystroke queued returns `X_ERROR_EMPTY`. The keystroke struct is zeroed first. |
| `XamInputGetKeystrokeEx` | Reads the user index through a pointer and takes the next keystroke. With the any user flag it takes the keystroke from the first of users 0 to 3 that has one, and writes back that user. Otherwise it leaves the user index alone. Fails like `XamInputGetKeystroke`. |
| `XamUserGetDeviceContext` | Writes 0. Succeeds for user 0 and `0xFF`, and returns `X_E_DEVICE_NOT_CONNECTED` for other users. |

A failure of the input system other than a missing device returns `X_ERROR_FUNCTION_FAILED`.

## Keystrokes

A gamepad produces keystrokes as well as state. Each change of a button, a trigger or a stick direction produces a keystroke, and the last one pressed repeats while it is held.
