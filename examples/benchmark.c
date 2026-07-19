/*
 * DSO Benchmark Suite
 * ===================
 * Quick throughput measurements for core subsystems.
 */

#define _GNU_SOURCE

#include "dso.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void nop_task(void *user) { (void)user; }

static void busy_task(void *user) {
    volatile double x = 1.0;
    for (int i = 0; i < 100000; i++)
        x = sin(x * 1.1 + 0.3);
    (void)x;
}

#define BENCH(name, code) do { \
    uint64_t _t0 = now_ns(); \
    code; \
    uint64_t _t1 = now_ns(); \
    double _us = (double)(_t1 - _t0) / 1000.0; \
    printf("  %-30s %10.1f us\n", name, _us); \
} while(0)

#define BENCH_OP(name, ops, code) do { \
    uint64_t _t0 = now_ns(); \
    code; \
    uint64_t _t1 = now_ns(); \
    double _us = (double)(_t1 - _t0) / 1000.0; \
    double _per_op = _us / (double)(ops); \
    double _ops_sec = (double)(ops) / (_us / 1000000.0); \
    printf("  %-30s %10.1f us  (%.0f ops/sec, %.3f us/op)\n", \
           name, _us, _ops_sec, _per_op); \
} while(0)

int main(void) {
    printf("\n=== DSO Benchmark Suite ===\n\n");

    // ── Arena allocator ───────────────────────────────────────────────────────
    printf("--- Arena Allocator ---\n");
    {
        unsigned char mem[1048576];
        dso_arena arena;
        dso_arena_init(&arena, mem, sizeof(mem));

        int N = 100000;
        BENCH_OP("arena_alloc 64 bytes", N, {
            for (int i = 0; i < N; i++) {
                void *p = dso_arena_alloc(&arena, 64, 8);
                (void)p;
            }
        });
        dso_arena_reset(&arena);
    }

    // ── SPSC Queue ────────────────────────────────────────────────────────────
    printf("\n--- SPSC Queue (64 slots) ---\n");
    {
        dso_queue q;
        dso_queue_init(&q, 64);

        int items[10000];
        int N = 100000;
        BENCH_OP("push/pop pairs", N, {
            for (int i = 0; i < N; i++) {
                dso_queue_push(&q, &items[i % 10000]);
                dso_queue_pop(&q);
            }
        });
        dso_queue_destroy(&q);
    }

    // ── Memory Pool ───────────────────────────────────────────────────────────
    printf("\n--- Memory Pool ---\n");
    {
        unsigned char mem[65536];
        dso_arena arena;
        dso_context ctx;
        dso_arena_init(&arena, mem, sizeof(mem));
        dso_context_init(&ctx, &arena);

        dso_pool *pool = dso_pool_create(&ctx, &arena, 64, 8, 256);
        int N = 10000;
        dso_handle handles[256];

        BENCH_OP("pool_alloc 64 (batch)", N, {
            for (int i = 0; i < N; i++) {
                for (int j = 0; j < 256; j++)
                    handles[j] = dso_pool_alloc(&ctx, pool);
                for (int j = 0; j < 256; j++)
                    dso_pool_free(pool, handles[j]);
            }
        });
    }

    // ── Sequential Execution ──────────────────────────────────────────────────
    printf("\n--- Sequential Execution ---\n");
    {
        unsigned char mem[65536];
        dso_arena arena;
        dso_context ctx;
        dso_verdict verdict;
        uint32_t t[8];
        int dummy = 0;

        dso_arena_init(&arena, mem, sizeof(mem));
        dso_context_init(&ctx, &arena);

        for (int i = 0; i < 8; i++)
            dso_task(&ctx, "nop", nop_task, &dummy, &t[i]);

        dso_contract c = {.max_tasks = 16, .max_deps = 16};
        dso_compile(&ctx, c, &verdict);

        int frames = 1000;
        BENCH_OP("8 nop tasks (1000 frames)", frames, {
            for (int f = 0; f < frames; f++)
                dso_execute(&ctx);
        });
    }

    // ── Parallel Execution (busy tasks) ───────────────────────────────────────
    printf("\n--- Parallel Execution (4 busy tasks) ---\n");
    {
        unsigned char mem[65536];
        dso_arena arena;
        dso_context ctx;
        dso_verdict verdict;
        uint32_t t[4];
        int dummy = 0;

        dso_arena_init(&arena, mem, sizeof(mem));
        dso_context_init(&ctx, &arena);

        for (int i = 0; i < 4; i++)
            dso_task(&ctx, "busy", busy_task, &dummy, &t[i]);

        dso_contract c = {.max_tasks = 8, .max_deps = 8};
        dso_compile(&ctx, c, &verdict);

        // Workers: 1 (sequential-ish)
        {
            dso_worker_pool pool;
            dso_worker_pool_init(&pool, 1);
            uint64_t t0 = now_ns();
            dso_execute_parallel(&ctx, &pool);
            uint64_t t1 = now_ns();
            printf("  %-30s %10.1f us\n", "1 worker (frame)", (double)(t1 - t0) / 1000.0);
            dso_worker_pool_destroy(&pool);
        }

        // Workers: 2
        {
            dso_worker_pool pool;
            dso_worker_pool_init(&pool, 2);
            uint64_t t0 = now_ns();
            dso_execute_parallel(&ctx, &pool);
            uint64_t t1 = now_ns();
            printf("  %-30s %10.1f us\n", "2 workers (frame)", (double)(t1 - t0) / 1000.0);
            dso_worker_pool_destroy(&pool);
        }

        // Workers: 4
        {
            dso_worker_pool pool;
            dso_worker_pool_init(&pool, 4);
            uint64_t t0 = now_ns();
            dso_execute_parallel(&ctx, &pool);
            uint64_t t1 = now_ns();
            printf("  %-30s %10.1f us\n", "4 workers (frame)", (double)(t1 - t0) / 1000.0);
            dso_worker_pool_destroy(&pool);
        }

        // Sequential reference
        {
            unsigned char mem2[65536];
            dso_arena arena2;
            dso_context ctx2;
            dso_verdict v2;

            dso_arena_init(&arena2, mem2, sizeof(mem2));
            dso_context_init(&ctx2, &arena2);
            for (int i = 0; i < 4; i++)
                dso_task(&ctx2, "busy", busy_task, &dummy, &t[i]);
            dso_compile(&ctx2, c, &v2);

            uint64_t t0 = now_ns();
            dso_execute(&ctx2);
            uint64_t t1 = now_ns();
            printf("  %-30s %10.1f us\n", "sequential (reference)", (double)(t1 - t0) / 1000.0);
        }
    }

    printf("\n=== Benchmark Complete ===\n");
    return 0;
}
