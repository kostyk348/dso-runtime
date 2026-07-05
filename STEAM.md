# Testing DSO Runtime With Steam Games

`dso-run` can be used as a Steam launch option for lightweight process-level contracts.

It does not change the game engine. It constrains the process before launch and reports basic resource usage when the process exits.

## Launch Option

In Steam game properties, set launch options to:

```text
/home/lain/dso-runtime/build/dso-run --cpu 0 --mem-mb 4096 -- %command%
```

This means:

- pin the game process to CPU 0
- cap virtual address space at 4096 MiB
- launch the original Steam command
- print a `dso-run report` after the process exits

For games that need more memory:

```text
/home/lain/dso-runtime/build/dso-run --cpu 0 --mem-mb 8192 -- %command%
```

For games that use many helper processes or Proton, keep the memory cap loose at first.

For a short smoke test:

```text
/home/lain/dso-runtime/build/dso-run --cpu 0 --mem-mb 4096 --timeout-sec 15 -- %command%
```

The wrapper will terminate the game after 15 seconds and still print a report.

## Good First Candidates

Prefer native or lightweight games first:

- FTL: Faster Than Light
- Undertale
- Dominions 5
- Necesse

Avoid starting with large Proton games or anti-cheat games. They add noise and may reject wrappers.

## What This Tests

This tests the Linux integration layer:

```text
Steam launch
  -> dso-run
  -> CPU affinity / memory limit
  -> game process
  -> resource report
```

Example report from FTL:

```text
dso-run report: elapsed_ms=15207 user_ms=2954 sys_ms=513 maxrss_kb=288584 cpu=0 mem_mb=2048 timed_out=1
```

It does not yet test in-engine DSO frame graphs. That requires integrating `libdso` into an application or game loop.

## Current Limitation

`dso-run` is a process wrapper. It cannot see inside the game's frame graph. For real DSO frame planning, the application must use `libdso` internally.
