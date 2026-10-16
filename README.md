# DSO Runtime

[![Release](https://img.shields.io/github/v/release/kostyk348/dso-runtime?sort=semver&color=blue)](https://github.com/kostyk348/dso-runtime/releases)
[![License](https://img.shields.io/badge/license-Apache-2.0-green)](https://github.com/kostyk348/dso-runtime/blob/main/LICENSE)
[![Last commit](https://img.shields.io/github/last-commit/kostyk348/dso-runtime?branch=main)](https://github.com/kostyk348/dso-runtime/commits/main)

Deterministic task graph execution framework for real-time systems.

```
libdso.a  —  C11  —  no deps beyond POSIX + pthreads  —  MIT
```

## Why DSO?

Real-time and game workloads need **guarantees**, not best-effort:

| Problem | Solution |
|---|---|
| Frame pacing jitter | `dso_contract` — per-task WCET limits, runtime overrun detection |
| Nondeterministic parallelism | Dependency levels — static schedule, no work-stealing, reproducible |
| Memory fragmentation | Arena + typed pools — zero alloc after init, O(1) alloc/free |
| Data race bugs | Handle-based memory — double-buffering via `dso_handle_redirect` |
| Debugging parallel systems | Chrome trace export — `dso_trace_export_chrome()` → `chrome://tracing` |

## Quick Start

```c
#include "dso.h"

// 1. Arena + context
unsigned char mem[65536];
dso_arena arena;
dso_arena_init(&arena, mem, sizeof(mem));
dso_context ctx;
dso_context_init(&ctx, &arena);

// 2. Task graph
uint32_t a, b;
dso_task(&ctx, "read_sensor",  read_fn,  &sensor, &a);
dso_task(&ctx, "process_data", process_fn, &data,  &b);
dso_dep(&ctx, a, b);  // b depends on a

// 3. Compile with contract
dso_contract c = { .max_tasks = 16, .max_step_ns = 2000000 };
dso_verdict v;
dso_compile(&ctx, c, &v);  // verifies bounds, detects cycles

// 4. Execute (sequential or parallel)
dso_execute(&ctx);
// — or —
dso_worker_pool pool;
dso_worker_pool_init(&pool, 4);
dso_execute_parallel(&ctx, &pool);  // level-based parallelism
```

## Features

### Core

- **Arena allocator** — bump-alloc, O(1), zero fragmentation
- **Handle-based memory** — `dso_handle → dso_mem_region → pointer`, supports double-buffering via redirect
- **Typed pools** — fixed-size object pool, O(1) alloc/free, handle-based access
- **Bounded SPSC queues** — lock-free ring buffer, power-of-2 capacity

### Scheduling

- **Topological sort** — cycle detection, Kahn's algorithm
- **Dependency levels** — groups independent tasks → parallel execution
- **Worker pool** — pthread-based, spin barriers, up to 16 workers
- **Level-aware execution** — `dso_execute_parallel()` / `dso_execute_frame_parallel()`

### Contracts

- **Compile-time** — max tasks, deps, arena bytes, plan steps
- **Runtime** — per-task WCET (`max_step_ns`), per-frame WCET (`max_frame_ns`)
- **Multi-layer** — Resource / Control / Runtime → `dso_deployment_contract`

### Tooling

- **Chrome trace export** — `dso_trace_export_chrome()` → `chrome://tracing`
- **Plan serialization** — `dso_plan_save()` / `dso_plan_load()` — binary snapshot
- **JSON trace** — `dso_write_trace_json()` — per-task durations
- **Metrics** — worst step, worst frame, overrun counter, arena high-water

### Sandbox

- `dso-run` — Linux process launcher with CPU pin, mem limit, mlock, timeout, **cgroup v2**

## Benchmarks

### Arena allocator

```
arena_alloc 64 bytes     195,139,085 ops/sec   0.005 us/op
```

### SPSC Queue (64 slots)

```
push/pop pairs            80,261,460 ops/sec   0.012 us/op
```

### ECS Demo (100k particles, 3 systems, 4 batches)

```
Schedule: 3 levels
  level 0: 4 tasks (integrate)     ← parallel
  level 1: 4 tasks (collision)     ← parallel
  level 2: 1 task  (render)

1 worker:  5.6 ms/frame
4 workers: 2.9 ms/frame
Speedup:   1.93x
```

### Flecs integration

```
3 Flecs systems, 50k entities, DSO orchestration
  369 us/frame (1 worker)
```

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

### Demos

```bash
# DSP pipeline with memory redirection + queues

./build/dso_dsp 10

# 100k particle ECS simulation

./build/dso_ecs 100000 100 4

# Parallel speedup comparison

./build/dso_parallel 5 4

# Full benchmark suite

./build/dso_benchmark

# Flecs integration demo (requires Flecs)

./build/flecs_dso 100000 100
```

## Flecs Integration

DSO can act as a deterministic scheduler for [Flecs](https://github.com/SanderMertens/flecs) ECS systems.

See [`examples/flecs_dso_adapter.c`](examples/flecs_dso_adapter.c):

```c
// Wrap Flecs systems as DSO tasks with WCET contracts
dso_task(&ctx, "integrate", flecs_system_wrapper, &integrate_job, &id);
dso_task_set_contract(&ctx, id, (dso_resource_contract){
    .wcet_us_max = 50000.0,     // 50ms WCET limit
    .cycles_max  = 10000000,
});
```

## Project Structure

```
include/dso.h          — Public API (fully documented with Doxygen)
src/dso.c              — Core runtime
src/dso_run.c          — Process sandbox
tests/test_runtime.c   — Unit tests (all pass)
examples/
├── ecs_demo.c         — 100k particle ECS simulation
├── dsp_pipeline.c     — Audio DSP pipeline with handle redirection
├── parallel_demo.c    — Sequential vs parallel comparison
├── benchmark.c        — Throughput measurements
├── flecs_dso_adapter.c — Flecs integration demo
├── pipeline.c         — Basic pipeline
└── frame_loop.c       — Frame loop demo
```

## License

Apache 2.0
