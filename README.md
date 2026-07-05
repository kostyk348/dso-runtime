# DSO Runtime

`dso-runtime` is a small Linux user-space runtime for Deterministic Systems Optimization.

The goal is not to replace the Linux scheduler. The goal is to let ordinary programs expose more structure before runtime:

- fixed arenas instead of unbounded allocation
- bounded task graphs instead of hidden dynamic scheduling
- explicit resource contracts
- compile-time topological planning
- deterministic execution order
- trace output for validation

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the example:

```bash
./build/dso_pipeline
```

Run a repeated frame/tick plan:

```bash
./build/dso_frame_loop 10000
```

Run a normal Linux program under simple process-level constraints:

```bash
./build/dso-run --cpu 0 --mem-mb 256 -- /bin/echo hello
```

Steam usage is documented in [STEAM.md](STEAM.md).

Short timeout smoke test:

```bash
./build/dso-run --cpu 0 --mem-mb 256 --timeout-sec 2 -- ./build/dso_frame_loop 50000
```

## Minimal Model

```text
arena
  -> task graph
  -> contract
  -> dso_compile()
  -> dso_execute()
  -> trace.json
```

## Current Scope

This first version is intentionally single-threaded. That is deliberate.

DSO starts by making execution structure explicit. Parallel execution can be added later once the graph, contracts and verification model are stable.

`dso-run` is the first Linux integration layer. It does not replace the scheduler; it applies simple process constraints before `execvp`:

- CPU pinning through `sched_setaffinity`
- address-space budget through `RLIMIT_AS`
- optional memory locking through `mlockall`

## Next Steps

- add cgroup v2 wrapper
- add bounded queues
- add worker pool with static schedule partitions
- export Chrome trace format
- add `dso-run` for launching normal programs under contracts
