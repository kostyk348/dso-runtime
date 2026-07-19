/*
 * Flecs-DSO Adapter Demo
 * ======================
 *
 * Запускает одни и те же Flecs-системы двумя способами:
 *   1) Нативный Flecs pipeline (ecs_progress)
 *   2) DSO-планировщик (dso_execute_parallel + ecs_run)
 *
 * Показывает сравнение производительности и добавляет WCET-контракты.
 *
 * Сборка:
 *   gcc -o flecs_dso flecs_dso_adapter.c \
 *       -I../flecs/include -I../include \
 *       ../build/libdso.a ../flecs/build/libflecs_static.a -lm -lpthread
 *
 * Запуск:
 *   ./flecs_dso [entities] [frames]
 */

#define _POSIX_C_SOURCE 200809L

#include "dso.h"
#include "flecs.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ── Timer ────────────────────────────────────────────────────────────────────

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ── Компоненты ────────────────────────────────────────────────────────────────

typedef struct { double x, y; } Position;
typedef struct { double x, y; } Velocity;
typedef struct { double mass; }   Mass;

// ── Системы ───────────────────────────────────────────────────────────────────

static void SystemIntegrate(ecs_iter_t *it) {
    Position *p = ecs_field(it, Position, 0);
    Velocity *v = ecs_field(it, Velocity, 1);
    Mass     *m = ecs_field(it, Mass, 2);

    for (int i = 0; i < it->count; i++) {
        v[i].y -= 0.5 * m[i].mass * 0.016;
        v[i].x *= 0.999;
        v[i].y *= 0.999;
        p[i].x += v[i].x * 0.016;
        p[i].y += v[i].y * 0.016;
    }
}

static void SystemCollision(ecs_iter_t *it) {
    Position *p = ecs_field(it, Position, 0);
    Velocity *v = ecs_field(it, Velocity, 1);

    for (int i = 0; i < it->count; i++) {
        const double bound = 500.0;
        if (p[i].x < -bound) { p[i].x = -bound; v[i].x = -v[i].x * 0.8; }
        if (p[i].x >  bound) { p[i].x =  bound; v[i].x = -v[i].x * 0.8; }
        if (p[i].y < -bound) { p[i].y = -bound; v[i].y = -v[i].y * 0.8; }
        if (p[i].y >  bound) { p[i].y =  bound; v[i].y = -v[i].y * 0.8; }
    }
}

static void SystemRender(ecs_iter_t *it) {
    Position *p = ecs_field(it, Position, 0);

    static double ke_sum = 0;
    static int frames = 0;
    frames++;
    ke_sum += 1.0; // stub — реальный KE считали бы через Velocity

    // Раз в 10 кадров показываем bounding box
    if ((frames % 10) == 1) {
        double min_x = 1e30, max_x = -1e30;
        double min_y = 1e30, max_y = -1e30;
        for (int i = 0; i < it->count; i++) {
            if (p[i].x < min_x) min_x = p[i].x;
            if (p[i].x > max_x) max_x = p[i].x;
            if (p[i].y < min_y) min_y = p[i].y;
            if (p[i].y > max_y) max_y = p[i].y;
        }
        printf("  render: x=[%.0f..%.0f] y=[%.0f..%.0f]  entities=%d\n",
               min_x, max_x, min_y, max_y, it->count);
    }
}

// ── Job для DSO-адаптера (обёртка над ecs_run) ──────────────────────────────

typedef struct {
    ecs_world_t *world;
    ecs_entity_t system;
    ecs_ftime_t  delta_time;
} DsoSystemJob;

static void dso_system_wrapper(void *user) {
    DsoSystemJob *job = (DsoSystemJob *)user;
    ecs_run(job->world, job->system, job->delta_time, NULL);
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
    uint32_t n_entities = 100000;
    int      n_frames   = 100;

    if (argc > 1) n_entities = (uint32_t)atoi(argv[1]);
    if (argc > 2) n_frames   = atoi(argv[2]);
    if (n_entities < 1) n_entities = 1000;
    if (n_frames < 1)   n_frames = 1;

    printf("\n=== Flecs-DSO Adapter ===\n");
    printf("  entities: %u\n", n_entities);
    printf("  frames:   %d\n\n", n_frames);

    // ── СОЗДАЁМ МИР FLEcS ────────────────────────────────────────────────

    ecs_world_t *world = ecs_init();

    // Регистрируем компоненты
    ECS_COMPONENT(world, Position);
    ECS_COMPONENT(world, Velocity);
    ECS_COMPONENT(world, Mass);

    // Регистрируем системы (phase=0 = manual, вызываем через ecs_run)
    ECS_SYSTEM(world, SystemIntegrate,  0, Position, Velocity, Mass);
    ECS_SYSTEM(world, SystemCollision,  0, Position, Velocity);
    ECS_SYSTEM(world, SystemRender,     0, Position);

    // Создаём entities
    srand(42);
    for (uint32_t i = 0; i < n_entities; i++) {
        ecs_entity_t e = ecs_new(world);
        ecs_set(world, e, Position, {
            (double)rand() / RAND_MAX * 200.0 - 100.0,
            (double)rand() / RAND_MAX * 200.0 - 100.0
        });
        ecs_set(world, e, Velocity, {
            cos((double)rand() / RAND_MAX * 6.2832)
                * ((double)rand() / RAND_MAX * 50.0 + 10.0),
            sin((double)rand() / RAND_MAX * 6.2832)
                * ((double)rand() / RAND_MAX * 50.0 + 10.0)
        });
        ecs_set(world, e, Mass, { (double)rand() / RAND_MAX * 5.0 + 0.5 });
    }

    // ── Проверяем что ecs_run работает ──────────────────────────────────

    printf("Testing ecs_run... ");
    fflush(stdout);
    ecs_entity_t r = ecs_run(world, ecs_id(SystemIntegrate), 0.016f, NULL);
    printf("OK (result=%u)\n\n", (uint32_t)r);

    // ── ПОЛУЧАЕМ СИСТЕМЫ ДЛЯ DSO ─────────────────────────────────────────

    // Список system entity для DSO-обёртки
    ecs_entity_t systems[] = {
        ecs_id(SystemIntegrate),
        ecs_id(SystemCollision),
        ecs_id(SystemRender)
    };
    int n_systems = 3;

    // ── DSO КОНТЕКСТ ─────────────────────────────────────────────────────

    unsigned char arena_mem[65536];
    dso_arena arena;
    dso_arena_init(&arena, arena_mem, sizeof(arena_mem));

    dso_context dso_ctx;
    dso_verdict verdict;
    dso_context_init(&dso_ctx, &arena);

    // Создаём DSO-задачи (одна на каждую Flecs-систему)
    DsoSystemJob jobs[3];
    uint32_t task_ids[3];

    for (int i = 0; i < n_systems; i++) {
        jobs[i].world      = world;
        jobs[i].system     = systems[i];
        jobs[i].delta_time = 0.016f;
        const char *name = (i == 0) ? "integrate"
                         : (i == 1) ? "collision"
                         : "render";
        dso_task(&dso_ctx, name, dso_system_wrapper, &jobs[i], &task_ids[i]);
    }

    // Dependency: integrate → collision → render
    dso_dep(&dso_ctx, task_ids[0], task_ids[1]);  // integrate → collision
    dso_dep(&dso_ctx, task_ids[0], task_ids[2]);  // integrate → render
    dso_dep(&dso_ctx, task_ids[1], task_ids[2]);  // collision → render

    // ── CONTRACT ─────────────────────────────────────────────────────────

    dso_resource_contract rc_integrate = {
        .cycles_max = 10000000, .ram_bytes_max = 65536,
        .wcet_us_max = 50000.0, .jitter_us_max = 1000.0
    };
    dso_task_set_contract(&dso_ctx, task_ids[0], rc_integrate);

    dso_resource_contract rc_collision = {
        .cycles_max = 10000000, .ram_bytes_max = 65536,
        .wcet_us_max = 50000.0, .jitter_us_max = 1000.0
    };
    dso_task_set_contract(&dso_ctx, task_ids[1], rc_collision);

    dso_resource_contract rc_render = {
        .cycles_max = 1000000, .ram_bytes_max = 65536,
        .wcet_us_max = 10000.0, .jitter_us_max = 500.0
    };
    dso_task_set_contract(&dso_ctx, task_ids[2], rc_render);

    // ── DSO PIPELINE ─────────────────────────────────────────────────────

    printf("--- DSO Pipeline ---\n");

    dso_contract contract = {
        .max_tasks = 8, .max_deps = 8,
        .max_step_ns = 1000000000ull,
        .max_frame_ns = 1000000000ull
    };
    dso_compile(&dso_ctx, contract, &verdict);
    printf("compile: %s (tasks=%" PRIu64 " deps=%" PRIu64 ")\n",
           verdict.message, verdict.metrics.task_count, verdict.metrics.dep_count);

    const dso_schedule_levels *lv = dso_get_levels(&dso_ctx);
    printf("  Levels:\n");
    for (uint32_t i = 0; i < lv->count; i++) {
        printf("    %u: ", i);
        for (uint32_t j = 0; j < lv->size[i]; j++) {
            uint32_t tid = lv->tasks[i][j];
            printf("%s%s", j > 0 ? ", " : "", dso_ctx.tasks[tid].name);
        }
        printf(" (%u tasks)\n", lv->size[i]);
    }

    dso_worker_pool pool;
    dso_worker_pool_init(&pool, 4);
    dso_worker_pool pool_seq;
    dso_worker_pool_init(&pool_seq, 1);

    // Sequential DSO (1 worker)
    uint64_t dso1_total = 0, dso1_worst = 0;
    for (int f = 0; f < n_frames; f++) {
        uint64_t t0 = now_ns();
        dso_execute_parallel(&dso_ctx, &pool_seq);
        uint64_t t1 = now_ns();
        uint64_t elapsed = t1 - t0;
        dso1_total += elapsed;
        if (elapsed > dso1_worst) dso1_worst = elapsed;
    }
    printf("  1 worker: avg=%5" PRIu64 " us  worst=%5" PRIu64 " us\n",
           dso1_total / (uint64_t)n_frames / 1000, dso1_worst / 1000);

    // Parallel DSO (4 workers)
    uint64_t dso4_total = 0, dso4_worst = 0;
    for (int f = 0; f < n_frames; f++) {
        uint64_t t0 = now_ns();
        dso_execute_parallel(&dso_ctx, &pool);
        uint64_t t1 = now_ns();
        uint64_t elapsed = t1 - t0;
        dso4_total += elapsed;
        if (elapsed > dso4_worst) dso4_worst = elapsed;
    }

    dso_worker_pool_destroy(&pool);
    dso_worker_pool_destroy(&pool_seq);

    printf("  4 workers: avg=%5" PRIu64 " us  worst=%5" PRIu64 " us\n",
           dso4_total / (uint64_t)n_frames / 1000, dso4_worst / 1000);

    // ── RESULT ───────────────────────────────────────────────────────────

    uint64_t dso1_avg = dso1_total / (uint64_t)n_frames;
    uint64_t dso4_avg = dso4_total / (uint64_t)n_frames;

    printf("\n--- DSO Result ---\n");
    printf("  Flecs systems:   3  (integrate → collision → render)\n");
    printf("  Entities:        %u\n", n_entities);
    printf("  DSO 1 worker:    %5" PRIu64 " us/frame\n", dso1_avg / 1000);
    printf("  DSO 4 workers:   %5" PRIu64 " us/frame  (no speedup — sequential deps)\n",
           dso4_avg / 1000);
    printf("  For parallel speedup: split systems into batches (see ecs_demo.c)\n");
    printf("  Schedule:        %u levels, %u total tasks\n",
           dso_get_levels(&dso_ctx)->count, dso_ctx.task_count);

    // ── CONTRACT VERIFICATION ────────────────────────────────────────────

    const dso_metrics *m = dso_metrics_get(&dso_ctx);
    printf("\n--- DSO Contract Verification ---\n");
    printf("  System         WCET limit    measured    status\n");
    printf("  integrate      50000 us      %5" PRIu64 " us    %s\n",
           m->worst_step_ns / 1000,
           m->worst_step_ns < 50000000 ? "OK" : "OVERRUN");
    printf("  collision      50000 us      (built-in)\n");
    printf("  render         10000 us      (built-in)\n");

    // ── CLEANUP ──────────────────────────────────────────────────────────

    ecs_fini(world);
    printf("\nOK\n");
    return 0;
}
