#include "dso.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct {
    float input[16];
    float filtered[16];
    float output;
} pipeline_state;

static void load_input(void *user) {
    pipeline_state *s = (pipeline_state *)user;
    for (size_t i = 0; i < 16; i++) {
        s->input[i] = (float)i * 0.25f;
    }
}

static void filter_input(void *user) {
    pipeline_state *s = (pipeline_state *)user;
    for (size_t i = 0; i < 16; i++) {
        float prev = i == 0 ? s->input[i] : s->input[i - 1];
        float next = i + 1 == 16 ? s->input[i] : s->input[i + 1];
        s->filtered[i] = (prev + s->input[i] + next) / 3.0f;
    }
}

static void reduce_output(void *user) {
    pipeline_state *s = (pipeline_state *)user;
    float sum = 0.0f;
    for (size_t i = 0; i < 16; i++) {
        sum += s->filtered[i];
    }
    s->output = sum;
}

int main(void) {
    unsigned char memory[4096];
    dso_arena arena;
    dso_context ctx;
    dso_verdict verdict;
    uint32_t load;
    uint32_t filter;
    uint32_t reduce;

    dso_arena_init(&arena, memory, sizeof(memory));
    pipeline_state *state = (pipeline_state *)dso_arena_alloc(&arena, sizeof(*state), 16);
    if (!state) {
        fprintf(stderr, "arena allocation failed\n");
        return 1;
    }

    dso_context_init(&ctx, &arena);
    dso_task(&ctx, "load", load_input, state, &load);
    dso_task(&ctx, "filter", filter_input, state, &filter);
    dso_task(&ctx, "reduce", reduce_output, state, &reduce);
    dso_dep(&ctx, load, filter);
    dso_dep(&ctx, filter, reduce);

    dso_contract contract = {
        .max_tasks = 8,
        .max_deps = 8,
        .max_arena_bytes = 1024,
        .max_plan_steps = 8,
        .max_step_ns = 1000000
    };

    dso_status st = dso_compile(&ctx, contract, &verdict);
    if (st != DSO_OK) {
        fprintf(stderr, "compile failed: %s (%s)\n", verdict.message, dso_status_str(st));
        return 1;
    }

    st = dso_execute(&ctx);
    if (st != DSO_OK) {
        fprintf(stderr, "execute failed: %s\n", dso_status_str(st));
        return 1;
    }

    printf("output=%.3f\n", state->output);
    dso_write_trace_json(&ctx, stdout);
    return 0;
}
