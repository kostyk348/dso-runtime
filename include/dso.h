#ifndef DSO_H
#define DSO_H

/**
 * @file dso.h
 * @brief DSO Runtime — deterministic task graph execution framework.
 *
 * Core primitives: arena allocator, memory handles, typed pools, SPSC queues,
 * task graphs with dependency levels, multi-layer contracts, parallel execution.
 *
 * Typical workflow:
 *   1. dso_arena_init + dso_context_init
 *   2. dso_task / dso_dep to build graph
 *   3. dso_compile to verify and schedule
 *   4. dso_execute / dso_execute_parallel to run
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Constants ─────────────────────────────────────────────────────────────────

#define DSO_MAX_TASKS    128   ///< Maximum tasks per graph
#define DSO_MAX_DEPS     512   ///< Maximum dependencies per graph
#define DSO_MAX_NAME      32   ///< Maximum task name length
#define DSO_TRACE_RING   256   ///< Trace event ring buffer size
#define DSO_MAX_REGIONS   64   ///< Maximum memory region table entries
#define DSO_MAX_QUEUES    64   ///< Maximum queues per context
#define DSO_MAX_POOLS     16   ///< Maximum memory pools per context
#define DSO_MAX_WORKERS   16   ///< Maximum worker threads for parallel execution
#define DSO_HANDLE_NULL    0   ///< Sentinel for invalid handle

// ── Status Codes ──────────────────────────────────────────────────────────────

/** @brief Operation result codes. */
typedef enum {
    DSO_OK              =  0,  ///< Success
    DSO_ERR_INVALID     = -1,  ///< Invalid argument or state
    DSO_ERR_CAPACITY    = -2,  ///< Capacity exceeded
    DSO_ERR_CONTRACT    = -3,  ///< Contract verification failed
    DSO_ERR_CYCLE       = -4,  ///< Dependency graph has a cycle
    DSO_ERR_ALLOC       = -5,  ///< Memory allocation failed
    DSO_ERR_RUNTIME     = -6   ///< Runtime error (e.g. step overrun)
} dso_status;

/** @brief Opaque handle for memory redirection. */
typedef uint32_t dso_handle;

/** @brief Memory permission flags. */
typedef enum {
    DSO_MEM_READ  = 1,
    DSO_MEM_WRITE = 2,
    DSO_MEM_EXEC  = 4,
} dso_mem_perm;

// ── Forward declarations ──────────────────────────────────────────────────────

typedef struct dso_context dso_context;
typedef void (*dso_task_fn)(void *user);

// ── Arena Allocator ────────────────────────────────────────────────────────────

/**
 * @brief Bump-pointer arena allocator.
 * All memory is pre-allocated by the user; arena does not call malloc/free.
 */
typedef struct {
    uint8_t *base;      ///< Backing memory
    size_t   capacity;  ///< Total size
    size_t   offset;    ///< Current allocation offset
    size_t   high_water;///< Peak offset reached (for contract checks)
} dso_arena;

/** @brief Initialize arena with pre-allocated memory. */
void dso_arena_init(dso_arena *arena, void *memory, size_t size);

/** @brief Bump-allocate from arena. Returns NULL if capacity exceeded. */
void *dso_arena_alloc(dso_arena *arena, size_t size, size_t align);

/** @brief Reset arena offset (does not free backing memory). */
void dso_arena_reset(dso_arena *arena);

// ── Memory Redirection (Handles) ──────────────────────────────────────────────

/**
 * @brief A mapped memory region tracked by handle.
 * Supports double-buffering via dso_handle_redirect.
 */
typedef struct {
    uint8_t     *base;
    size_t       size;
    dso_handle   handle;
    dso_mem_perm perms;
    uint8_t      active;
} dso_mem_region;

/** @brief Allocate from arena and return a handle pointing to it. */
dso_handle dso_arena_alloc_handle(dso_context *ctx, dso_arena *arena, size_t size, size_t align);

/** @brief Resolve a handle to its current memory pointer. Returns NULL if inactive. */
void *dso_handle_resolve(const dso_context *ctx, dso_handle h);

/** @brief Redirect a handle to point at a different memory region (double-buffering). */
dso_status dso_handle_redirect(dso_context *ctx, dso_handle h, void *new_base, size_t size);

/** @brief Map an externally-owned memory region to a handle. */
dso_status dso_handle_map(dso_context *ctx, dso_handle h, uint8_t *base, size_t size, dso_mem_perm perms);

/** @brief Unmap a handle (marks region inactive). */
dso_status dso_handle_unmap(dso_context *ctx, dso_handle h);

// ── Typed Memory Pool ─────────────────────────────────────────────────────────

/**
 * @brief Fixed-size object pool with O(1) alloc/free.
 * Handle-based allocation via free list. Backed by arena memory.
 */
typedef struct {
    void       *slots;         ///< Backing memory (block_size * capacity)
    dso_handle *free_list;     ///< LIFO free list of handles
    uint32_t    capacity;
    uint32_t    free_count;
    uint32_t    block_size;
    uint32_t    align;
    dso_handle  base_handle;   ///< First handle in this pool
    uint8_t     owned;         ///< 1 = pool owns slots memory (calloc'd)
} dso_pool;

/** @brief Create a typed pool. Allocates metadata from arena. */
dso_pool  *dso_pool_create(dso_context *ctx, dso_arena *arena,
                           uint32_t block_size, uint32_t align,
                           uint32_t capacity);
/** @brief Destroy pool (no-op if arena-backed). */
void       dso_pool_destroy(dso_pool *pool);
/** @brief Allocate an object from pool. Returns handle. */
dso_handle dso_pool_alloc(dso_context *ctx, dso_pool *pool);
/** @brief Free an object back to pool. */
void       dso_pool_free(dso_pool *pool, dso_handle h);
/** @brief Resolve pool handle to pointer (direct pointer math, not region table). */
void      *dso_pool_resolve(const dso_context *ctx, const dso_pool *pool, dso_handle h);
/** @brief Number of used slots. */
uint32_t   dso_pool_used(const dso_pool *pool);
/** @brief Number of free slots. */
uint32_t   dso_pool_free_count(const dso_pool *pool);

// ── Bounded SPSC Queue ──────────────────────────────────────────────────────

/**
 * @brief Lock-free single-producer single-consumer bounded queue.
 * Capacity must be a power of 2.
 */
typedef struct {
    void   **slots;
    uint32_t capacity;
    uint32_t mask;
    uint32_t head;      ///< Producer write index
    uint32_t tail;      ///< Consumer read index
    uint8_t  owned;     ///< 1 = queue owns slots memory
} dso_queue;

/** @brief Initialize queue with given capacity (must be power of 2). */
dso_status dso_queue_init(dso_queue *q, uint32_t capacity);
/** @brief Destroy queue, free slots if owned. */
void       dso_queue_destroy(dso_queue *q);
/** @brief Push item (blocking — spins if full). */
dso_status dso_queue_push(dso_queue *q, void *item);
/** @brief Pop item (blocking — spins if empty). */
void      *dso_queue_pop(dso_queue *q);
/** @brief Try push. Returns 0 if full, 1 on success. */
int        dso_queue_try_push(dso_queue *q, void *item);
/** @brief Try pop. Returns NULL if empty. */
void      *dso_queue_try_pop(dso_queue *q);
/** @brief Current number of items in queue. */
uint32_t   dso_queue_len(const dso_queue *q);
/** @brief Number of free slots. */
uint32_t   dso_queue_free_slots(const dso_queue *q);

// ── Contracts ─────────────────────────────────────────────────────────────────

/**
 * @brief Compile-time contract: limits checked during dso_compile.
 * Set to 0 to disable a check.
 */
typedef struct {
    size_t   max_tasks;       ///< Max allowed tasks
    size_t   max_deps;        ///< Max allowed dependencies
    size_t   max_arena_bytes; ///< Max arena high-water mark
    uint64_t max_plan_steps;  ///< Max execution plan steps
    uint64_t max_step_ns;     ///< Max nanoseconds per task step
    uint64_t max_frame_ns;    ///< Max nanoseconds per frame
} dso_contract;

/** @brief Per-task resource budget. */
typedef struct {
    uint64_t cycles_max;
    uint64_t ram_bytes_max;
    double   wcet_us_max;
    double   jitter_us_max;
} dso_resource_contract;

/** @brief Control-loop quality constraints. */
typedef struct {
    double  iae_max;
    double  overshoot_max;
    double  max_abs_y;
    double  final_error_max;
} dso_control_contract;

/** @brief Runtime execution guarantees. */
typedef struct {
    uint8_t finite_required;
    double  saturation_fraction_max;
} dso_runtime_contract;

/** @brief Full deployment contract: resource + control + runtime. */
typedef struct {
    dso_resource_contract resource;
    dso_control_contract  control;
    dso_runtime_contract  runtime;
} dso_deployment_contract;

// ── Metrics & Verdict ─────────────────────────────────────────────────────────

/** @brief Execution metrics (accumulated across frames). */
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

/** @brief Compilation verdict: status + human-readable message + metrics snapshot. */
typedef struct {
    int code;
    char message[160];
    dso_metrics metrics;
} dso_verdict;

// ── Task & Trace ──────────────────────────────────────────────────────────────

/** @brief A single trace event recorded during dso_execute. */
typedef struct {
    uint32_t task_id;
    uint64_t start_ns;
    uint64_t end_ns;
    int      status;
} dso_trace_event;

// ── Schedule Levels (for parallel execution) ──────────────────────────────────

/** @brief Dependency levels computed during dso_compile. */
typedef struct {
    uint32_t count;                                ///< Number of levels
    uint32_t size[DSO_MAX_TASKS];                  ///< Tasks per level
    uint32_t tasks[DSO_MAX_TASKS][DSO_MAX_TASKS]; ///< Task IDs per level
} dso_schedule_levels;

// ── Core API ──────────────────────────────────────────────────────────────────

/** @brief Initialize an execution context with an arena. */
void dso_context_init(dso_context *ctx, dso_arena *arena);

/** @brief Register a task. @p out_id receives the assigned task ID (0-based). */
dso_status dso_task(dso_context *ctx, const char *name, dso_task_fn fn, void *user, uint32_t *out_id);

/** @brief Add a dependency: @p before must complete before @p after starts. */
dso_status dso_dep(dso_context *ctx, uint32_t before, uint32_t after);

/**
 * @brief Topological sort + contract verification.
 * Must be called before dso_execute / dso_execute_parallel.
 */
dso_status dso_compile(dso_context *ctx, dso_contract contract, dso_verdict *verdict);

/** @brief Execute all tasks sequentially (one frame). */
dso_status dso_execute(dso_context *ctx);

/** @brief Execute one frame with frame timing and overrun detection. */
dso_status dso_execute_frame(dso_context *ctx);

/** @brief Execute @p frames sequential frames. */
dso_status dso_run_frames(dso_context *ctx, uint64_t frames);

/** @brief Write execution trace in DSO JSON format. */
dso_status dso_write_trace_json(const dso_context *ctx, FILE *out);

/** @brief Export trace in Chrome Trace Event Format (for chrome://tracing). */
dso_status dso_trace_export_chrome(const dso_context *ctx, FILE *out);

/** @brief Return human-readable name for a status code. */
const char *dso_status_str(dso_status status);

/** @brief Return pointer to accumulated metrics. */
const dso_metrics *dso_metrics_get(const dso_context *ctx);

/**
 * @brief Verify deployment contract against the compiled graph.
 * Produces a human-readable report.
 */
dso_status dso_contract_verify(const dso_context *ctx, dso_deployment_contract contract,
                               char *out_report, size_t report_size);

/** @brief Attach a resource contract to a specific task. */
dso_status dso_task_set_contract(dso_context *ctx, uint32_t task_id, dso_resource_contract rc);

/** @brief Get computed dependency levels (for inspection/debugging). */
const dso_schedule_levels *dso_get_levels(const dso_context *ctx);

// ── Plan Serialization (binary format) ─────────────────────────────────────────

#define DSO_PLAN_MAGIC   0x504F5344u  /* "DSOP" */
#define DSO_PLAN_VERSION 1

/** @brief Save compiled plan (tasks + deps, no bindings) to binary file. */
dso_status dso_plan_save(const dso_context *ctx, FILE *out);

/**
 * @brief Load plan from binary file.
 * Caller must re-bind fn/user pointers and set ctx->arena before compile.
 */
dso_status dso_plan_load(dso_context *ctx, FILE *in);

// ── Parallel Execution ────────────────────────────────────────────────────────

/** @brief Reusable worker pool for parallel execution. */
typedef struct {
    uint32_t   worker_count;
    void      *impl;    ///< Internal pool_internal pointer (opaque)
    uint8_t    ready;
} dso_worker_pool;

/** @brief Initialize a worker pool with @p worker_count threads (1..DSO_MAX_WORKERS). */
dso_status dso_worker_pool_init(dso_worker_pool *pool, uint32_t worker_count);

/** @brief Destroy worker pool (join threads if running). */
void       dso_worker_pool_destroy(dso_worker_pool *pool);

/** @brief Execute all tasks using the worker pool (dependency-level parallelism). */
dso_status dso_execute_parallel(dso_context *ctx, dso_worker_pool *pool);

/** @brief Execute one frame with parallel workers + frame timing. */
dso_status dso_execute_frame_parallel(dso_context *ctx, dso_worker_pool *pool);

// ── Private Context Layout ────────────────────────────────────────────────────

/** @internal Context structure — layout is part of the public API for inline access. */
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
        dso_resource_contract resource_contract;
        uint8_t has_contract;
    } tasks[DSO_MAX_TASKS];
    struct {
        uint32_t before;
        uint32_t after;
    } deps[DSO_MAX_DEPS];
    uint32_t schedule[DSO_MAX_TASKS];
    dso_trace_event trace[DSO_TRACE_RING];
    uint32_t trace_count;
    dso_mem_region regions[DSO_MAX_REGIONS];
    uint32_t region_count;
    dso_handle next_handle;
    dso_queue queues[DSO_MAX_QUEUES];
    uint32_t queue_count;
    dso_schedule_levels levels;
    uint8_t has_levels;
    dso_pool *pools[DSO_MAX_POOLS];
    uint32_t pool_count;
};

#ifdef __cplusplus
}
#endif

#endif /* DSO_H */
