# Xbox Remote Play Step 10 Validation

> Archived with [Feature 005](../005_xbox_remote_play.md).

## Status

`PASS (AUTOMATED FAULT-RECOVERY GATE; LIVE FAULT TESTS SKIPPED)` — bounded reconnect, recovery classification, session epochs, input watchdog neutralization, cancellable waits/transfers, and deterministic cleanup are implemented. Per the owner's instruction, no potentially long hardware or soak test was run. This result does not replace Step 12's real Moonlight/Xbox fault acceptance.

## Implemented recovery policy

- Retryable failures use a fresh connection object and monotonically increasing epoch with exponential delays of 250 ms, 500 ms, 1 s, capped at 4 s, and at most three retries after the initial attempt.
- HTTP requests retain their 15-second hard deadline and can now abort an in-flight libcurl transfer through the worker cancellation callback. SDP polling, ICE polling, offer gathering, transport readiness, provisioning, WakeUp, startup acknowledgement, and keepalive retain their existing individual deadlines.
- Authentication rejection stops with `reauthentication_required`; invalid configuration, console selection, SDP/ICE input, and startup state stop with `permanent`; network, timeout, rate-limit, Xbox unavailable, peer/channel, send, and keepalive failures are retryable.
- Each new peer has a new epoch. Delayed progress callbacks from an old epoch are ignored, and feedback is delivered only for the active epoch. Destruction of the old connection before creating the next prevents old-peer vibration from entering the new session.
- Reconnect first queues a neutral state and clears controls and the digital transition journal. Once ready, the queue emits only the most recent absolute state received after that boundary; an old press is not replayed.
- A two-second input watchdog queues a neutral state if Moonlight stops submitting while any control is active.
- Every worker-owned connection exit attempts neutral input, gamepad removal, peer close, and Home session deletion. DELETE gets an independent two-second cancellation window even after shutdown cancellation; a failed DELETE is surfaced as the sanitized `delete_unconfirmed` cleanup stage. Cleanup is idempotent, including libdatachannel global teardown.
- The authenticated status endpoint and Input page expose only state, stage, failure stage, recovery kind, and epoch. They expose no account, credential, console, SDP, ICE, or session identifier.

## Automated fault matrix

| Injected condition | Expected recovery/state | Automated result | Neutral and cleanup evidence |
| --- | --- | --- | --- |
| Xbox unavailable / failed provisioning | Retry within the bounded worker budget | PASS: Home `Failed` and WakeUp timeout paths are categorized without response-body leakage | Every post-create failure attempts DELETE; worker finalizes neutral/remove/close |
| HTTP 401 | Refresh exactly once; terminal rejection requires reauthentication | PASS: refreshed request succeeds; failed refresh maps to `reauthentication_required` and does not loop | Worker cleanup runs before terminal `failed` |
| HTTP 429 | Retryable | PASS: REST layer reports `rate_limited`; policy maps it to retryable | Retry boundary neutralizes and clears the edge journal |
| HTTP timeout / network stall | Hard deadline plus retryable classification; shutdown cancellable | PASS: auth/session timeout tests and production pre-transfer cancellation pass; blocking worker open stops in about 1 ms in the test | Cancellation exits through the common cleanup path |
| ICE failed | Retryable peer failure | PASS: transport tracker reports `peer_failed` | Common worker cleanup and new epoch on retry |
| DTLS / peer connection failed | Retryable peer failure | PASS: failed peer state reports `peer` / `peer_failed` | Common worker cleanup and new epoch on retry |
| Required data channel closed | Retryable transport health failure | PASS: critical `input` channel close fails readiness; optional post-open `chat` close remains accepted | Common worker cleanup and new epoch on retry |
| Keepalive failed | Retryable up to three retries | PASS: REST network failure and worker retry-budget exhaustion create exactly three total connection epochs when configured for two retries | Three connections are each closed; terminal queue is neutral |
| Malformed vibration packet | Ignore packet without exposing payload or failing the session | PASS: strict vibration parser rejects malformed size/type/version/value fixtures | No cleanup is required; no vibration callback is emitted |
| Old peer progress callback | Ignore callback from a previous epoch | PASS: epoch 1 callback cannot change epoch 2 `ready` status | Both connection epochs close exactly once |
| Moonlight input silence while pressed | Neutralize after watchdog deadline | PASS: a real Xbox A-button state is followed by an automatic neutral operation in 12 ms with a 10 ms test deadline | Worker remains ready and later performs normal cleanup |
| Permanent configuration failure | Stop without consuming retry budget | PASS: exactly one connection is created and closed | Neutral/remove/close attempted once |
| Home DELETE failure | Record cleanup as unconfirmed without blocking reconnect/stop | PASS: injected `delete_unconfirmed` cleanup result does not prevent epoch 2 readiness or final close | Warning contains only fixed stage and recovery kind |

## Automated evidence

- Required `./scripts/build-rkmpp.sh`: final exit 0; `sunshine`, `test_sunshine`, and `xbox-remote-probe` built after the cancellation increment.
- Final focused recovery/protocol/auth/REST/transport/status rerun after cleanup-deadline hardening: 73/73 tests passed across 11 suites in 210 ms.
- Worker-specific recovery run within that gate: 9/9 passed in 33 ms.
- Project LLVM 22 formatting and `git diff --check`: pass after the increment.
- A stale `.gcda` merge warning was isolated from test execution by directing the focused run to a fresh temporary coverage prefix; it did not affect test results. A new changed-code coverage report is deferred to Step 11.

## Deferred evidence

- The real fault sequence (Moonlight disconnect, ROCK network removal, Sunshine stop, Xbox Remote Play session close, then network recovery) was not executed because it requires live hardware and external services.
- The Step 07 30-minute stability window and Step 12 two-hour soak remain explicitly skipped/deferred rather than passed.
- Full regression, Doxygen, changed-code coverage, Windows/MSYS2, and hardware resource-leak measurements remain Step 11/12 gates. The immediately preceding Step 09 short regression remains 704 tests with zero failures.
