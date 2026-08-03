# Blink example

A minimal Structured Text program that toggles a direct-addressed BOOL output
(`%Q0.0`) at a fixed rate, counted in scan cycles rather than wall-clock time.

## Run it

```sh
./build/apps/plc_runner/plc_runner examples/blink/blink.st
```

`plc_runner` runs the scan engine at a 10ms cycle time by default (override with
a second argument, e.g. `plc_runner examples/blink/blink.st 20`) and prints the
value of every `%Q`-area tag a few times a second. `LedState` should flip
between `TRUE` and `FALSE` roughly once per second. Press Ctrl+C to stop; the
runner prints final scan diagnostics (cycle count, overrun count, max wake
jitter, resync count) on exit.

Optional real-time flags (all off by default, safe to combine with the above):

```sh
./build/apps/plc_runner/plc_runner examples/blink/blink.st 10 --rt-priority=50 --rt-affinity=0 --lock-memory
```

`--rt-priority=N` requests `SCHED_FIFO` priority `N` (Linux) or
`THREAD_PRIORITY_TIME_CRITICAL` (Windows) for the scan thread; `--rt-affinity=N`
pins it to logical CPU `N`; `--lock-memory` locks the process's memory
(`mlockall` on Linux — no equivalent on Windows). Each degrades to a printed
`rt warning: ...` line instead of failing if the OS denies the request (e.g. no
`CAP_SYS_NICE`/root). See `docs/architecture.md`'s "Real-time characteristics"
section for what this does and doesn't guarantee.
