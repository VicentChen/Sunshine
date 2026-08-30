# Xbox Remote Play Step 06 Validation

> Archived with [Feature 005](../005_xbox_remote_play.md).

## Status

`PASS (protocol delivery and cleanup gate; user-visible mapping deferred to Sunshine/Moonlight)` — Step 04 transport, Step 05 startup, and the explicit XCCS WakeUp/HDMI gate pass on the real console. The complete scripted input matrix was delivered successfully, the owner confirmed normal button input had no problem, the final neutral state was sent, and cleanup completed without resource residue. The plan no longer treats an unobservable timed probe as the acceptance interface for stick signs, trigger range, non-zero vibration, or reconnect behavior; those items move to the integrated Moonlight validation.

Sunshine HEAD: `a8e3c460077d06484798e6744ce623b599d5d70a`

LunarNX HEAD: `5b3490deb5113714b52b3147bf11b24318b62359`

XStreaming HEAD: `9d7649d477b71a674ab72842e950e977e60baedb`

## Implemented scope

- `src/xbox_remote/input_queue.h` and `input_queue.cpp`: non-blocking, single-owner bounded scheduling layer.
- One latest-state slot for analog updates; analog-only submissions overwrite in constant bounded space.
- A 64-entry digital press/release journal with a 50 ms lifetime. Ordinary analog updates cannot overwrite retained digital edges.
- Separate eight-entry priority control queue for attach, neutralize, and detach. Neutralize/detach clear stale edges; capacity failure is explicit and non-blocking.
- Overflow drops the oldest digital edge, increments a diagnostic counter, and retains the current absolute state for eventual convergence.
- Reconnect clears control and edge history and schedules only the current absolute state; old presses are not replayed.
- Sender-thread-owned packetizer assigns continuous sequence and monotonic timestamp only at encode/send time and has tested `uint32` wrap/reset behavior.
- Trigger expansion is exact `uint8 * 257`; physical activity is derived for supported buttons, D-pad, shoulders, thumbs, triggers, and both stick axes.
- Existing protocol code continues to byte-encode 38-byte snapshots and parse bounded, validated four-motor vibration reports.
- `xbox-remote-probe input-check` performs the real startup handshake, repeats complete absolute states at a 16 ms cadence, inserts explicit neutral-release phases, monitors transport/cleanup, and parses valid four-motor feedback without storing raw packets.
- `xbox-remote-probe wake-check` sends the separate XCCS `Power/WakeUp` command for `ConnectedStandby`, polls to `On`, skips the command when already `On`, and never replays an ambiguous POST response.
- Every live transport/startup/input command now runs the same wake gate before creating a Home session. The HDMI RX lock remains an external hardware assertion after `wake-check` reports `On`.
- Fixed cases cover neutral, A, D-pad Up, both signed directions for all four stick axes, LT, RT, the complete ordered matrix, and a 60-second vibration observation mode.
- Stick cases are deliberately named by wire sign. No Y-axis inversion is committed until a user observes the Xbox result.

Modified or added files:

- `src/xbox_remote/input_queue.h`
- `src/xbox_remote/input_queue.cpp`
- `src/xbox_remote/session.h`
- `src/xbox_remote/session.cpp`
- `src/xbox_remote/probe_main.cpp`
- `tests/unit/test_xbox_remote_input_queue.cpp`
- `tests/unit/test_xbox_remote_session.cpp`
- `cmake/compile_definitions/common.cmake`
- `cmake/targets/common.cmake`
- `docs/rkmpp/feature/005_xbox_remote_play.md`
- `docs/rkmpp/feature/xbox-remote-play-validation/step-06.md`

## Automated evidence

- Required `./scripts/build-rkmpp.sh`: exit 0; `sunshine`, `test_sunshine`, and `xbox-remote-probe` all built.
- Final clean Xbox filter after the WakeUp correction: 67/67 pass across nine suites. Seven input-queue tests cover latest-state overwrite, press/release retention and expiry, bounded overflow, control priority, reconnect clearing, sequence wrap/reset, trigger endpoints, and activity masks.
- Existing protocol tests cover exact little-endian gamepad fixture, malformed/truncated/oversized gamepad packets, and valid/invalid vibration packets.
- Regression excluding the known external-network DownloadFile tests: 685 run, 672 pass, 13 environment/platform skips, 0 failures.
- Clean production line coverage: `input_queue.cpp` 146/146 and `input_queue.h` 3/3, both 100%.
- Clean session coverage after the final UUID case: `session.cpp` 99.06% (420/424) and `session.h` 100%. Every new executable WakeUp line is covered; the four uncovered `session.cpp` lines are pre-existing cancellation/function-tail paths outside the WakeUp change.
- Project LLVM 22 `clang-format --dry-run --Werror` and `git diff --check` exit 0.

## 2026-08-30 explicit WakeUp correction

The owner observed that the console remained dark and HDMI RX had no lock while discovery reported `ConnectedStandby`. Reference-client comparison established that Home Remote Play session creation and physical/display wake are separate operations. XStreaming sends `Power/WakeUp` through XCCS before starting the stream when its power-on option is enabled; the original plan incorrectly excluded this operation even though the Sunshine path depends on HDMI rather than Remote Play video.

The existing authentication chain already retained the required web XSTS token and user hash. `session::client_t::wake_and_wait()` now sends the fixed XCCS command envelope, performs at most one known-safe retry after HTTP 401 refresh, treats a lost POST response as ambiguous without replaying it, and waits with injected monotonic time for the selected console to report `On`. The probe generates a fresh command UUID and exposes a bounded `wake-check`; live transport, startup, and input checks use the same gate.

Offline evidence after the correction:

- Required `./scripts/build-rkmpp.sh`: exit 0; rebuilt `sunshine`, `test_sunshine`, and `xbox-remote-probe`.
- `XboxRemoteSessionTest.*`: 17/17 pass, including six wake-focused tests.
- `XboxRemote*`: 67/67 pass across nine suites.
- Project LLVM 22 formatting and `git diff --check`: exit 0.

Historical failed wake attempts before route recovery:

1. The first bounded `wake-check` ended when its outer safety timeout overlapped the internal wake window. A post-check still reported `ConnectedStandby`, and HDMI RX returned no timings lock.
2. A second run with separated time budgets failed during `xsts_web` authentication before any WakeUp command could be sent.
3. Immediate read-only TLS probes to both XSTS and XCCS then timed out. No probe process remained.
4. A final 90-second bounded retry after the completed build failed during `gssv_xhome` authentication before console discovery. HDMI RX still reported `No locks available`, and no probe process remained.

Read-only route diagnosis found that the ROCK sends Xbox endpoints through a proxy-managed route. Microsoft login and Xbox UserAuth completed TLS, but XSTS, xHome, and XCCS showed selective TLS failures or timeouts. Runtime state reported the selected group as alive, but that health check used only a generic connectivity endpoint: an explicit group delay check still timed out for XSTS and XCCS while XHome happened to respond. Therefore the generic alive status does not prove that the complete Xbox authentication/WakeUp path is reachable. No proxy selection or routing configuration was changed.

These failures did not show the console rejecting a valid WakeUp command; the later successful run below confirms they were transient route failures.

## 2026-08-30 live WakeUp PASS

After the scoped Xbox route recovered sufficiently for the required endpoints:

1. `xbox-remote-probe consoles` discovered exactly one Home console, `岁岁游戏机` (`XboxSeriesX`), in `ConnectedStandby`.
2. The bounded `wake-check` completed successfully and the service reported the selected console as `On`.
3. HDMI RX acquired a real timing lock at 1920x1080 progressive, 59.94 frames per second.
4. The owner independently confirmed that the Xbox woke and HDMI output was visible.

This closes the explicit WakeUp and HDMI assertion as `PASS`. It does not yet pass the Step 06 controller input or vibration observations.

## 2026-08-30 live input matrix partial PASS

With the Xbox `On` and HDMI locked, `input-check ... matrix` completed a real Home Remote Play session and sent the complete ordered absolute-state script: neutral, A, D-pad Up, both wire signs of both axes on both sticks, LT, RT, and an explicit neutral release after every active phase.

- Handshake, authorization, attach, capabilities, and ClientMetadata completed.
- All four data channels remained ready; 22,178 video and 2,292 audio RTP packets were consumed without decoders.
- The final neutral state was sent, deterministic cleanup restored thread and file-descriptor counts, and no transport or input-channel failure occurred.
- The owner confirmed that normal button input had no problem.
- One valid all-zero vibration packet was parsed. A separate 60-second observation also produced only one all-zero packet, so non-zero four-motor mapping is still pending rather than failed.

The run proves live delivery and basic button behavior. It does not by itself individually attest stick Y sign, trigger range, vibration motor percentages, or reconnect behavior.

The owner correctly identified that timed probe actions do not provide a reliable attribution interface for detailed mapping. The plan was therefore corrected: this step gates byte-level protocol behavior, real data-channel delivery, final neutral state, and cleanup only. User-visible mapping is verified after the Sunshine sink exists, using a physical controller through Moonlight.

## 2026-08-30 bounded live attempts

The Xbox was on and the owner was available to observe the scripted matrix. Three complete `input-check ... matrix` invocations were bounded and cleaned up locally:

1. Failed at `create_session` with a sanitized Xbox REST network failure.
2. Reached Home session state polling, then the service returned `XccsUnknownError` during provisioning.
3. Generated the Xbox-compatible offer with three local candidates and reached SDP exchange, then failed at `send_ice` with a sanitized Xbox REST network failure.

All three failures occurred before the startup handshake and scripted input phases. No gamepad report was sent, no user-visible mapping claim was made, and no probe process remained. The gateway policy was not changed; Microsoft/STUN traffic remained isolated to its scoped route.

## Deferred Moonlight observation matrix

Current per-item status:

| Item | Status / remaining observation |
| --- | --- |
| Neutral | `PASS`: the matrix ended with an explicit neutral state and the owner reported no normal-input problem. |
| A | `PASS`: delivered live; the owner confirmed normal button input had no problem. |
| D-pad Up | `PASS`: delivered live; the owner confirmed normal button input had no problem. |
| Left stick | `DEFERRED TO MOONLIGHT`: individually attest four screen directions and determine whether Sunshine Y must be inverted. |
| Right stick | `DEFERRED TO MOONLIGHT`: individually attest four screen directions and determine whether Sunshine Y must be inverted. |
| LT / RT | `DEFERRED TO MOONLIGHT`: individually attest 0/hold/release and full-range behavior on Xbox. |
| Ordinary vibration | `DEFERRED TO MOONLIGHT`: confirm non-zero left/right motor percentage and stop behavior. |
| Trigger vibration | `DEFERRED TO MOONLIGHT`: confirm non-zero left/right trigger motor percentage and stop behavior. |
| Reconnect | `DEFERRED TO MOONLIGHT`: confirm only the current absolute state is sent and old edges never replay. |

No assumption about Y-axis sign is committed until the individual direction observations are recorded.

## Gate result and next step

Step 06 is `PASS` under the corrected gate: byte-level protocol tests, queue behavior, real input data-channel delivery, final neutral state, and deterministic cleanup all pass. Detailed stick/trigger, non-zero vibration, and reconnect observations are not Step 06 blockers; they are explicit Sunshine/Moonlight integration acceptance items.

Step 07 may now perform the 30-minute transport/resource run and make the sidecar-versus-built-in decision. After that stability gate passes, Step 08 can implement the Sunshine input sink and expose a valid Moonlight-based mapping test path.
