#include "dso.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    float input;
    float position;
    float velocity;
    float frame_accum;
    uint32_t tick;
} game_state;

static void read_input(void *user) {
    game_state *s = (game_state *)user;
    s->input = (s->tick % 120u < 60u) ? 1.0f : -1.0f;
}

static void simulate(void *user) {
    game_state *s = (game_state *)user;
    s->velocity += s->input * 0.001f;
    s->velocity *= 0.999f;
    s->position += s->velocity;
    s->tick++;
}

static void render_prepare(void *user) {
    game_state *s = (game_state *)user;
    float accum = 0.0f;
    for (int i = 0; i < 256; i++) {
        accum += s->position * 0.0001f + (float)i * 0.000001f;
    }
    s->frame_accum = accum;
}

int main(int argc, char **argv) {
    uint64_t frames = 10000;
    if (argc > 1) {
        frames = (uint64_t)strtoull(argv[1], NULL, 10);
        if (frames == 0) {
            frames = 1;
        }
    }

    unsigned char memory[4096];
    dso_arena arena;
    dso_context ctx;
    dso_verdict verdict;
    uint32_t input;
    uint32_t sim;
    uint32_t render;

    dso_arena_init(&arena, memory, sizeof(memory));
    game_state *state = (game_state *)dso_arena_alloc(&arena, sizeof(*state), 16);
    if (!state) {
        fprintf(stderr, "arena allocation failed\n");
        return 1;
    }

    dso_context_init(&ctx, &arena);
    dso_task(&ctx, "input", read_input, state, &input);
    dso_task(&ctx, "simulate", simulate, state, &sim);
    dso_task(&ctx, "render_prepare", render_prepare, state, &render);
    dso_dep(&ctx, input, sim);
    dso_dep(&ctx, sim, render);

    dso_contract contract = {
        .max_tasks = 8,
        .max_deps = 8,
        .max_arena_bytes = 1024,
        .max_plan_steps = 8,
        .max_step_ns = 1000000,
        .max_frame_ns = 16666666
    };

    dso_status st = dso_compile(&ctx, contract, &verdict);
    if (st != DSO_OK) {
        fprintf(stderr, "compile failed: %s (%s)\n", verdict.message, dso_status_str(st));
        return 1;
    }

    st = dso_run_frames(&ctx, frames);
    if (st != DSO_OK) {
        fprintf(stderr, "frame loop failed: %s\n", dso_status_str(st));
        return 1;
    }

    const dso_metrics *m = dso_metrics_get(&ctx);
    printf("frames=%" PRIu64 " avg_frame_ns=%" PRIu64 " worst_frame_ns=%" PRIu64 " overruns=%" PRIu64 " position=%.3f\n",
           m->frame_count,
           m->frame_count ? m->total_frame_ns / m->frame_count : 0,
           m->worst_frame_ns,
           m->frame_overruns,
           state->position);
    dso_write_trace_json(&ctx, stdout);
    return 0;
}
