#ifndef DSO_H
#define DSO_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSO_MAX_TASKS 128
#define DSO_MAX_DEPS 512
#define DSO_MAX_NAME 32
#define DSO_TRACE_RING 256

typedef enum {
    DSO_OK = 0,
    DSO_ERR_INVALID = -1,
    DSO_ERR_CAPACITY = -2,
    DSO_ERR_CONTRACT = -3,
    DSO_ERR_CYCLE = -4,
    DSO_ERR_ALLOC = -5,
    DSO_ERR_RUNTIME = -6
} dso_status;

typedef struct {
    uint8_t *base;
    size_t capacity;
    size_t offset;
    size_t high_water;
} dso_arena;

typedef struct {
    size_t max_tasks;
    size_t max_deps;
    size_t max_arena_bytes;
    uint64_t max_plan_steps;
    uint64_t max_step_ns;
    uint64_t max_frame_ns;
} dso_contract;

typedef struct {
    uint64_t task_count;
    uint64_t dep_count;
    uint64_t arena_bytes;
    uint64_t plan_steps;
    uint64_t worst_step_ns;
    uint64_t frame_count;
    uint64_t worst_frame_ns;
    uint64_t total_frame_ns;
    uint64_t frame_overruns;
} dso_metrics;

typedef struct {
    int code;
    char message[160];
    dso_metrics metrics;
} dso_verdict;

typedef struct dso_context dso_context;
typedef void (*dso_task_fn)(void *user);

typedef struct {
    uint32_t task_id;
    uint64_t start_ns;
    uint64_t end_ns;
    int status;
} dso_trace_event;

void dso_arena_init(dso_arena *arena, void *memory, size_t size);
void *dso_arena_alloc(dso_arena *arena, size_t size, size_t align);
void dso_arena_reset(dso_arena *arena);

void dso_context_init(dso_context *ctx, dso_arena *arena);
dso_status dso_task(dso_context *ctx, const char *name, dso_task_fn fn, void *user, uint32_t *out_id);
dso_status dso_dep(dso_context *ctx, uint32_t before, uint32_t after);
dso_status dso_compile(dso_context *ctx, dso_contract contract, dso_verdict *verdict);
dso_status dso_execute(dso_context *ctx);
dso_status dso_execute_frame(dso_context *ctx);
dso_status dso_run_frames(dso_context *ctx, uint64_t frames);
dso_status dso_write_trace_json(const dso_context *ctx, FILE *out);

const char *dso_status_str(dso_status status);
const dso_metrics *dso_metrics_get(const dso_context *ctx);

struct dso_context {
    dso_arena *arena;
    dso_contract contract;
    dso_metrics metrics;
    uint32_t task_count;
    uint32_t dep_count;
    uint32_t schedule_count;
    struct {
        char name[DSO_MAX_NAME];
        dso_task_fn fn;
        void *user;
    } tasks[DSO_MAX_TASKS];
    struct {
        uint32_t before;
        uint32_t after;
    } deps[DSO_MAX_DEPS];
    uint32_t schedule[DSO_MAX_TASKS];
    dso_trace_event trace[DSO_TRACE_RING];
    uint32_t trace_count;
};

#ifdef __cplusplus
}
#endif

#endif
