#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#include "../bench_time.h"


#define DIM 64
#define VOXELS 262144
#define WIDTH 200
#define HEIGHT 150
#define MAX_STEPS 192
#define PASSES 3

typedef struct {
    double t;
    int32_t vx;
    int32_t vy;
    int32_t vz;
    int32_t axis;
    int32_t sign;
    int32_t found;
} Hit;

static int32_t idx3(int32_t x, int32_t y, int32_t z) {
  return (z * DIM + y) * DIM + x;
}

static void build_world(uint8_t *solid, uint32_t seed) {
  uint32_t state = seed;
  int32_t i = 0;
  while (i < VOXELS) {
    solid[i] = 0;
    i += 1;
  }
  int32_t z = 0;
  while (z < DIM) {
    int32_t y = 0;
    while (y < DIM) {
      int32_t x = 0;
      while (x < DIM) {
        state ^= (state << 13);
        state ^= (state >> 17);
        state ^= (state << 5);
        uint8_t v = 0;
        if (y < 3) {
          v = 1;
        } else {
          int32_t col = (int32_t)(state % 1024);
          int32_t height = 4 + ((x * 7 + z * 13) % 19);
          if (y < height && col < 210) {
            v = 1;
          }
        }
        solid[idx3(x, y, z)] = v;
        x += 1;
      }
      y += 1;
    }
    z += 1;
  }
}

static void trace(uint8_t *solid, double ox, double oy, double oz, double dx, double dy, double dz, Hit *h) {
  h->found = 0;
  h->t = 0.0;
  h->axis = 0;
  h->sign = 0;

  int32_t x = (int32_t)ox;
  int32_t y = (int32_t)oy;
  int32_t z = (int32_t)oz;

  int32_t stepx = 1;
  int32_t stepy = 1;
  int32_t stepz = 1;
  if (dx < 0.0) { stepx = -1; }
  if (dy < 0.0) { stepy = -1; }
  if (dz < 0.0) { stepz = -1; }

  double idx = 1.0 / dx;
  double idy = 1.0 / dy;
  double idz = 1.0 / dz;
  if (idx < 0.0) { idx = -idx; }
  if (idy < 0.0) { idy = -idy; }
  if (idz < 0.0) { idz = -idz; }

  double bx = (double)x;
  double by = (double)y;
  double bz = (double)z;

  double tmx = 0.0;
  double tmy = 0.0;
  double tmz = 0.0;
  if (stepx > 0) { tmx = (bx + 1.0 - ox) * idx; } else { tmx = (ox - bx) * idx; }
  if (stepy > 0) { tmy = (by + 1.0 - oy) * idy; } else { tmy = (oy - by) * idy; }
  if (stepz > 0) { tmz = (bz + 1.0 - oz) * idz; } else { tmz = (oz - bz) * idz; }

  double t = 0.0;
  int32_t axis = 0;
  int32_t steps = 0;
  while (steps < MAX_STEPS) {
    if (x < 0 || y < 0 || z < 0 || x >= DIM || y >= DIM || z >= DIM) {
      return;
    }
    if (solid[idx3(x, y, z)] != 0) {
      h->found = 1;
      h->t = t;
      h->vx = x;
      h->vy = y;
      h->vz = z;
      h->axis = axis;
      if (axis == 0) { h->sign = stepx; }
      if (axis == 1) { h->sign = stepy; }
      if (axis == 2) { h->sign = stepz; }
      return;
    }
    if (tmx < tmy && tmx < tmz) {
      t = tmx;
      tmx += idx;
      x += stepx;
      axis = 0;
    } else if (tmy < tmz) {
      t = tmy;
      tmy += idy;
      y += stepy;
      axis = 1;
    } else {
      t = tmz;
      tmz += idz;
      z += stepz;
      axis = 2;
    }
    steps += 1;
  }
}

static double shade(uint8_t *solid, Hit *h, double dx, double dy, double dz) {
  double nx = 0.0;
  double ny = 0.0;
  double nz = 0.0;
  if (h->axis == 0) { nx = (double)(-h->sign); }
  if (h->axis == 1) { ny = (double)(-h->sign); }
  if (h->axis == 2) { nz = (double)(-h->sign); }

  double lambert = nx * 0.5 + ny * 0.75 + nz * 0.25;
  if (lambert < 0.0) {
    lambert = 0.0;
  }
  double atten = 1.0 / (1.0 + h->t * 0.0625);
  double base = 0.125 + lambert * 0.75;

  double rx = dx;
  double ry = dy;
  double rz = dz;
  if (h->axis == 0) { rx = -dx; }
  if (h->axis == 1) { ry = -dy; }
  if (h->axis == 2) { rz = -dz; }

  double ox = (double)h->vx + 0.5 + nx * 0.75;
  double oy = (double)h->vy + 0.5 + ny * 0.75;
  double oz = (double)h->vz + 0.5 + nz * 0.75;

  Hit bounce;
  trace(solid, ox, oy, oz, rx, ry, rz, &bounce);
  if (bounce.found != 0) {
    base *= 0.5;
  } else {
    base += 0.125;
  }
  return base * atten;
}

static uint64_t render(uint8_t *solid, int32_t *out_hits) {
  uint64_t h = 14695981039346656037ULL;
  int32_t hits = 0;
  double ox = 32.5;
  double oy = 40.5;
  double oz = 2.5;

  int32_t py = 0;
  while (py < HEIGHT) {
    double v = ((double)py / (double)HEIGHT - 0.5) * 2.0;
    int32_t px = 0;
    while (px < WIDTH) {
      double u = ((double)px / (double)WIDTH - 0.5) * 2.0;
      double dx = u * 1.5 + 0.0009765625;
      double dy = -v + 0.0009765625;
      double dz = 1.0;

      Hit hit;
      trace(solid, ox, oy, oz, dx, dy, dz, &hit);
      double shade_v = 0.0;
      if (hit.found != 0) {
        hits += 1;
        shade_v = shade(solid, &hit, dx, dy, dz);
      }
      int32_t q = (int32_t)(shade_v * 255.0);
      if (q < 0) { q = 0; }
      if (q > 255) { q = 255; }
      h ^= (uint64_t)q;
      h *= 1099511628211ULL;
      px += 1;
    }
    py += 1;
  }
  *out_hits = hits;
  return h;
}

static uint64_t round_trip(uint8_t *solid, int32_t *out_hits) {
  build_world(solid, 2463534242ULL);
  int32_t hits = 0;
  uint64_t r = render(solid, &hits);
  uint64_t h = 1469598103934665603ULL;
  h = h * 1000003 + r;
  h = h * 31 + (uint64_t)hits;
  *out_hits = hits;
  return h;
}

int main(void) {
    uint8_t *solid = (uint8_t *)malloc((size_t)VOXELS);
    if (solid == NULL) {
        printf("malloc failed\n");
        return 1;
    }

    printf("Voxel trace: %d^3 grid, %dx%d rays, DDA with one bounce\n", DIM, WIDTH, HEIGHT);

    int32_t hits = 0;
    uint64_t check = round_trip(solid, &hits);
    printf("  primary hits = %d\n", hits);
    printf("Checksum = %" PRIu64 "\n", check);

    printf("Benchmark: %d passes\n", PASSES);

    uint64_t t0 = bench_time_us();
    uint64_t bench_hash = 0;
    int32_t pass = 0;
    while (pass < PASSES) {
        int32_t h2 = 0;
        bench_hash = bench_hash * 1000003 + round_trip(solid, &h2);
        pass += 1;
    }
    uint64_t elapsed_us = bench_time_us() - t0;

    printf("Bench hash = %" PRIu64 "\n", bench_hash);
    printf("Time: %" PRIu64 " us\n", elapsed_us);

    uint64_t per_pass_us = elapsed_us / (uint64_t)PASSES;
    printf("Per pass: ~%" PRIu64 " us\n", per_pass_us);

    free(solid);
    return 0;
}
