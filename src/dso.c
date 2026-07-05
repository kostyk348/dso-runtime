#include "dso.h"

#include <inttypes.h>
#include <stdio.h>
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
    if (!arena) {
        return;
    }
    arena->base = (uint8_t *)memory;
    arena->capacity = size;
    arena->offset = 0;
    arena->high_water = 0;
}

void *dso_arena_alloc(dso_arena *arena, size_t size, size_t align) {
    if (!arena || !arena->base || align == 0 || (align & (align - 1u)) != 0) {
        return NULL;
    }
    size_t start = align_up(arena->offset, align);
    if (start > arena->capacity || size > arena->capacity - start) {
        return NULL;
    }
    arena->offset = start + size;
    if (arena->offset > arena->high_water) {
        arena->high_water = arena->offset;
    }
    return arena->base + start;
}

void dso_arena_reset(dso_arena *arena) {
    if (arena) {
        arena->offset = 0;
    }
}

void dso_context_init(dso_context *ctx, dso_arena *arena) {
    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->arena = arena;
}

dso_status dso_task(dso_context *ctx, const char *name, dso_task_fn fn, void *user, uint32_t *out_id) {
    if (!ctx || !name || !fn) {
        return DSO_ERR_INVALID;
    }
    if (ctx->task_count >= DSO_MAX_TASKS) {
        return DSO_ERR_CAPACITY;
    }
    uint32_t id = ctx->task_count++;
    snprintf(ctx->tasks[id].name, sizeof(ctx->tasks[id].name), "%s", name);
    ctx->tasks[id].fn = fn;
    ctx->tasks[id].user = user;
    if (out_id) {
        *out_id = id;
    }
    return DSO_OK;
}

dso_status dso_dep(dso_context *ctx, uint32_t before, uint32_t after) {
    if (!ctx || before >= ctx->task_count || after >= ctx->task_count || before == after) {
        return DSO_ERR_INVALID;
    }
    if (ctx->dep_count >= DSO_MAX_DEPS) {
        return DSO_ERR_CAPACITY;
    }
    ctx->deps[ctx->dep_count].before = before;
    ctx->deps[ctx->dep_count].after = after;
    ctx->dep_count++;
    return DSO_OK;
}

static void verdict_set(dso_verdict *verdict, dso_status code, const char *message, dso_metrics metrics) {
    if (!verdict) {
        return;
    }
    verdict->code = code;
    snprintf(verdict->message, sizeof(verdict->message), "%s", message);
    verdict->metrics = metrics;
}

dso_status dso_compile(dso_context *ctx, dso_contract contract, dso_verdict *verdict) {
    if (!ctx) {
        return DSO_ERR_INVALID;
    }
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
    for (uint32_t i = 0; i < ctx->dep_count; i++) {
        indegree[ctx->deps[i].after]++;
    }

    while (ctx->schedule_count < ctx->task_count) {
        int found = 0;
        for (uint32_t id = 0; id < ctx->task_count; id++) {
            if (!emitted[id] && indegree[id] == 0) {
                emitted[id] = 1;
                ctx->schedule[ctx->schedule_count++] = id;
                for (uint32_t d = 0; d < ctx->dep_count; d++) {
                    if (ctx->deps[d].before == id) {
                        indegree[ctx->deps[d].after]--;
                    }
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

    verdict_set(verdict, DSO_OK, "plan verified", ctx->metrics);
    return DSO_OK;
}

dso_status dso_execute(dso_context *ctx) {
    if (!ctx || ctx->schedule_count != ctx->task_count) {
        return DSO_ERR_INVALID;
    }
    ctx->trace_count = 0;
    ctx->metrics.worst_step_ns = 0;
    for (uint32_t i = 0; i < ctx->schedule_count; i++) {
        uint32_t id = ctx->schedule[i];
        uint64_t start = monotonic_ns();
        ctx->tasks[id].fn(ctx->tasks[id].user);
        uint64_t end = monotonic_ns();
        uint64_t elapsed = end - start;
        if (elapsed > ctx->metrics.worst_step_ns) {
            ctx->metrics.worst_step_ns = elapsed;
        }
        if (ctx->trace_count < DSO_TRACE_RING) {
            dso_trace_event *ev = &ctx->trace[ctx->trace_count++];
            ev->task_id = id;
            ev->start_ns = start;
            ev->end_ns = end;
            ev->status = 0;
        }
        if (ctx->contract.max_step_ns && elapsed > ctx->contract.max_step_ns) {
            return DSO_ERR_RUNTIME;
        }
    }
    return DSO_OK;
}

dso_status dso_execute_frame(dso_context *ctx) {
    if (!ctx || ctx->schedule_count != ctx->task_count) {
        return DSO_ERR_INVALID;
    }

    uint64_t frame_start = monotonic_ns();
    dso_status st = dso_execute(ctx);
    uint64_t frame_end = monotonic_ns();
    uint64_t frame_ns = frame_end - frame_start;

    ctx->metrics.frame_count++;
    ctx->metrics.total_frame_ns += frame_ns;
    if (frame_ns > ctx->metrics.worst_frame_ns) {
        ctx->metrics.worst_frame_ns = frame_ns;
    }
    if (ctx->contract.max_frame_ns && frame_ns > ctx->contract.max_frame_ns) {
        ctx->metrics.frame_overruns++;
        return DSO_ERR_RUNTIME;
    }
    return st;
}

dso_status dso_run_frames(dso_context *ctx, uint64_t frames) {
    if (!ctx || frames == 0) {
        return DSO_ERR_INVALID;
    }
    dso_status last = DSO_OK;
    for (uint64_t i = 0; i < frames; i++) {
        last = dso_execute_frame(ctx);
        if (last != DSO_OK) {
            return last;
        }
    }
    return last;
}

dso_status dso_write_trace_json(const dso_context *ctx, FILE *out) {
    if (!ctx || !out) {
        return DSO_ERR_INVALID;
    }
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

const char *dso_status_str(dso_status status) {
    switch (status) {
    case DSO_OK:
        return "ok";
    case DSO_ERR_INVALID:
        return "invalid";
    case DSO_ERR_CAPACITY:
        return "capacity";
    case DSO_ERR_CONTRACT:
        return "contract";
    case DSO_ERR_CYCLE:
        return "cycle";
    case DSO_ERR_ALLOC:
        return "alloc";
    case DSO_ERR_RUNTIME:
        return "runtime";
    default:
        return "unknown";
    }
}

const dso_metrics *dso_metrics_get(const dso_context *ctx) {
    return ctx ? &ctx->metrics : NULL;
}
