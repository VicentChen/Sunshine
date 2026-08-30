# Xbox Remote Play Step 09 Validation

> Archived with [Feature 005](../005_xbox_remote_play.md).

## Status

`PASS (AUTOMATED LIFECYCLE GATE; LIVE VALIDATION SKIPPED; 2026-08-31 AUTO-START UPDATE)` — application-scoped production startup, configuration validation, sanitized status visibility, deterministic stop cleanup, and Web/API consistency are implemented. The later auto-start update makes the built-in Xbox application start Remote Play by default and permits unique-console discovery without a configured ID. No potentially long hardware test was run. This does not replace the skipped Step 07 soak or the Moonlight/Xbox live acceptance required later.

## Implemented lifecycle

- `xbox_remote_enabled` defaults to enabled on the supported build, so launching the built-in `Xbox` application automatically creates the worker. It remains an explicit rollback switch.
- Only the exact `xbox_remote_app` name may create a worker. Nintendo Switch keeps its existing configured virtual/NXBT route and every unrelated application remains disabled for external controller forwarding.
- Application selection starts authentication, discovery, optional WakeUp, Home session provisioning, SDP/ICE, WebRTC readiness, and the startup handshake on one background worker. No network, disk, timer, or WebRTC wait runs on Sunshine's input parser thread.
- Application stop, replacement, and Sunshine input shutdown first disable new routing, then cancel and join the worker. The worker sends a final neutral state and gamepad detach before closing WebRTC and deleting the Home session.
- A repeated launch stops the old worker before creating the replacement. Moonlight pause/resume continues to use the sink's existing bounded rebind path.
- A different Moonlight client now takes ownership of the single Xbox controller route by neutralizing and migrating the controller retained for the previous client without detaching it from the Xbox. Reconnecting the same paired client still uses the fast retained-input path.

## Configuration and first-login workflow

The first version deliberately keeps Microsoft Device Code entry in the isolated compatibility probe so OAuth credentials never pass through the Web UI:

1. Create the intended private token-file parent directory.
2. Run `xbox-remote-probe login <absolute-token-file>` and complete the displayed Microsoft Device Code flow.
3. If the account has multiple Home consoles, run `xbox-remote-probe consoles <absolute-token-file>`. The command prints each Home console's stable ID; treat it as private configuration data and configure the intended one.
4. Save/restart Sunshine, launch the built-in `Xbox` application, and watch the credential-free status card. A single discovered console is selected automatically.

Enabled configuration is rejected unless the application selector is non-empty and the token path is absolute, regular, owner-readable/writable, and inaccessible to group/other users. An empty console selector means unique auto-selection; discovery fails safely if zero or multiple consoles are returned. Unsupported builds/platforms force the feature back to disabled. Token contents are never returned by the configuration status API or written to logs.

## Status and diagnostics

- Authenticated `GET /api/xbox-remote/status` returns exactly `state`, `stage`, `failure_stage`, `failure_kind`, and the non-sensitive connection `epoch`.
- The Input configuration page refreshes this sanitized status every two seconds.
- Connection stages distinguish `authentication`, `discovery`, `wake`, `provisioning`, `signaling_sdp`, `signaling_ice`, `transport`, `handshake`, and `ready`.
- Worker failures retain a fixed stage such as `token_store_load`, authentication substage, Home REST substage, transport/ICE stage, startup stage, `input_send`, or keepalive/poll stage.
- Structured Xbox worker logs contain only fixed state/stage names. OAuth values, console IDs, account identifiers, SDP, and ICE credentials are not emitted.

## Automated evidence

### 2026-08-31 automatic-start update

- Required `./scripts/build-rkmpp.sh`: exit 0; rebuilt `sunshine`, `test_sunshine`, and `xbox-remote-probe` with the automatic-start behavior.
- Focused Xbox, application-lifecycle, and configuration-consistency run: `93/93 PASS` after adding the cross-client controller-takeover regression.
- Tests cover default automatic startup, explicit opt-out, unique-console automatic selection, zero-console and multi-console safe failures, permanent recovery classification for ambiguous selection, repeated application start, and all existing Xbox protocol/session/transport paths.
- `git diff --check` and project LLVM 22 formatting: pass.

### 2026-08-31 live client-switch correction

- Live logs showed the Xbox worker remained `ready` while a second Moonlight client connected, so the delay was not authentication, WakeUp, provisioning, or WebRTC reconnection.
- The old paired client's retained `input_t` still owned the Xbox sink's only logical-controller slot. The new client therefore could not allocate that slot even though streaming was already connected.
- The first correction did run in production, but its immediate detach/attach pair violated the Xbox protocol's required remove-to-add interval, so the console could ignore the re-add even though the local slot was released.
- The lifecycle now transfers that slot on a genuinely different Moonlight session: it cancels the old controller timer, sends neutral input, rebinds feedback to the new stream, moves local slot ownership, and leaves the remote logical gamepad attached.
- A regression test proves same-client resume retains its binding, cross-client connection migrates the same global slot, and the handoff emits neither a remote detach nor a duplicate attach.
- Required RKMPP build and the focused 93-test gate both pass after the correction.

- Required `./scripts/build-rkmpp.sh`: exit 0 after production connection, worker, lifecycle, config, Web UI, and tests were compiled into `sunshine` and `test_sunshine`; the isolated probe also built.
- Final focused lifecycle/config/router/sink/worker/consistency/status run: 35/35 passed in 1.89 seconds.
- Short regression run: 704 tests executed in 12.25 seconds; 691 passed and 13 documented platform/environment cases skipped, with zero failures.
- Tests prove default automatic startup, explicit rollback, unique-console selection, ambiguous-console rejection, complete and rejected configuration, non-target application isolation, target application start, repeated-start replacement, status snapshots, final neutral/detach/close, Xbox-only routing without a host virtual gamepad, and sanitized API fields.
- `ConfigConsistencyTest`: 5/5 passed, including Web defaults, documentation section/order, and `en` localization coverage.
- Project LLVM 22 formatting and `git diff --check`: pass.

## Deferred live evidence

- Device Code login, stable-console selection, real WakeUp, ready state, and Home DELETE were not repeated in this step because they require external Xbox services/hardware.
- A live automatic start reached `ready` after one provisioning retry, and the owner confirmed ordinary controller input reached the Xbox. Stick signs, trigger range, ordinary/trigger vibration, and physical disconnect/reconnect remain pending.
- The 30-minute Step 07 soak remains explicitly skipped rather than passed.
- Windows/MSYS2, full Doxygen, changed-code coverage, and final live acceptance remain later gates.

## Deployment boundary

Step 09 uses the in-process production connection behind the transport-neutral sink/session interface. This is a practical implementation choice for the current ROCK 5B+ build, not a retroactive claim that Step 07's skipped sidecar-versus-built-in stability comparison passed.
