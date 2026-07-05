#include "dso.h"

#include <assert.h>
#include <stdio.h>

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

int main(void) {
    test_pipeline();
    test_contract_rejects();
    test_cycle_rejects();
    puts("dso runtime tests passed");
    return 0;
}
