- Use `scripts/build-rkmpp.sh` to build this project.

- Save RKMPP documents in `docs/rkmpp/`.

- Keep implemented behavior in `docs/rkmpp/FEATURES.md` and user-facing setup in
  `docs/rkmpp/README.md`. Keep only active, unfinished plans as separate files.
  When a plan is completed and confirmed, merge its lasting information into
  `FEATURES.md` or `README.md`, then delete the plan instead of archiving it.

- Never run `test_sunshine` for RKMPP work.

- When C++ tests are needed, build and run only `test_sunshine_rkmpp`. This target must not compile or
  execute `tests/unit/test_http_pairing.cpp`. Do not remove its `SUNSHINE_RKMPP_TESTS` state-isolation
  definition or the test-build write guard in `src/nvhttp.cpp`.
