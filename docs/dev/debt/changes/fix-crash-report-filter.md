# runtime

## remove [the crash report depends on the unhandled exception filter set at load]

## add Functional

- [Runtime](../../../core/libs/runtime/src/Runtime.cpp) (runtime) - to keep the crash report when a C runtime or the title replaces the unhandled exception filter, a vectored handler puts the AnyBox filter back on top on every first-chance exception, not only fatal ones (C++ throws, `OutputDebugString`, thread naming, guard-page probes); a title's later `SetUnhandledExceptionFilter` call gets the AnyBox filter back as the previous one, and the title's own filter runs after the report is written
  Closes when: the crash report no longer needs the top-level filter
- [Runtime](../../../core/libs/runtime/src/Runtime.cpp) (runtime) - a fault that the title's own unhandled exception filter recovers from (`EXCEPTION_CONTINUE_EXECUTION`) still gets a `crash` report, because the AnyBox filter writes the report before it chains; a clean exit overwrites it with `none`, but a later death that writes nothing leaves the report pointing at the recovered fault
  Closes when: the report is written only after the chained filter returns `EXCEPTION_CONTINUE_SEARCH`, or the title's recovery is told apart from a crash
- [Runtime](../../../core/libs/runtime/src/Runtime.cpp) (runtime) - the guard against re-entering the AnyBox filter is process-wide, so a second thread that crashes while the first is inside the filter returns `EXCEPTION_CONTINUE_SEARCH` without a report and may end the process before the first report is complete
  Closes when: the guard is per thread
