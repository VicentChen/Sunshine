# Xbox Remote Play Step 03 Validation

> Archived with [Feature 005](../005_xbox_remote_play.md).

## Status

`PASS` — implementation, offline validation, console discovery, five create/delete cycles, and explicit process-resource before/after checks all pass.

## Scope

- Home console discovery and exact stable-ID selection.
- Home play creation, `Provisioned`-only state polling, configuration parsing, monotonic keepalive scheduling, SDP/ICE REST exchange models, and idempotent deletion.
- One controlled retry after HTTP 401 with an updated in-memory GSSV context.
- Best-effort DELETE after every failure that occurs after a valid session ID is received.
- Fixed 15-second deadline on every Home REST request.

## Reference provenance

- LunarNX commit `5b3490deb5113714b52b3147bf11b24318b62359`, MIT-licensed Xbox API/session paths.
- XStreaming commit `9d7649d477b71a674ab72842e950e977e60baedb`, MIT-licensed Home signaling and state semantics.
- Sunshine baseline commit `a8e3c460077d06484798e6744ce623b599d5d70a`.

No source was copied from LunarNX's AGPL PlayStation/Chiaki paths.

## Automated evidence

- Official RKMPP build entry: `PASS`.
- Xbox Remote Play suites: `42/42 PASS` across authentication, production runtime guards, protocol contracts, Home session lifecycle, token refresh, and token storage.
- Home session lifecycle suite: `11/11 PASS`.
- Session implementation line coverage: `99.08%`; the only three uncovered gcov entries are compiler-attributed closing braces after already-covered return paths. Executable failure and success lines are covered.
- Formatting: LLVM `clang-format` 22.1.6 applied; `git diff --check` passes.

Covered Home REST outcomes include success, `Provisioning → Provisioned`, exact selection, duplicate/missing console, one-time 401 refresh, 403, 404, 410, 418, 429, 500, network failure, malformed JSON, invalid identifiers, cancellation, timeout, keepalive scheduling, signaling pending responses, and cleanup attempts after post-creation failures.

## Live evidence

Authentication resumed from the owner-only credential store without another Device Code prompt. Discovery initially returned zero consoles while Remote Features were disabled. After the console-side setting was enabled, discovery returned exactly one online Home console. The full stable ID, device name, account, and addresses are intentionally omitted.

The first live probes exposed a current-service compatibility detail: `serverDetails.ipV4Port` is not guaranteed to contain a usable integer, and modern Home configuration can omit other legacy direct-transport fields. These fields are not consumed by the SDP/ICE REST flow. The parser now reads valid legacy details on a best-effort basis, strictly validates a supplied keepalive value, and uses XStreaming's current 20-second compatibility default when keepalive is absent. Every failed probe occurred after receiving a valid session ID and attempted DELETE before returning the sanitized field-path error.

One compatibility confirmation cycle passed with provisioning/deletion elapsed times of 7322/1377 ms and HTTP `202/200/200/202` for create/final-state/configuration/delete.

The required five-cycle run then passed:

| Run | Provisioned | DELETE | HTTP create/state/config/delete |
| --- | ---: | ---: | --- |
| 1 | 4368 ms | 1480 ms | `202/200/200/202` |
| 2 | 4968 ms | 1584 ms | `202/200/200/202` |
| 3 | 4900 ms | 1655 ms | `202/200/200/202` |
| 4 | 5004 ms | 1559 ms | `202/200/200/202` |
| 5 | 5199 ms | 1634 ms | `202/200/200/202` |

The service exposes no session-list query in this flow. DELETE 202 followed by the next successful create is therefore the available no-stale-session evidence, as allowed by the plan.

The probe measures `/proc/self/task` and `/proc/self/fd` before and after its cycles and fails if either count grows. Earlier attempts to collect this evidence encountered sanitized transient network failures at GSSV authentication, XSTS, discovery, create, and state stages.

The final instrumented five-cycle repeat passed:

| Run | Provisioned | DELETE | HTTP create/state/config/delete |
| --- | ---: | ---: | --- |
| 1 | 9015 ms | 1634 ms | `202/200/200/202` |
| 2 | 5330 ms | 1633 ms | `202/200/200/202` |
| 3 | 5431 ms | 2765 ms | `202/200/200/202` |
| 4 | 5238 ms | 1726 ms | `202/200/200/202` |
| 5 | 5114 ms | 1744 ms | `202/200/200/202` |

Process resources were stable across that run: threads `1 -> 1` and file descriptors `4 -> 4`. The probe exited successfully after deleting the fifth session.

## Final closeout

- `git diff --check`: `PASS` (exit 0).
- `./scripts/build-rkmpp.sh`: `PASS` (exit 0); rebuilt `sunshine`, `test_sunshine`, and `xbox-remote-probe` from the required build directory.
- `test_sunshine --gtest_filter='XboxRemote*'`: `42/42 PASS`, including `11/11` Home session lifecycle tests.
- Normal cleanup: every successful cycle received DELETE HTTP 202; the final resource check found no thread or file-descriptor growth.
- Error, timeout, and cancellation cleanup: covered by the offline session tests listed above.
- Sensitive-data review: no token, response body, full console identifier, account data, or address is present in this record.
- Next-step risk: Step 04 remains a hard WebRTC compatibility gate; Sunshine input routing must not be modified until all four data channels and no-op media transport pass on the real console.
