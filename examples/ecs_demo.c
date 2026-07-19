/*
 * DSO ECS Demo — 100k particles with gravity + collision + integration
 * ===================================================================
 *
 * Showcases:
 *   dso_pool                  — компонентное хранилище
 *   dso_execute_parallel      — параллельное выполнение по уровням
 *   dso_task / dso_dep        — граф систем
 *   dso_contract / dso_task_set_contract — per-task WCET контракты
 *   dso_schedule_levels       — фазы выполнения
 *   dso_trace_export_chrome   — chrome://tracing
 *   dso_plan_save             — сериализация сцены
 *   double-buffering          — два Position-пула, переключение каждый кадр
 *
 * Уровни (BATCH=4 → 9 задач):
 *   Level 0 — Integrate   (4 задачи, parallel)
 *   Level 1 — Collision   (4 задачи, parallel, зависят от integrate)
 *   Level 2 — Render      (1 задача, зависит от collision)
 */

#define _POSIX_C_SOURCE 200809L
#include "dso.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ── Конфигурация ──────────────────────────────────────────────────────────────

#define MAX_ENTITIES 200000
#define BATCH_COUNT  4
#define G            0.5
#define DT           0.016f
#define DAMPING      0.999f
#define BOUNCE       0.8f
#define WORLD_BOUND  500.0

// ── Компоненты ────────────────────────────────────────────────────────────────

typedef struct { double x, y; } Position;
typedef struct { double x, y; } Velocity;
typedef struct { double mass; }  Mass;

// ── Результат рендера ─────────────────────────────────────────────────────────

typedef struct {
    double   total_ke;
    double   min_x, max_x;
    double   min_y, max_y;
    uint32_t count;
} RenderResult;

// ── World ──────────────────────────────────────────────────────────────────────

typedef struct {
    dso_context  ctx;
    dso_arena    arena;
    uint8_t     *arena_memory;
    size_t       arena_size;

    uint32_t     entity_count;

    // Пулы: Position ×2 (double-buffer) + Velocity + Mass
    dso_pool    *pos_pool;
    dso_pool    *vel_pool;
    dso_pool    *mass_pool;
    dso_pool    *pos_next_pool;

    // Handle arrays: [entity_id] → handle in pool
    dso_handle  *pos_h;
    dso_handle  *vel_h;
    dso_handle  *mass_h;
    dso_handle  *pos_next_h;

    // Какой буфер читаем в этом кадре (0 = pos, 1 = pos_next)
    int          front_buffer;

    // Результат рендера (выводим каждый 10-й кадр)
    RenderResult render_result;
} World;

// ── Job для system_integrate / system_collision ───────────────────────────────

typedef struct {
    World      *world;
    uint32_t    start;
    uint32_t    count;
} RangeJob;

// ── Job для system_render ─────────────────────────────────────────────────────

typedef struct {
    World        *world;
    RenderResult *out;
} RenderJob;

// ── Timer ────────────────────────────────────────────────────────────────────

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ── Инициализация мира ─────────────────────────────────────────────────────────

static int world_init(World *w, uint32_t n) {
    memset(w, 0, sizeof(*w));
    w->entity_count = n;

    // Арена: slot data + free lists (dso_pool_create) + handle arrays + overhead
    // На entity: (Pos+V+Mass+Pos)×8..16 + 4 handles×4 + 4 freelist×4 = ~72 байта
    w->arena_size = (size_t)n * 96 + 4 * 1024 * 1024;

    w->arena_memory = (uint8_t *)calloc(w->arena_size, 1);
    if (!w->arena_memory) return -1;

    dso_arena_init(&w->arena, w->arena_memory, w->arena_size);
    dso_context_init(&w->ctx, &w->arena);

    // Handle arrays
    w->pos_h      = (dso_handle *)dso_arena_alloc(&w->arena, n * sizeof(dso_handle), 4);
    w->vel_h      = (dso_handle *)dso_arena_alloc(&w->arena, n * sizeof(dso_handle), 4);
    w->mass_h     = (dso_handle *)dso_arena_alloc(&w->arena, n * sizeof(dso_handle), 4);
    w->pos_next_h = (dso_handle *)dso_arena_alloc(&w->arena, n * sizeof(dso_handle), 4);
    if (!w->pos_h || !w->vel_h || !w->mass_h || !w->pos_next_h) return -1;

    // Пулы (dso_pool_resolve не использует region table — handles регистрируются
    // только первые DSO_MAX_REGIONS, остальные работают через pointer math)
    w->pos_pool      = dso_pool_create(&w->ctx, &w->arena, sizeof(Position), 8, n);
    w->vel_pool      = dso_pool_create(&w->ctx, &w->arena, sizeof(Velocity), 8, n);
    w->mass_pool     = dso_pool_create(&w->ctx, &w->arena, sizeof(Mass),     8, n);
    w->pos_next_pool = dso_pool_create(&w->ctx, &w->arena, sizeof(Position), 8, n);
    if (!w->pos_pool || !w->vel_pool || !w->mass_pool || !w->pos_next_pool)
        return -1;

    w->front_buffer = 0;
    return 0;
}

// ── Entity ────────────────────────────────────────────────────────────────────

static int entity_create(World *w, uint32_t id,
                         double x, double y,
                         double vx, double vy, double mass) {
    if (id >= w->entity_count) return -1;

    w->pos_h[id]      = dso_pool_alloc(&w->ctx, w->pos_pool);
    w->vel_h[id]      = dso_pool_alloc(&w->ctx, w->vel_pool);
    w->mass_h[id]     = dso_pool_alloc(&w->ctx, w->mass_pool);
    w->pos_next_h[id] = dso_pool_alloc(&w->ctx, w->pos_next_pool);
    if (w->pos_h[id] == DSO_HANDLE_NULL || w->vel_h[id] == DSO_HANDLE_NULL ||
        w->mass_h[id] == DSO_HANDLE_NULL || w->pos_next_h[id] == DSO_HANDLE_NULL)
        return -1;

    Position *p  = dso_pool_resolve(&w->ctx, w->pos_pool,  w->pos_h[id]);
    Position *pn = dso_pool_resolve(&w->ctx, w->pos_next_pool, w->pos_next_h[id]);
    Velocity *v  = dso_pool_resolve(&w->ctx, w->vel_pool,  w->vel_h[id]);
    Mass     *m  = dso_pool_resolve(&w->ctx, w->mass_pool, w->mass_h[id]);
    if (!p || !pn || !v || !m) return -1;

    p->x = pn->x = x;  p->y = pn->y = y;
    v->x = vx; v->y = vy;
    m->mass = mass;
    return 0;
}

// ── System: Integrate (Level 0) ───────────────────────────────────────────────
// Читает Position из текущего буфера, пишет Velocity + Position в следующий.

static void system_integrate(void *user) {
    RangeJob *job = (RangeJob *)user;
    World *w = job->world;
    uint32_t end = job->start + job->count;

    dso_pool *rpool = w->front_buffer ? w->pos_next_pool : w->pos_pool;
    dso_pool *wpool = w->front_buffer ? w->pos_pool : w->pos_next_pool;
    dso_handle *rh  = w->front_buffer ? w->pos_next_h : w->pos_h;
    dso_handle *wh  = w->front_buffer ? w->pos_h : w->pos_next_h;

    for (uint32_t i = job->start; i < end; i++) {
        Position *p  = dso_pool_resolve(&w->ctx, rpool, rh[i]);
        Velocity *v  = dso_pool_resolve(&w->ctx, w->vel_pool, w->vel_h[i]);
        Mass     *m  = dso_pool_resolve(&w->ctx, w->mass_pool, w->mass_h[i]);
        Position *pn = dso_pool_resolve(&w->ctx, wpool, wh[i]);
        if (!p || !v || !m || !pn) continue;

        v->y    -= G * m->mass * DT;
        v->x    *= DAMPING;
        v->y    *= DAMPING;
        pn->x    = p->x + v->x * DT;
        pn->y    = p->y + v->y * DT;
    }
}

// ── System: Collision (Level 1) ───────────────────────────────────────────────
// Читает/пишет текущий Position + Velocity.

static void system_collision(void *user) {
    RangeJob *job = (RangeJob *)user;
    World *w = job->world;
    uint32_t end = job->start + job->count;

    dso_pool *pp = w->front_buffer ? w->pos_next_pool : w->pos_pool;
    dso_handle *ph = w->front_buffer ? w->pos_next_h : w->pos_h;

    for (uint32_t i = job->start; i < end; i++) {
        Position *p = dso_pool_resolve(&w->ctx, pp, ph[i]);
        Velocity *v = dso_pool_resolve(&w->ctx, w->vel_pool, w->vel_h[i]);
        if (!p || !v) continue;

        if (p->x < -WORLD_BOUND) { p->x = -WORLD_BOUND; v->x = -v->x * BOUNCE; }
        if (p->x >  WORLD_BOUND) { p->x =  WORLD_BOUND; v->x = -v->x * BOUNCE; }
        if (p->y < -WORLD_BOUND) { p->y = -WORLD_BOUND; v->y = -v->y * BOUNCE; }
        if (p->y >  WORLD_BOUND) { p->y =  WORLD_BOUND; v->y = -v->y * BOUNCE; }
    }
}

// ── System: Render (Level 2) ─────────────────────────────────────────────────
// Собирает статистику по всем частицам.

static void system_render(void *user) {
    RenderJob *job = (RenderJob *)user;
    World *w = job->world;
    RenderResult *rr = job->out;

    dso_pool *pp = w->front_buffer ? w->pos_next_pool : w->pos_pool;
    dso_handle *ph = w->front_buffer ? w->pos_next_h : w->pos_h;

    rr->count = 0;
    rr->total_ke = 0;
    rr->min_x = rr->min_y = 1e30;
    rr->max_x = rr->max_y = -1e30;

    for (uint32_t i = 0; i < w->entity_count; i++) {
        Position *p = dso_pool_resolve(&w->ctx, pp, ph[i]);
        Velocity *v = dso_pool_resolve(&w->ctx, w->vel_pool, w->vel_h[i]);
        Mass     *m = dso_pool_resolve(&w->ctx, w->mass_pool, w->mass_h[i]);
        if (!p || !v || !m) continue;

        double ke = 0.5 * m->mass * (v->x * v->x + v->y * v->y);
        rr->total_ke += ke;
        if (p->x < rr->min_x) rr->min_x = p->x;
        if (p->x > rr->max_x) rr->max_x = p->x;
        if (p->y < rr->min_y) rr->min_y = p->y;
        if (p->y > rr->max_y) rr->max_y = p->y;
        rr->count++;
    }
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    uint32_t n_entities = 100000;
    int      n_frames   = 100;
    int      n_workers  = 4;

    if (argc > 1) n_entities = (uint32_t)atoi(argv[1]);
    if (argc > 2) n_frames   = atoi(argv[2]);
    if (argc > 3) n_workers  = atoi(argv[3]);

    if (n_entities < 1 || n_entities > MAX_ENTITIES) n_entities = MAX_ENTITIES;
    if (n_frames < 1)   n_frames = 1;
    if (n_workers < 1 || n_workers > 16) n_workers = 4;

    printf("\n=== DSO ECS Demo ===\n");
    printf("  entities: %u\n", n_entities);
    printf("  frames:   %d\n", n_frames);
    printf("  workers:  %d\n", n_workers);
    printf("  batches:  %d\n\n", BATCH_COUNT);

    // ── World ─────────────────────────────────────────────────────────────

    World *world = (World *)calloc(1, sizeof(World));
    if (!world || world_init(world, n_entities) != 0) {
        fprintf(stderr, "FATAL: world_init\n");
        return 1;
    }

    // ── Заполняем частицами ──────────────────────────────────────────────

    srand(42);
    for (uint32_t i = 0; i < n_entities; i++) {
        double angle = (double)rand() / RAND_MAX * 6.2832;
        double speed = (double)rand() / RAND_MAX * 50.0 + 10.0;
        entity_create(world, i,
                      (double)rand() / RAND_MAX * 200.0 - 100.0,
                      (double)rand() / RAND_MAX * 200.0 - 100.0,
                      cos(angle) * speed, sin(angle) * speed,
                      (double)rand() / RAND_MAX * 5.0 + 0.5);
    }

    // ── Строим граф ──────────────────────────────────────────────────────

    uint32_t batch_size = (n_entities + BATCH_COUNT - 1) / BATCH_COUNT;

    // Аллоцируем jobs в арене (они живут пока жив world)
    RangeJob *integrate_jobs = (RangeJob *)dso_arena_alloc(
        &world->arena, BATCH_COUNT * sizeof(RangeJob), 8);
    RangeJob *collision_jobs = (RangeJob *)dso_arena_alloc(
        &world->arena, BATCH_COUNT * sizeof(RangeJob), 8);
    RenderJob *render_job = (RenderJob *)dso_arena_alloc(
        &world->arena, sizeof(RenderJob), 8);
    if (!integrate_jobs || !collision_jobs || !render_job) {
        fprintf(stderr, "FATAL: job alloc\n");
        return 1;
    }

    uint32_t t[9]; // 4+4+1
    int ti = 0;

    // Level 0: Integrate × BATCH_COUNT
    for (int b = 0; b < BATCH_COUNT; b++) {
        integrate_jobs[b].world = world;
        integrate_jobs[b].start = (uint32_t)(b * batch_size);
        integrate_jobs[b].count = (uint32_t)((b == BATCH_COUNT - 1)
            ? (n_entities - integrate_jobs[b].start) : batch_size);
        dso_task(&world->ctx, "integrate", system_integrate, &integrate_jobs[b], &t[ti++]);
    }

    // Level 1: Collision × BATCH_COUNT (depend on ALL integrate)
    for (int b = 0; b < BATCH_COUNT; b++) {
        collision_jobs[b].world = world;
        collision_jobs[b].start = (uint32_t)(b * batch_size);
        collision_jobs[b].count = (uint32_t)((b == BATCH_COUNT - 1)
            ? (n_entities - collision_jobs[b].start) : batch_size);
        dso_task(&world->ctx, "collision", system_collision, &collision_jobs[b], &t[ti++]);
        for (int p = 0; p < BATCH_COUNT; p++)
            dso_dep(&world->ctx, t[p], t[BATCH_COUNT + b]);
    }

    // Level 2: Render (depends on ALL collision)
    {
        render_job->world = world;
        render_job->out   = &world->render_result;
        dso_task(&world->ctx, "render", system_render, render_job, &t[ti++]);
        for (int p = 0; p < BATCH_COUNT; p++)
            dso_dep(&world->ctx, t[BATCH_COUNT + p], t[BATCH_COUNT * 2]);
    }

    // ── Контракты ────────────────────────────────────────────────────────

    for (int b = 0; b < BATCH_COUNT; b++) {
        dso_resource_contract rc = {
            .cycles_max = 10000000, .ram_bytes_max = 65536,
            .wcet_us_max = 50000.0, .jitter_us_max = 1000.0
        };
        dso_task_set_contract(&world->ctx, t[b], rc);
        dso_task_set_contract(&world->ctx, t[BATCH_COUNT + b], rc);
    }
    {
        dso_resource_contract rc = {
            .cycles_max = 1000000, .ram_bytes_max = 1024,
            .wcet_us_max = 1000.0, .jitter_us_max = 100.0
        };
        dso_task_set_contract(&world->ctx, t[BATCH_COUNT * 2], rc);
    }

    // ── Compile ──────────────────────────────────────────────────────────

    dso_contract contract = {
        .max_tasks = DSO_MAX_TASKS, .max_deps = DSO_MAX_DEPS,
        .max_step_ns = 1000000000ull, .max_frame_ns = 1000000000ull
    };
    dso_verdict verdict;
    if (dso_compile(&world->ctx, contract, &verdict) != DSO_OK) {
        printf("compile: %s\n", verdict.message);
        return 1;
    }

    const dso_schedule_levels *lv = dso_get_levels(&world->ctx);
    printf("Schedule: %u levels\n", lv->count);
    for (uint32_t i = 0; i < lv->count; i++)
        printf("  level %u: %u tasks\n", i, lv->size[i]);

    // ── Frame loop ───────────────────────────────────────────────────────

    dso_worker_pool pool;
    dso_worker_pool_init(&pool, (uint32_t)n_workers);

    uint64_t total_ns = 0, worst_ns = 0;
    int errors = 0;

    for (int f = 0; f < n_frames; f++) {
        uint64_t t0 = now_ns();
        dso_status st = dso_execute_frame_parallel(&world->ctx, &pool);

        // Double-buffer swap
        world->front_buffer = !world->front_buffer;

        uint64_t t1 = now_ns();
        uint64_t elapsed = t1 - t0;
        total_ns += elapsed;
        if (elapsed > worst_ns) worst_ns = elapsed;
        if (st != DSO_OK) errors++;

        // Статистика каждый 10-й кадр
        if ((f % 10) == 0 || f == n_frames - 1) {
            RenderResult *r = &world->render_result;
            printf("  frame %4d: %5" PRIu64 " us  |  KE=%.1f  x=[%+.0f..%+.0f]  y=[%+.0f..%+.0f]\n",
                   f, elapsed / 1000, r->total_ke, r->min_x, r->max_x, r->min_y, r->max_y);
        }
    }

    dso_worker_pool_destroy(&pool);

    // ── Итоги ────────────────────────────────────────────────────────────

    const dso_metrics *m = dso_metrics_get(&world->ctx);
    printf("\n--- Results ---\n");
    printf("  avg frame:  %5" PRIu64 " us\n", total_ns / (uint64_t)n_frames / 1000);
    printf("  worst frame:%5" PRIu64 " us\n", worst_ns / 1000);
    printf("  worst step: %5" PRIu64 " us\n", m->worst_step_ns / 1000);
    printf("  frames:     %" PRIu64 "\n", m->frame_count);
    printf("  errors:     %d\n", errors);
    printf("  tasks:      %" PRIu64 "\n", m->task_count);
    printf("  deps:       %" PRIu64 "\n", m->dep_count);
    printf("  arena used: %zu / %zu bytes\n",
           world->arena.high_water, world->arena_size);

    // Chrome trace
    {
        FILE *f = fopen("/tmp/dso_ecs_trace.json", "w");
        if (f) {
            dso_trace_export_chrome(&world->ctx, f);
            fclose(f);
            printf("\nTrace: /tmp/dso_ecs_trace.json\n");
        }
    }

    // Plan save
    {
        FILE *f = fopen("/tmp/dso_ecs_plan.bin", "wb");
        if (f) {
            dso_plan_save(&world->ctx, f);
            fclose(f);
            printf("Plan:  /tmp/dso_ecs_plan.bin\n");
        }
    }

    printf("\nOK\n");
    free(world->arena_memory);
    free(world);
    return 0;
}
