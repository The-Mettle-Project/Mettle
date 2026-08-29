#include "compiler_self_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <stdint.h>

#define MTLC_SELF_PROFILE_CAPACITY (1u << 21)
#define MTLC_SELF_PROFILE_SYM_NAME 512
#define MTLC_SELF_PROFILE_REPORT_ROWS 45

typedef struct {
  char name[MTLC_SELF_PROFILE_SYM_NAME];
  unsigned long long samples;
} MtlcSelfProfileEntry;

static unsigned long long *g_self_profile_addresses;
static unsigned long long *g_self_profile_callers;
static volatile long g_self_profile_used;
static volatile long g_self_profile_stop;
static HANDLE g_self_profile_thread;
static HANDLE g_self_profile_target;
static unsigned long long g_self_profile_interval_ticks;
static unsigned long long g_self_profile_missed;

static unsigned long long mettle_self_profile_ticks(void) {
  LARGE_INTEGER counter;

  QueryPerformanceCounter(&counter);
  return (unsigned long long)counter.QuadPart;
}

static DWORD WINAPI mettle_self_profile_sampler(LPVOID unused) {
  unsigned long long next = mettle_self_profile_ticks();

  (void)unused;
  while (!g_self_profile_stop) {
    CONTEXT context;
    unsigned long long now = mettle_self_profile_ticks();
    long slot;

    if (now < next) {
      SwitchToThread();
      continue;
    }
    next = now + g_self_profile_interval_ticks;

    memset(&context, 0, sizeof(context));
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (SuspendThread(g_self_profile_target) == (DWORD)-1) {
      continue;
    }
    if (!GetThreadContext(g_self_profile_target, &context)) {
      ResumeThread(g_self_profile_target);
      continue;
    }
    ResumeThread(g_self_profile_target);

    slot = InterlockedIncrement(&g_self_profile_used) - 1;
    if (slot < (long)MTLC_SELF_PROFILE_CAPACITY - 1) {
      unsigned long long caller = 0;
      SIZE_T read = 0;

      ReadProcessMemory(GetCurrentProcess(),
                        (LPCVOID)(ULONG_PTR)(context.Rbp + 8), &caller,
                        sizeof(caller), &read);
      g_self_profile_addresses[slot] = (unsigned long long)context.Rip;
      g_self_profile_callers[slot] = read == sizeof(caller) ? caller : 0;
    } else {
      g_self_profile_missed++;
    }
  }
  return 0;
}

void mettle_compiler_self_profile_start(void) {
  const char *spec = getenv("METTLE_SELF_PROFILE");
  LARGE_INTEGER frequency;
  unsigned interval_us = 200;
  DWORD thread_id = 0;

  if (!spec || spec[0] == '\0' || strcmp(spec, "0") == 0) {
    return;
  }
  if (spec[0] >= '1' && spec[0] <= '9') {
    unsigned parsed = (unsigned)atoi(spec);
    if (parsed > 1) {
      interval_us = parsed;
    }
  }

  g_self_profile_addresses = (unsigned long long *)VirtualAlloc(
      NULL, (SIZE_T)MTLC_SELF_PROFILE_CAPACITY * sizeof(unsigned long long),
      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  g_self_profile_callers = (unsigned long long *)VirtualAlloc(
      NULL, (SIZE_T)MTLC_SELF_PROFILE_CAPACITY * sizeof(unsigned long long),
      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!g_self_profile_addresses || !g_self_profile_callers) {
    return;
  }
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                       GetCurrentProcess(), &g_self_profile_target, 0, FALSE,
                       DUPLICATE_SAME_ACCESS)) {
    return;
  }
  QueryPerformanceFrequency(&frequency);
  g_self_profile_interval_ticks =
      (unsigned long long)frequency.QuadPart * interval_us / 1000000ull;
  if (g_self_profile_interval_ticks == 0) {
    g_self_profile_interval_ticks = 1;
  }
  g_self_profile_thread =
      CreateThread(NULL, 0, mettle_self_profile_sampler, NULL, 0, &thread_id);
}

static int mettle_self_profile_symbolize(HANDLE process,
                                         unsigned long long address,
                                         char *out, size_t capacity) {
  unsigned char storage[sizeof(SYMBOL_INFO) + MTLC_SELF_PROFILE_SYM_NAME];
  SYMBOL_INFO *symbol = (SYMBOL_INFO *)storage;
  DWORD64 displacement = 0;

  memset(storage, 0, sizeof(storage));
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = MTLC_SELF_PROFILE_SYM_NAME - 1;
  if (SymFromAddr(process, (DWORD64)address, &displacement, symbol) &&
      symbol->Name[0]) {
    snprintf(out, capacity, "%s", symbol->Name);
    return 1;
  }
  snprintf(out, capacity, "<0x%llx>", address);
  return 0;
}

static int mettle_self_profile_compare(const void *left, const void *right) {
  const MtlcSelfProfileEntry *a = (const MtlcSelfProfileEntry *)left;
  const MtlcSelfProfileEntry *b = (const MtlcSelfProfileEntry *)right;

  if (a->samples > b->samples) {
    return -1;
  }
  if (a->samples < b->samples) {
    return 1;
  }
  return 0;
}

static unsigned long long mettle_self_profile_module_base(void) {
  return (unsigned long long)(uintptr_t)GetModuleHandleA(NULL);
}

static void mettle_self_profile_dump(long taken) {
  const char *path = getenv("METTLE_SELF_PROFILE_OUT");
  unsigned long long base = mettle_self_profile_module_base();
  FILE *out;

  if (!path || path[0] == '\0') {
    path = "mettle.selfprof";
  }
  out = fopen(path, "w");
  if (!out) {
    return;
  }
  fprintf(out, "module %llx\n", base);
  for (long i = 0; i < taken; i++) {
    fprintf(out, "%llx %llx\n", g_self_profile_addresses[i],
            g_self_profile_callers[i]);
  }
  fclose(out);
}

void mettle_compiler_self_profile_report(void) {
  HANDLE process = GetCurrentProcess();
  MtlcSelfProfileEntry *entries;
  long taken;
  size_t entry_count = 0;
  size_t entry_capacity;
  unsigned long long total = 0;

  if (!g_self_profile_thread) {
    return;
  }
  g_self_profile_stop = 1;
  WaitForSingleObject(g_self_profile_thread, 2000);

  taken = g_self_profile_used;
  if (taken > (long)MTLC_SELF_PROFILE_CAPACITY) {
    taken = (long)MTLC_SELF_PROFILE_CAPACITY;
  }
  if (taken <= 0) {
    fprintf(stderr, "-- self profile: no samples --\n");
    return;
  }

  mettle_self_profile_dump(taken);

  entry_capacity = (size_t)taken;
  entries = (MtlcSelfProfileEntry *)calloc(entry_capacity,
                                           sizeof(MtlcSelfProfileEntry));
  if (!entries) {
    return;
  }

  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS |
                SYMOPT_FAIL_CRITICAL_ERRORS);
  SymInitialize(process, NULL, TRUE);

  for (long i = 0; i < taken; i++) {
    char name[MTLC_SELF_PROFILE_SYM_NAME];
    size_t slot;

    mettle_self_profile_symbolize(process, g_self_profile_addresses[i], name,
                                  sizeof(name));
    for (slot = 0; slot < entry_count; slot++) {
      if (strcmp(entries[slot].name, name) == 0) {
        break;
      }
    }
    if (slot == entry_count) {
      if (entry_count == entry_capacity) {
        continue;
      }
      snprintf(entries[slot].name, sizeof(entries[slot].name), "%s", name);
      entry_count++;
    }
    entries[slot].samples++;
    total++;
  }

  qsort(entries, entry_count, sizeof(MtlcSelfProfileEntry),
        mettle_self_profile_compare);

  fprintf(stderr, "-- compiler self profile: %llu samples, %zu symbols --\n",
          total, entry_count);
  for (size_t i = 0; i < entry_count && i < MTLC_SELF_PROFILE_REPORT_ROWS;
       i++) {
    fprintf(stderr, "  %6.2f%%  %8llu  %s\n",
            total ? (double)entries[i].samples * 100.0 / (double)total : 0.0,
            entries[i].samples, entries[i].name);
  }
  if (g_self_profile_missed) {
    fprintf(stderr, "  (%llu samples dropped: buffer full)\n",
            g_self_profile_missed);
  }
  free(entries);
}

#else

void mettle_compiler_self_profile_start(void) {}
void mettle_compiler_self_profile_report(void) {}

#endif
