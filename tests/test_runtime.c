#define _GNU_SOURCE

#include "dso.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int value;
} state;

static void inc(void *user) {
    state *s = (state *)user;
    s->value += 1;
}

static void dbl(void *user) {
    state *s = (state *)user;
    s->value *= 2;
}

static void test_pipeline(void) {
    unsigned char memory[512];
    dso_arena arena;
    dso_context ctx;
    dso_verdict verdict;
    uint32_t a;
    uint32_t b;

    dso_arena_init(&arena, memory, sizeof(memory));
    state *s = (state *)dso_arena_alloc(&arena, sizeof(*s), 4);
    assert(s);
    s->value = 3;

    dso_context_init(&ctx, &arena);
    assert(dso_task(&ctx, "inc", inc, s, &a) == DSO_OK);
    assert(dso_task(&ctx, "dbl", dbl, s, &b) == DSO_OK);
    assert(dso_dep(&ctx, a, b) == DSO_OK);

    dso_contract contract = {
        .max_tasks = 4,
        .max_deps = 4,
        .max_arena_bytes = 128,
        .max_plan_steps = 4,
        .max_step_ns = 0
    };

    assert(dso_compile(&ctx, contract, &verdict) == DSO_OK);
    assert(dso_execute(&ctx) == DSO_OK);
    assert(s->value == 8);
    assert(dso_metrics_get(&ctx)->task_count == 2);
}

static void test_contract_rejects(void) {
    unsigned char memory[512];
    dso_arena arena;
    dso_context ctx;
    dso_verdict verdict;
    uint32_t a;
    uint32_t b;

    dso_arena_init(&arena, memory, sizeof(memory));
    state s = {0};
    dso_context_init(&ctx, &arena);
    assert(dso_task(&ctx, "inc", inc, &s, &a) == DSO_OK);
    assert(dso_task(&ctx, "dbl", dbl, &s, &b) == DSO_OK);

    dso_contract contract = {
        .max_tasks = 1,
        .max_deps = 0,
        .max_arena_bytes = 0,
        .max_plan_steps = 0,
        .max_step_ns = 0
    };
    assert(dso_compile(&ctx, contract, &verdict) == DSO_ERR_CONTRACT);
}

static void test_cycle_rejects(void) {
    unsigned char memory[512];
    dso_arena arena;
    dso_context ctx;
    dso_verdict verdict;
    uint32_t a;
    uint32_t b;
    state s = {0};

    dso_arena_init(&arena, memory, sizeof(memory));
    dso_context_init(&ctx, &arena);
    assert(dso_task(&ctx, "a", inc, &s, &a) == DSO_OK);
    assert(dso_task(&ctx, "b", dbl, &s, &b) == DSO_OK);
    assert(dso_dep(&ctx, a, b) == DSO_OK);
    assert(dso_dep(&ctx, b, a) == DSO_OK);
    dso_contract contract = {.max_tasks = 4, .max_deps = 4};
    assert(dso_compile(&ctx, contract, &verdict) == DSO_ERR_CYCLE);
}

static void test_frames(void) {
    unsigned char memory[512];
    dso_arena arena;
    dso_context ctx;
    dso_verdict verdict;
    uint32_t a;
    state s = {0};

    dso_arena_init(&arena, memory, sizeof(memory));
    dso_context_init(&ctx, &arena);
    assert(dso_task(&ctx, "inc", inc, &s, &a) == DSO_OK);
    dso_contract contract = {
        .max_tasks = 4,
        .max_deps = 4,
        .max_frame_ns = 1000000000
    };
    assert(dso_compile(&ctx, contract, &verdict) == DSO_OK);
    assert(dso_run_frames(&ctx, 5) == DSO_OK);
    assert(s.value == 5);
    assert(dso_metrics_get(&ctx)->frame_count == 5);
    assert(dso_metrics_get(&ctx)->worst_frame_ns > 0);
}

static void test_handle_alloc_resolve(void) {
    unsigned char memory[1024];
    dso_arena arena;
    dso_context ctx;

    dso_arena_init(&arena, memory, sizeof(memory));
    dso_context_init(&ctx, &arena);

    dso_handle h = dso_arena_alloc_handle(&ctx, &arena, 64, 4);
    assert(h != DSO_HANDLE_NULL);

    void *ptr = dso_handle_resolve(&ctx, h);
    assert(ptr != NULL);
    assert((uint8_t *)ptr >= memory && (uint8_t *)ptr < memory + sizeof(memory));

    memset(ptr, 0xAB, 64);
    uint8_t *buf = (uint8_t *)ptr;
    assert(buf[0] == 0xAB && buf[63] == 0xAB);

    dso_handle h2 = dso_arena_alloc_handle(&ctx, &arena, 32, 4);
    assert(h2 != DSO_HANDLE_NULL);
    assert(h2 != h);

    void *ptr2 = dso_handle_resolve(&ctx, h2);
    assert(ptr2 != NULL);
    assert(ptr2 != ptr);

    assert(dso_handle_resolve(&ctx, DSO_HANDLE_NULL) == NULL);
}

static void test_handle_redirect(void) {
    unsigned char memory[1024];
    dso_arena arena;
    dso_context ctx;

    dso_arena_init(&arena, memory, sizeof(memory));
    dso_context_init(&ctx, &arena);

    unsigned char backing_a[128] = {0};
    unsigned char backing_b[128] = {0};

    backing_a[0] = 0xAA;
    backing_b[0] = 0xBB;

    dso_handle h = dso_arena_alloc_handle(&ctx, &arena, 64, 4);
    assert(h != DSO_HANDLE_NULL);

    dso_status st = dso_handle_redirect(&ctx, h, backing_a, sizeof(backing_a));
    assert(st == DSO_OK);

    void *ptr = dso_handle_resolve(&ctx, h);
    assert(ptr == backing_a);
    assert(((uint8_t *)ptr)[0] == 0xAA);

    st = dso_handle_redirect(&ctx, h, backing_b, sizeof(backing_b));
    assert(st == DSO_OK);

    ptr = dso_handle_resolve(&ctx, h);
    assert(ptr == backing_b);
    assert(((uint8_t *)ptr)[0] == 0xBB);
}

static void test_handle_map_unmap(void) {
    unsigned char memory[256];
    dso_arena arena;
    dso_context ctx;

    dso_arena_init(&arena, memory, sizeof(memory));
    dso_context_init(&ctx, &arena);

    unsigned char ext_buf[64];
    dso_handle h = 42;
    dso_status st = dso_handle_map(&ctx, h, ext_buf, sizeof(ext_buf), DSO_MEM_READ | DSO_MEM_WRITE);
    assert(st == DSO_OK);

    void *ptr = dso_handle_resolve(&ctx, h);
    assert(ptr == ext_buf);

    st = dso_handle_unmap(&ctx, h);
    assert(st == DSO_OK);

    ptr = dso_handle_resolve(&ctx, h);
    assert(ptr == NULL);
}

static void test_task_contract(void) {
    unsigned char memory[512];
    dso_arena arena;
    dso_context ctx;
    dso_verdict verdict;
    uint32_t a;

    dso_arena_init(&arena, memory, sizeof(memory));
    state s = {0};
    dso_context_init(&ctx, &arena);
    assert(dso_task(&ctx, "inc", inc, &s, &a) == DSO_OK);

    dso_resource_contract rc = {
        .cycles_max = 100,
        .ram_bytes_max = 256,
        .wcet_us_max = 1000.0,
        .jitter_us_max = 50.0
    };
    assert(dso_task_set_contract(&ctx, a, rc) == DSO_OK);

    dso_contract c = {.max_tasks = 4, .max_deps = 4};
    assert(dso_compile(&ctx, c, &verdict) == DSO_OK);
    assert(dso_execute(&ctx) == DSO_OK);
    assert(s.value == 1);
}

static void test_deployment_contract(void) {
    unsigned char memory[512];
    dso_arena arena;
    dso_context ctx;
    dso_verdict verdict;
    uint32_t a;

    dso_arena_init(&arena, memory, sizeof(memory));
    state s = {0};
    dso_context_init(&ctx, &arena);
    assert(dso_task(&ctx, "inc", inc, &s, &a) == DSO_OK);

    dso_contract c = {.max_tasks = 4, .max_deps = 4};
    assert(dso_compile(&ctx, c, &verdict) == DSO_OK);

    dso_deployment_contract dc = {
        .resource = {.cycles_max = 1000, .ram_bytes_max = 4096, .wcet_us_max = 5000.0, .jitter_us_max = 100.0},
        .control = {.iae_max = 10.0, .overshoot_max = 1.0},
        .runtime = {.finite_required = 1, .saturation_fraction_max = 0.5}
    };

    char report[1024];
    dso_status st = dso_contract_verify(&ctx, dc, report, sizeof(report));
    assert(st == DSO_OK);
    assert(strstr(report, "PASS") != NULL);
}

static void test_deployment_contract_fail(void) {
    unsigned char memory[512];
    dso_arena arena;
    dso_context ctx;
    dso_verdict verdict;
    uint32_t a;

    dso_arena_init(&arena, memory, sizeof(memory));
    state s = {0};
    dso_context_init(&ctx, &arena);
    assert(dso_task(&ctx, "inc", inc, &s, &a) == DSO_OK);

    dso_contract c = {.max_tasks = 4, .max_deps = 4};
    assert(dso_compile(&ctx, c, &verdict) == DSO_OK);

    dso_resource_contract rc = {
        .cycles_max = 1,
        .ram_bytes_max = 1,
        .wcet_us_max = 0.001,
        .jitter_us_max = 0.001
    };
    assert(dso_task_set_contract(&ctx, a, rc) == DSO_OK);

    dso_deployment_contract dc = {
        .resource = {.cycles_max = 1, .ram_bytes_max = 1, .wcet_us_max = 0.001, .jitter_us_max = 0.001},
        .control = {.iae_max = 0},
        .runtime = {0}
    };

    char report[1024];
    dso_status st = dso_contract_verify(&ctx, dc, report, sizeof(report));
    assert(st == DSO_ERR_CONTRACT);
    assert(strstr(report, "FAIL") != NULL);
}

static void test_queue_push_pop(void) {
    dso_queue q;
    assert(dso_queue_init(&q, 8) == DSO_OK);
    assert(q.capacity == 8);

    int a = 1, b = 2, c = 3;
    assert(dso_queue_push(&q, &a) == DSO_OK);
    assert(dso_queue_push(&q, &b) == DSO_OK);
    assert(dso_queue_push(&q, &c) == DSO_OK);
    assert(dso_queue_len(&q) == 3);

    assert(dso_queue_pop(&q) == &a);
    assert(dso_queue_pop(&q) == &b);
    assert(dso_queue_pop(&q) == &c);
    assert(dso_queue_len(&q) == 0);

    dso_queue_destroy(&q);
}

static void test_queue_try(void) {
    dso_queue q;
    assert(dso_queue_init(&q, 4) == DSO_OK);

    int x = 42;
    assert(dso_queue_try_push(&q, &x) == 1);
    assert(dso_queue_try_push(&q, &x) == 1);
    assert(dso_queue_try_push(&q, &x) == 1);
    assert(dso_queue_try_push(&q, &x) == 1);
    assert(dso_queue_try_push(&q, &x) == 0); // full
    assert(dso_queue_free_slots(&q) == 0);

    assert(dso_queue_try_pop(&q) == &x);
    assert(dso_queue_try_pop(&q) == &x);
    assert(dso_queue_try_pop(&q) == &x);
    assert(dso_queue_try_pop(&q) == &x);
    assert(dso_queue_try_pop(&q) == NULL);   // empty
    assert(dso_queue_len(&q) == 0);
    assert(dso_queue_free_slots(&q) == 4);

    dso_queue_destroy(&q);
}

static void test_queue_wrap(void) {
    // Проверяем, что очередь корректно работает с переполнением индекса
    dso_queue q;
    assert(dso_queue_init(&q, 4) == DSO_OK);

    int items[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    // Цикл push/pop чтобы провернуть head/tail
    for (int round = 0; round < 10; round++) {
        for (int i = 0; i < 3; i++) {
            assert(dso_queue_try_push(&q, &items[i]) == 1);
        }
        assert(dso_queue_len(&q) == 3);
        for (int i = 0; i < 3; i++) {
            assert(dso_queue_try_pop(&q) == &items[i]);
        }
        assert(dso_queue_len(&q) == 0);
    }
    dso_queue_destroy(&q);
}

static void test_queue_invalid_capacity(void) {
    dso_queue q;
    assert(dso_queue_init(&q, 0) == DSO_ERR_INVALID);
    assert(dso_queue_init(&q, 1) == DSO_ERR_INVALID);
    assert(dso_queue_init(&q, 3) == DSO_ERR_INVALID); // не степень двойки
    assert(dso_queue_init(&q, 5) == DSO_ERR_INVALID);
}

static void test_levels(void) {
    unsigned char memory[512];
    dso_arena arena;
    dso_context ctx;
    dso_verdict verdict;
    uint32_t a, b, c;
    state s = {0};

    dso_arena_init(&arena, memory, sizeof(memory));
    dso_context_init(&ctx, &arena);
    assert(dso_task(&ctx, "a", inc, &s, &a) == DSO_OK);
    assert(dso_task(&ctx, "b", inc, &s, &b) == DSO_OK);
    assert(dso_task(&ctx, "c", inc, &s, &c) == DSO_OK);
    assert(dso_dep(&ctx, a, c) == DSO_OK);
    assert(dso_dep(&ctx, b, c) == DSO_OK);

    dso_contract contract = {.max_tasks = 8, .max_deps = 8};
    assert(dso_compile(&ctx, contract, &verdict) == DSO_OK);

    const dso_schedule_levels *lv = dso_get_levels(&ctx);
    assert(lv != NULL);
    assert(lv->count == 2); // level 0: a,b | level 1: c

    // level 0 should have 2 tasks (a and b, in any order)
    assert(lv->size[0] == 2);
    // level 1 should have 1 task (c)
    assert(lv->size[1] == 1);
    assert(lv->tasks[1][0] == c);
}

static void test_memory_pool(void) {
    unsigned char memory[4096];
    dso_arena arena;
    dso_context ctx;

    dso_arena_init(&arena, memory, sizeof(memory));
    dso_context_init(&ctx, &arena);

    dso_pool *pool = dso_pool_create(&ctx, &arena, 32, 4, 8);
    assert(pool != NULL);
    assert(pool->capacity == 8);
    assert(pool->free_count == 8);

    dso_handle h1 = dso_pool_alloc(&ctx, pool);
    assert(h1 != DSO_HANDLE_NULL);
    assert(dso_pool_used(pool) == 1);
    assert(dso_pool_free_count(pool) == 7);

    dso_handle h2 = dso_pool_alloc(&ctx, pool);
    assert(h2 != DSO_HANDLE_NULL);
    assert(h2 != h1);

    void *p1 = dso_pool_resolve(&ctx, pool, h1);
    void *p2 = dso_pool_resolve(&ctx, pool, h2);
    assert(p1 != NULL);
    assert(p2 != NULL);
    assert(p1 != p2);

    // Write and read
    *(int *)p1 = 42;
    assert(*(int *)p1 == 42);

    // Free and re-alloc
    dso_pool_free(pool, h1);
    assert(dso_pool_free_count(pool) == 7); // 8-2+1 = 7 free
    assert(dso_pool_used(pool) == 1);        // 1 still allocated (h2)

    dso_handle h3 = dso_pool_alloc(&ctx, pool);
    assert(h3 != DSO_HANDLE_NULL);
    assert(dso_pool_free_count(pool) == 6);
    assert(dso_pool_used(pool) == 2);
}

typedef struct {
    int id;
    int seen;
} par_state;

static void par_task(void *user) {
    par_state *ps = (par_state *)user;
    ps->seen++;
}

static void test_parallel(void) {
    unsigned char memory[4096];
    dso_arena arena;
    dso_context ctx;
    dso_verdict verdict;
    uint32_t t[4];
    par_state states[4] = {{0,0},{1,0},{2,0},{3,0}};

    dso_arena_init(&arena, memory, sizeof(memory));
    dso_context_init(&ctx, &arena);

    for (int i = 0; i < 4; i++)
        assert(dso_task(&ctx, "p", par_task, &states[i], &t[i]) == DSO_OK);

    // All independent — should be one level
    dso_contract contract = {.max_tasks = 8, .max_deps = 8};
    assert(dso_compile(&ctx, contract, &verdict) == DSO_OK);

    const dso_schedule_levels *lv = dso_get_levels(&ctx);
    assert(lv->count == 1);
    assert(lv->size[0] == 4);

    dso_worker_pool pool;
    assert(dso_worker_pool_init(&pool, 4) == DSO_OK);
    assert(dso_execute_parallel(&ctx, &pool) == DSO_OK);

    for (int i = 0; i < 4; i++)
        assert(states[i].seen == 1);

    // Test frame parallel
    for (int i = 0; i < 4; i++) states[i].seen = 0;
    assert(dso_execute_frame_parallel(&ctx, &pool) == DSO_OK);
    for (int i = 0; i < 4; i++)
        assert(states[i].seen == 1);

    dso_worker_pool_destroy(&pool);
}

// ── SPSC Queue Stress: producer + consumer threads ────────────────────────────

typedef struct {
    dso_queue *q;
    int count;
    int *results;
} spsc_args;

static void *spsc_producer(void *arg) {
    spsc_args *a = (spsc_args *)arg;
    for (int i = 0; i < a->count; i++) {
        int *val = &a->results[i];
        *val = i;
        while (!dso_queue_try_push(a->q, val))
            ; // spin until room
    }
    return NULL;
}

static void *spsc_consumer(void *arg) {
    spsc_args *a = (spsc_args *)arg;
    int last = -1;
    for (int i = 0; i < a->count; i++) {
        int *val;
        while ((val = (int *)dso_queue_try_pop(a->q)) == NULL)
            ; // spin until available
        assert(*val > last); // must be monotonic
        last = *val;
    }
    return NULL;
}

static void test_queue_spsc_stress(void) {
    dso_queue q;
    assert(dso_queue_init(&q, 64) == DSO_OK); // power-of-2

    int count = 10000;
    int *results = (int *)calloc((size_t)count, sizeof(int));
    assert(results);

    spsc_args args = {&q, count, results};

    pthread_t prod, cons;
    assert(pthread_create(&prod, NULL, spsc_producer, &args) == 0);
    assert(pthread_create(&cons, NULL, spsc_consumer, &args) == 0);
    pthread_join(prod, NULL);
    pthread_join(cons, NULL);

    assert(dso_queue_len(&q) == 0);
    free(results);
    dso_queue_destroy(&q);
}

// ── Pool alloc/free stress ─────────────────────────────────────────────────────

static void test_pool_stress(void) {
    unsigned char memory[65536];
    dso_arena arena;
    dso_context ctx;

    dso_arena_init(&arena, memory, sizeof(memory));
    dso_context_init(&ctx, &arena);

    dso_pool *pool = dso_pool_create(&ctx, &arena, 16, 4, 32);
    assert(pool);

    dso_handle handles[32];
    // Allocate all
    for (int i = 0; i < 32; i++) {
        handles[i] = dso_pool_alloc(&ctx, pool);
        assert(handles[i] != DSO_HANDLE_NULL);
        void *ptr = dso_pool_resolve(&ctx, pool, handles[i]);
        assert(ptr);
        *(int *)ptr = i;
    }
    assert(dso_pool_used(pool) == 32);
    assert(dso_pool_free_count(pool) == 0);

    // Free half, re-alloc
    for (int i = 0; i < 16; i++) {
        dso_pool_free(pool, handles[i]);
    }
    assert(dso_pool_free_count(pool) == 16);

    for (int i = 0; i < 16; i++) {
        handles[i] = dso_pool_alloc(&ctx, pool);
        assert(handles[i] != DSO_HANDLE_NULL);
    }
    assert(dso_pool_free_count(pool) == 0);

    // Free all
    for (int i = 0; i < 32; i++)
        dso_pool_free(pool, handles[i]);
    assert(dso_pool_free_count(pool) == 32);
}

// ── Save/Load round-trip ──────────────────────────────────────────────────────

static void test_plan_save_load(void) {
    unsigned char memory[1024];
    dso_arena arena;
    dso_context ctx;
    dso_verdict verdict;
    uint32_t a, b;
    state s = {0};

    dso_arena_init(&arena, memory, sizeof(memory));
    dso_context_init(&ctx, &arena);

    assert(dso_task(&ctx, "inc", inc, &s, &a) == DSO_OK);
    assert(dso_task(&ctx, "dbl", dbl, &s, &b) == DSO_OK);
    assert(dso_dep(&ctx, a, b) == DSO_OK);

    dso_contract c = {.max_tasks = 4, .max_deps = 4};
    assert(dso_compile(&ctx, c, &verdict) == DSO_OK);

    // Save to memory buffer
    char buf[1024];
    FILE *out = fmemopen(buf, sizeof(buf), "wb");
    assert(out);

    assert(dso_plan_save(&ctx, out) == DSO_OK);
    fclose(out);

    // Load into fresh context
    dso_context ctx2;
    memset(&ctx2, 0, sizeof(ctx2));
    FILE *in = fmemopen(buf, sizeof(buf), "rb");
    assert(in);

    assert(dso_plan_load(&ctx2, in) == DSO_OK);
    fclose(in);

    // Verify loaded data
    assert(ctx2.task_count == 2);
    assert(ctx2.dep_count == 1);
    assert(strcmp(ctx2.tasks[0].name, "inc") == 0);
    assert(strcmp(ctx2.tasks[1].name, "dbl") == 0);
    assert(ctx2.deps[0].before == 0);
    assert(ctx2.deps[0].after == 1);

    // Re-bind functions and execute
    ctx2.tasks[0].fn = inc;
    ctx2.tasks[1].fn = dbl;
    ctx2.tasks[0].user = &s;
    ctx2.tasks[1].user = &s;
    ctx2.arena = &arena;

    s.value = 3;
    assert(dso_compile(&ctx2, c, &verdict) == DSO_OK);
    assert(dso_execute(&ctx2) == DSO_OK);
    assert(s.value == 8);
}

// ── Chrome trace export ───────────────────────────────────────────────────────

static void test_chrome_trace(void) {
    unsigned char memory[512];
    dso_arena arena;
    dso_context ctx;
    dso_verdict verdict;
    uint32_t a;
    state s = {0};

    dso_arena_init(&arena, memory, sizeof(memory));
    dso_context_init(&ctx, &arena);
    assert(dso_task(&ctx, "inc", inc, &s, &a) == DSO_OK);
    dso_contract c = {.max_tasks = 4, .max_deps = 4};
    assert(dso_compile(&ctx, c, &verdict) == DSO_OK);
    assert(dso_execute(&ctx) == DSO_OK);

    char buf[2048];
    FILE *out = fmemopen(buf, sizeof(buf), "w");
    assert(out);
    assert(dso_trace_export_chrome(&ctx, out) == DSO_OK);
    fclose(out);

    // Verify it looks like valid JSON with traceEvents
    assert(strstr(buf, "traceEvents") != NULL);
    assert(strstr(buf, "\"ph\":\"X\"") != NULL);
    assert(strstr(buf, "\"name\":\"inc\"") != NULL);
}

// ── Parallel with step contract ────────────────────────────────────────────────

static void test_parallel_contract(void) {
    unsigned char memory[4096];
    dso_arena arena;
    dso_context ctx;
    dso_verdict verdict;
    uint32_t t[4];
    par_state states[4] = {{0,0},{1,0},{2,0},{3,0}};

    dso_arena_init(&arena, memory, sizeof(memory));
    dso_context_init(&ctx, &arena);

    for (int i = 0; i < 4; i++)
        assert(dso_task(&ctx, "p", par_task, &states[i], &t[i]) == DSO_OK);

    dso_contract c = {.max_tasks = 8, .max_deps = 8, .max_step_ns = 1000000};
    assert(dso_compile(&ctx, c, &verdict) == DSO_OK);

    dso_worker_pool pool;
    assert(dso_worker_pool_init(&pool, 4) == DSO_OK);
    assert(dso_execute_parallel(&ctx, &pool) == DSO_OK);

    for (int i = 0; i < 4; i++)
        assert(states[i].seen == 1);

    dso_worker_pool_destroy(&pool);
}

int main(void) {
    test_pipeline();
    test_contract_rejects();
    test_cycle_rejects();
    test_frames();

    test_handle_alloc_resolve();
    test_handle_redirect();
    test_handle_map_unmap();

    test_task_contract();
    test_deployment_contract();
    test_deployment_contract_fail();

    test_queue_push_pop();
    test_queue_try();
    test_queue_wrap();
    test_queue_invalid_capacity();

    test_levels();
    test_memory_pool();
    test_parallel();

    test_queue_spsc_stress();
    test_pool_stress();
    test_plan_save_load();
    test_chrome_trace();
    test_parallel_contract();

    puts("dso runtime tests passed");
    return 0;
}
