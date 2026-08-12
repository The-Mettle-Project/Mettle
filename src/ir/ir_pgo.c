/* Zero-run PGO: compile-time interpreted profiling. See ir_pgo.h. */
#include "ir_pgo.h"
#include "ir_interp.h"
#include "../common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IR_PGO_FUEL 64000000LL
#define IR_PGO_DEFAULT_HOT 1024LL

typedef struct {
  char *name;
  long long calls;
  long long body_steps; /* instructions executed inside the function */
} IRPgoEntry;

typedef struct {
  char *function_name;
  char *filename;
  size_t line;
  size_t column;
  long long count;
} IRPgoSiteEntry;

static IRPgoEntry *g_entries = NULL;
static size_t g_entry_count = 0;
static size_t g_entry_capacity = 0;
static IRPgoSiteEntry *g_sites = NULL;
static size_t g_site_count = 0;
static size_t g_site_capacity = 0;
static int g_profile_valid = 0;
static long long g_total_steps = 0;
static char g_run_status[64] = "";

long long ir_pgo_hot_threshold(void) {
  static long long cached = -1;
  if (cached < 0) {
    const char *env = getenv("METTLE_PGO_HOT");
    cached = env && env[0] ? atoll(env) : IR_PGO_DEFAULT_HOT;
    if (cached < 1) {
      cached = 1;
    }
  }
  return cached;
}

void ir_pgo_reset(void) {
  for (size_t i = 0; i < g_entry_count; i++) {
    mettle_free_string(g_entries[i].name);
  }
  for (size_t i = 0; i < g_site_count; i++) {
    free(g_sites[i].function_name);
    free(g_sites[i].filename);
  }
  free(g_entries);
  free(g_sites);
  g_entries = NULL;
  g_sites = NULL;
  g_entry_count = 0;
  g_entry_capacity = 0;
  g_site_count = 0;
  g_site_capacity = 0;
  g_profile_valid = 0;
  g_total_steps = 0;
  g_run_status[0] = '\0';
}

int ir_pgo_enabled(void) { return g_profile_valid; }

static IRPgoEntry *ir_pgo_entry(const char *name, int create) {
  for (size_t i = 0; i < g_entry_count; i++) {
    if (strcmp(g_entries[i].name, name) == 0) {
      return &g_entries[i];
    }
  }
  if (!create) {
    return NULL;
  }
  if (g_entry_count >= g_entry_capacity) {
    size_t new_capacity = g_entry_capacity ? g_entry_capacity * 2 : 64;
    IRPgoEntry *grown =
        realloc(g_entries, new_capacity * sizeof(IRPgoEntry));
    if (!grown) {
      return NULL;
    }
    g_entries = grown;
    g_entry_capacity = new_capacity;
  }
  IRPgoEntry *entry = &g_entries[g_entry_count];
  entry->name = mettle_strdup(name);
  if (!entry->name) {
    return NULL;
  }
  entry->calls = 0;
  entry->body_steps = 0;
  g_entry_count++;
  return entry;
}

static int ir_pgo_location_usable(SourceLocation location) {
  return location.line > 0;
}

static int ir_pgo_location_matches(const IRPgoSiteEntry *entry,
                                   const char *function_name,
                                   SourceLocation location) {
  const char *filename = location.filename ? location.filename : "";
  return entry && function_name && entry->function_name &&
         strcmp(entry->function_name, function_name) == 0 &&
         entry->line == location.line && entry->column == location.column &&
         entry->filename && strcmp(entry->filename, filename) == 0;
}

static IRPgoSiteEntry *ir_pgo_site_entry(const char *function_name,
                                         SourceLocation location,
                                         int create) {
  if (!function_name || !ir_pgo_location_usable(location)) {
    return NULL;
  }
  for (size_t i = 0; i < g_site_count; i++) {
    if (ir_pgo_location_matches(&g_sites[i], function_name, location)) {
      return &g_sites[i];
    }
  }
  if (!create) {
    return NULL;
  }
  if (g_site_count >= g_site_capacity) {
    size_t new_capacity = g_site_capacity ? g_site_capacity * 2 : 256;
    IRPgoSiteEntry *grown =
        realloc(g_sites, new_capacity * sizeof(IRPgoSiteEntry));
    if (!grown) {
      return NULL;
    }
    g_sites = grown;
    g_site_capacity = new_capacity;
  }
  IRPgoSiteEntry *entry = &g_sites[g_site_count];
  entry->function_name = mettle_strdup(function_name);
  entry->filename =
      mettle_strdup(location.filename ? location.filename : "");
  entry->line = location.line;
  entry->column = location.column;
  entry->count = 0;
  if (!entry->function_name || !entry->filename) {
    free(entry->function_name);
    free(entry->filename);
    entry->function_name = NULL;
    entry->filename = NULL;
    return NULL;
  }
  g_site_count++;
  return entry;
}

static void ir_pgo_note_site(const char *function_name, SourceLocation location,
                             long long count) {
  IRPgoSiteEntry *entry = ir_pgo_site_entry(function_name, location, 1);
  if (entry) {
    entry->count += count;
  }
}

int ir_pgo_profile_program(IRProgram *program) {
  ir_pgo_reset();
  if (!program) {
    return 0;
  }
  IRFunction *main_fn = NULL;
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *fn = program->functions[i];
    if (fn && fn->name && strcmp(fn->name, "main") == 0 &&
        fn->instruction_count > 0) {
      main_fn = fn;
      break;
    }
  }
  if (!main_fn) {
    snprintf(g_run_status, sizeof(g_run_status), "no main()");
    return 0;
  }

  IRInterpMachine *machine = ir_interp_create(program);
  if (!machine) {
    return 0;
  }
  ir_interp_enable_counting(machine);
  IRInterpValue result = {0, 0, 0};
  IRInterpStatus status =
      ir_interp_run(machine, main_fn, NULL, 0, &result, IR_PGO_FUEL);
  switch (status) {
  case IR_INTERP_OK:
    snprintf(g_run_status, sizeof(g_run_status), "ran to completion");
    break;
  case IR_INTERP_FUEL:
    snprintf(g_run_status, sizeof(g_run_status),
             "partial (fuel cap %lldM steps)", IR_PGO_FUEL / 1000000);
    break;
  case IR_INTERP_GUARD_TRAP:
    snprintf(g_run_status, sizeof(g_run_status), "partial (guard trap)");
    break;
  case IR_INTERP_UNSUPPORTED:
    snprintf(g_run_status, sizeof(g_run_status), "partial (%s)",
             ir_interp_status_detail(machine));
    break;
  default:
    snprintf(g_run_status, sizeof(g_run_status), "partial (trap)");
    break;
  }

  /* Harvest: for each executed CALL, credit the callee; sum body steps. */
  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *fn = program->functions[f];
    if (!fn) {
      continue;
    }
    size_t n = 0;
    const long long *counts = ir_interp_get_counts(machine, fn, &n);
    if (!counts) {
      continue;
    }
    for (size_t i = 0; i < n && i < fn->instruction_count; i++) {
      if (counts[i] == 0) {
        continue;
      }
      g_total_steps += counts[i];
      IRPgoEntry *self = ir_pgo_entry(fn->name, 1);
      if (self) {
        self->body_steps += counts[i];
      }
      const IRInstruction *insn = &fn->instructions[i];
      ir_pgo_note_site(fn->name, insn->location, counts[i]);
      if (insn->op == IR_OP_CALL && insn->text) {
        IRPgoEntry *callee = ir_pgo_entry(insn->text, 1);
        if (callee) {
          callee->calls += counts[i];
        }
      }
    }
  }
  ir_interp_destroy(machine);

  /* A profile that never left main() is still valid (it proves everything
   * else is cold on this run), but demand SOME execution. */
  g_profile_valid = g_total_steps > 0;
  return g_profile_valid;
}

long long ir_pgo_callee_calls(const char *name) {
  if (!g_profile_valid || !name) {
    return -1;
  }
  IRPgoEntry *entry = ir_pgo_entry(name, 0);
  return entry ? entry->calls : 0;
}

long long ir_pgo_function_body_steps(const char *name) {
  if (!g_profile_valid || !name) {
    return -1;
  }
  IRPgoEntry *entry = ir_pgo_entry(name, 0);
  return entry ? entry->body_steps : 0;
}

long long ir_pgo_site_count(const char *function_name,
                            SourceLocation location) {
  if (!g_profile_valid || !function_name ||
      !ir_pgo_location_usable(location)) {
    return -1;
  }
  IRPgoSiteEntry *entry = ir_pgo_site_entry(function_name, location, 0);
  if (entry) {
    return entry->count;
  }

  /* Inlining moves a callee's instructions into the caller but preserves the
   * original source location. If the function-qualified lookup misses, fall
   * back to the location total so a hot inlined loop does not become falsely
   * cold just because its containing IRFunction changed. */
  const char *filename = location.filename ? location.filename : "";
  long long total = 0;
  for (size_t i = 0; i < g_site_count; i++) {
    if (g_sites[i].line == location.line &&
        g_sites[i].column == location.column && g_sites[i].filename &&
        strcmp(g_sites[i].filename, filename) == 0) {
      total += g_sites[i].count;
    }
  }
  return total;
}

int ir_pgo_function_is_hot(const char *name) {
  if (!g_profile_valid || !name) {
    return 0;
  }
  IRPgoEntry *entry = ir_pgo_entry(name, 0);
  return entry && entry->calls >= ir_pgo_hot_threshold();
}

void ir_pgo_print_summary(void) {
  if (!g_profile_valid) {
    fprintf(stderr, "pgo: no profile (%s); optimizer falls back to static "
                    "heuristics\n",
            g_run_status[0] ? g_run_status : "?");
    return;
  }
  fprintf(stderr,
          "pgo: interpreted main() at compile time - %lld steps (%s), "
          "%zu functions touched, hot threshold %lld calls\n",
          g_total_steps, g_run_status, g_entry_count, ir_pgo_hot_threshold());

  /* Top functions by interpreted calls. */
  unsigned char shown_flags[512] = {0};
  for (int rank = 0; rank < 5; rank++) {
    size_t best = (size_t)-1;
    for (size_t i = 0; i < g_entry_count && i < 512; i++) {
      if (shown_flags[i] || g_entries[i].calls == 0) {
        continue;
      }
      if (best == (size_t)-1 || g_entries[i].calls > g_entries[best].calls) {
        best = i;
      }
    }
    if (best == (size_t)-1) {
      break;
    }
    shown_flags[best] = 1;
    fprintf(stderr, "pgo:   %s: %lld calls%s\n", g_entries[best].name,
            g_entries[best].calls,
            g_entries[best].calls >= ir_pgo_hot_threshold() ? "  [hot]" : "");
  }
}
