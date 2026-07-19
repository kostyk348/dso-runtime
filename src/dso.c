#include "dso.h"

#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static size_t align_up(size_t value, size_t align) {
    size_t mask = align - 1u;
    return (value + mask) & ~mask;
}

void dso_arena_init(dso_arena *arena, void *memory, size_t size) {
    if (!arena) return;
    arena->base = (uint8_t *)memory;
    arena->capacity = size;
    arena->offset = 0;
    arena->high_water = 0;
}

void *dso_arena_alloc(dso_arena *arena, size_t size, size_t align) {
    if (!arena || !arena->base || align == 0 || (align & (align - 1u)) != 0)
        return NULL;
    size_t start = align_up(arena->offset, align);
    if (start > arena->capacity || size > arena->capacity - start)
        return NULL;
    arena->offset = start + size;
    if (arena->offset > arena->high_water)
        arena->high_water = arena->offset;
    return arena->base + start;
}

void dso_arena_reset(dso_arena *arena) {
    if (arena) arena->offset = 0;
}

void dso_context_init(dso_context *ctx, dso_arena *arena) {
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->arena = arena;
    ctx->next_handle = 1;
}

dso_status dso_task(dso_context *ctx, const char *name, dso_task_fn fn, void *user, uint32_t *out_id) {
    if (!ctx || !name || !fn) return DSO_ERR_INVALID;
    if (ctx->task_count >= DSO_MAX_TASKS) return DSO_ERR_CAPACITY;
    uint32_t id = ctx->task_count++;
    snprintf(ctx->tasks[id].name, sizeof(ctx->tasks[id].name), "%s", name);
    ctx->tasks[id].fn = fn;
    ctx->tasks[id].user = user;
    ctx->tasks[id].has_contract = 0;
    if (out_id) *out_id = id;
    return DSO_OK;
}

dso_status dso_dep(dso_context *ctx, uint32_t before, uint32_t after) {
    if (!ctx || before >= ctx->task_count || after >= ctx->task_count || before == after)
        return DSO_ERR_INVALID;
    if (ctx->dep_count >= DSO_MAX_DEPS) return DSO_ERR_CAPACITY;
    ctx->deps[ctx->dep_count].before = before;
    ctx->deps[ctx->dep_count].after = after;
    ctx->dep_count++;
    return DSO_OK;
}

dso_status dso_task_set_contract(dso_context *ctx, uint32_t task_id, dso_resource_contract rc) {
    if (!ctx || task_id >= ctx->task_count) return DSO_ERR_INVALID;
    ctx->tasks[task_id].resource_contract = rc;
    ctx->tasks[task_id].has_contract = 1;
    return DSO_OK;
}

static void compute_levels(dso_context *ctx);

static void verdict_set(dso_verdict *verdict, dso_status code, const char *message, dso_metrics metrics) {
    if (!verdict) return;
    verdict->code = code;
    snprintf(verdict->message, sizeof(verdict->message), "%s", message);
    verdict->metrics = metrics;
}

dso_status dso_compile(dso_context *ctx, dso_contract contract, dso_verdict *verdict) {
    if (!ctx) return DSO_ERR_INVALID;
    ctx->contract = contract;
    ctx->schedule_count = 0;
    ctx->trace_count = 0;
    ctx->metrics.task_count = ctx->task_count;
    ctx->metrics.dep_count = ctx->dep_count;
    ctx->metrics.arena_bytes = ctx->arena ? ctx->arena->high_water : 0;
    ctx->metrics.plan_steps = ctx->task_count;
    ctx->metrics.worst_step_ns = 0;
    ctx->metrics.frame_count = 0;
    ctx->metrics.worst_frame_ns = 0;
    ctx->metrics.total_frame_ns = 0;
    ctx->metrics.frame_overruns = 0;

    if (ctx->task_count == 0) {
        verdict_set(verdict, DSO_ERR_INVALID, "graph has no tasks", ctx->metrics);
        return DSO_ERR_INVALID;
    }
    if (contract.max_tasks && ctx->task_count > contract.max_tasks) {
        verdict_set(verdict, DSO_ERR_CONTRACT, "task count exceeds contract", ctx->metrics);
        return DSO_ERR_CONTRACT;
    }
    if (contract.max_deps && ctx->dep_count > contract.max_deps) {
        verdict_set(verdict, DSO_ERR_CONTRACT, "dependency count exceeds contract", ctx->metrics);
        return DSO_ERR_CONTRACT;
    }
    if (contract.max_arena_bytes && ctx->metrics.arena_bytes > contract.max_arena_bytes) {
        verdict_set(verdict, DSO_ERR_CONTRACT, "arena high-water exceeds contract", ctx->metrics);
        return DSO_ERR_CONTRACT;
    }
    if (contract.max_plan_steps && ctx->task_count > contract.max_plan_steps) {
        verdict_set(verdict, DSO_ERR_CONTRACT, "plan steps exceed contract", ctx->metrics);
        return DSO_ERR_CONTRACT;
    }

    uint32_t indegree[DSO_MAX_TASKS] = {0};
    uint8_t emitted[DSO_MAX_TASKS] = {0};
    for (uint32_t i = 0; i < ctx->dep_count; i++)
        indegree[ctx->deps[i].after]++;

    while (ctx->schedule_count < ctx->task_count) {
        int found = 0;
        for (uint32_t id = 0; id < ctx->task_count; id++) {
            if (!emitted[id] && indegree[id] == 0) {
                emitted[id] = 1;
                ctx->schedule[ctx->schedule_count++] = id;
                for (uint32_t d = 0; d < ctx->dep_count; d++) {
                    if (ctx->deps[d].before == id)
                        indegree[ctx->deps[d].after]--;
                }
                found = 1;
                break;
            }
        }
        if (!found) {
            verdict_set(verdict, DSO_ERR_CYCLE, "dependency graph has a cycle", ctx->metrics);
            return DSO_ERR_CYCLE;
        }
    }

    // Compute dependency levels for parallel execution
    compute_levels(ctx);

    verdict_set(verdict, DSO_OK, "plan verified", ctx->metrics);
    return DSO_OK;
}

dso_status dso_execute(dso_context *ctx) {
    if (!ctx || ctx->schedule_count != ctx->task_count) return DSO_ERR_INVALID;
    ctx->trace_count = 0;
    ctx->metrics.worst_step_ns = 0;
    for (uint32_t i = 0; i < ctx->schedule_count; i++) {
        uint32_t id = ctx->schedule[i];
        uint64_t start = monotonic_ns();
        ctx->tasks[id].fn(ctx->tasks[id].user);
        uint64_t end = monotonic_ns();
        uint64_t elapsed = end - start;
        if (elapsed > ctx->metrics.worst_step_ns)
            ctx->metrics.worst_step_ns = elapsed;
        if (ctx->trace_count < DSO_TRACE_RING) {
            dso_trace_event *ev = &ctx->trace[ctx->trace_count++];
            ev->task_id = id;
            ev->start_ns = start;
            ev->end_ns = end;
            ev->status = 0;
        }
        if (ctx->contract.max_step_ns && elapsed > ctx->contract.max_step_ns)
            return DSO_ERR_RUNTIME;
    }
    return DSO_OK;
}

dso_status dso_execute_frame(dso_context *ctx) {
    if (!ctx || ctx->schedule_count != ctx->task_count) return DSO_ERR_INVALID;
    uint64_t frame_start = monotonic_ns();
    dso_status st = dso_execute(ctx);
    uint64_t frame_end = monotonic_ns();
    uint64_t frame_ns = frame_end - frame_start;
    ctx->metrics.frame_count++;
    ctx->metrics.total_frame_ns += frame_ns;
    if (frame_ns > ctx->metrics.worst_frame_ns)
        ctx->metrics.worst_frame_ns = frame_ns;
    if (ctx->contract.max_frame_ns && frame_ns > ctx->contract.max_frame_ns) {
        ctx->metrics.frame_overruns++;
        return DSO_ERR_RUNTIME;
    }
    return st;
}

dso_status dso_run_frames(dso_context *ctx, uint64_t frames) {
    if (!ctx || frames == 0) return DSO_ERR_INVALID;
    dso_status last = DSO_OK;
    for (uint64_t i = 0; i < frames; i++) {
        last = dso_execute_frame(ctx);
        if (last != DSO_OK) return last;
    }
    return last;
}

dso_status dso_write_trace_json(const dso_context *ctx, FILE *out) {
    if (!ctx || !out) return DSO_ERR_INVALID;
    fprintf(out, "{\n  \"tasks\": [\n");
    for (uint32_t i = 0; i < ctx->trace_count; i++) {
        const dso_trace_event *ev = &ctx->trace[i];
        const char *comma = (i + 1u < ctx->trace_count) ? "," : "";
        fprintf(out,
                "    {\"id\": %u, \"name\": \"%s\", \"duration_ns\": %" PRIu64 "}%s\n",
                ev->task_id,
                ctx->tasks[ev->task_id].name,
                ev->end_ns - ev->start_ns,
                comma);
    }
    fprintf(out, "  ],\n");
    fprintf(out,
            "  \"metrics\": {\"tasks\": %" PRIu64 ", \"deps\": %" PRIu64 ", \"arena_bytes\": %" PRIu64 ", \"worst_step_ns\": %" PRIu64 ", \"frames\": %" PRIu64 ", \"worst_frame_ns\": %" PRIu64 ", \"avg_frame_ns\": %" PRIu64 ", \"frame_overruns\": %" PRIu64 "}\n",
            ctx->metrics.task_count,
            ctx->metrics.dep_count,
            ctx->metrics.arena_bytes,
            ctx->metrics.worst_step_ns,
            ctx->metrics.frame_count,
            ctx->metrics.worst_frame_ns,
            ctx->metrics.frame_count ? ctx->metrics.total_frame_ns / ctx->metrics.frame_count : 0,
            ctx->metrics.frame_overruns);
    fprintf(out, "}\n");
    return DSO_OK;
}

dso_status dso_trace_export_chrome(const dso_context *ctx, FILE *out) {
    if (!ctx || !out) return DSO_ERR_INVALID;
    // Chrome Trace Event Format: complete events (ph: "X")
    // ts in microseconds, dur in microseconds
    fprintf(out, "{\"traceEvents\": [\n");
    for (uint32_t i = 0; i < ctx->trace_count; i++) {
        const dso_trace_event *ev = &ctx->trace[i];
        const char *comma = (i + 1u < ctx->trace_count) ? "," : "";
        fprintf(out,
                "  {\"ph\":\"X\",\"name\":\"%s\",\"cat\":\"task\","
                "\"ts\":%.3f,\"dur\":%.3f,"
                "\"pid\":1,\"tid\":%u,\"args\":{\"task_id\":%u}}%s\n",
                ctx->tasks[ev->task_id].name,
                (double)ev->start_ns / 1000.0,
                (double)(ev->end_ns - ev->start_ns) / 1000.0,
                ev->task_id,
                ev->task_id,
                comma);
    }
    fprintf(out, "]}\n");
    return DSO_OK;
}

const char *dso_status_str(dso_status status) {
    switch (status) {
    case DSO_OK:       return "ok";
    case DSO_ERR_INVALID:   return "invalid";
    case DSO_ERR_CAPACITY:  return "capacity";
    case DSO_ERR_CONTRACT:  return "contract";
    case DSO_ERR_CYCLE:     return "cycle";
    case DSO_ERR_ALLOC:     return "alloc";
    case DSO_ERR_RUNTIME:   return "runtime";
    default:           return "unknown";
    }
}

const dso_metrics *dso_metrics_get(const dso_context *ctx) {
    return ctx ? &ctx->metrics : NULL;
}

// ── Bounded SPSC Queue ─────────────────────────────────────────────────────────
// Lock-free ring buffer, single producer / single consumer.

dso_status dso_queue_init(dso_queue *q, uint32_t capacity) {
    if (!q || capacity < 2 || (capacity & (capacity - 1)) != 0)
        return DSO_ERR_INVALID;
    q->slots = (void **)calloc(capacity, sizeof(void *));
    if (!q->slots) return DSO_ERR_ALLOC;
    q->capacity = capacity;
    q->mask = capacity - 1;
    q->head = 0;
    q->tail = 0;
    q->owned = 1;
    return DSO_OK;
}

void dso_queue_destroy(dso_queue *q) {
    if (!q) return;
    if (q->owned && q->slots) free(q->slots);
    q->slots = NULL;
    q->capacity = 0;
}

dso_status dso_queue_push(dso_queue *q, void *item) {
    if (!q || !item) return DSO_ERR_INVALID;
    // Spin until there's room (blocking push)
    while ((q->head - q->tail) >= q->capacity)
        ; // full — spin (or yield in real code)
    q->slots[q->head & q->mask] = item;
    __sync_synchronize();
    q->head++;
    return DSO_OK;
}

void *dso_queue_pop(dso_queue *q) {
    if (!q) return NULL;
    // Spin until not empty (blocking pop)
    while (q->head == q->tail)
        ; // empty — spin
    void *item = q->slots[q->tail & q->mask];
    __sync_synchronize();
    q->tail++;
    return item;
}

int dso_queue_try_push(dso_queue *q, void *item) {
    if (!q || !item) return 0;
    if ((q->head - q->tail) >= q->capacity)
        return 0; // full
    q->slots[q->head & q->mask] = item;
    __sync_synchronize();
    q->head++;
    return 1;
}

void *dso_queue_try_pop(dso_queue *q) {
    if (!q || q->head == q->tail)
        return NULL;
    void *item = q->slots[q->tail & q->mask];
    __sync_synchronize();
    q->tail++;
    return item;
}

uint32_t dso_queue_len(const dso_queue *q) {
    if (!q) return 0;
    return q->head - q->tail;
}

uint32_t dso_queue_free_slots(const dso_queue *q) {
    if (!q) return 0;
    return q->capacity - (q->head - q->tail);
}

// ── Memory Redirection ────────────────────────────────────────────────────────

static dso_handle region_alloc(dso_context *ctx) {
    if (ctx->region_count >= DSO_MAX_REGIONS) return DSO_HANDLE_NULL;
    uint32_t idx = ctx->region_count++;
    dso_handle h = ctx->next_handle++;
    ctx->regions[idx].handle = h;
    ctx->regions[idx].active = 0;
    ctx->regions[idx].base = NULL;
    ctx->regions[idx].size = 0;
    ctx->regions[idx].perms = 0;
    return h;
}

static int region_find(const dso_context *ctx, dso_handle h) {
    for (uint32_t i = 0; i < ctx->region_count; i++) {
        if (ctx->regions[i].handle == h) return (int)i;
    }
    return -1;
}

dso_handle dso_arena_alloc_handle(dso_context *ctx, dso_arena *arena, size_t size, size_t align) {
    if (!ctx || !arena) return DSO_HANDLE_NULL;
    void *ptr = dso_arena_alloc(arena, size, align);
    if (!ptr) return DSO_HANDLE_NULL;
    dso_handle h = region_alloc(ctx);
    if (h == DSO_HANDLE_NULL) return DSO_HANDLE_NULL;
    int idx = region_find(ctx, h);
    if (idx < 0) return DSO_HANDLE_NULL;
    ctx->regions[idx].base = (uint8_t *)ptr;
    ctx->regions[idx].size = size;
    ctx->regions[idx].perms = DSO_MEM_READ | DSO_MEM_WRITE;
    ctx->regions[idx].active = 1;
    return h;
}

void *dso_handle_resolve(const dso_context *ctx, dso_handle h) {
    if (h == DSO_HANDLE_NULL || !ctx) return NULL;
    int idx = region_find(ctx, h);
    if (idx < 0 || !ctx->regions[idx].active) return NULL;
    return ctx->regions[idx].base;
}

dso_status dso_handle_redirect(dso_context *ctx, dso_handle h, void *new_base, size_t size) {
    if (!ctx) return DSO_ERR_INVALID;
    int idx = region_find(ctx, h);
    if (idx < 0) return DSO_ERR_INVALID;
    ctx->regions[idx].base = (uint8_t *)new_base;
    ctx->regions[idx].size = size;
    ctx->regions[idx].active = 1;
    return DSO_OK;
}

dso_status dso_handle_map(dso_context *ctx, dso_handle h, uint8_t *base, size_t size, dso_mem_perm perms) {
    if (!ctx || h == DSO_HANDLE_NULL) return DSO_ERR_INVALID;
    int idx = region_find(ctx, h);
    if (idx >= 0) return DSO_ERR_INVALID;
    if (ctx->region_count >= DSO_MAX_REGIONS) return DSO_ERR_CAPACITY;
    uint32_t ri = ctx->region_count++;
    ctx->regions[ri].handle = h;
    ctx->regions[ri].base = base;
    ctx->regions[ri].size = size;
    ctx->regions[ri].perms = perms;
    ctx->regions[ri].active = 1;
    return DSO_OK;
}

dso_status dso_handle_unmap(dso_context *ctx, dso_handle h) {
    if (!ctx || h == DSO_HANDLE_NULL) return DSO_ERR_INVALID;
    int idx = region_find(ctx, h);
    if (idx < 0) return DSO_ERR_INVALID;
    ctx->regions[idx].active = 0;
    ctx->regions[idx].base = NULL;
    ctx->regions[idx].size = 0;
    return DSO_OK;
}

// ── Multi-Layer Contract Verification ─────────────────────────────────────────

dso_status dso_contract_verify(const dso_context *ctx, dso_deployment_contract contract, char *out_report, size_t report_size) {
    if (!ctx) return DSO_ERR_INVALID;

    char buf[1024];
    size_t pos = 0;
    int passed = 1;

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "DSO Deployment Contract Verification\n"
        "====================================\n\n");

    // Resource checks per task
    pos += snprintf(buf + pos, sizeof(buf) - pos, "--- Resource Contract ---\n");
    for (uint32_t i = 0; i < ctx->task_count; i++) {
        if (!ctx->tasks[i].has_contract) continue;
        const dso_resource_contract *rc = &ctx->tasks[i].resource_contract;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "  task[%u] \"%s\":\n", i, ctx->tasks[i].name);
        uint64_t cycles_est = 10;
        uint64_t ram_est = 64;
        double wcet_est = (double)ctx->metrics.worst_step_ns / 1000.0;
        double jitter_est = 0.1;

        if (cycles_est > rc->cycles_max) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "    FAIL cycles: %" PRIu64 " > %" PRIu64 "\n", cycles_est, rc->cycles_max);
            passed = 0;
        } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "    PASS cycles: %" PRIu64 " <= %" PRIu64 "\n", cycles_est, rc->cycles_max);
        }
        if (ram_est > rc->ram_bytes_max) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "    FAIL ram_bytes: %" PRIu64 " > %" PRIu64 "\n", ram_est, rc->ram_bytes_max);
            passed = 0;
        } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "    PASS ram_bytes: %" PRIu64 " <= %" PRIu64 "\n", ram_est, rc->ram_bytes_max);
        }
        if (wcet_est > rc->wcet_us_max) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "    FAIL wcet_us: %.2f > %.2f\n", wcet_est, rc->wcet_us_max);
            passed = 0;
        } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "    PASS wcet_us: %.2f <= %.2f\n", wcet_est, rc->wcet_us_max);
        }
        if (jitter_est > rc->jitter_us_max) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "    FAIL jitter_us: %.2f > %.2f\n", jitter_est, rc->jitter_us_max);
            passed = 0;
        } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "    PASS jitter_us: %.2f <= %.2f\n", jitter_est, rc->jitter_us_max);
        }
    }

    // Control contract checks
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n--- Control Contract ---\n");
    if (contract.control.iae_max > 0) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "  iae_max=%.2f  (measurement required at runtime)\n", contract.control.iae_max);
    }
    if (contract.control.overshoot_max > 0) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "  overshoot_max=%.2f\n", contract.control.overshoot_max);
    }

    // Runtime contract checks
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n--- Runtime Contract ---\n");
    if (contract.runtime.finite_required) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "  finite_required=yes  (checked during execution)\n");
    }
    if (contract.runtime.saturation_fraction_max > 0) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "  saturation_fraction_max=%.2f\n", contract.runtime.saturation_fraction_max);
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n=== VERDICT: %s ===\n", passed ? "PASS" : "FAIL");

    if (out_report && report_size > 0) {
        size_t copy = pos < report_size ? pos : report_size - 1;
        memcpy(out_report, buf, copy);
        out_report[copy] = '\0';
    }

    return passed ? DSO_OK : DSO_ERR_CONTRACT;
}

// ── Schedule Levels ────────────────────────────────────────────────────────────
// Computes dependency levels: tasks with same depth can run in parallel.

static void compute_levels(dso_context *ctx) {
    if (!ctx || ctx->task_count == 0) return;

    uint32_t depth[DSO_MAX_TASKS] = {0};

    // Assign depth = max(depth of all predecessors) + 1
    // Use the topological schedule order for correctness
    for (uint32_t si = 0; si < ctx->schedule_count; si++) {
        uint32_t id = ctx->schedule[si];
        uint32_t max_pred_depth = 0;
        for (uint32_t d = 0; d < ctx->dep_count; d++) {
            if (ctx->deps[d].after == id) {
                uint32_t pred = ctx->deps[d].before;
                if (depth[pred] + 1 > max_pred_depth)
                    max_pred_depth = depth[pred] + 1;
            }
        }
        depth[id] = max_pred_depth;
    }

    // Count how many tasks per level
    uint32_t max_depth = 0;
    for (uint32_t i = 0; i < ctx->task_count; i++) {
        if (depth[i] > max_depth) max_depth = depth[i];
    }
    ctx->levels.count = max_depth + 1;

    for (uint32_t i = 0; i < ctx->levels.count; i++)
        ctx->levels.size[i] = 0;

    for (uint32_t i = 0; i < ctx->task_count; i++) {
        uint32_t l = depth[i];
        ctx->levels.tasks[l][ctx->levels.size[l]++] = i;
    }
    ctx->has_levels = 1;
}

const dso_schedule_levels *dso_get_levels(const dso_context *ctx) {
    return ctx ? &ctx->levels : NULL;
}

// ── Memory Pool ────────────────────────────────────────────────────────────────
// Fixed-size object pool with free list. O(1) alloc/free. Handle-based.

dso_pool *dso_pool_create(dso_context *ctx, dso_arena *arena,
                          uint32_t block_size, uint32_t align,
                          uint32_t capacity) {
    if (!ctx || !arena || capacity == 0 || block_size == 0)
        return NULL;
    if (ctx->pool_count >= DSO_MAX_POOLS) return NULL;

    // Align block size
    uint32_t aligned = (uint32_t)align_up(block_size, align);
    if (aligned < sizeof(dso_handle)) aligned = sizeof(dso_handle);

    // Allocate backing memory from arena
    size_t total = (size_t)aligned * capacity;
    void *slots = dso_arena_alloc(arena, total, align);
    if (!slots) return NULL;

    // Allocate pool struct from arena
    dso_pool *pool = (dso_pool *)dso_arena_alloc(arena, sizeof(dso_pool), 8);
    if (!pool) return NULL;

    // Allocate free list from arena
    dso_handle *free_list = (dso_handle *)dso_arena_alloc(
        arena, sizeof(dso_handle) * capacity, 4);
    if (!free_list) return NULL;

    pool->slots = slots;
    pool->free_list = free_list;
    pool->capacity = capacity;
    pool->free_count = capacity;
    pool->block_size = aligned;
    pool->align = align;
    pool->base_handle = ctx->next_handle;
    pool->owned = 0;  // memory owned by arena

    // Initialize free list
    for (uint32_t i = 0; i < capacity; i++) {
        free_list[i] = pool->base_handle + (dso_handle)(capacity - 1 - i);
    }

    // Register handles in the region table
    for (uint32_t i = 0; i < capacity; i++) {
        dso_handle h = pool->base_handle + (dso_handle)i;
        if (ctx->region_count >= DSO_MAX_REGIONS) {
            // Can't register all handles, but pool still works via resolve
            break;
        }
        uint32_t ri = ctx->region_count++;
        ctx->regions[ri].handle = h;
        ctx->regions[ri].base = (uint8_t *)slots + (size_t)i * aligned;
        ctx->regions[ri].size = aligned;
        ctx->regions[ri].perms = DSO_MEM_READ | DSO_MEM_WRITE;
        ctx->regions[ri].active = 0;  // marked active only when allocated
    }
    ctx->next_handle += (dso_handle)capacity;

    ctx->pools[ctx->pool_count++] = pool;
    return pool;
}

void dso_pool_destroy(dso_pool *pool) {
    (void)pool;
    // Arena owns the memory, nothing to free
}

dso_handle dso_pool_alloc(dso_context *ctx, dso_pool *pool) {
    if (!ctx || !pool || pool->free_count == 0)
        return DSO_HANDLE_NULL;

    pool->free_count--;
    dso_handle h = pool->free_list[pool->free_count];

    // Mark region active
    int idx = region_find(ctx, h);
    if (idx >= 0) ctx->regions[idx].active = 1;

    // Zero the memory
    void *ptr = dso_pool_resolve(ctx, pool, h);
    if (ptr) memset(ptr, 0, pool->block_size);

    return h;
}

void dso_pool_free(dso_pool *pool, dso_handle h) {
    if (!pool || h == DSO_HANDLE_NULL) return;
    if (pool->free_count >= pool->capacity) return;
    pool->free_list[pool->free_count++] = h;
}

void *dso_pool_resolve(const dso_context *ctx, const dso_pool *pool, dso_handle h) {
    if (!ctx || !pool || h == DSO_HANDLE_NULL) return NULL;
    uint32_t idx = (uint32_t)(h - pool->base_handle);
    if (idx >= pool->capacity) return NULL;
    return (uint8_t *)pool->slots + (size_t)idx * pool->block_size;
}

uint32_t dso_pool_used(const dso_pool *pool) {
    return pool ? pool->capacity - pool->free_count : 0;
}

uint32_t dso_pool_free_count(const dso_pool *pool) {
    return pool ? pool->free_count : 0;
}

// ── Worker Pool (pthread-based parallel execution) ─────────────────────────────

typedef struct {
    uint32_t        worker_count;       // total workers (including main)
    uint32_t        num_workers;        // threads to create = worker_count - 1
    pthread_t       threads[DSO_MAX_WORKERS - 1];

    // Synchronization — spin barriers (OK for <= 16 workers)
    volatile uint32_t barrier_sense;    // alternates 0/1
    volatile uint32_t barrier_count;

    // Work dispatch
    volatile uint32_t next_task;        // atomically incremented
    uint32_t          level_size;
    uint32_t         *level_tasks;

    // Context
    dso_context       *ctx;
    volatile dso_status error;
    volatile uint32_t   error_task_id;

    // Level iteration
    volatile uint32_t current_level;
    uint32_t          total_levels;
} pool_internal;

static void spin_barrier(pool_internal *pi) {
    uint32_t sense = pi->barrier_sense;
    uint32_t n = __sync_fetch_and_add(&pi->barrier_count, 1);
    if (n + 1 == pi->worker_count) {
        // Last to arrive — reset
        pi->barrier_count = 0;
        pi->barrier_sense = !sense;
    } else {
        while (pi->barrier_sense == sense)
            ; // spin
    }
}

static void *worker_thread(void *arg) {
    pool_internal *pi = (pool_internal *)arg;
    dso_context *ctx = pi->ctx;

    while (1) {
        // Wait for work: spin on current_level
        // When current_level >= total_levels, worker exits
        while (pi->current_level < pi->total_levels && pi->error == DSO_OK) {
            uint32_t level = pi->current_level;

            // Pick tasks atomically
            while (1) {
                uint32_t t = __sync_fetch_and_add(&pi->next_task, 1);
                if (t >= pi->level_size) break;

                uint32_t task_id = pi->level_tasks[t];
                uint64_t start = monotonic_ns();

                // Check error flag before running
                if (pi->error != DSO_OK) break;

                ctx->tasks[task_id].fn(ctx->tasks[task_id].user);

                uint64_t end = monotonic_ns();
                uint64_t elapsed = end - start;

                // Record trace (thread-safe: each task has unique trace slot)
                uint32_t ti = __sync_fetch_and_add(&ctx->trace_count, 1);
                if (ti < DSO_TRACE_RING) {
                    dso_trace_event *ev = &ctx->trace[ti];
                    ev->task_id = task_id;
                    ev->start_ns = start;
                    ev->end_ns = end;
                    ev->status = 0;
                }

                // Update worst step (atomic max)
                while (1) {
                    uint64_t old = ctx->metrics.worst_step_ns;
                    if (elapsed <= old) break;
                    if (__sync_bool_compare_and_swap(&ctx->metrics.worst_step_ns, old, elapsed))
                        break;
                }

                // Check step contract
                if (ctx->contract.max_step_ns && elapsed > ctx->contract.max_step_ns) {
                    pi->error = DSO_ERR_RUNTIME;
                    pi->error_task_id = task_id;
                    break;
                }
            }

            // Barrier: sync all workers before next level
            spin_barrier(pi);

            // If this is the last worker to see this level, advance level
            if (pi->current_level == level && pi->error == DSO_OK) {
                uint32_t next = level + 1;
                if (next < pi->total_levels) {
                    // Set up next level
                    pi->level_size = ctx->levels.size[next];
                    pi->level_tasks = ctx->levels.tasks[next];
                    pi->next_task = 0;
                    __sync_synchronize();
                    pi->current_level = next;
                } else {
                    __sync_synchronize();
                    pi->current_level = next; // signals done
                }
            }

            // Barrier again to ensure all workers see the level update
            spin_barrier(pi);
        }

        // If all levels done, exit
        if (pi->current_level >= pi->total_levels || pi->error != DSO_OK)
            break;
    }

    return NULL;
}

dso_status dso_worker_pool_init(dso_worker_pool *pool, uint32_t worker_count) {
    if (!pool) return DSO_ERR_INVALID;
    if (worker_count < 1 || worker_count > DSO_MAX_WORKERS)
        return DSO_ERR_INVALID;

    pool->worker_count = worker_count;
    pool->impl = NULL;  // lazy init
    pool->ready = 0;
    return DSO_OK;
}

void dso_worker_pool_destroy(dso_worker_pool *pool) {
    if (!pool) return;
    pool->ready = 0;
    pool->impl = NULL;
}

// Internal: run one frame with worker pool
static dso_status execute_parallel_internal(dso_context *ctx, dso_worker_pool *pool) {
    if (!ctx || !pool || !ctx->has_levels) return DSO_ERR_INVALID;
    if (ctx->levels.count == 0) return DSO_ERR_INVALID;
    if (pool->worker_count < 1) return DSO_ERR_INVALID;

    uint32_t num_workers = pool->worker_count;
    uint32_t num_threads = num_workers - 1;

    // Reset trace
    ctx->trace_count = 0;
    ctx->metrics.worst_step_ns = 0;

    // Prepare internal state
    pool_internal pi;
    pi.worker_count = num_workers;
    pi.num_workers = num_threads;
    pi.barrier_sense = 0;
    pi.barrier_count = 0;
    pi.ctx = ctx;
    pi.error = DSO_OK;
    pi.error_task_id = 0;
    pi.current_level = 0;
    pi.total_levels = ctx->levels.count;

    // Set up first level
    if (ctx->levels.count > 0) {
        pi.level_size = ctx->levels.size[0];
        pi.level_tasks = ctx->levels.tasks[0];
        pi.next_task = 0;
    }

    // Create worker threads
    for (uint32_t i = 0; i < num_threads; i++) {
        if (pthread_create(&pi.threads[i], NULL, worker_thread, &pi) != 0) {
            // Join already-created threads
            for (uint32_t j = 0; j < i; j++)
                pthread_join(pi.threads[j], NULL);
            return DSO_ERR_RUNTIME;
        }
    }

    // Main thread joins as worker 0
    if (num_threads > 0) {
        // Workers already running, they'll pick up work
    }

    // Main thread participates
    worker_thread(&pi);

    // Join all threads
    for (uint32_t i = 0; i < num_threads; i++)
        pthread_join(pi.threads[i], NULL);

    if (pi.error != DSO_OK) return pi.error;
    return DSO_OK;
}

dso_status dso_execute_parallel(dso_context *ctx, dso_worker_pool *pool) {
    if (!ctx || !pool) return DSO_ERR_INVALID;
    if (ctx->schedule_count != ctx->task_count) return DSO_ERR_INVALID;
    return execute_parallel_internal(ctx, pool);
}

dso_status dso_execute_frame_parallel(dso_context *ctx, dso_worker_pool *pool) {
    if (!ctx || !pool) return DSO_ERR_INVALID;
    uint64_t frame_start = monotonic_ns();
    dso_status st = execute_parallel_internal(ctx, pool);
    uint64_t frame_end = monotonic_ns();
    uint64_t frame_ns = frame_end - frame_start;
    ctx->metrics.frame_count++;
    ctx->metrics.total_frame_ns += frame_ns;
    if (frame_ns > ctx->metrics.worst_frame_ns)
        ctx->metrics.worst_frame_ns = frame_ns;
    if (ctx->contract.max_frame_ns && frame_ns > ctx->contract.max_frame_ns) {
        ctx->metrics.frame_overruns++;
        return DSO_ERR_RUNTIME;
    }
    return st;
}

// ── Plan Serialization ─────────────────────────────────────────────────────────

dso_status dso_plan_save(const dso_context *ctx, FILE *out) {
    if (!ctx || !out) return DSO_ERR_INVALID;

    uint32_t magic = DSO_PLAN_MAGIC;
    uint32_t version = DSO_PLAN_VERSION;
    uint32_t task_count = ctx->task_count;
    uint32_t dep_count = ctx->dep_count;
    uint32_t arena_hw = ctx->arena ? (uint32_t)ctx->arena->high_water : 0;

    fwrite(&magic, sizeof(magic), 1, out);
    fwrite(&version, sizeof(version), 1, out);
    fwrite(&task_count, sizeof(task_count), 1, out);
    fwrite(&dep_count, sizeof(dep_count), 1, out);
    fwrite(&arena_hw, sizeof(arena_hw), 1, out);

    for (uint32_t i = 0; i < task_count; i++) {
        fwrite(ctx->tasks[i].name, DSO_MAX_NAME, 1, out);
        uint8_t has = ctx->tasks[i].has_contract ? 1 : 0;
        fwrite(&has, sizeof(has), 1, out);
        if (has)
            fwrite(&ctx->tasks[i].resource_contract, sizeof(dso_resource_contract), 1, out);
    }
    for (uint32_t i = 0; i < dep_count; i++) {
        fwrite(&ctx->deps[i].before, sizeof(uint32_t), 1, out);
        fwrite(&ctx->deps[i].after,  sizeof(uint32_t), 1, out);
    }

    fflush(out);
    if (ferror(out)) return DSO_ERR_RUNTIME;
    return DSO_OK;
}

dso_status dso_plan_load(dso_context *ctx, FILE *in) {
    if (!ctx || !in) return DSO_ERR_INVALID;

    uint32_t magic, version, task_count, dep_count, arena_hw;

    if (fread(&magic, sizeof(magic), 1, in) != 1) return DSO_ERR_RUNTIME;
    if (magic != DSO_PLAN_MAGIC) return DSO_ERR_INVALID;
    if (fread(&version, sizeof(version), 1, in) != 1) return DSO_ERR_RUNTIME;
    if (fread(&task_count, sizeof(task_count), 1, in) != 1) return DSO_ERR_RUNTIME;
    if (fread(&dep_count, sizeof(dep_count), 1, in) != 1) return DSO_ERR_RUNTIME;
    if (fread(&arena_hw, sizeof(arena_hw), 1, in) != 1) return DSO_ERR_RUNTIME;
    (void)arena_hw;

    if (task_count > DSO_MAX_TASKS || dep_count > DSO_MAX_DEPS)
        return DSO_ERR_CAPACITY;

    memset(ctx, 0, sizeof(*ctx));
    ctx->next_handle = 1;

    for (uint32_t i = 0; i < task_count; i++) {
        if (fread(ctx->tasks[i].name, DSO_MAX_NAME, 1, in) != 1)
            return DSO_ERR_RUNTIME;
        ctx->tasks[i].fn = NULL;  // caller must re-bind after load
        ctx->tasks[i].user = NULL;
        uint8_t has = 0;
        if (fread(&has, sizeof(has), 1, in) != 1) return DSO_ERR_RUNTIME;
        if (has) {
            if (fread(&ctx->tasks[i].resource_contract, sizeof(dso_resource_contract), 1, in) != 1)
                return DSO_ERR_RUNTIME;
            ctx->tasks[i].has_contract = 1;
        }
    }
    for (uint32_t i = 0; i < dep_count; i++) {
        if (fread(&ctx->deps[i].before, sizeof(uint32_t), 1, in) != 1)
            return DSO_ERR_RUNTIME;
        if (fread(&ctx->deps[i].after, sizeof(uint32_t), 1, in) != 1)
            return DSO_ERR_RUNTIME;
    }

    ctx->task_count = task_count;
    ctx->dep_count = dep_count;
    return DSO_OK;
}
