# Xbox Remote Play Step 05 Validation

> Archived with [Feature 005](../005_xbox_remote_play.md).

## Status

`PASS` — the deterministic startup coordinator is connected to the real WebRTC data channels, a real Xbox accepted the complete startup sequence, and the initialized session held for 300 seconds without gamepad reports or protocol timeout.

Sunshine HEAD: `a8e3c460077d06484798e6744ce623b599d5d70a`

LunarNX HEAD: `5b3490deb5113714b52b3147bf11b24318b62359`

XStreaming HEAD: `9d7649d477b71a674ab72842e950e977e60baedb`

## Implemented scope

- `src/xbox_remote/startup.h` and `startup.cpp`: single-owner, non-blocking startup state machine with injectable monotonic time and abstract channel sender.
- Exact order: message Handshake; validated `messageV1` HandshakeAck; control authorization; `gamepadChanged: false`; 500 ms cancellable delay; `gamepadChanged: true`; six ordered capability messages; binary ClientMetadata on input.
- Explicit 5-second Ack deadline, cancellation, invalid-version, required-channel-close, text/binary-send-failure, invalid-lifecycle, and duplicate-Ack handling.
- Duplicate valid Ack after the first transition cannot repeat authorization, gamepad add, capability, or metadata sends.
- Errors contain only fixed stage and diagnostic text and never echo an inbound payload.
- `transport::peer_t` provides bounded in-memory inbound message delivery plus text/binary sends without logging channel payloads.
- The live `startup-check` command drains inbound messages, runs the coordinator, verifies transport health during the hold, and deterministically closes both peer and Home session.
- `control`, `input`, and `message` remain mandatory. `chat` closure is non-terminal after the real console opened all four channels, matching the Step 04 live trace and the reference clients' passthrough treatment.

Modified or added files:

- `src/xbox_remote/startup.h`
- `src/xbox_remote/startup.cpp`
- `src/xbox_remote/transport.h`
- `src/xbox_remote/transport.cpp`
- `src/xbox_remote/probe_main.cpp`
- `tests/unit/test_xbox_remote_startup.cpp`
- `tests/unit/test_xbox_remote_transport.cpp`
- `cmake/compile_definitions/common.cmake`
- `cmake/targets/common.cmake`
- `docs/rkmpp/feature/xbox-remote-play-validation/step-05.md`

## Automated evidence

- Required `./scripts/build-rkmpp.sh`: exit 0; rebuilt `sunshine`, `test_sunshine`, and `xbox-remote-probe` from `build-rkmpp-review` after the live-channel integration.
- Final clean `test_sunshine --gtest_filter='XboxRemote*'`: 62/62 pass across nine suites after deleting only 115 generated stale build-tree `.gcda` files; the five startup tests pass.
- Earlier regression `test_sunshine --gtest_filter='-*DownloadFileTest*'`: 678 run, 665 pass, 13 environment/platform skips, 0 failures.
- Earlier clean `llvm-cov gcov` line coverage: `startup.cpp` 87/87 and `startup.h` 7/7, both 100%.
- Project LLVM 22 `clang-format --dry-run --Werror`: exit 0 for all files touched by the live integration.
- `git diff --check`: exit 0.
- `ldd` resolves `libc++.so.1`, `libc++abi.so.1`, and `libunwind.so.1` from the bundled LLVM 22 target-triple directory.

## 2026-08-30 real-console evidence

Command: bounded `startup-check` against the selected Home console with a 300-second hold and the existing owner-only token store. No token, SDP, ICE candidate, address, identifier, or channel payload was printed or persisted.

- Generated the Xbox-compatible offer and gathered three local ICE candidates.
- Peer and ICE connected; all four negotiated channels opened at least once.
- Received and validated the real `messageV1` HandshakeAck.
- Sent authorization, `gamepadChanged: false`, waited at least 500 ms, sent `gamepadChanged: true`, sent all six capability messages, then sent ClientMetadata.
- Sent no gamepad input reports during startup or the 300-second hold.
- Consumed RTP without decoders: 24,179 video packets and 2,612 audio packets.
- Hold resources: RSS 15,444 -> 15,556 KiB (peak 15,556); threads 12 -> 11 (peak 12); file descriptors 7 -> 4 (peak 7).
- After deterministic peer close, Home session DELETE, and libdatachannel cleanup: threads 1 -> 1 and file descriptors 4 -> 4. The command exited 0.

The previously validated scoped Microsoft/STUN route remained unchanged during this validation.

## Gate result

Step 05 is `PASS`. The real console accepted the exact startup sequence and remained initialized for the required five-minute hold with clean teardown.

Next gate: Step 06 must validate neutral input, axis signs, triggers, button press/release behavior, Guide handling, disconnect/reconnect neutralization, and vibration values against the real console. Those user-visible effects must not be marked as passed without observation.
