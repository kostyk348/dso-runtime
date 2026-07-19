/*
 * DSO Parallel Execution Demo
 * ============================
 *
 * Сравнивает sequential vs parallel выполнение одного графа.
 * Граф: 4 независимых "тяжелых" таска (симулируем CPU-bound работу)
 *       + 1 таск-редьюсер, который зависит от всех четырёх.
 *
 * Уровни: level 0 = 4 независимых таска, level 1 = редьюсер.
 * В sequential: 5 тасков по очереди.
 * В parallel: 4 таска одновременно + 1 потом.
 */

#define _POSIX_C_SOURCE 200809L

#include "dso.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WORK_UNIT 5000000  // итераций "тяжелой работы"

typedef struct {
    int id;
    double result;
    uint64_t work_ns;
} worker_state;

// ── CPU-bound "работа" ─────────────────────────────────────────────────────────

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static double busy_work(int seed, int iterations) {
    volatile double x = (double)seed * 0.01;
    for (int i = 0; i < iterations; i++) {
        x = sin(x * 1.1 + 0.3) * cos(x * 0.7 + 0.1) + 0.5 * x;
    }
    return x;
}

static void heavy_task(void *user) {
    worker_state *ws = (worker_state *)user;
    uint64_t t0 = now_ns();
    ws->result = busy_work(ws->id * 7, WORK_UNIT);
    uint64_t t1 = now_ns();
    ws->work_ns = t1 - t0;
}

static void reducer_task(void *user) {
    worker_state *states = (worker_state *)user;
    double sum = 0.0;
    for (int i = 0; i < 4; i++)
        sum += states[i].result;
    // просто печатаем в конце
    (void)sum;
}

int main(int argc, char **argv) {
    int frames = 5;
    int num_workers = 4;
    if (argc > 1) frames = atoi(argv[1]);
    if (argc > 2) num_workers = atoi(argv[2]);
    if (frames < 1) frames = 1;
    if (num_workers < 1) num_workers = 1;
    if (num_workers > 16) num_workers = 16;

    printf("DSO Parallel Execution Demo\n");
    printf("  workers: %d\n", num_workers);
    printf("  frames:  %d\n", frames);
    printf("  work:    %d iterations/task\n\n", WORK_UNIT);

    // ── Shared state ──────────────────────────────────────────────────────────

    unsigned char arena_memory[65536];
    dso_arena arena;
    dso_arena_init(&arena, arena_memory, sizeof(arena_memory));

    worker_state *states = (worker_state *)dso_arena_alloc(
        &arena, sizeof(worker_state) * 4, 8);
    for (int i = 0; i < 4; i++) {
        states[i].id = i + 1;
        states[i].result = 0;
        states[i].work_ns = 0;
    }

    // ── Sequential execution ──────────────────────────────────────────────────

    dso_context ctx_seq;
    dso_context_init(&ctx_seq, &arena);

    uint32_t t[5];
    for (int i = 0; i < 4; i++)
        dso_task(&ctx_seq, "work", heavy_task, &states[i], &t[i]);
    dso_task(&ctx_seq, "reduce", reducer_task, states, &t[4]);

    for (int i = 0; i < 4; i++)
        dso_dep(&ctx_seq, t[i], t[4]);

    dso_contract c = {.max_tasks = 8, .max_deps = 8, .max_step_ns = 1000000000};
    dso_verdict v;
    dso_compile(&ctx_seq, c, &v);

    uint64_t seq_total = 0;
    for (int f = 0; f < frames; f++) {
        uint64_t t0 = now_ns();
        dso_execute(&ctx_seq);
        uint64_t t1 = now_ns();
        seq_total += (t1 - t0);
    }

    const dso_metrics *m_seq = dso_metrics_get(&ctx_seq);

    // ── Parallel execution ────────────────────────────────────────────────────

    dso_context ctx_par;
    dso_context_init(&ctx_par, &arena);

    uint32_t tp[5];
    for (int i = 0; i < 4; i++)
        dso_task(&ctx_par, "work", heavy_task, &states[i], &tp[i]);
    dso_task(&ctx_par, "reduce", reducer_task, states, &tp[4]);

    for (int i = 0; i < 4; i++)
        dso_dep(&ctx_par, tp[i], tp[4]);

    dso_compile(&ctx_par, c, &v);

    // Показываем уровни
    const dso_schedule_levels *lv = dso_get_levels(&ctx_par);
    printf("Dependency levels: %u\n", lv->count);
    for (uint32_t i = 0; i < lv->count; i++) {
        printf("  level %u: %u tasks [", i, lv->size[i]);
        for (uint32_t j = 0; j < lv->size[i]; j++) {
            printf("%s%u", j > 0 ? ", " : "", lv->tasks[i][j]);
        }
        printf("]\n");
    }

    dso_worker_pool pool;
    dso_worker_pool_init(&pool, (uint32_t)num_workers);

    uint64_t par_total = 0;
    for (int f = 0; f < frames; f++) {
        uint64_t t0 = now_ns();
        dso_execute_parallel(&ctx_par, &pool);
        uint64_t t1 = now_ns();
        par_total += (t1 - t0);
    }

    const dso_metrics *m_par = dso_metrics_get(&ctx_par);

    // ── Results ───────────────────────────────────────────────────────────────

    uint64_t seq_avg = seq_total / frames;
    uint64_t par_avg = par_total / frames;

    printf("\n--- Comparison (avg over %d frames) ---\n", frames);
    printf("  Sequential:  %5" PRIu64 " us  (worst step: %" PRIu64 " ns)\n",
           seq_avg / 1000, m_seq->worst_step_ns);
    printf("  Parallel:    %5" PRIu64 " us  (worst step: %" PRIu64 " ns)\n",
           par_avg / 1000, m_par->worst_step_ns);

    if (par_avg > 0) {
        double speedup = (double)seq_avg / (double)par_avg;
        printf("  Speedup:     %.2fx\n", speedup);
        printf("  Efficiency:  %.0f%%\n", speedup / num_workers * 100.0);
    }

    printf("\nWorker breakdown:\n");
    for (int i = 0; i < 4; i++) {
        printf("  worker %d: %5" PRIu64 " us/frame\n",
               states[i].id, states[i].work_ns / frames / 1000);
    }

    dso_worker_pool_destroy(&pool);
    printf("\nOK\n");
    return 0;
}
