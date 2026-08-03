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
runner prints final scan diagnostics (cycle count, overrun count) on exit.
