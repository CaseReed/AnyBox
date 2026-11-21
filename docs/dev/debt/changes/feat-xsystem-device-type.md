# xgameruntime

## add Functional

- [XSystemGetDeviceType](../../../core/libs/xdll/xgameruntime/System.cpp) (xgameruntime) - derived from `TargetDeviceFamily` of the first `<Executable>` in `MicrosoftGame.config`, not from the host: `Scarlett` reports `XboxScarlettAnaconda`, `XboxOne` or no value reports `XboxOneX`, `PC` reports `Pc`, no readable config reports `Unknown`, any other value stops with `AbxUnsupported`; the top model of each family is reported whatever the host hardware, not checked against a console title
  Closes when: a console title's reaction to the other models of its family is measured
- [XPackageIsPackagedProcess](../../../core/libs/xdll/xgameruntime/Package.cpp) (xgameruntime) - always returns `false`: a relinked title started from a folder has no package identity on the host
