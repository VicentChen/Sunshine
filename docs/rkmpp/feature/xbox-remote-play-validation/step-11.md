# Xbox Remote Play Step 11 Validation

> Archived with [Feature 005](../005_xbox_remote_play.md).

## Status

`PASS FOR AVAILABLE SHORT LOCAL GATES; LONG OR UNAVAILABLE GATES EXPLICITLY SKIPPED` — the official ROCK 5B+ build, feature-disabled test build, complete local gtest regression, configuration/locale consistency, formatting, documentation update, and dependency pin/license review pass. Doxygen generation, changed-code coverage, Windows/MSYS2, and long soak/hardware work were not run under the owner's instruction that later long tests may be skipped. This is not a claim that those omitted gates passed.

## Engineering and documentation gate

- The project LLVM 22 `clang-format` executable formatted all modified Xbox Remote Play C/C++ sources and tests using the repository `.clang-format`.
- `git diff --check` exits successfully.
- Public Xbox protocol, authentication, REST, transport, queue, startup, sink, worker, status, cancellation, and configuration APIs have primary Doxygen comments. New fields use inline `///<` comments.
- The English configuration reference now includes first-login, stable-console selection, private file requirements, exact application selection, restart, status interpretation, and rollback instructions.
- Only `src_assets/common/assets/web/public/assets/locale/en.json` was changed for localization; no language variant was modified.
- The feature option defaults to OFF. Runtime configuration additionally disables the feature outside Linux or when the production build definition is absent, leaving existing output behavior unchanged.

## Dependency and license gate

- libdatachannel is pinned by full immutable Git commit `c6696d157b5612df2a741d9a03b192b47ab6cefb` (upstream release line v0.24.3).
- Static build controls are explicit: examples, tests, and WebSocket support OFF; media transport ON; libnice and system SRTP/usrsctp OFF; shared libraries OFF for the dependency and restored afterward for the parent build.
- The transport is MPL-2.0. Its enabled libjuice dependency is MPL-2.0; usrsctp and libsrtp use permissive BSD-style licenses. These file-level/permissive terms are compatible with aggregation into Sunshine's GPL-3.0-only program when their notices and source obligations are retained.
- Root `NOTICE` now records the exact pin, license families, and upstream source/license locations. The Step 01 reference-source provenance and Step 04 transport record remain part of the audit trail; no LunarNX or XStreaming source was copied into Sunshine.

## Build and test evidence

- Required `./scripts/build-rkmpp.sh`: exit 0; built `build-rkmpp-review/sunshine`, `build-rkmpp-review/tests/test_sunshine`, and `build-rkmpp-review/xbox-remote-probe`.
- Final Step 10 focused gate after cleanup-deadline hardening: 73/73 tests passed across 11 suites in 210 ms.
- Complete regression: 712 tests across 113 suites; 699 passed, 13 existing platform/environment cases skipped, 0 failed.
- Feature-disabled test build: configuring with `SUNSHINE_BUILD_XBOX_REMOTE_PROBE=OFF` and building `test_sunshine` succeeded. Production transport-only tests and sources are excluded without leaving unresolved references.
- The 13 skips are the non-Windows UTF test, disabled tray test, four unavailable audio encoder cases, four unavailable mouse/HID cases, and three unavailable encoder backends. None is an Xbox Remote Play test.
- Configuration consistency: 5/5 pass as part of the complete regression. Locale consistency: 9/9 pass. After the final user-guide edit, both suites were rerun together and passed 14/14 in 1.995 seconds.

## Explicitly skipped or unavailable gates

- Doxygen executable: unavailable in the current environment. The official RKMPP build intentionally configures `BUILD_DOCS=OFF`, so no generated Doxygen warning/error report exists. A manual documentation audit was performed, but it is not represented as a Doxygen build pass.
- Changed-code coverage: not generated. Prior in-tree `.gcda` files were stale after source changes and emitted merge warnings; test runs used fresh temporary coverage prefixes so results remained clean. Producing and reviewing a new 100% changed-code report was skipped as a potentially long gate.
- Windows/MSYS2 UCRT64: unavailable on this Linux/aarch64 host and not executed.
- Full feature-OFF application build and cross-platform compile were not executed. The feature-OFF `test_sunshine` target was compiled locally, but that is not represented as full non-RKMPP or cross-platform validation.
- Step 07's 30-minute soak, Step 12's two-hour soak, and all real Moonlight/Xbox fault and input acceptance remain skipped/deferred.
