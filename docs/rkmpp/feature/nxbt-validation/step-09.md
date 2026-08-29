# NXBT validation — step 09 single-controller hardware acceptance

## Status

**REAL-HARDWARE FUNCTIONAL PASS; STRICT SOAK/RECOVERY GATE INCOMPLETE** — the target completed a real
Moonlight → Sunshine → Bridge → Nintendo Switch run. The user confirmed that the streamed picture came from the
Switch and that controller input worked. A later reboot-and-reconnect run also completed automatically from the
normal Switch HOME screen without entering Change Grip/Order or pressing A.

After installation on 2026-08-29, the read-only preflight passed: `bluetooth.service` is active, BlueZ exposes the
approved adapter, both Python imports work, and the effective command contains exactly `--compat --noplugin=input`.
The enabled production Bridge is active, its restricted socket has the expected ownership and mode, and an authorized
client completed a protocol handshake. The user explicitly chose to omit the 30-minute soak. The complete disruptive
recovery and watchdog matrices were not run, so this document does not claim the plan's strict stage 9 acceptance.

## Real-hardware evidence

- Initial real-hardware pairing succeeded. The user confirmed both Nintendo Switch video and working controller input.
- After reboot, the first retest accidentally launched a stale binary from an unrelated prior build. The production
  binary was then rebuilt with the required `./scripts/build-rkmpp.sh` and verified to contain the NXBT
  prewarm path before the successful run below.
- A new streaming session began at `14:16:28.382`; NXBT prewarm ran at `14:16:28.383`.
- The stored-device path reported `reconnecting` at `14:16:28.489` and `connected` at `14:16:29.391`.
- Stream-session start to controller connection was approximately 1.009 seconds; reconnecting status to connected was
  approximately 0.902 seconds. These are software lifecycle timings, not button-to-pixel latency measurements.
- The Switch remained on its normal HOME screen and connected without Change Grip/Order or an A-button prompt.
- `nxbt-bridge.service` remained active with zero recorded restarts during this run.
- Bluetooth addresses and network identifiers are omitted from the evidence.

## Required configuration

```text
controller_output = nxbt
nxbt_socket = /run/nxbt-bridge/control.sock
nxbt_controller_slot = 0
nxbt_face_buttons = labels
nxbt_trigger_press_threshold = 64
nxbt_trigger_release_threshold = 48
nxbt_watchdog_timeout = 150
```

`gamepad = switch` is not required for NXBT and remains the separate host virtual-gamepad profile.

## Functional matrix

Record PASS/FAIL for all of the following on the Switch input test screen or a deterministic game:

- A/B/X/Y under both `labels` and `positions` mapping policies.
- D-pad, PLUS, MINUS, HOME, CAPTURE, L, R, L3, and R3.
- ZL/ZR press and release around configured hysteresis boundaries.
- Both sticks at center, cardinals, diagonals, full travel, and slow circles.
- Moonlight controller arrival/removal from each supported client type available to the tester.

## Lifecycle and recovery matrix

- First pairing and subsequent reconnect without re-pairing.
- Moonlight disconnect/reconnect and Sunshine pause/resume.
- Sunshine normal exit and forced kill.
- Bridge restart and forced kill followed by bounded systemd recovery.
- Bluetooth adapter off/on recovery and Switch sleep/wake.
- Verify BlueZ PID stays unchanged for all Bridge-only lifecycle operations.

## Watchdog safety matrix

Hold each of A, right stick direction, ZL, and ZR, then separately interrupt Moonlight, Sunshine, the Unix socket,
Bridge, Bluetooth, and Switch connectivity. Every case must visibly return to neutral within the configured bound plus
measured transport variance, with no stuck input after recovery.

## Latency and soak

- Measure Moonlight input arrival to Bridge submission and visible Switch reaction separately where instrumentation
  permits; record median, p95, maximum, and sample count without claiming end-to-end precision from timestamps that do
  not share a clock.
- Run `scripts/nxbt-hardware-validation.sh <evidence-directory> 1800` during a 30-minute mixed-input session.
- Save its address-redacted service/Sunshine logs, service PIDs, socket ownership/mode, latency notes, client versions,
  adapter path, kernel, BlueZ, Python, and NXBT commit.

## Strict acceptance

Stage 9 passes only when every functional, lifecycle, watchdog, permission, and 30-minute soak item passes on real
hardware. A fake backend, partial matrix, unexplained reconnect, stuck input, service crash, wildcard BlueZ plugin
disable, or missing sanitized evidence keeps this stage incomplete. The 2026-08-29 run is therefore recorded as a
functional hardware pass and fast-reconnect pass, but not a strict full-stage pass because the user skipped the
30-minute soak and the full recovery/watchdog matrix remains incomplete.
