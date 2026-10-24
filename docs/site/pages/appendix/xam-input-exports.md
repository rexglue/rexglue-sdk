# XAM Input Exports

This page lists the results of the XAM input exports, which a game calls to read controllers. The exports read the four users of `IInputSystem`. See [Input](../devices/input.md#xam-input-exports) for the connection changes and the UI block.

## User and Device Selection

- User index `0xFF` selects user 0. So does the any user flag in the query flags, except for `XamInputSetState`, which takes no flags.
- The low byte of the query flags selects device types. A query with device type bits set but without the gamepad bit returns `X_ERROR_DEVICE_NOT_CONNECTED`.
- A user above 3, or a user with no device, returns `X_ERROR_DEVICE_NOT_CONNECTED`.

## Results

| Export | Result |
|---|---|
| `XamInputGetState` | Writes the state of the user, when the state pointer is not null. |
| `XamInputGetCapabilities`, `XamInputGetCapabilitiesEx` | Write the capabilities of the user's device. A null pointer returns `X_ERROR_BAD_ARGUMENTS`. |
| `XamInputSetState` | Sets the vibration of the user. A null pointer returns `X_ERROR_BAD_ARGUMENTS`. |
| `XamInputGetKeystroke` | Takes the next keystroke of the user. A null pointer returns `X_ERROR_BAD_ARGUMENTS`, and a user with no keystroke queued returns `X_ERROR_EMPTY`. |
| `XamInputGetKeystrokeEx` | Reads the user index through a pointer, takes the next keystroke, and writes back the user that produced it. Fails like `XamInputGetKeystroke`. |
| `XamUserGetDeviceContext` | Writes 0. Succeeds for user 0 and `0xFF`, and returns `X_E_DEVICE_NOT_CONNECTED` for other users. |

A failure of the input system other than a missing device returns `X_ERROR_FUNCTION_FAILED`.

## Keystrokes

A gamepad produces keystrokes as well as state. Each change of a button, a trigger or a stick direction produces a keystroke, and the last one pressed repeats while it is held.
