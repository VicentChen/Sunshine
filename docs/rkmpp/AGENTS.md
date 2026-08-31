- Use `scripts/build-rkmpp.sh` to build this project.

- Save RKMPP documents in `docs/rkmpp/`.

- Keep implemented behavior in `docs/rkmpp/FEATURES.md` and user-facing setup in
  `docs/rkmpp/README.md`. Keep only active, unfinished plans as separate files.
  When a plan is completed and confirmed, merge its lasting information into
  `FEATURES.md` or `README.md`, then delete the plan instead of archiving it.

- Never run the upstream `test_sunshine` executable for project-specific RKMPP,
  NS, or Xbox work.

- VicentChen-authored unit tests are split into `tests/unit/rkmpp/`,
  `tests/unit/ns/`, and `tests/unit/xbox/`. Build and run only the matching
  `test_sunshine_rkmpp`, `test_sunshine_ns`, or `test_sunshine_xbox` target for
  the behavior being changed.

- Module test targets must not compile or execute
  `tests/unit/test_http_pairing.cpp`. Do not remove their
  `SUNSHINE_MODULE_TESTS` state isolation or the test-build write guard in
  `src/nvhttp.cpp`.
