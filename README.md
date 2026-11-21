# AnyBox

Tool for porting Xbox One / Xbox Series (GDK) executables to Windows PCs, by relinking them against reimplemented system libraries.

No emulation and no separate runtime process. A GDK title is already an x86-64 PE program. The [relinker](core/relinker) points its imports at AnyBox's own DLLs ([core/libs/xdll](core/libs/xdll)) and switches the Xbox subsystem to a Windows one. The converted program then runs as an ordinary Windows process.

[Usage](docs/user/USAGE.md) · [Build](docs/dev/BUILD.md) · [Architecture](docs/dev/ARCHITECTURE.md) · [Contributing](CONTRIBUTING.md) · [Conventions](docs/dev/CONVENTIONS.md) · [Technical debt](docs/dev/TechnicalDebt.md) · [Progress tracking](docs/dev/PROGRESS.md) · [Game testing](docs/dev/GAME_TESTING.md)

## Status

Early. The relinker, the library build, the run reports and the progress tracking work end to end on a test title. Most system functions are still stubs that stop the title with a report naming the function.

[![libraries](https://dark3581.github.io/AnyBox/badge-libraries.svg)](https://dark3581.github.io/AnyBox/progress.html)

[![progress map](https://dark3581.github.io/AnyBox/progress.svg)](https://dark3581.github.io/AnyBox/progress.html)

<sub>Each tile is a library, sized by its number of functions, and each square in it is one function: implemented (green), forwarded to Windows (blue), stub (grey) or known but not declared yet (dark). A dashed red tile is a DLL that titles import and no AnyBox library serves yet. `*` marks a library whose full export list is not known yet, so its total grows as titles are audited. Known functions come from the [export inventories](docs/inventory) and [title needs records](docs/needs).</sub>

The [progress page](https://dark3581.github.io/AnyBox/progress.html) lists every function as implemented, forwarded, stub or not declared, and every DLL with no library. It is regenerated on each push to `main` by [`tools/progress.py`](tools/progress.py); the [history](https://dark3581.github.io/AnyBox/history.html) shows how it grows.

| Library | Serves | State |
|---|---|---|
| [xgameruntime](core/libs/xdll/xgameruntime) | `xgameruntime.dll` | XGameRuntime init, XTaskQueue, XAsync implemented; XUser, XGameSave, XPackage, XSystem stubs |
| [d3d12_x](core/libs/xdll/d3d12_x) | `d3d12_x.dll`, `d3d12_xs.dll` | entry points declared, all stubs: needs a D3D12.X → desktop D3D12/Vulkan layer |
| [xaudio2](core/libs/xdll/xaudio2) | `xaudio2_9.dll` | forwarded to the host's XAudio2 |
| [gameinput](core/libs/xdll/gameinput) | `gameinput.dll` | forwarded to the host's GameInput |
| [kernelx](core/libs/xdll/kernelx) | `kernelx.dll` | Win32 subset forwarded to `kernel32` |

Unsupported or unexpected states stop the process with a [run report](docs/run-report.schema.json) (`anybox-report.json`) that names the library and function. Nothing returns a made-up success.

## Compatibility

Per-title status lives in the **Game bring-up** discussions, one thread per title ID. Start a thread with the [bring-up form](.github/DISCUSSION_TEMPLATE/game-bring-up.yml) and paste the line from `python tools/run_report_summary.py anybox-report.json`.

## Where to start contributing

1. Pick the top item on the [needs page](https://dark3581.github.io/AnyBox/needs.html), which ranks what titles need and AnyBox lacks. Or audit a title you own with `python tools/import_audit.py <package> --needs docs/needs/<TitleId>.json` and contribute the record so it counts.
2. Read [CONTRIBUTING.md](CONTRIBUTING.md) and the [XTaskQueue implementation](core/libs/xdll/xgameruntime/TaskQueue.cpp) as a worked example.
3. Open a pull request with the [template](.github/pull_request_template.md). CI runs the conventions check and posts the progress change.

## Disclaimer

This project is intended for interoperability, research, preservation and compatibility. It does not include, distribute or require copyrighted software, firmware, cryptographic keys or proprietary libraries, and it does not decrypt or unpack protected packages. It works only on executables the user has already obtained in plain form, in accordance with applicable laws and license terms.

AnyBox is an independent project. It is not affiliated with, authorized or endorsed by Microsoft. "Xbox", "Xbox One" and "Xbox Series X|S" are trademarks of the Microsoft group of companies.

AnyBox is built to be like [AnyPS5](https://github.com/boykopovar/AnyPS5), for Xbox.

## License

GNU General Public License version 2 only. See [LICENSE](LICENSE).
