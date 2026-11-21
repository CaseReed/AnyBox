# gameinput

## add Functional

- [Forwards](../../../core/libs/xdll/gameinput/Forwards.def) (gameinput) - `GameInput.lib` imports `GameInputCreate` by ordinal 1, and `ab_gameinput.dll` does not pin that ordinal in its definition file

# xgameruntime

## add Functional

- [XTaskQueueIsEmpty](../../../core/libs/xdll/xgameruntime/TaskQueue.cpp) (xgameruntime) - not in the public GDK 2610 headers or the runtime's export list; kept for internal callers, no title imports it

## change [XAsync, XUser, XGameSave]

- [Inventory](../../../core/libs/xdll/xgameruntime/Inventory.cpp) (xgameruntime) - every public runtime export in [the GDK 2610 inventory](../../inventory/gdk-2610-pc.json) not implemented in TaskQueue.cpp, Runtime.cpp, System.cpp or Package.cpp is declared without parameters until implemented
