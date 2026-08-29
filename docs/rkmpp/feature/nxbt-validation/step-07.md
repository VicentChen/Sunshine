# NXBT validation — step 07 Sunshine configuration and lifecycle integration

## Status

**PASS** — NXBT output is selectable without changing the default virtual-gamepad behavior, and the retained gamepad
lifecycle routes pause, resume, termination, transport reconnect, and shutdown through the Bridge contract.

## Configuration

The input configuration and Linux Web UI now expose:

```text
controller_output = virtual | nxbt | both
nxbt_socket = /run/nxbt-bridge/control.sock
nxbt_controller_slot = 0..15
nxbt_face_buttons = labels | positions
nxbt_trigger_press_threshold = 0..255
nxbt_trigger_release_threshold = 0..255
nxbt_watchdog_timeout = 50..1000
```

The release threshold must be lower than the press threshold, the socket path must be absolute, and NXBT selection is
rejected outside Linux. Invalid values retain the last valid value and emit a specific configuration error.

`controller_output` defaults to `virtual`. The existing `gamepad` option still configures only the host virtual-HID
profile, so `gamepad = switch` has not been redefined to activate NXBT.

Only the English locale was changed. Configuration documentation and the Web UI defaults use the same option order and
values checked by `ConfigConsistencyTest`.

## Runtime lifecycle

- `virtual` creates only the existing virtual-HID sink.
- `nxbt` creates only a non-blocking reconnecting Bridge client and a fixed-slot NXBT sink.
- `both` allocates virtual-HID first and NXBT second, with reverse rollback if NXBT allocation fails.
- Pause neutralizes the retained controller without detaching it.
- Resume rebinds the retained slot and preserves the latest-state-wins transport behavior.
- Application/session termination sends neutralize and detach; process shutdown destroys the router before the platform
  input backend.
- A socket or heartbeat failure does not erase Sunshine's desired attachment. Reconnection negotiates protocol version
  1, reattaches, neutralizes first, and then submits only the latest retained state.

## Diagnostics and logging

The client exposes a synchronized snapshot containing the socket endpoint, negotiated protocol version, connection and
heartbeat health, per-slot controller status, and last transport/protocol error. Runtime logs use structured event,
slot, status, protocol, and numeric error fields. They never include Bluetooth addresses, pairing keys, or credentials.
Repeated failure categories remain rate-limited.

## Automated evidence

- `NxbtConfigTest` covers defaults, every supported value, invalid ranges, invalid paths, invalid policies, hysteresis,
  and watchdog limits.
- NXBT mapping/sink tests cover configurable thresholds, fixed Bridge slot selection, one-controller enforcement,
  neutralize/rebind/free, and diagnostic status.
- `InputGamepadSessionTest` preserves virtual-only pause/resume/termination behavior.
- `ConfigConsistencyTest` verifies C++, Web UI, English locale, and documentation consistency.
- RKMPP release and Web UI builds compile the configured production path.
