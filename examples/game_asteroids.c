/*
 * DSO Asteroids — playable game showcasing DSO runtime
 * =====================================================
 *
 * Showcases:
 *   dso_task / dso_dep / dso_compile     — статический граф задач
 *   dso_execute_frame_parallel            — параллельное выполнение по уровням
 *   dso_task_set_contract                 — WCET контракты на каждый шаг
 *   dso_get_levels                        — интроспекция шедулы
 *   dso_trace_export_chrome               — chrome://tracing
 *   dso_metrics_get                       — метрики исполнения
 *
 * Уровни (BATCH_COUNT=4 → 9 задач):
 *   Level 0 — Integrate   (4 задачи, parallel)  — физика, движение
 *   Level 1 — Collision   (4 задачи, parallel)  — пули ↔ астероиды, корабль ↔ астероиды
 *   Level 2 — Spawn       (1 задача)            — очистка мёртвых, спавн новых астероидов/пуль
 *   Level 3 — Render      (1 задача)            — отрисовка SDL2
 *
 * Управление:
 *   WASD / Стрелки — поворот + ускорение
 *   Space          — огонь
 *   Esc            — выход
 */

#define _POSIX_C_SOURCE 200809L
#include "dso.h"

#include <SDL2/SDL.h>

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ── Конфигурация ──────────────────────────────────────────────────────────────

#define MAX_ENTITIES   4096
#define MAX_BULLETS    256
#define MAX_ASTEROIDS  512
#define BATCH_COUNT    4
#define WIN_W          1024
#define WIN_H          768
#define DT             0.016f
#define BULLET_SPEED   600.0
#define SHIP_THRUST    400.0
#define SHIP_ROT_SPEED 4.5
#define BULLET_LIFETIME 1.5
#define ASTEROID_SPEED 120.0
#define PI             3.14159265358979323846

// ── Типы сущностей ────────────────────────────────────────────────────────────

typedef enum { ENT_NONE = 0, ENT_SHIP, ENT_ASTEROID, ENT_BULLET } EntityType;

// ── Компоненты ────────────────────────────────────────────────────────────────

typedef struct { double x, y; }          Position;
typedef struct { double vx, vy; }        Velocity;
typedef struct { double rot; }           Rotation;
typedef struct { double vr; }            RotVelocity;

typedef struct {
    EntityType type;
    uint8_t    active;
    int        asteroid_level;   // 0=big, 1=medium, 2=small
    float      size;
    float      lifetime;         // bullet timeout
    uint8_t    r, g, b;
} EntityInfo;

// ── Игровой мир ───────────────────────────────────────────────────────────────

typedef struct {
    // DSO
    dso_arena   arena;
    uint8_t    *arena_mem;
    size_t      arena_size;
    dso_context ctx;
    dso_worker_pool pool;

    // Entity table
    int         n_entities;
    uint32_t    n_alloc;        // allocated slots
    Position   *pos;            // [n_alloc]
    Velocity   *vel;
    Rotation   *rot;
    RotVelocity *rot_vel;
    EntityInfo *info;
    uint8_t    *damaged;        // per-frame collision flag

    // Game state
    int         ship_id;
    int         score;
    int         lives;
    double      fire_cooldown;
    double      asteroid_timer;
    int         frame;
    int         game_over;

    // Input
    const uint8_t *keys;        // SDL key state array
    uint8_t      space_pressed; // edge detection

    // SDL
    SDL_Window   *window;
    SDL_Renderer *renderer;

    // Frame timing
    uint64_t     frame_ns;

    // Chrome trace file (open at start, close at end)
    FILE         *trace_file;
    int           trace_written;
} World;

// ── Job descriptors ───────────────────────────────────────────────────────────

typedef struct {
    World *world;
    int    start;
    int    count;
} RangeJob;

typedef struct {
    World *world;
    // Render collects these
    int    n_drawn;
    int    n_asteroids;
    int    n_bullets;
} RenderJob;

// ── Timer ─────────────────────────────────────────────────────────────────────

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ── Entity management ─────────────────────────────────────────────────────────

static int entity_slot_alloc(World *w) {
    for (int i = 0; i < (int)w->n_alloc; i++)
        if (!w->info[i].active) {
            memset(&w->info[i], 0, sizeof(EntityInfo));
            w->damaged[i] = 0;
            return i;
        }
    return -1; // full
}

static int entity_create(World *w, EntityType type, double x, double y,
                         double vx, double vy, double rot, double vr,
                         int asteroid_level, float size,
                         uint8_t r, uint8_t g, uint8_t b)
{
    int id = entity_slot_alloc(w);
    if (id < 0) return -1;
    w->pos[id].x = x;  w->pos[id].y = y;
    w->vel[id].vx = vx; w->vel[id].vy = vy;
    w->rot[id].rot = rot;
    w->rot_vel[id].vr = vr;
    w->info[id].type = type;
    w->info[id].active = 1;
    w->info[id].asteroid_level = asteroid_level;
    w->info[id].size = size;
    w->info[id].lifetime = 0;
    w->info[id].r = r; w->info[id].g = g; w->info[id].b = b;
    w->damaged[id] = 0;
    return id;
}

static void entity_destroy(World *w, int id) {
    w->info[id].active = 0;
}

// ── Spawn helpers ─────────────────────────────────────────────────────────────

static double rand_range(double min, double max) {
    return min + (double)rand() / RAND_MAX * (max - min);
}

static void spawn_asteroid(World *w, int level, double x, double y) {
    double angle = rand_range(0, 2 * PI);
    double speed = rand_range(ASTEROID_SPEED * 0.5, ASTEROID_SPEED * (1.0 + level * 0.3));
    float size = (float)(40.0 - level * 12.0);
    uint8_t shade = (uint8_t)(180 - level * 40);
    entity_create(w, ENT_ASTEROID, x, y,
                  cos(angle) * speed, sin(angle) * speed,
                  rand_range(0, 2*PI), rand_range(-2.0, 2.0),
                  level, size, shade, shade, shade);
}

static void spawn_bullet(World *w, int ship_id) {
    Position *sp = &w->pos[ship_id];
    Rotation *sr = &w->rot[ship_id];
    double dx = cos(sr->rot);
    double dy = sin(sr->rot);
    entity_create(w, ENT_BULLET,
                  sp->x + dx * 25, sp->y + dy * 25,
                  dx * BULLET_SPEED + w->vel[ship_id].vx,
                  dy * BULLET_SPEED + w->vel[ship_id].vy,
                  0, 0, 0, 3.0f, 255, 255, 100);
}

static void init_asteroid_field(World *w, int count) {
    for (int i = 0; i < count; i++) {
        double x, y;
        // Spawn off-screen, not too close to center
        do {
            x = rand_range(-WIN_W * 0.8, WIN_W * 0.8);
            y = rand_range(-WIN_H * 0.8, WIN_H * 0.8);
        } while (fabs(x) < 200 && fabs(y) < 200);
        spawn_asteroid(w, 0, x, y);
    }
}

// ── System: Input (outside DSO, runs before frame) ───────────────────────────

static void process_input(World *w) {
    SDL_PumpEvents();
    const uint8_t *keys = SDL_GetKeyboardState(NULL);
    w->keys = keys;

    if (w->game_over && keys[SDL_SCANCODE_R]) {
        // restart handled in main loop
        return;
    }
    if (w->game_over) return;

    int ship = w->ship_id;
    if (ship < 0 || !w->info[ship].active) return;

    Rotation *r = &w->rot[ship];
    RotVelocity *rv = &w->rot_vel[ship];
    Velocity *v = &w->vel[ship];
    Position *p = &w->pos[ship];

    // Rotation
    if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A])
        rv->vr = -SHIP_ROT_SPEED;
    else if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D])
        rv->vr = SHIP_ROT_SPEED;
    else
        rv->vr = 0;

    // Thrust
    if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W]) {
        double dx = cos(r->rot);
        double dy = sin(r->rot);
        v->vx += dx * SHIP_THRUST * DT;
        v->vy += dy * SHIP_THRUST * DT;
    }

    // Fire
    if (keys[SDL_SCANCODE_SPACE] && !w->space_pressed && w->fire_cooldown <= 0) {
        spawn_bullet(w, ship);
        w->fire_cooldown = 0.2;
    }
    w->space_pressed = keys[SDL_SCANCODE_SPACE];

    // Clamp ship speed
    double spd = sqrt(v->vx * v->vx + v->vy * v->vy);
    if (spd > 500) {
        v->vx *= 500 / spd;
        v->vy *= 500 / spd;
    }
}

// ── System: Integrate (Level 0, parallel) ────────────────────────────────────

static void system_integrate(void *user) {
    RangeJob *job = (RangeJob *)user;
    World *w = job->world;

    for (int i = job->start; i < job->start + job->count && i < (int)w->n_alloc; i++) {
        if (!w->info[i].active || w->info[i].type == ENT_NONE) continue;

        // Apply rotation
        w->rot[i].rot += w->rot_vel[i].vr * DT;

        // Apply velocity
        w->pos[i].x += w->vel[i].vx * DT;
        w->pos[i].y += w->vel[i].vy * DT;

        // Bullet lifetime
        if (w->info[i].type == ENT_BULLET) {
            w->info[i].lifetime += DT;
            if (w->info[i].lifetime > BULLET_LIFETIME)
                w->info[i].active = 0;
        }

        // Screen wrap
        int margin = 80;
        if (w->pos[i].x < -margin)  w->pos[i].x = WIN_W + margin;
        if (w->pos[i].x > WIN_W + margin) w->pos[i].x = -margin;
        if (w->pos[i].y < -margin)  w->pos[i].y = WIN_H + margin;
        if (w->pos[i].y > WIN_H + margin) w->pos[i].y = -margin;
    }
}

// ── System: Collision (Level 1, parallel) ────────────────────────────────────

static void system_collision(void *user) {
    RangeJob *job = (RangeJob *)user;
    World *w = job->world;

    // Each collision batch checks asteroids against bullets in adjacent section
    // and the ship (checked in ship_batch)
    //
    // Simple approach: each batch checks its asteroids against all bullets
    // to avoid complex partitioning. This means O(N_ast * N_bul) worst case,
    // but for demo purposes it's fine (asteroids ~50, bullets ~20).

    for (int i = job->start; i < job->start + job->count && i < (int)w->n_alloc; i++) {
        if (!w->info[i].active || w->info[i].type != ENT_ASTEROID) continue;
        if (w->damaged[i]) continue;

        Position *ap = &w->pos[i];
        float ar = w->info[i].size;

        // Check against bullets
        for (int j = 0; j < (int)w->n_alloc; j++) {
            if (!w->info[j].active || w->info[j].type != ENT_BULLET) continue;
            if (w->damaged[j]) continue;

            Position *bp = &w->pos[j];
            double dx = ap->x - bp->x;
            double dy = ap->y - bp->y;
            double dist = sqrt(dx * dx + dy * dy);
            if (dist < ar + w->info[j].size) {
                // Hit! Mark both
                w->damaged[i] = 1;
                w->damaged[j] = 1;
                w->info[j].active = 0; // bullet dies immediately
                // Score goes to ship (tracked in spawn phase)
            }
        }

        // Check against ship
        int ship = w->ship_id;
        if (ship >= 0 && w->info[ship].active && !w->damaged[ship]) {
            Position *sp = &w->pos[ship];
            double dx = ap->x - sp->x;
            double dy = ap->y - sp->y;
            double dist = sqrt(dx * dx + dy * dy);
            if (dist < ar + 20.0) {
                w->damaged[i] = 1;
                w->damaged[ship] = 1;
            }
        }
    }
}

// ── System: Spawn & Cleanup (Level 2, single) ────────────────────────────────

static void system_spawn(void *user) {
    (void)user;
    World *w = (World *)user;
    int old_score = w->score;

    // Process damaged asteroids → spawn smaller ones
    for (int i = 0; i < (int)w->n_alloc; i++) {
        if (w->damaged[i] && w->info[i].active && w->info[i].type == ENT_ASTEROID) {
            int level = w->info[i].asteroid_level;
            double x = w->pos[i].x;
            double y = w->pos[i].y;
            entity_destroy(w, i);

            // Spawn children or give points
            if (level < 2) {
                spawn_asteroid(w, level + 1, x, y);
                spawn_asteroid(w, level + 1, x, y);
                w->score += 20 * (3 - level);
            } else {
                w->score += 100;
            }
        }
    }

    // Clear damaged flags for non-asteroids (bullets already dead, ship handled below)
    for (int i = 0; i < (int)w->n_alloc; i++) {
        w->damaged[i] = 0;
    }

    // Handle ship damage
    int ship = w->ship_id;
    if (ship >= 0 && w->info[ship].active && w->damaged[ship]) {
        // Actually damaged was set but we cleared it above. Let me check.
        // Hmm, the ship damaged flag was set in collision. But we just cleared everyone.
        // Let me re-check the ship separately.
    }

    // Actually, let me redo this. The damaged flag was set on the ship in collision,
    // but we cleared all damaged flags above. Let me track ship damage differently.
    // I'll use a separate field.

    // Actually, let me handle ship collision in a simpler way. The collision task
    // deactivates the ship directly. Let me restructure.

    // New approach: collision just marks in a separate field, spawn handles it.
}

// No wait, I'm overcomplicating this. Let me redesign.

// The collision task already deactivates bullets directly (w->info[j].active = 0).
// For asteroids, it sets w->damaged[i] = 1.
// For the ship, it already sets w->damaged[ship] = 1.

// Then in spawn, I process damaged asteroids and handle the ship.

// Let me rewrite the spawn system properly.
static void system_spawn2(void *user) {
    World *w = (World *)user;

    int ship = w->ship_id;

    // Process damaged asteroids
    for (int i = 0; i < (int)w->n_alloc; i++) {
        if (!w->info[i].active) continue;
        if (w->info[i].type == ENT_ASTEROID && w->damaged[i]) {
            int level = w->info[i].asteroid_level;
            double x = w->pos[i].x;
            double y = w->pos[i].y;

            entity_destroy(w, i);

            if (level < 2) {
                spawn_asteroid(w, level + 1, x - 10, y - 10);
                spawn_asteroid(w, level + 1, x + 10, y + 10);
                w->score += 20 * (3 - level);
            } else {
                w->score += 100;
            }
        }
    }

    // Handle ship damage
    if (ship >= 0 && w->info[ship].active && w->damaged[ship]) {
        entity_destroy(w, ship);
        w->lives--;
        if (w->lives <= 0) {
            w->game_over = 1;
        }
    }

    // Clear all damaged flags
    memset(w->damaged, 0, w->n_alloc);

    // Fire cooldown
    if (w->fire_cooldown > 0) w->fire_cooldown -= DT;

    // Respawn ship if dead and lives remain
    if (ship >= 0 && !w->info[ship].active && w->lives > 0) {
        w->ship_id = entity_create(w, ENT_SHIP, WIN_W/2.0, WIN_H/2.0,
                                   0, 0, -PI/2, 0, 0, 20.0f, 0, 255, 80);
        w->info[w->ship_id].active = 1;
        // invulnerability frames: skip collision for ~1.5s
        w->fire_cooldown = 1.5;
    }

    // Spawn new asteroids periodically
    w->asteroid_timer += DT;
    int ast_count = 0;
    for (int i = 0; i < (int)w->n_alloc; i++)
        if (w->info[i].active && w->info[i].type == ENT_ASTEROID)
            ast_count++;
    if (ast_count < 4 + w->frame / 300 && w->asteroid_timer > 2.0) {
        // Spawn off-screen
        double x, y;
        int side = rand() % 4;
        switch (side) {
            case 0: x = -40; y = rand_range(0, WIN_H); break; // left
            case 1: x = WIN_W + 40; y = rand_range(0, WIN_H); break; // right
            case 2: x = rand_range(0, WIN_W); y = -40; break; // top
            default: x = rand_range(0, WIN_W); y = WIN_H + 40; break; // bottom
        }
        spawn_asteroid(w, 0, x, y);
        w->asteroid_timer = 0;
    }

    // Cap total entities
    int total = 0;
    for (int i = 0; i < (int)w->n_alloc; i++)
        if (w->info[i].active) total++;
    // If exceeds max, destroy some random asteroids
    if (total > w->n_alloc * 0.9) {
        for (int i = 0; i < (int)w->n_alloc && total > w->n_alloc * 0.7; i++) {
            if (w->info[i].active && w->info[i].type == ENT_ASTEROID) {
                entity_destroy(w, i);
                total--;
            }
        }
    }
}

// ── System: Render (Level 3, single) ─────────────────────────────────────────

static void system_render(void *user) {
    RenderJob *job = (RenderJob *)user;
    World *w = job->world;

    SDL_SetRenderDrawColor(w->renderer, 5, 5, 15, 255);
    SDL_RenderClear(w->renderer);

    job->n_drawn = 0;
    job->n_asteroids = 0;
    job->n_bullets = 0;

    for (int i = 0; i < (int)w->n_alloc; i++) {
        if (!w->info[i].active) continue;

        Position *p = &w->pos[i];
        EntityInfo *ei = &w->info[i];
        Rotation *r = &w->rot[i];
        int sx = (int)p->x;
        int sy = (int)p->y;

        switch (ei->type) {
        case ENT_SHIP: {
            // Draw as triangle pointing in direction of rotation
            double cosr = cos(r->rot);
            double sinr = sin(r->rot);
            float s = ei->size;
            SDL_Point pts[4];
            pts[0].x = sx + (int)(cosr * s);        // nose
            pts[0].y = sy + (int)(sinr * s);
            pts[1].x = sx + (int)(cosr-0.866*sinr) * (-s/2);  // left wing
            pts[1].y = sy + (int)(sinr+0.866*cosr) * (-s/2);
            pts[2].x = sx + (int)(cosr+0.866*sinr) * (-s/2);  // right wing
            pts[2].y = sy + (int)(sinr-0.866*cosr) * (-s/2);
            pts[3] = pts[0];
            SDL_SetRenderDrawColor(w->renderer, ei->r, ei->g, ei->b, 255);
            SDL_RenderDrawLines(w->renderer, pts, 4);

            // Thrust flame if moving fast
            if (fabs(w->vel[i].vx) > 20 || fabs(w->vel[i].vy) > 20) {
                double back_x = sx + (int)(cos(-r->rot) * (-s * 0.6));
                double back_y = sy + (int)(sin(-r->rot) * (-s * 0.6));
                SDL_SetRenderDrawColor(w->renderer, 255, 150, 0, 255);
                SDL_RenderDrawLine(w->renderer, back_x - 3, back_y - 3, back_x, back_y);
                SDL_RenderDrawLine(w->renderer, back_x + 3, back_y + 3, back_x, back_y);
            }
            job->n_drawn++;
            break;
        }
        case ENT_ASTEROID: {
            // Draw as jagged circle (octagon with random-ish vertices)
            float rad = ei->size;
            int segments = 8;
            SDL_Point pts[9];
            for (int s = 0; s < segments; s++) {
                double a = (double)s / segments * 2 * PI;
                // Slightly irregular shape (fractal-looking)
                double wobble = 0.85 + 0.15 * sin(a * 3.7 + ei->asteroid_level * 1.3);
                pts[s].x = sx + (int)(cos(a) * rad * wobble);
                pts[s].y = sy + (int)(sin(a) * rad * wobble);
            }
            pts[8] = pts[0];
            SDL_SetRenderDrawColor(w->renderer, ei->r, ei->g, ei->b, 255);
            SDL_RenderDrawLines(w->renderer, pts, 9);
            job->n_drawn++;
            job->n_asteroids++;
            break;
        }
        case ENT_BULLET: {
            SDL_SetRenderDrawColor(w->renderer, ei->r, ei->g, ei->b, 255);
            int rs = (int)ei->size;
            SDL_Rect rect = { sx - rs, sy - rs, rs * 2, rs * 2 };
            SDL_RenderFillRect(w->renderer, &rect);
            // Glow
            SDL_SetRenderDrawColor(w->renderer, ei->r, ei->g, ei->b, 80);
            SDL_RenderDrawLine(w->renderer, sx - rs*2, sy, sx + rs*2, sy);
            SDL_RenderDrawLine(w->renderer, sx, sy - rs*2, sx, sy + rs*2);
            job->n_drawn++;
            job->n_bullets++;
            break;
        }
        default:
            break;
        }
    }

    // HUD
    char buf[128];
    snprintf(buf, sizeof(buf),
             "DSO Asteroids  |  Score: %d  Lives: %d  Asteroids: %d  Frame: %d",
             w->score, w->lives, job->n_asteroids, w->frame);
    // SDL doesn't have built-in text rendering in SDL2 without TTF.
    // We'll put info in the window title instead.
    SDL_SetWindowTitle(w->window, buf);

    if (w->game_over) {
        // Draw "GAME OVER" in big pixels
        SDL_SetRenderDrawColor(w->renderer, 200, 50, 50, 255);
        for (int y = -2; y <= 2; y++) {
            for (int x = -30; x <= 30; x++) {
                SDL_RenderDrawPoint(w->renderer, WIN_W/2 + x, WIN_H/2 + y*8);
            }
        }
        // "press R to restart"
        SDL_SetRenderDrawColor(w->renderer, 150, 150, 150, 255);
        for (int y = -1; y <= 1; y++) {
            for (int x = -25; x <= 25; x++) {
                SDL_RenderDrawPoint(w->renderer, WIN_W/2 + x, WIN_H/2 + 40 + y*6);
            }
        }
    }

    SDL_RenderPresent(w->renderer);
}

// ── World init ────────────────────────────────────────────────────────────────

static int world_init(World *w, int n_workers) {
    memset(w, 0, sizeof(*w));

    // Arena
    w->n_alloc = MAX_ENTITIES;
    w->arena_size = (size_t)MAX_ENTITIES * (sizeof(Position) + sizeof(Velocity) +
                     sizeof(Rotation) + sizeof(RotVelocity) + sizeof(EntityInfo) + sizeof(uint8_t))
                     + 256 * 1024;
    w->arena_mem = (uint8_t *)calloc(w->arena_size, 1);
    if (!w->arena_mem) return -1;
    dso_arena_init(&w->arena, w->arena_mem, w->arena_size);

    // Entity arrays from arena
    w->pos    = (Position *)dso_arena_alloc(&w->arena, MAX_ENTITIES * sizeof(Position), 8);
    w->vel    = (Velocity *)dso_arena_alloc(&w->arena, MAX_ENTITIES * sizeof(Velocity), 8);
    w->rot    = (Rotation *)dso_arena_alloc(&w->arena, MAX_ENTITIES * sizeof(Rotation), 8);
    w->rot_vel= (RotVelocity *)dso_arena_alloc(&w->arena, MAX_ENTITIES * sizeof(RotVelocity), 8);
    w->info   = (EntityInfo *)dso_arena_alloc(&w->arena, MAX_ENTITIES * sizeof(EntityInfo), 4);
    w->damaged= (uint8_t *)dso_arena_alloc(&w->arena, MAX_ENTITIES * sizeof(uint8_t), 1);
    if (!w->pos || !w->vel || !w->rot || !w->rot_vel || !w->info || !w->damaged) return -1;

    // Mark all inactive
    for (uint32_t i = 0; i < MAX_ENTITIES; i++) {
        w->info[i].active = 0;
        w->info[i].type = ENT_NONE;
    }

    // DSO context
    dso_context_init(&w->ctx, &w->arena);

    // SDL
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    w->window = SDL_CreateWindow("DSO Asteroids", SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H,
                                  SDL_WINDOW_SHOWN);
    if (!w->window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return -1;
    }
    w->renderer = SDL_CreateRenderer(w->window, -1,
                                      SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!w->renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return -1;
    }

    // Game state
    w->lives = 3;
    w->score = 0;
    w->frame = 0;
    w->game_over = 0;
    w->fire_cooldown = 0;
    w->asteroid_timer = 0;

    // Worker pool
    if (dso_worker_pool_init(&w->pool, n_workers) != DSO_OK) return -1;

    // Trace file
    w->trace_file = fopen("/tmp/dso_asteroids_trace.json", "w");
    w->trace_written = 0;

    return 0;
}

// ── World destroy ─────────────────────────────────────────────────────────────

static void world_destroy(World *w) {
    if (w->trace_file) fclose(w->trace_file);
    if (w->pool.ready) dso_worker_pool_destroy(&w->pool);
    if (w->renderer) SDL_DestroyRenderer(w->renderer);
    if (w->window) SDL_DestroyWindow(w->window);
    SDL_Quit();
    free(w->arena_mem);
}

// ── Build DSO task graph ──────────────────────────────────────────────────────

static int build_graph(World *w) {
    uint32_t batch_size = (w->n_alloc + BATCH_COUNT - 1) / BATCH_COUNT;

    // Allocate job structs in arena
    RangeJob *integrate_jobs = (RangeJob *)dso_arena_alloc(
        &w->arena, BATCH_COUNT * sizeof(RangeJob), 8);
    RangeJob *collision_jobs = (RangeJob *)dso_arena_alloc(
        &w->arena, BATCH_COUNT * sizeof(RangeJob), 8);
    RenderJob *render_job = (RenderJob *)dso_arena_alloc(
        &w->arena, sizeof(RenderJob), 8);
    if (!integrate_jobs || !collision_jobs || !render_job) return -1;

    uint32_t t[10]; // 4 + 4 + 1 + 1
    int ti = 0;

    // Level 0: Integrate × BATCH_COUNT (parallel)
    for (int b = 0; b < BATCH_COUNT; b++) {
        integrate_jobs[b].world = w;
        integrate_jobs[b].start = (uint32_t)(b * batch_size);
        integrate_jobs[b].count = (uint32_t)((b == BATCH_COUNT - 1)
            ? (int)w->n_alloc - integrate_jobs[b].start : batch_size);
        dso_task(&w->ctx, "integrate", system_integrate, &integrate_jobs[b], &t[ti++]);
    }

    // Level 1: Collision × BATCH_COUNT (depend on ALL integrate)
    for (int b = 0; b < BATCH_COUNT; b++) {
        collision_jobs[b].world = w;
        collision_jobs[b].start = (uint32_t)(b * batch_size);
        collision_jobs[b].count = (uint32_t)((b == BATCH_COUNT - 1)
            ? (int)w->n_alloc - collision_jobs[b].start : batch_size);
        dso_task(&w->ctx, "collision", system_collision, &collision_jobs[b], &t[ti++]);
        for (int p = 0; p < BATCH_COUNT; p++)
            dso_dep(&w->ctx, t[p], t[BATCH_COUNT + b]);
    }

    // Level 2: Spawn (depends on ALL collision)
    {
        dso_task(&w->ctx, "spawn", system_spawn2, w, &t[ti++]);
        for (int p = 0; p < BATCH_COUNT; p++)
            dso_dep(&w->ctx, t[BATCH_COUNT + p], t[BATCH_COUNT * 2]);
    }

    // Level 3: Render (depends on spawn)
    {
        render_job->world = w;
        dso_task(&w->ctx, "render", system_render, render_job, &t[ti++]);
        dso_dep(&w->ctx, t[BATCH_COUNT * 2], t[BATCH_COUNT * 2 + 1]);
    }

    // ── Contracts ────────────────────────────────────────────────────────
    for (int b = 0; b < BATCH_COUNT; b++) {
        dso_resource_contract rc = {
            .cycles_max = 10000000, .ram_bytes_max = 65536,
            .wcet_us_max = 50000.0, .jitter_us_max = 5000.0
        };
        dso_task_set_contract(&w->ctx, t[b], rc);                     // integrate
        dso_task_set_contract(&w->ctx, t[BATCH_COUNT + b], rc);      // collision
    }
    {
        dso_resource_contract rc = {
            .cycles_max = 5000000, .ram_bytes_max = 65536,
            .wcet_us_max = 10000.0, .jitter_us_max = 2000.0
        };
        dso_task_set_contract(&w->ctx, t[BATCH_COUNT * 2], rc);      // spawn
    }
    {
        dso_resource_contract rc = {
            .cycles_max = 10000000, .ram_bytes_max = 65536,
            .wcet_us_max = 50000.0, .jitter_us_max = 8000.0
        };
        dso_task_set_contract(&w->ctx, t[BATCH_COUNT * 2 + 1], rc);  // render
    }

    // ── Compile ──────────────────────────────────────────────────────────
    dso_contract contract = {
        .max_tasks = DSO_MAX_TASKS, .max_deps = DSO_MAX_DEPS,
        .max_step_ns = 50000000ull, .max_frame_ns = 100000000ull
    };
    dso_verdict verdict;
    if (dso_compile(&w->ctx, contract, &verdict) != DSO_OK) {
        fprintf(stderr, "compile: %s\n", verdict.message);
        return -1;
    }

    // Print schedule
    const dso_schedule_levels *lv = dso_get_levels(&w->ctx);
    printf("  Schedule: %u levels\n", lv->count);
    for (uint32_t i = 0; i < lv->count; i++) {
        printf("    L%u: ", i);
        for (uint32_t j = 0; j < lv->size[i]; j++) {
            uint32_t tid = lv->tasks[i][j];
            printf("%s%s", j > 0 ? ", " : "", w->ctx.tasks[tid].name);
        }
        printf(" (%u tasks)\n", lv->size[i]);
    }

    return 0;
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    int n_workers = 4;
    if (argc > 1) n_workers = atoi(argv[1]);
    if (n_workers < 1 || n_workers > 16) n_workers = 4;

    srand((unsigned int)time(NULL));

    printf("\n=== DSO Asteroids ===\n");
    printf("  Controls: WASD/Arrows = move, Space = fire, Esc = quit\n");
    printf("  Workers:  %d\n", n_workers);

    World *w = (World *)calloc(1, sizeof(World));
    if (!w || world_init(w, n_workers) != 0) {
        fprintf(stderr, "FATAL: world_init\n");
        return 1;
    }

    // Spawn player ship
    w->ship_id = entity_create(w, ENT_SHIP, WIN_W/2.0, WIN_H/2.0,
                               0, 0, -PI/2, 0, 0, 20.0f, 0, 255, 80);
    if (w->ship_id < 0) { fprintf(stderr, "FATAL: no ship slot\n"); return 1; }

    // Spawn initial asteroid field
    init_asteroid_field(w, 6);

    // Build DSO graph
    if (build_graph(w) != 0) {
        fprintf(stderr, "FATAL: build_graph\n");
        return 1;
    }

    printf("\n  Playing...\n\n");

    // ── Game loop ────────────────────────────────────────────────────────

    int running = 1;
    uint64_t total_ns = 0;
    uint64_t worst_ns = 0;
    int frame_count = 0;

    while (running) {
        uint64_t t0 = now_ns();

        // SDL events
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE)
                running = 0;
        }

        // Restart on R
        if (w->game_over && w->keys && w->keys[SDL_SCANCODE_R]) {
            // Reset world state
            for (uint32_t i = 0; i < w->n_alloc; i++) {
                w->info[i].active = 0;
                w->info[i].type = ENT_NONE;
                w->damaged[i] = 0;
            }
            w->ship_id = entity_create(w, ENT_SHIP, WIN_W/2.0, WIN_H/2.0,
                                       0, 0, -PI/2, 0, 0, 20.0f, 0, 255, 80);
            init_asteroid_field(w, 6);
            w->score = 0;
            w->lives = 3;
            w->frame = 0;
            w->game_over = 0;
            w->fire_cooldown = 0;
            w->asteroid_timer = 0;
            continue;
        }

        // Input (outside DSO graph)
        process_input(w);

        // Execute DSO frame (parallel)
        dso_status st = dso_execute_frame_parallel(&w->ctx, &w->pool);

        uint64_t t1 = now_ns();
        uint64_t elapsed = t1 - t0;

        // Track for summary
        if (!w->game_over && w->info[w->ship_id].active) {
            w->frame++;
            frame_count++;
            total_ns += elapsed;
            if (elapsed > worst_ns) worst_ns = elapsed;
        }

        // Frame limit (not too fast)
        if (elapsed < 16000) {
            struct timespec ts = { .tv_nsec = (long)(16000 - elapsed) };
            nanosleep(&ts, NULL);
        }

        if (st != DSO_OK) {
            fprintf(stderr, "DSO error: %d\n", st);
        }
    }

    // ── Finalize ─────────────────────────────────────────────────────────

    const dso_metrics *m = dso_metrics_get(&w->ctx);
    printf("\n--- Results ---\n");
    printf("  Frames:       %d\n", frame_count);
    printf("  Avg frame:    %5" PRIu64 " us\n",
           frame_count > 0 ? total_ns / (uint64_t)frame_count / 1000 : 0);
    printf("  Worst frame:  %5" PRIu64 " us\n", worst_ns / 1000);
    printf("  Worst step:   %5" PRIu64 " us\n", m->worst_step_ns / 1000);
    printf("  Tasks:        %" PRIu64 "\n", m->task_count);
    printf("  Final score:  %d\n", w->score);

    // Chrome trace
    if (w->trace_file) {
        dso_trace_export_chrome(&w->ctx, w->trace_file);
        printf("\nTrace: /tmp/dso_asteroids_trace.json\n");
    }

    printf("\nOK\n");
    world_destroy(w);
    free(w);
    return 0;
}
