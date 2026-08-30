# Xbox Remote Play Step 07 Validation

> Archived with [Feature 005](../005_xbox_remote_play.md).

## Execution disposition

`SKIPPED BY OWNER ON 2026-08-30` — the owner explicitly directed implementation to continue with Step 08 without waiting for this step's 30-minute stability gate. The validation result below remains `BLOCKED`; it has not been converted to `PASS`, and no missing soak or deployment decision is being represented as completed. Step 08 therefore uses a transport-neutral in-process sink/session boundary and does not depend on a final sidecar-versus-built-in decision.

## Status

`BLOCKED (30-minute stability window not available)` — the corrected Step 06 protocol-delivery gate is `PASS`, and the probe now has a neutral-only soak mode. A short real-console soak reached the ready state and exercised the input channel, but the post-fix repeat and the required 30-minute run are blocked by recurring Xbox REST and ICE/STUN route failures. The sidecar-versus-built-in decision remains pending.

## Corrected validation boundary

The owner identified that timed probe actions do not provide enough attribution to judge detailed controller mapping. Step 07 therefore measures transport, keepalive, neutral input delivery, RTP consumption, resource stability, cancellation, and cleanup only. Stick signs, trigger range, vibration, and reconnect behavior are deferred until a Sunshine sink can be exercised through Moonlight.

## Implemented increment

- Added `xbox-remote-probe soak-check <token-file> <console-number> [hold-seconds]`.
- The default hold is 1,800 seconds, bounded to 3,600 seconds.
- The command runs WakeUp, Home session provisioning, SDP/ICE, all four data channels, and the startup handshake.
- During the hold it sends only a complete neutral gamepad state at an approximately 16 ms cadence; it sends no visible buttons, axes, or triggers.
- It continues Home keepalive, consumes inbound data-channel messages plus undecoded audio/video RTP, samples RSS/thread/file-descriptor high-water marks, and emits five-minute progress checkpoints.
- Normal completion closes the peer, deletes the Home session, waits for libdatachannel global cleanup, and verifies resources against the pre-session baseline.

## Automated evidence

- Required `./scripts/build-rkmpp.sh`: exit 0; `sunshine`, `test_sunshine`, and `xbox-remote-probe` built.
- `XboxRemote*`: 67/67 pass across nine suites after the new command was added.
- Project LLVM 22 formatting and `git diff --check`: exit 0.

## Short live evidence and cleanup diagnosis

The first 15-second neutral soak reached ready, kept all four data channels open, consumed 9,140 video and 1,060 audio RTP packets, and held RSS, threads, and file descriptors flat. Its immediate post-cleanup assertion observed threads `1 -> 2` and conservatively failed.

A debugger-only repeat identified the extra count as a cleanup timing race rather than a persistent leak: RTC workers, SCTP iterator/timer, and ICE poll threads exited; a transient final worker appeared during teardown and then exited. The process returned to one thread and four file descriptors, sent 918 neutral packets during 15 seconds, and exited normally. The probe now allows a bounded two-second resource-quiescence window before asserting the final baseline; growth beyond that window still fails.

## Current live blocker

After the cleanup-sampling correction was built:

1. Two bounded repeats failed before the soak at local SDP/ICE gathering timeout.
2. Runtime inspection showed that the scoped Xbox route had selected a member without ordinary UDP support, while STUN traffic used the same group.
3. Refreshing the XCCS and XHome group checks restored an alive UDP-capable selection, but the next bounded repeat failed earlier at Home `create_session` with a sanitized Xbox REST network failure.
4. No probe or debugger process remains.

Provider capability inspection explains why an HTTPS-only fallback can alternate between REST success and ICE failure: some members report ordinary UDP support while others provide only separate tunneling modes. When the health check selects a member without ordinary UDP support, the group can be TCP-healthy while the probe's STUN path cannot gather. The minimal operator-side correction is to restrict the scoped Microsoft/STUN route to members with ordinary UDP support.

These failures do not demonstrate a soak, input-channel, or cleanup defect. They show that the current route cannot yet provide the uninterrupted window required for a defensible 30-minute result.

## Remaining Step 07 work

- Obtain a stable Xbox REST plus UDP/STUN route and rerun the post-fix 15-second smoke test.
- Run the full 1,800-second neutral soak and record every five-minute checkpoint plus final cleanup.
- Add the reusable cancellable lifecycle state machine required by the plan.
- Compare the measured built-in dependency/size impact against the 2.7 MiB standalone probe and record the final sidecar-versus-built-in decision.
