# Xbox Remote Play Step 08 Validation

> Archived with [Feature 005](../005_xbox_remote_play.md).

## Status

`PASS (AUTOMATED SINK GATE)` — the Sunshine Xbox Remote Play sink, controller mapping, feedback routing, and three-backend router support are implemented and pass their offline tests. The owner explicitly skipped Step 07; this result does not imply that the missing 30-minute soak passed. Moonlight-to-Xbox mapping observations remain pending until Step 09 supplies the production application/session lifecycle.

## Scope

- `src/input/xbox_remote_sink.*` adds a transport-neutral, non-blocking session interface and a first-version single-controller sink.
- `alloc()` registers Xbox gamepad zero and retains the current Moonlight feedback queue without waiting for authentication, REST, ICE, or WebRTC readiness.
- `update()` converts complete Sunshine states and performs one bounded session submission. Button bits, unsigned triggers, signed axes, physical activity, and the separate misc physicality bit are explicit.
- `neutralize()` and `free()` submit priority release operations through the session interface; feedback is unbound before detach so late vibration cannot reach a released Moonlight controller.
- `rebind()` replaces the client-relative feedback destination only after the session accepts the resume notification.
- Parsed ordinary and trigger vibration percentages map to Sunshine's full unsigned 16-bit feedback range, with the current rebound `clientRelativeIndex`.
- `gamepad::router_t` now supports Xbox-only, virtual+Xbox, NXBT+Xbox, and all-three modes while retaining deterministic virtual → NXBT → Xbox allocation/update order and reverse free order.

## Threading and blocking contract

- The input parser never performs HTTP, DNS, disk, timer, or WebRTC waits through this API.
- The future production session implementation owns the bounded latest-state/edge/control queues and its background sender thread.
- Session unready, reconnecting, and failed behavior is represented by the immediate boolean result of `attach/rebind/submit/neutralize`; the sink does not wait for a state transition.
- Vibration delivery copies the active queue binding under a short mutex and raises feedback after releasing that mutex.
- Session exceptions are contained at the sink boundary so one Xbox failure cannot prevent another router sink from receiving input.

## Automated evidence

- Required `./scripts/build-rkmpp.sh`: exit 0; `sunshine`, `test_sunshine`, and `xbox-remote-probe` rebuilt successfully.
- Final clean focused run: 36/36 passed across `XboxRemoteSinkTest`, `GamepadRouterTest`, `XboxRemoteInputQueueTest`, and `XboxRemoteProtocolTest`.
- Tests cover complete mapping, unchanged axis signs, trigger endpoints, one-controller ownership, alloc/rebind/update/neutralize/free ordering, a second-controller rejection, unavailable/failed/throwing session isolation, current feedback queue rebinding, four-motor conversion, invalid/late vibration suppression, and selected router combinations.
- Clean production line coverage: `xbox_remote_sink.cpp` 145/145 and `gamepad_router.cpp` 92/92, both 100%.
- Regression excluding the two existing external-network `DownloadFileTest` cases: 691 run, 678 pass, 13 documented environment/platform skips, 0 failures.
- Project LLVM 22 formatting and `git diff --check`: pass.

## Deferred live evidence

- Left/right stick direction, final Y sign, LT/RT range, ordinary vibration, trigger vibration, and reconnect behavior require the production Step 09 session lifecycle and a physical controller through Moonlight.
- The Step 07 30-minute neutral soak and final sidecar-versus-built-in comparison remain explicitly skipped, not passed.
- Windows/MSYS2, Doxygen, final full regression, and changed-code coverage are Step 11 gates.

## Deployment boundary

The current sink is in-process Sunshine code, while `session_t` is deliberately transport-neutral. Step 09 may bind it to an in-process lifecycle worker or a sidecar IPC adapter without changing the input parser or sink contract. No final deployment-form claim is made in this step.
