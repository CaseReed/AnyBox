# AGENTS.md

AnyBox relinks GDK console PE32+ executables into Windows programs that load reimplemented system DLLs. It is not an emulator. The rules are [CONTRIBUTING.md](CONTRIBUTING.md) and [CONVENTIONS](docs/dev/CONVENTIONS.md); these are the ones agents break.

- No comments in code, except `#endif`, namespace ends, and the licence and copyright notice of code adapted from an upstream source. Known gaps go in `docs/dev/debt/<library>.md` through a fragment.
- An export you cannot implement calls `AbxNotImplemented(__func__)`. A path whose behaviour is unknown calls `AbxUnsupported(__func__, "what is unknown")`. Never return a made-up success.
- Implement the function for every input. Never special-case a value, title ID or path seen in one title's trace.
- Every `ABX_API` definition has exactly one declaration in its library's `include/` headers, reachable from `<library>.hpp`. `tools/check_conventions.py` enforces it.
- A library is one folder under `core/libs/xdll/<name>` with `Imports.txt` (the DLL names it serves), `CMakeLists.txt` (`anybox_add_xdll`), `<name>.hpp`, `include/Exports.hpp`, optionally `Forwards.def` for exports the host provides unchanged, and optionally `Ordinals.txt` (`<Name> @<ordinal>` per line) for exports titles import by ordinal. `tools/stubgen.py --new-library` creates one.
- Do not replace modules that titles ship themselves (middleware, engine DLLs, XSAPI thunks). The relinker keeps a bundled module's name, and it wins over an AnyBox library.
- New third-party code is a submodule at `3rdparty/<name>`, built from source with CMake.
- Commit subjects and PR titles are Conventional Commits: `feat(xgameruntime): implement XAsyncRun`.
- PR body: copy [the template](.github/pull_request_template.md) and fill every line, including `AI-assisted`.
- Before pushing: `python tools/check_conventions.py`, `ctest --test-dir build --output-on-failure`.
- A relinker-only change builds anywhere with `-DANYBOX_RELINKER_ONLY=ON`; library changes need a Windows build.
