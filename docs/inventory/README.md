# Export inventories

Lists of every function a system DLL exports, so the needs page can show the whole surface of a DLL and not only what audited titles happen to import.

| File | Source | DLLs |
|---|---|---|
| [gdk-2610-pc.json](gdk-2610-pc.json) | GDK 2610.0.9661, the public PC GDK | `xgameruntime.dll`, `gameinput.dll` |

Make one from import libraries (`.lib`, short format) or from DLLs. The public PC GDK needs two adjustments:

- On PC, `xgameruntime.lib` is a static library and `xgameruntime.dll` exports only its loader. The public runtime API is exported by `xgameruntime.thunks.dll`, so read that and record it under the console name with `--rename`.
- Leave out the extension libraries (PlayFab, Party, GameChat2, libHttpClient, XCurl, the XSAPI thunks). Titles ship those modules themselves, and AnyBox does not replace them.

The GDK zip holds MSI packages. Extract them without installing: `msiexec /a "<package>.msi" TARGETDIR="<folder>" /qn`. Then, with `<gdk>` the GDK folder inside `<folder>`:

```
python tools/lib_inventory.py "<gdk>\<version>\windows\bin\x64\xgameruntime.thunks.dll" "<gdk>\<version>\windows\lib\x64\GameInput.lib" --rename xgameruntime.thunks.dll=xgameruntime.dll --label "GDK <version> (public PC GDK)" --json docs/inventory/gdk-<version>-pc.json
```

Console-only libraries (`d3d12_x`, `kernelx` and the rest) come with the licensed GXDK, not the public GDK. Their surface comes from title audits until someone with access adds an inventory.

An inventory holds DLL and function names only. Never commit the libraries, headers or DLLs themselves.
