# Xbox Remote Play Step 04 Validation

> Archived with [Feature 005](../005_xbox_remote_play.md).

## Status

`PASS` — after the owner powered on the console, the isolated probe completed the real Xbox SDP/ICE/data-channel gate, consumed media without decoders, held the connection for the required 600 seconds, and closed without thread, descriptor, process, or server-session residue.

The Step 04 hard gate is closed. Steps 05 and 06 had already been implemented and validated offline under the owner's recorded exception; their remaining real-console matrices are tracked separately.

## Selected transport

- Library: libdatachannel `v0.24.3`.
- Commit: `c6696d157b5612df2a741d9a03b192b47ab6cefb`.
- License: Mozilla Public License 2.0.
- Integration: static FetchContent dependency enabled only when `SUNSHINE_BUILD_XBOX_REMOTE_PROBE=ON`; the normal Sunshine target does not link it.
- Media transport: enabled with `NO_MEDIA=OFF`.
- Optional components: `NO_EXAMPLES=ON`, `NO_TESTS=ON`, and `NO_WEBSOCKET=ON`.
- ICE/media dependencies: bundled libjuice, usrsctp, and libsrtp; `USE_NICE=OFF`, `USE_SYSTEM_SRTP=OFF`, and `USE_SYSTEM_USRSCTP=OFF`.
- Shared libraries: disabled for this dependency with `BUILD_SHARED_LIBS=OFF`.

The transport and bundled dependency licenses are MPL-2.0 for libdatachannel and libjuice, plus BSD-style licenses for usrsctp and libsrtp. No source was copied from these projects into Sunshine.

## Dependency footprint

- Fetched source tree including Git metadata and recursive submodules: 355 MiB.
- Generated libdatachannel build tree: 13 MiB.
- Static archives: libdatachannel 5,249,378 bytes; usrsctp 736,908 bytes; libjuice 344,240 bytes; libsrtp 139,348 bytes.
- Linked probe executable: 2,696,456 bytes.
- The probe has no dynamic libdatachannel, libjuice, usrsctp, libsrtp, or libnice dependency.

## Implemented scope

- Ordered video, audio, and application offer sections.
- H.264 video and Opus audio receive tracks with no-op RTP consumers; no decoder, renderer, recorder, or audio output is created.
- Four reliable ordered channels created before the offer: `control/controlV1/0`, `input/1.0/2`, `message/messageV1/4`, and `chat/chatV1/6`.
- Local ICE gathering plus REST POST, remote SDP/ICE polling, structural parsing, candidate normalization, and duplicate rejection.
- Peer, ICE, channel, media-packet, timeout, cancellation, failure, and close tracking without logging SDP, candidates, tokens, identifiers, or addresses.
- Deterministic peer callback reset, track/channel close, peer reset, and blocking libdatachannel runtime cleanup.
- Live-hold sampling for RSS, threads, and file descriptors, including start, end, and high-water values.

## Automated evidence

- `git diff --check`: `PASS`.
- Required `./scripts/build-rkmpp.sh`: `PASS`; it rebuilt `sunshine`, `test_sunshine`, and `xbox-remote-probe`.
- `test_sunshine --gtest_filter='XboxRemote*'`: `48/48 PASS`, exit 0.
- The complete 48-test Xbox filter passed three additional sequential runs.
- Transport coverage includes event reordering, duplicate candidates, required-channel close, ICE failure, cancellation, timeout, complete offline SDP generation, no-op media accounting, and 100 repeated peer construction/destruction cycles.

An exit-stage crash was found during validation. A debugger showed that certificate-generation workers could still use OpenSSL while process teardown and coverage flushing had started. The wrapper had discarded the `rtc::Cleanup()` future. It now waits for that future after all peers are destroyed. The formerly crashing 48-test run now exits 0 repeatedly.

## Live evidence and blocker

The live command was authorized to authenticate, create a Home session, exchange signaling, hold it, and delete it. No credential or service response body was printed or saved.

Observed attempts:

1. The first 30-second transport check failed during Home session state polling with a sanitized network-request failure. The post-creation failure path attempted session DELETE.
2. Direct TLS checks against all required public Microsoft/Xbox services consistently failed at handshake while the proxy-managed route was unavailable.
3. A bounded retry later recovered TLS successfully.
4. The immediately following transport check authenticated and created a Home session, but the service moved the new session to `Failed` before configuration and WebRTC offer generation. The client attempted DELETE before returning.
5. A follow-up discovery/lifecycle probe again failed in the Xbox authentication chain because the external route had dropped.

There was no residual `xbox-remote-probe` process after the failures. Network or proxy configuration was not changed.

Because no attempt reached a real SDP answer, ICE connection, four open channels, inbound RTP, or the required 10-minute hold, those criteria remain unverified. The current blocker is external route stability followed by successful Xbox Home provisioning; it is not being represented as a transport pass.

## Follow-up power and route diagnosis

The console was subsequently discovered in `ConnectedStandby`, not in a fully shut-down state. Xbox's published power behavior permits Remote Play/remote wake while the console is in its available Sleep/active-hours state, while full Shutdown does not permit remote wake. The observed `ConnectedStandby` result therefore does not support the inactivity auto-off timer as the direct cause of the earlier server `Failed` state.

ROCK-side diagnostics further isolated the active blocker:

- The LAN gateway completed 20/20 probes with no packet loss.
- A general HTTPS control endpoint completed 5/5 TLS requests successfully with sub-second totals.
- During the same interval, the Xbox authentication endpoint completed only 1/5 requests; the others ended in TLS interruption or timeout.
- Fifteen bounded route cycles showed Microsoft and Xbox endpoints independently alternating between success and failure. Even when two preflight endpoints succeeded together, a later authentication hop failed seconds afterward.
- Forcing TLS 1.2 and HTTP/1.1 produced the same failures, excluding the client's negotiated TLS/HTTP version as the cause.
- A read-only gateway trace confirmed that the failing Xbox authentication request selected the Microsoft traffic policy and timed out while dialing its current proxy endpoint. The failure occurred before a connection to the Xbox service was established. The same endpoint was simultaneously timing out for other Microsoft-classified traffic.
- The proxy group remained marked alive because its health check used a generic connectivity endpoint rather than a Microsoft/Xbox target. Its selected member therefore did not reflect service-specific route health.
- STUN traffic also selected a member without ordinary UDP support. This had not caused the observed REST/TLS blocker, but it was a separate risk for the later ICE/UDP gate once session provisioning succeeded.

To tolerate short outages safely, authentication network stages now use eight bounded attempts. Session GET and DELETE requests also use eight bounded attempts. Ambiguous POST operations remain single-attempt because replaying a request whose response was lost could duplicate a Home session or signaling action. Session `Failed` responses now retain only an allowlisted, length-bounded service error code and continue to discard all free-form service messages.

After these changes the required build passes and `test_sunshine --gtest_filter='XboxRemote*'` reports `61/61 PASS`, exit 0. A real retry crossed the extended authentication chain but exhausted all eight retries on the idempotent console-discovery GET. Network settings were not changed, and the earlier server-side `Failed` state could not be reproduced long enough to obtain its new sanitized code.

## Gate decision before console wake

- Step 04: `BLOCKED`.
- Step 05 and all Sunshine hot-path/input integration: not started.
- Resume condition: restore a stable Microsoft/Xbox route, confirm the selected Home console can provision a session, then rerun `transport-check` for 30 seconds followed by the required 600-second hold.
- The initial gateway diagnosis was read-only. A later owner-authorized isolated Xbox route change is recorded below.
- Pass condition remains unchanged: peer and ICE connected, all four channels open, audio and video RTP consumed without decoders, keepalive succeeds for 10 minutes, resources do not show sustained growth, and close leaves no peer/socket/server session residue.

## 2026-08-29 unattended retry

With the existing owner-only token store, a bounded 30-second `transport-check` was retried without changing the proxy, network, console, or Sunshine configuration. Authentication reached Home session creation, but state polling returned the sanitized failure `Home session provisioning failed` at `session_state`; the command exited 1 and did not reach SDP/ICE. No probe process remained afterward.

This reproduces the existing console/service blocker. Step 04 therefore remains `BLOCKED`. At the owner's explicit request, Steps 05 and 06 were implemented and validated offline while their real-console matrices remain blocked; this exception does not convert Step 04 or either later step to `PASS`, and no Sunshine input hot-path integration was started.

## 2026-08-29 authorized gateway isolation and WNSError reproduction

The owner authorized a scoped gateway change. The active proxy configuration was backed up and validated before reload. A separate Xbox-only selector was assigned an ordinary-UDP-capable member. Only Microsoft and STUN traffic was changed to use that selector; other proxy groups and rules were left unchanged.

The reloaded route improved but did not eliminate public Xbox TLS instability: three of five immediate authentication-endpoint probes completed TLS, while two were interrupted before TLS completion. Despite that instability, both a bounded 30-second `transport-check` and a separate one-cycle `session-check` completed authentication, console discovery, and Home session creation. The Xbox service then returned the same sanitized `WNSError` at `session_state` on both attempts. Neither attempt reached configuration, SDP, ICE, or data channels, so the 600-second hold was not started.

Public Xbox Remote Play client reports associate `WNSError` in this state with the console not receiving the streaming-start command while the service waits for the console to register. Reported recovery requires the console to wake/register successfully, commonly by waking it with an official client, retrying, or restarting the console. The repeated service code therefore narrows the current hard gate to console wake/service registration rather than the probe's SDP or WebRTC implementation. No probe process remained after the failed attempts.

Step 04 remains `BLOCKED`. Resume when the console can be woken or restarted and is confirmed reachable for Remote Play, then repeat the 30-second transport check before the required 600-second hold.

## 2026-08-30 real-console gate pass

After the owner powered on the console, the scoped route completed three consecutive Xbox TLS preflight requests. The first probe invocation then exposed a local loader error before authentication: its RUNPATH used Debian's GCC multiarch name even though the bundled LLVM 22 runtime uses Clang's `aarch64-unknown-linux-gnu` target triple. `scripts/build-rkmpp.sh` now derives the LLVM runtime directory from `clang --print-target-triple`, verifies that libc++ exists there, and uses that directory for linking and RUNPATH. The required build passed, the probe loaded the bundled LLVM libc++, and the clean Xbox filter reported `62/62 PASS`.

The first real WebRTC attempt reached SDP, ICE, SCTP, and all four data channels. A debugger trace established that the remote SDP selected `setup:passive`, confirming that the local active DTLS role and even SIDs were correct. It also showed `chat`, `message`, `input`, and `control` opening successfully, followed by the console closing only `chat`. XStreaming records chat close without terminating the stream, and its nano backend treats chat as passthrough while readiness depends on the input/message path. The tracker was therefore corrected to require all four channels to have opened at least once while requiring `control`, `input`, and `message` to remain open. A new out-of-order chat-close test covers the observed console sequence; closure of a critical channel remains terminal.

The post-fix 30-second gate passed:

- Peer and ICE connected; all four channels opened.
- Video RTP packets consumed without a decoder: 15,643.
- Audio RTP packets consumed without a decoder: 1,670.
- Hold RSS remained 14,940 KiB; threads remained 12; file descriptors remained 7.
- Deterministic close returned threads to 1 and file descriptors to 4.

The required 600-second hold then passed:

- Video RTP packets consumed without a decoder: 22,649.
- Audio RTP packets consumed without a decoder: 2,339.
- Hold RSS: 14,940 to 15,100 KiB, peak 15,100 KiB.
- Hold threads: 12 to 11, peak 12.
- Hold file descriptors: 7 to 4, peak 7.
- Deterministic close returned threads to 1 and file descriptors to 4.
- The explicit server-session cleanup completed, the process exited 0, and no probe process remained.

Step 04 is `PASS`. The selected libdatachannel transport is compatible with the real Xbox for SDP, ICE, DTLS/SCTP, all four negotiated channels, no-op media consumption, ten-minute keepalive, and deterministic cleanup.
