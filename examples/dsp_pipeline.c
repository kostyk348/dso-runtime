/*
 * DSO DSP Pipeline Demo
 * ======================
 *
 * Показывает все возможности libdso в реальном сценарии:
 *   generator -> filter -> analyzer -> display
 *
 * - Memory redirection: filter переключает input-handle между A/B буферами
 * - Bounded SPSC queues: данные текут между тасками
 * - Resource contracts: каждой стадии свой бюджет
 * - Deployment contract: проверка перед запуском
 * - Frame loop: все стабильно повторяется
 */

#include "dso.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SAMPLE_RATE 64       // сэмплов за фрейм
#define NUM_SAMPLES 64
#define QUEUE_DEPTH 16

// ── Shared Pipeline State ──────────────────────────────────────────────────────

typedef struct {
    // ссылка на контекст нужна для handle_redirect
    dso_context *ctx;

    double   freq;           // частота генерации (нормализованная)
    double   phase;          // текущая фаза

    // Memory handles на буферы A и B
    dso_handle h_a;
    dso_handle h_b;

    // Output handle — generator перенаправляет его A<->B каждый фрейм
    dso_handle h_output;

    // Очереди между стадиями
    dso_queue *q_to_filter;
    dso_queue *q_to_analyzer;
    dso_queue *q_to_display;

    // Какой буфер использовать в этом фрейме
    int which_buf;

    // Фильтр: счетчик
    int filter_tap;

    // Анализатор: статистика
    double peak;
    double rms;
    double dc_offset;

    // Display: накопленные данные
    int frames_processed;
} pipeline_state;

// ── Stage 1: Generator ─────────────────────────────────────────────────────────
// Пишет сэмплы в буфер через хендл, переключает A/B каждый фрейм

static void stage_generator(void *user) {
    pipeline_state *p = (pipeline_state *)user;

    // Выбираем буфер на этот фрейм
    dso_handle h_buf = p->which_buf ? p->h_a : p->h_b;
    double *buf = (double *)dso_handle_resolve(p->ctx, h_buf);
    if (!buf) return;

    // Перенаправляем output-хендл на текущий буфер
    dso_handle_redirect(p->ctx, p->h_output, buf, sizeof(double) * NUM_SAMPLES);

    p->which_buf = !p->which_buf;  // на следующий раз другой

    // Генерируем синус + шум
    double inv_ns = 1.0 / SAMPLE_RATE;
    for (int i = 0; i < NUM_SAMPLES; i++) {
        double t = (double)i * inv_ns;
        double noise = ((double)rand() / RAND_MAX - 0.5) * 0.05;
        buf[i] = sin(2.0 * M_PI * p->freq * t + p->phase) + noise;
    }
    p->phase += p->freq * 2.0 * M_PI * inv_ns;
    if (p->phase > 2.0 * M_PI) p->phase -= 2.0 * M_PI;

    // Передаём указатель на буфер следующей стадии
    dso_queue_push(p->q_to_filter, buf);
}

// ── Stage 2: Filter ────────────────────────────────────────────────────────────
// Скользящее среднее (3-tap) прямо в буфере

static void stage_filter(void *user) {
    pipeline_state *p = (pipeline_state *)user;

    double *in = (double *)dso_queue_pop(p->q_to_filter);
    if (!in) return;

    double prev = in[NUM_SAMPLES - 1];
    for (int i = 0; i < NUM_SAMPLES; i++) {
        double next = in[(i + 1) % NUM_SAMPLES];
        double filtered = (prev + in[i] + next) / 3.0;
        prev = in[i];
        in[i] = filtered;
    }
    p->filter_tap++;

    dso_queue_push(p->q_to_analyzer, in);
}

// ── Stage 3: Analyzer ──────────────────────────────────────────────────────────

static void stage_analyzer(void *user) {
    pipeline_state *p = (pipeline_state *)user;

    double *buf = (double *)dso_queue_pop(p->q_to_analyzer);
    if (!buf) return;

    double sum = 0.0, sum_sq = 0.0;
    p->peak = 0.0;
    for (int i = 0; i < NUM_SAMPLES; i++) {
        sum += buf[i];
        sum_sq += buf[i] * buf[i];
        double abs_v = fabs(buf[i]);
        if (abs_v > p->peak) p->peak = abs_v;
    }
    p->dc_offset = sum / NUM_SAMPLES;
    p->rms = sqrt(sum_sq / NUM_SAMPLES);

    dso_queue_push(p->q_to_display, buf);
}

// ── Stage 4: Display ───────────────────────────────────────────────────────────

static void stage_display(void *user) {
    pipeline_state *p = (pipeline_state *)user;

    double *buf = (double *)dso_queue_pop(p->q_to_display);
    (void)buf;

    p->frames_processed++;

    if (p->frames_processed % 10 == 0) {
        printf("frame %4d | peak=%.4f  rms=%.4f  dc_offset=%+.4f  tap=%d  phase=%.2f\n",
               p->frames_processed, p->peak, p->rms, p->dc_offset,
               p->filter_tap, fmod(p->phase, 2.0 * M_PI));
    }
}

// ── Main ───────────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    int frames = 50;
    if (argc > 1) frames = atoi(argv[1]);
    if (frames < 1) frames = 1;

    printf("DSO DSP Pipeline Demo — %d frames\n", frames);
    printf("  generator -> filter (in-place, memory redirection) -> analyzer -> display\n");
    printf("  buffer: %d doubles (%zu bytes), queue depth: %d\n\n",
           NUM_SAMPLES, sizeof(double) * NUM_SAMPLES, QUEUE_DEPTH);

    // ── Арена и контекст ──────────────────────────────────────────────────────

    unsigned char arena_memory[65536];
    dso_arena arena;
    dso_arena_init(&arena, arena_memory, sizeof(arena_memory));

    dso_context ctx;
    dso_context_init(&ctx, &arena);

    // ── Pipeline State (в арене) ──────────────────────────────────────────────

    pipeline_state *state = (pipeline_state *)dso_arena_alloc(&arena, sizeof(pipeline_state), 8);
    if (!state) { fprintf(stderr, "arena alloc failed\n"); return 1; }
    memset(state, 0, sizeof(*state));
    state->ctx = &ctx;
    state->freq = 0.05;
    state->phase = 0.0;
    state->which_buf = 0;
    state->filter_tap = 0;
    state->frames_processed = 0;

    // ── Memory Handles ─────────────────────────────────────────────────────────

    // Выделяем два буфера через handle (чтобы можно было resolve/redirect)
    state->h_a = dso_arena_alloc_handle(&ctx, &arena, sizeof(double) * NUM_SAMPLES, 8);
    state->h_b = dso_arena_alloc_handle(&ctx, &arena, sizeof(double) * NUM_SAMPLES, 8);
    if (state->h_a == DSO_HANDLE_NULL || state->h_b == DSO_HANDLE_NULL) {
        fprintf(stderr, "handle alloc failed\n"); return 1;
    }

    // Output handle — будет перенаправляться A<->B в каждом фрейме
    state->h_output = state->h_a;  // стартуем с A

    // ── Bounded Queues ─────────────────────────────────────────────────────────

    // Храним dso_queue прямо в арене (внутренние буферы — malloc, это ок для демо)
    dso_queue *q1 = (dso_queue *)dso_arena_alloc(&arena, sizeof(dso_queue), 8);
    dso_queue *q2 = (dso_queue *)dso_arena_alloc(&arena, sizeof(dso_queue), 8);
    dso_queue *q3 = (dso_queue *)dso_arena_alloc(&arena, sizeof(dso_queue), 8);

    if (dso_queue_init(q1, QUEUE_DEPTH) != DSO_OK) { fprintf(stderr, "q1 init failed\n"); return 1; }
    if (dso_queue_init(q2, QUEUE_DEPTH) != DSO_OK) { fprintf(stderr, "q2 init failed\n"); return 1; }
    if (dso_queue_init(q3, QUEUE_DEPTH) != DSO_OK) { fprintf(stderr, "q3 init failed\n"); return 1; }

    state->q_to_filter  = q1;
    state->q_to_analyzer = q2;
    state->q_to_display  = q3;

    // ── Task Graph ─────────────────────────────────────────────────────────────

    uint32_t t_gen, t_filt, t_analyze, t_disp;
    dso_task(&ctx, "generator", stage_generator, state, &t_gen);
    dso_task(&ctx, "filter",   stage_filter,   state, &t_filt);
    dso_task(&ctx, "analyzer", stage_analyzer,  state, &t_analyze);
    dso_task(&ctx, "display",  stage_display,   state, &t_disp);

    // generator -> filter -> analyzer -> display
    dso_dep(&ctx, t_gen, t_filt);
    dso_dep(&ctx, t_filt, t_analyze);
    dso_dep(&ctx, t_analyze, t_disp);

    // ── Per-Task Resource Contracts ────────────────────────────────────────────

    dso_task_set_contract(&ctx, t_gen,
        (dso_resource_contract){ .cycles_max = 500, .ram_bytes_max = 256,
                                 .wcet_us_max = 200.0, .jitter_us_max = 50.0 });
    dso_task_set_contract(&ctx, t_filt,
        (dso_resource_contract){ .cycles_max = 300, .ram_bytes_max = 128,
                                 .wcet_us_max = 150.0, .jitter_us_max = 30.0 });
    dso_task_set_contract(&ctx, t_analyze,
        (dso_resource_contract){ .cycles_max = 400, .ram_bytes_max = 256,
                                 .wcet_us_max = 180.0, .jitter_us_max = 40.0 });
    dso_task_set_contract(&ctx, t_disp,
        (dso_resource_contract){ .cycles_max = 200, .ram_bytes_max = 128,
                                 .wcet_us_max = 100.0, .jitter_us_max = 20.0 });

    // ── Compile ────────────────────────────────────────────────────────────────

    dso_contract plan = {
        .max_tasks = 8, .max_deps = 8, .max_arena_bytes = 16384,
        .max_plan_steps = 8, .max_step_ns = 10000000, .max_frame_ns = 50000000
    };
    dso_verdict v;
    dso_status st = dso_compile(&ctx, plan, &v);
    if (st != DSO_OK) {
        fprintf(stderr, "compile failed: %s (%s)\n", v.message, dso_status_str(st));
        return 1;
    }
    printf("compile: %s (tasks=%u, deps=%u, schedule steps=%u, arena=%zu)\n",
           v.message, ctx.task_count, ctx.dep_count, ctx.schedule_count,
           arena.high_water);

    // ── Deployment Contract Verification ───────────────────────────────────────

    dso_deployment_contract dc = {
        .resource = { .cycles_max = 500, .ram_bytes_max = 1024,
                      .wcet_us_max = 200.0, .jitter_us_max = 50.0 },
        .control  = { .iae_max = 0 },
        .runtime  = { .finite_required = 1 }
    };
    char report[2048];
    st = dso_contract_verify(&ctx, dc, report, sizeof(report));
    if (st != DSO_OK) {
        fprintf(stderr, "deployment contract FAILED:\n%s\n", report);
        return 1;
    }
    printf("deployment contract: PASS\n");

    // ── Execute Frames ─────────────────────────────────────────────────────────

    printf("\n--- Pipeline Output ---\n");
    st = dso_run_frames(&ctx, (uint64_t)frames);
    if (st != DSO_OK) {
        fprintf(stderr, "frame loop failed: %s (frame %" PRIu64 ")\n",
                dso_status_str(st), ctx.metrics.frame_count);
        return 1;
    }

    // ── Results ────────────────────────────────────────────────────────────────

    const dso_metrics *m = dso_metrics_get(&ctx);
    printf("\n--- DSO Metrics ---\n");
    printf("frames:       %" PRIu64 "\n", m->frame_count);
    printf("avg frame:    %" PRIu64 " ns\n",
           m->frame_count ? m->total_frame_ns / m->frame_count : 0);
    printf("worst frame:  %" PRIu64 " ns\n", m->worst_frame_ns);
    printf("worst step:   %" PRIu64 " ns\n", m->worst_step_ns);
    printf("arena used:   %" PRIu64 " / %zu bytes\n", m->arena_bytes, sizeof(arena_memory));
    printf("overruns:     %" PRIu64 "\n", m->frame_overruns);

    // Memory redirection: куда сейчас указывает h_output?
    void *resolved = dso_handle_resolve(&ctx, state->h_output);
    void *buf_a    = dso_handle_resolve(&ctx, state->h_a);
    void *buf_b    = dso_handle_resolve(&ctx, state->h_b);
    printf("memory redirect: h_output -> %s\n",
           resolved == buf_a ? "buf_a" : resolved == buf_b ? "buf_b" : "???");

    // Trace
    printf("\n--- Trace (JSON) ---\n");
    dso_write_trace_json(&ctx, stdout);

    // Cleanup
    dso_queue_destroy(q1);
    dso_queue_destroy(q2);
    dso_queue_destroy(q3);

    printf("\nDSP pipeline demo completed successfully.\n");
    return 0;
}
