# Xbox Remote Play Step 12 Validation

> Archived with [Feature 005](../005_xbox_remote_play.md).

## Status

`DEFERRED — REAL END-TO-END HARDWARE ACCEPTANCE NOT RUN` — Step 12 requires simultaneous control of a ROCK 5B+, HDMI RX, a real Xbox, a Moonlight client, and a physical controller. Those interactions cannot be performed or observed defensibly from the unattended build/test terminal. The owner also explicitly allowed later long tests to be skipped, so the two-hour soak was not started. No Step 12 row is represented as passed.

## Readiness evidence from earlier gates

- The final ROCK 5B+ build produces Sunshine, the complete test executable, and the compatibility probe.
- The complete local regression passes 697 tests, skips 13 existing environment cases, and has zero failures.
- Automated tests cover exact controller mapping, trigger expansion, bounded input edges, neutralization, vibration parsing/forwarding, application-scoped routing, reconnect epochs, watchdog, cancellation, and cleanup attempts.
- Earlier Step 04 live compatibility evidence established real Xbox SDP, ICE, DTLS/SCTP, four data channels, media consumption, keepalive, and deterministic peer teardown. It does not substitute for the final Moonlight-to-Xbox acceptance.

## Acceptance matrix disposition

| Scenario | Disposition | Evidence still required |
| --- | --- | --- |
| Cold start | NOT RUN | Launch the configured Xbox/HDMI application from a disconnected state; observe `ready` and the first correct input |
| Basic input | NOT RUN | Check every button, D-pad direction, both sticks, and both triggers on the Xbox |
| Combined input | NOT RUN | Hold both sticks, triggers, and two buttons; confirm no loss or stuck release |
| Vibration | NOT RUN | Trigger ordinary and impulse-trigger vibration; confirm the Moonlight controller starts and stops each motor |
| Five-minute interaction | NOT RUN | Play while rapidly pressing/releasing; observe no stale input or queue buildup |
| Two-hour endurance | SKIPPED AS LONG TEST | Record RSS, file descriptors, threads, keepalive, media counters, reconnects, and final cleanup |
| Moonlight reconnect | NOT RUN | Disconnect/reconnect the client; confirm a neutral boundary, no old press replay, and a fresh feedback queue |
| Xbox/network fault | NOT RUN | Remove and restore networking or close the Xbox session; confirm neutralization and bounded recovery |
| Application stop | NOT RUN | Confirm neutral input, gamepad removal, peer close, Home DELETE or sanitized `delete_unconfirmed` |
| Non-Xbox application isolation | NOT RUN | Launch another Sunshine application and confirm it neither controls nor wakes the Xbox |
| Rollback | NOT RUN | Disable Xbox Remote Play and confirm prior virtual-HID/NXBT and HDMI RX behavior |

## Safety and evidence rules for a future run

- Do not record tokens, Microsoft account details, complete console IDs, session IDs, SDP, ICE credentials, or network addresses.
- Capture the sanitized status fields (`state`, `stage`, `failure_stage`, `failure_kind`, `epoch`) and fixed worker logs for each scenario.
- For every disconnect or stop, observe a neutral controller state before the next action and confirm that no old press is replayed.
- Record whether Home DELETE was confirmed. Treat `delete_unconfirmed` as a cleanup warning requiring server-state follow-up, not as a successful delete.
- Keep the two-hour endurance test optional under the owner's current instruction; all shorter physical acceptance rows still require an operator and remain pending.
