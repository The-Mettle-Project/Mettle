/* are-we-fast-yet :: Bounce  (C reference, matches bounce.mettle 1:1) */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "../bench_time.h"

typedef struct { int x, y, xVel, yVel; } Ball;

static int rnd_seed = 74755;
static void rnd_reset(void) { rnd_seed = 74755; }
static int rnd_next(void) { rnd_seed = ((rnd_seed * 1309) + 13849) & 65535; return rnd_seed; }

static void ball_init(Ball* b) {
    b->x = rnd_next() % 500;
    b->y = rnd_next() % 500;
    b->xVel = (rnd_next() % 300) - 150;
    b->yVel = (rnd_next() % 300) - 150;
}

static int ball_bounce(Ball* b) {
    int xLimit = 500, yLimit = 500, bounced = 0;
    b->x += b->xVel;
    b->y += b->yVel;
    if (b->x > xLimit) { b->x = xLimit; b->xVel = 0 - abs(b->xVel); bounced = 1; }
    if (b->x < 0)      { b->x = 0;      b->xVel = abs(b->xVel);     bounced = 1; }
    if (b->y > yLimit) { b->y = yLimit; b->yVel = 0 - abs(b->yVel); bounced = 1; }
    if (b->y < 0)      { b->y = 0;      b->yVel = abs(b->yVel);     bounced = 1; }
    return bounced;
}

static int benchmark(Ball* balls) {
    rnd_reset();
    int ballCount = 100, bounces = 0;
    for (int i = 0; i < ballCount; i++) ball_init(&balls[i]);
    for (int i = 0; i < 50; i++)
        for (int j = 0; j < ballCount; j++)
            if (ball_bounce(&balls[j])) bounces++;
    return bounces;
}

int main(void) {
    int passes = 1500;
    Ball balls[100];
    int last = 0;
    uint64_t t0 = bench_time_us();
    for (int it = 0; it < passes; it++) last = benchmark(balls);
    uint64_t elapsed = bench_time_us() - t0;
    printf("Bounce\n  result   = %d\n  expected = 1331\n  iters    = %d\n  time_us  = %llu\n",
           last, passes, (unsigned long long)elapsed);
    return last == 1331 ? 0 : 1;
}
