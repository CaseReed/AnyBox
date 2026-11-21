# xgameruntime

## add Functional

- [XSystemGetDeviceType](../../../core/libs/xdll/xgameruntime/System.cpp) (xgameruntime) - derived from `TargetDeviceFamily` of the first `<Executable>` in `MicrosoftGame.config`, not from the host: `Scarlett` reports `XboxScarlettAnaconda`, `XboxOne` or an `<Executable>` without the attribute reports `XboxOneX`, `PC` reports `Pc`; a missing config, a config without `<Executable>` or any other value stops with `AbxUnsupported`; the top model of each family is reported whatever the host hardware, not checked against a console title
  Closes when: a console title's reaction to the other models of its family is measured
- [XPackageIsPackagedProcess](../../../core/libs/xdll/xgameruntime/Package.cpp) (xgameruntime) - always returns `false`: a relinked title started from a folder has no package identity on the host

# runtime

## add Functional

- [TitleConfig](../../../core/libs/runtime/src/TitleConfig.cpp) (runtime) - the `MicrosoftGame.config` reader does not skip XML comments, so a `<TitleId>`, `<Identity>`, `<ShellVisuals>` or `<Executable>` inside a comment before the real element is read in its place
