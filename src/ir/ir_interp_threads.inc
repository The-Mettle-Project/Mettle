#define II_CPU_SLICE 2000
#define II_CPU_STACK (16u << 20)
#define II_CPU_WORKERS 4
#define II_WAIT_TIMEOUT 0x102u
#define II_WAIT_INFINITE 0xFFFFFFFFull

typedef int (*IICpuReady)(IRInterpMachine *machine, const void *argument);

static int ii_cpu_me(const IRInterpMachine *machine) {
  return machine->cpu.current ? machine->cpu.current->id : 0;
}

static long long ii_cpu_now_ns(IRInterpMachine *machine) {
  if (!machine->cpu.clock_started) {
    machine->cpu.clock_started = 1;
    machine->cpu.fuel_start = machine->fuel;
  }
  return machine->cpu.clock_ns + (machine->cpu.fuel_start - machine->fuel);
}

static void ii_cpu_pass_time(IRInterpMachine *machine, long long ns) {
  ii_cpu_now_ns(machine);
  machine->cpu.clock_ns += ns > 0 ? ns : 0;
}

static void ii_cpu_save(IRInterpMachine *machine, IICpuThread *t, size_t base) {
  t->depth = machine->depth;
  t->pure_depth = machine->pure_depth;
  t->pure_fn = machine->pure_fn;
  t->pure_declared = machine->pure_declared;
  t->current_insn = machine->current_insn;
  t->current_fn = machine->current_fn;
  t->current_call_loc = machine->current_call_loc;
  size_t extra = machine->effect_frame_count > base
                     ? machine->effect_frame_count - base
                     : 0;
  if (extra) {
    size_t bytes = extra * sizeof(*machine->effect_frames);
    t->effects = malloc(bytes);
    if (t->effects) {
      memcpy(t->effects, &machine->effect_frames[base], bytes);
      t->effect_count = extra;
    }
  }
  machine->effect_frame_count = base;
}

static int ii_cpu_load(IRInterpMachine *machine, IICpuThread *t) {
  machine->depth = t->depth;
  machine->pure_depth = t->pure_depth;
  machine->pure_fn = t->pure_fn;
  machine->pure_declared = t->pure_declared;
  machine->current_insn = t->current_insn;
  machine->current_fn = t->current_fn;
  machine->current_call_loc = t->current_call_loc;
  if (!t->effect_count) {
    return 1;
  }
  size_t need = machine->effect_frame_count + t->effect_count;
  if (need > machine->effect_frame_capacity) {
    void *grown =
        realloc(machine->effect_frames, need * sizeof(*machine->effect_frames));
    if (!grown) {
      ii_fail(machine, IR_INTERP_TRAP, "out of memory switching threads");
      return 0;
    }
    machine->effect_frames = grown;
    machine->effect_frame_capacity = need;
  }
  memcpy(&machine->effect_frames[machine->effect_frame_count], t->effects,
         t->effect_count * sizeof(*machine->effect_frames));
  machine->effect_frame_count = need;
  free(t->effects);
  t->effects = NULL;
  t->effect_count = 0;
  return 1;
}

static int ii_cpu_parallel_work(IRInterpMachine *machine, IIParallelJob *job,
                                long long slot);

static void ii_cpu_entry(void *argument) {
  IICpuThread *t = (IICpuThread *)argument;
  IRInterpMachine *machine = t->machine;
  if (t->job) {
    t->ok = ii_cpu_parallel_work(machine, t->job, t->slot);
    t->job->workers--;
    t->job = NULL;
  } else {
    IRInterpValue arg = t->arg;
    t->ok = ii_exec_function(machine, t->body, &arg,
                             t->body->parameter_count ? 1 : 0, &t->result);
  }
  t->finished = 1;
  machine->cpu.live--;
  machine->cpu.progress++;
  for (;;) {
    ir_fiber_yield(t->fiber);
  }
}

static int ii_cpu_resume(IRInterpMachine *machine, IICpuThread *t) {
  if (!t->fiber) {
    t->fiber = ir_fiber_create(machine->cpu.scheduler, II_CPU_STACK,
                               ii_cpu_entry, t);
    if (!t->fiber) {
      ii_fail(machine, IR_INTERP_TRAP, "could not start a thread");
      return 0;
    }
  }
  IICpuThread caller;
  memset(&caller, 0, sizeof(caller));
  size_t base = machine->effect_frame_count;
  ii_cpu_save(machine, &caller, base);
  if (!ii_cpu_load(machine, t)) {
    return 0;
  }
  machine->cpu.current = t;
  machine->cpu.slice = 0;
  ir_fiber_run(t->fiber);
  machine->cpu.current = NULL;
  ii_cpu_save(machine, t, base);
  ii_cpu_load(machine, &caller);
  if (t->finished) {
    ir_fiber_destroy(t->fiber);
    t->fiber = NULL;
    if (!t->ok && machine->status == IR_INTERP_OK) {
      ii_fail(machine, IR_INTERP_TRAP, "a thread failed");
    }
  }
  return machine->status == IR_INTERP_OK;
}

static int ii_cpu_pump(IRInterpMachine *machine) {
  for (size_t i = 0; machine->status == IR_INTERP_OK && i < machine->cpu.count;
       i++) {
    IICpuThread *t = machine->cpu.threads[i];
    if (!t->finished && !ii_cpu_resume(machine, t)) {
      return 0;
    }
  }
  return machine->status == IR_INTERP_OK;
}

static int ii_cpu_yield(IRInterpMachine *machine) {
  IICpuThread *t = machine->cpu.current;
  if (t) {
    ir_fiber_yield(t->fiber);
    return machine->status == IR_INTERP_OK;
  }
  return machine->cpu.live == 0 || ii_cpu_pump(machine);
}

static int ii_cpu_tick(IRInterpMachine *machine) {
  machine->cpu.slice = 0;
  return ii_cpu_yield(machine);
}

static int ii_cpu_expire_one(IRInterpMachine *machine) {
  for (size_t i = 0; i < machine->cpu.count; i++) {
    IICpuThread *t = machine->cpu.threads[i];
    if (!t->finished && t->timed && !t->timed_out) {
      t->timed_out = 1;
      return 1;
    }
  }
  return 0;
}

static int ii_cpu_wait(IRInterpMachine *machine, IICpuReady ready,
                       const void *argument, unsigned long long timeout_ms) {
  long long limit =
      timeout_ms >= II_WAIT_INFINITE ? -1 : (long long)timeout_ms + 1;
  for (long long rounds = 0;; rounds++) {
    if (ready(machine, argument)) {
      return 1;
    }
    if (limit >= 0 && rounds >= limit) {
      ii_cpu_pass_time(machine, (long long)timeout_ms * 1000000);
      return 0;
    }
    IICpuThread *t = machine->cpu.current;
    if (t) {
      t->timed = limit >= 0;
      ir_fiber_yield(t->fiber);
      t->timed = 0;
      if (machine->status != IR_INTERP_OK) {
        return -1;
      }
      if (t->timed_out) {
        t->timed_out = 0;
        if (ready(machine, argument)) {
          return 1;
        }
        ii_cpu_pass_time(machine, (long long)timeout_ms * 1000000);
        return 0;
      }
      continue;
    }
    unsigned long long progress = machine->cpu.progress;
    long long fuel = machine->fuel;
    if (!ii_cpu_pump(machine)) {
      return -1;
    }
    if (ready(machine, argument)) {
      return 1;
    }
    if (progress == machine->cpu.progress && fuel == machine->fuel &&
        !ii_cpu_expire_one(machine)) {
      if (limit >= 0) {
        ii_cpu_pass_time(machine, (long long)timeout_ms * 1000000);
        return 0;
      }
      ii_fail(machine, IR_INTERP_TRAP,
              "deadlock: every thread waits on another, and none can run");
      return -1;
    }
  }
}

static IICpuThread *ii_cpu_spawn(IRInterpMachine *machine, IRFunction *body,
                                 IRInterpValue arg, IIParallelJob *job,
                                 long long slot) {
  if (!machine->cpu.scheduler) {
    machine->cpu.scheduler = ir_fiber_scheduler_begin();
    if (!machine->cpu.scheduler) {
      ii_fail(machine, IR_INTERP_TRAP, "could not start the thread scheduler");
      return NULL;
    }
  }
  if (machine->cpu.count == machine->cpu.capacity) {
    size_t grown = machine->cpu.capacity ? machine->cpu.capacity * 2 : 8;
    IICpuThread **table = (IICpuThread **)realloc(
        machine->cpu.threads, grown * sizeof(IICpuThread *));
    if (!table) {
      ii_fail(machine, IR_INTERP_TRAP, "out of memory starting a thread");
      return NULL;
    }
    machine->cpu.threads = table;
    machine->cpu.capacity = grown;
  }
  IICpuThread *t = (IICpuThread *)calloc(1, sizeof(IICpuThread));
  if (!t) {
    ii_fail(machine, IR_INTERP_TRAP, "out of memory starting a thread");
    return NULL;
  }
  t->machine = machine;
  t->body = body;
  t->arg = arg;
  t->job = job;
  t->slot = slot;
  t->handle = ++machine->next_thread_handle;
  t->id = (int)machine->cpu.count + 1;
  machine->cpu.threads[machine->cpu.count++] = t;
  machine->cpu.live++;
  machine->cpu.progress++;
  return t;
}

static IICpuThread *ii_cpu_thread(IRInterpMachine *machine,
                                  unsigned long long handle) {
  for (size_t i = 0; i < machine->cpu.count; i++) {
    if (machine->cpu.threads[i]->handle == handle) {
      return machine->cpu.threads[i];
    }
  }
  return NULL;
}

static IISyncObject *ii_cpu_object(IRInterpMachine *machine,
                                   unsigned long long key, int create) {
  for (size_t i = 0; i < machine->cpu.object_count; i++) {
    if (machine->cpu.objects[i].key == key) {
      return &machine->cpu.objects[i];
    }
  }
  if (!create) {
    return NULL;
  }
  if (machine->cpu.object_count == machine->cpu.object_capacity) {
    size_t grown =
        machine->cpu.object_capacity ? machine->cpu.object_capacity * 2 : 16;
    IISyncObject *table = (IISyncObject *)realloc(
        machine->cpu.objects, grown * sizeof(IISyncObject));
    if (!table) {
      ii_fail(machine, IR_INTERP_TRAP, "out of memory for a lock");
      return NULL;
    }
    machine->cpu.objects = table;
    machine->cpu.object_capacity = grown;
  }
  IISyncObject *o = &machine->cpu.objects[machine->cpu.object_count++];
  memset(o, 0, sizeof(*o));
  o->key = key;
  return o;
}

static int ii_cpu_thread_done(IRInterpMachine *machine, const void *argument) {
  (void)machine;
  return ((const IICpuThread *)argument)->finished;
}

static int ii_cpu_lock_free(IRInterpMachine *machine, const void *argument) {
  const IISyncObject *o =
      ii_cpu_object(machine, *(const unsigned long long *)argument, 0);
  return !o || o->owner == 0 || o->owner == ii_cpu_me(machine) + 1;
}

typedef struct {
  unsigned long long key;
  unsigned long long seen;
} IICpuSignal;

static int ii_cpu_signalled(IRInterpMachine *machine, const void *argument) {
  const IICpuSignal *s = (const IICpuSignal *)argument;
  const IISyncObject *o = ii_cpu_object(machine, s->key, 0);
  return o && o->generation != s->seen;
}

static int ii_cpu_lock(IRInterpMachine *machine, unsigned long long key,
                       unsigned long long timeout_ms) {
  if (!ii_cpu_object(machine, key, 1)) {
    return -1;
  }
  int got = ii_cpu_wait(machine, ii_cpu_lock_free, &key, timeout_ms);
  if (got == 1) {
    IISyncObject *o = ii_cpu_object(machine, key, 1);
    o->owner = ii_cpu_me(machine) + 1;
    o->depth++;
  }
  return got;
}

static void ii_cpu_unlock(IRInterpMachine *machine, unsigned long long key) {
  IISyncObject *o = ii_cpu_object(machine, key, 0);
  if (o && o->owner == ii_cpu_me(machine) + 1 && --o->depth <= 0) {
    o->owner = 0;
    o->depth = 0;
    machine->cpu.progress++;
  }
}

static int ii_cpu_wait_signal(IRInterpMachine *machine, unsigned long long cv,
                              unsigned long long lock,
                              unsigned long long timeout_ms) {
  IISyncObject *o = ii_cpu_object(machine, cv, 1);
  if (!o) {
    return -1;
  }
  IICpuSignal s = {cv, o->generation};
  ii_cpu_unlock(machine, lock);
  int woke = ii_cpu_wait(machine, ii_cpu_signalled, &s, timeout_ms);
  if (woke < 0 || ii_cpu_lock(machine, lock, II_WAIT_INFINITE) < 0) {
    return -1;
  }
  return woke;
}

static void ii_cpu_signal(IRInterpMachine *machine, unsigned long long cv) {
  IISyncObject *o = ii_cpu_object(machine, cv, 1);
  if (o) {
    o->generation++;
    machine->cpu.progress++;
  }
}

static int ii_cpu_parallel_work(IRInterpMachine *machine, IIParallelJob *job,
                                long long slot) {
  while (job->next < job->count && machine->status == IR_INTERP_OK) {
    long long chunk = job->next++;
    long long lo = job->lo + job->span * chunk;
    long long hi = lo + job->span < job->hi ? lo + job->span : job->hi;
    IRInterpValue args[4] = {job->ctx, ii_int_value(lo), ii_int_value(hi),
                             ii_int_value(slot)};
    IRInterpValue result = ii_int_value(0);
    if (!ii_exec_function(machine, job->body, args, job->slots ? 4 : 3,
                          &result)) {
      return 0;
    }
    job->done++;
    machine->cpu.progress++;
  }
  return 1;
}

static int ii_cpu_job_done(IRInterpMachine *machine, const void *argument) {
  (void)machine;
  const IIParallelJob *job = (const IIParallelJob *)argument;
  return job->done == job->count && job->workers == 0;
}

static int ii_cpu_parallel_range(IRInterpMachine *machine,
                                 const IRInterpValue *args, int slots) {
  const char *extern_name = NULL;
  IRFunction *body = ii_token_function(
      machine, (unsigned long long)ii_as_int(&args[0]), &extern_name);
  long long lo = ii_as_int(&args[2]);
  long long hi = ii_as_int(&args[3]);
  long long min_chunk = ii_as_int(&args[4]);
  if (!body || body->instruction_count == 0) {
    return -1;
  }
  IIParallelJob job;
  memset(&job, 0, sizeof(job));
  job.body = body;
  job.ctx = args[1];
  job.slots = slots;
  job.lo = lo;
  job.hi = hi;
  long long total = hi - lo;
  if (total <= 0) {
    return 1;
  }
  long long count = total / (min_chunk < 1 ? 1 : min_chunk);
  long long limit = (long long)II_CPU_WORKERS * 4;
  count = count > limit ? limit : count;
  job.span = count < 2 ? total : (total + count - 1) / count;
  job.count = (total + job.span - 1) / job.span;
  for (long long w = 0; job.count > 1 && w + 1 < II_CPU_WORKERS; w++) {
    if (!ii_cpu_spawn(machine, NULL, ii_int_value(0), &job, w)) {
      return -1;
    }
    job.workers++;
  }
  if (!ii_cpu_parallel_work(machine, &job, II_CPU_WORKERS - 1) ||
      ii_cpu_wait(machine, ii_cpu_job_done, &job, II_WAIT_INFINITE) != 1) {
    return -1;
  }
  return 1;
}

static int ii_cpu_fill(IRInterpMachine *machine, const IRInterpValue *args) {
  unsigned long long dest = (unsigned long long)ii_as_int(&args[0]);
  long long count = ii_as_int(&args[1]);
  unsigned long long value = (unsigned long long)ii_as_int(&args[2]);
  int size = (int)ii_as_int(&args[3]);
  if (size != 1 && size != 2 && size != 4 && size != 8) {
    return -1;
  }
  for (long long i = 0; i < count; i++) {
    if (!ii_mem_write(machine, dest + (unsigned long long)(i * size), size,
                      value)) {
      return -1;
    }
  }
  return 1;
}

static int ii_cpu_copy(IRInterpMachine *machine, const IRInterpValue *args) {
  unsigned long long dest = (unsigned long long)ii_as_int(&args[0]);
  unsigned long long src = (unsigned long long)ii_as_int(&args[1]);
  long long bytes = ii_as_int(&args[2]);
  long long dst_off = 0;
  long long src_off = 0;
  IIBuffer *d = bytes > 0 ? ii_addr_to_buffer(machine, dest, bytes, &dst_off)
                          : NULL;
  IIBuffer *s = bytes > 0 ? ii_addr_to_buffer(machine, src, bytes, &src_off)
                          : NULL;
  if (bytes > 0 && (!d || !s)) {
    ii_fail(machine, IR_INTERP_TRAP, "parallel copy out of bounds");
    return -1;
  }
  if (bytes > 0) {
    memmove(d->data + dst_off, s->data + src_off, (size_t)bytes);
    ii_mark_bytes(d, dst_off, bytes, 1);
  }
  return 1;
}

static int ii_cpu_start_thread(IRInterpMachine *machine, const char *name,
                               const IRInterpValue *args, IRInterpValue *result) {
  int posix = strcmp(name, "pthread_create") == 0;
  const char *extern_name = NULL;
  IRFunction *body = ii_token_function(
      machine, (unsigned long long)ii_as_int(&args[2]), &extern_name);
  if (!body || body->instruction_count == 0) {
    return -1;
  }
  IICpuThread *t = ii_cpu_spawn(machine, body, args[3], NULL, 0);
  if (!t) {
    return -1;
  }
  unsigned long long out =
      posix ? (unsigned long long)ii_as_int(&args[0])
            : (unsigned long long)ii_as_int(&args[5]);
  if (out && !ii_mem_write(machine, out, posix ? 8 : 4,
                           posix ? t->handle : (unsigned long long)t->id)) {
    return -1;
  }
  *result = ii_int_value(posix ? 0 : (long long)t->handle);
  return 1;
}

static int ii_cpu_join(IRInterpMachine *machine, IICpuThread *t,
                       unsigned long long timeout_ms, IRInterpValue *result) {
  int got = ii_cpu_wait(machine, ii_cpu_thread_done, t, timeout_ms);
  if (got < 0) {
    return -1;
  }
  *result = ii_int_value(got ? 0 : (long long)II_WAIT_TIMEOUT);
  return 1;
}

static int ii_cpu_wait_handle(IRInterpMachine *machine,
                              const IRInterpValue *args, size_t arg_count,
                              IRInterpValue *result) {
  unsigned long long handle = (unsigned long long)ii_as_int(&args[0]);
  unsigned long long ms = arg_count > 1
                              ? (unsigned long long)(uint32_t)ii_as_int(&args[1])
                              : II_WAIT_INFINITE;
  IICpuThread *t = ii_cpu_thread(machine, handle);
  if (t) {
    return ii_cpu_join(machine, t, ms, result);
  }
  int got = ii_cpu_lock(machine, handle, ms);
  if (got < 0) {
    return -1;
  }
  *result = ii_int_value(got ? 0 : (long long)II_WAIT_TIMEOUT);
  return 1;
}

static int ii_cpu_make_mutex(IRInterpMachine *machine,
                             const IRInterpValue *args, size_t arg_count,
                             IRInterpValue *result) {
  unsigned long long handle = ++machine->next_thread_handle;
  IISyncObject *o = ii_cpu_object(machine, handle, 1);
  if (!o) {
    return -1;
  }
  if (arg_count >= 2 && ii_as_int(&args[1]) != 0) {
    o->owner = ii_cpu_me(machine) + 1;
    o->depth = 1;
  }
  *result = ii_int_value((long long)handle);
  return 1;
}

static int ii_cpu_name_is(const char *name, const char *const *names) {
  for (size_t i = 0; names[i]; i++) {
    if (strcmp(name, names[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ii_extern_cpu_locks(IRInterpMachine *machine, const char *name,
                               const IRInterpValue *args, size_t arg_count,
                               IRInterpValue *result) {
  static const char *const kLock[] = {"AcquireSRWLockExclusive",
                                      "AcquireSRWLockShared",
                                      "pthread_mutex_lock", NULL};
  static const char *const kUnlock[] = {
      "ReleaseSRWLockExclusive", "ReleaseSRWLockShared", "pthread_mutex_unlock",
      "ReleaseMutex", "mettle_mutex_release", NULL};
  unsigned long long key =
      arg_count ? (unsigned long long)ii_as_int(&args[0]) : 0;
  int locks = ii_cpu_name_is(name, kLock) ||
              strcmp(name, "pthread_mutex_trylock") == 0 ||
              strcmp(name, "mettle_mutex_wait") == 0;
  if (arg_count >= 1 && ir_trace_collecting() &&
      (locks || ii_cpu_name_is(name, kUnlock))) {
    ir_trace_record(locks ? "lock" : "unlock", name,
                    machine->current_call_loc.filename,
                    machine->current_call_loc.line,
                    machine->current_call_loc.column, (long long)key);
  }
  if (arg_count >= 1 && ii_cpu_name_is(name, kLock)) {
    *result = ii_int_value(0);
    return ii_cpu_lock(machine, key, II_WAIT_INFINITE) < 0 ? -1 : 1;
  }
  if (arg_count >= 1 && ii_cpu_name_is(name, kUnlock)) {
    ii_cpu_unlock(machine, key);
    *result = ii_int_value(strcmp(name, "pthread_mutex_unlock") == 0 ? 0 : 1);
    return 1;
  }
  if (arg_count >= 1 && strcmp(name, "pthread_mutex_trylock") == 0) {
    int free_now = ii_cpu_lock_free(machine, &key);
    if (free_now && ii_cpu_lock(machine, key, 0) < 0) {
      return -1;
    }
    *result = ii_int_value(free_now ? 0 : 16);
    return 1;
  }
  if (arg_count >= 1 && (strcmp(name, "CreateMutexA") == 0 ||
                         strcmp(name, "mettle_mutex_create") == 0)) {
    return ii_cpu_make_mutex(machine, args, arg_count, result);
  }
  if (arg_count >= 1 && (strcmp(name, "WaitForSingleObject") == 0 ||
                         strcmp(name, "mettle_mutex_wait") == 0 ||
                         strcmp(name, "mettle_thread_wait") == 0)) {
    return ii_cpu_wait_handle(machine, args, arg_count, result);
  }
  return II_EXTERN_UNHANDLED;
}

static int ii_extern_cpu_signals(IRInterpMachine *machine, const char *name,
                                 const IRInterpValue *args, size_t arg_count,
                                 IRInterpValue *result) {
  static const char *const kWake[] = {
      "WakeAllConditionVariable", "WakeConditionVariable",
      "pthread_cond_signal", "pthread_cond_broadcast", NULL};
  if (arg_count >= 1 && ii_cpu_name_is(name, kWake)) {
    ii_cpu_signal(machine, (unsigned long long)ii_as_int(&args[0]));
    *result = ii_int_value(0);
    return 1;
  }
  if (arg_count >= 2 && (strcmp(name, "SleepConditionVariableSRW") == 0 ||
                         strcmp(name, "pthread_cond_wait") == 0 ||
                         strcmp(name, "pthread_cond_timedwait") == 0)) {
    int windows = strcmp(name, "SleepConditionVariableSRW") == 0;
    unsigned long long ms =
        windows && arg_count >= 3
            ? (unsigned long long)(uint32_t)ii_as_int(&args[2])
        : strcmp(name, "pthread_cond_timedwait") == 0 ? 0
                                                      : II_WAIT_INFINITE;
    int woke = ii_cpu_wait_signal(machine, (unsigned long long)ii_as_int(&args[0]),
                                  (unsigned long long)ii_as_int(&args[1]), ms);
    if (woke < 0) {
      return -1;
    }
    *result = ii_int_value(windows ? woke : (woke ? 0 : 110));
    return 1;
  }
  return II_EXTERN_UNHANDLED;
}

static long long ii_cpu_sleep_ns(IRInterpMachine *machine, const char *name,
                                  const IRInterpValue *args,
                                  size_t arg_count) {
  if (!arg_count) {
    return 0;
  }
  long long v = ii_as_int(&args[0]);
  if (strcmp(name, "Sleep") == 0 || strcmp(name, "mettle_thread_sleep_ms") == 0) {
    return (long long)(uint32_t)v * 1000000;
  }
  if (strcmp(name, "usleep") == 0) {
    return v * 1000;
  }
  if (strcmp(name, "nanosleep") == 0 && v) {
    unsigned long long sec = 0;
    unsigned long long nsec = 0;
    if (ii_mem_read(machine, (unsigned long long)v, 8, &sec) &&
        ii_mem_read(machine, (unsigned long long)v + 8, 8, &nsec)) {
      return (long long)sec * 1000000000 + (long long)nsec;
    }
  }
  return 0;
}

static int ii_extern_clock(IRInterpMachine *machine, const char *name,
                           const IRInterpValue *args, size_t arg_count,
                           IRInterpValue *result) {
  unsigned long long out =
      arg_count ? (unsigned long long)ii_as_int(&args[arg_count - 1]) : 0;
  long long now = ii_cpu_now_ns(machine);
  if (strcmp(name, "QueryPerformanceFrequency") == 0) {
    *result = ii_int_value(1);
    return out && ii_mem_write(machine, out, 8, 1000000000ull) ? 1 : -1;
  }
  if (strcmp(name, "QueryPerformanceCounter") == 0) {
    *result = ii_int_value(1);
    return out && ii_mem_write(machine, out, 8, (unsigned long long)now) ? 1
                                                                          : -1;
  }
  if (strcmp(name, "GetSystemTimePreciseAsFileTime") == 0 ||
      strcmp(name, "GetSystemTimeAsFileTime") == 0) {
    unsigned long long ticks = 133800000000000000ull + (unsigned long long)now / 100;
    return out && ii_mem_write(machine, out, 8, ticks) ? 1 : -1;
  }
  if (strcmp(name, "clock_gettime") == 0 && arg_count >= 2) {
    unsigned long long wall = ii_as_int(&args[0]) == 0 ? 1767225600ull : 0;
    *result = ii_int_value(0);
    return out && ii_mem_write(machine, out, 8, wall + (unsigned long long)now / 1000000000) &&
                   ii_mem_write(machine, out + 8, 8, (unsigned long long)now % 1000000000)
               ? 1
               : -1;
  }
  return II_EXTERN_UNHANDLED;
}

#if defined(__aarch64__)
#define II_SYS_FUTEX 98
#define II_SYS_CLOCK_GETTIME 113
#define II_SYS_NANOSLEEP 101
#define II_SYS_SCHED_YIELD 124
#else
#define II_SYS_FUTEX 202
#define II_SYS_CLOCK_GETTIME 228
#define II_SYS_NANOSLEEP 35
#define II_SYS_SCHED_YIELD 24
#endif

static int ii_cpu_futex_wait(IRInterpMachine *machine, unsigned long long addr,
                             long long expected, unsigned long long ts,
                             IRInterpValue *result) {
  unsigned long long current = 0;
  if (!ii_mem_read(machine, addr, 4, &current)) {
    return 0;
  }
  if ((uint32_t)current != (uint32_t)expected) {
    *result = ii_int_value(-11);
    return 1;
  }
  unsigned long long timeout_ms = II_WAIT_INFINITE;
  unsigned long long sec = 0;
  unsigned long long nsec = 0;
  if (ts) {
    if (!ii_mem_read(machine, ts, 8, &sec) ||
        !ii_mem_read(machine, ts + 8, 8, &nsec)) {
      return 0;
    }
    timeout_ms = sec * 1000 + (nsec + 999999) / 1000000;
  }
  IISyncObject *o = ii_cpu_object(machine, addr, 1);
  if (!o) {
    return 0;
  }
  IICpuSignal s = {addr, o->generation};
  o->waiters++;
  int woke = ii_cpu_wait(machine, ii_cpu_signalled, &s, timeout_ms);
  o = ii_cpu_object(machine, addr, 0);
  if (o) {
    o->waiters--;
  }
  *result = ii_int_value(woke ? 0 : -110);
  return woke >= 0;
}

static int ii_cpu_syscall(IRInterpMachine *machine, const IRInterpValue *args,
                          size_t arg_count, IRInterpValue *result) {
  long long number = arg_count ? ii_as_int(&args[0]) : -1;
  long long a[5] = {0, 0, 0, 0, 0};
  for (size_t i = 1; i < arg_count && i <= 5; i++) {
    a[i - 1] = ii_as_int(&args[i]);
  }
  if (number == II_SYS_FUTEX) {
    long long op = a[1] & 127;
    if (op == 0) {
      return ii_cpu_futex_wait(machine, (unsigned long long)a[0], a[2],
                               (unsigned long long)a[3], result);
    }
    if (op != 1) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "futex operation");
      return 0;
    }
    IISyncObject *o = ii_cpu_object(machine, (unsigned long long)a[0], 0);
    long long woken = o ? (o->waiters < a[2] ? o->waiters : a[2]) : 0;
    if (o) {
      ii_cpu_signal(machine, (unsigned long long)a[0]);
    }
    *result = ii_int_value(woken);
    return 1;
  }
  if (number == II_SYS_CLOCK_GETTIME && arg_count >= 3) {
    return ii_extern_clock(machine, "clock_gettime", args + 1, 2, result) == 1;
  }
  if (number == II_SYS_NANOSLEEP || number == II_SYS_SCHED_YIELD) {
    ii_cpu_pass_time(machine, number == II_SYS_NANOSLEEP && arg_count >= 2
                                  ? ii_cpu_sleep_ns(machine, "nanosleep",
                                                    args + 1, 1)
                                  : 0);
    *result = ii_int_value(0);
    return ii_cpu_yield(machine);
  }
  return II_EXTERN_UNHANDLED;
}

static int ii_extern_cpu(IRInterpMachine *machine, const char *name,
                         const IRInterpValue *args, size_t arg_count,
                         IRInterpValue *result) {
  static const char *const kYield[] = {
      "Sleep", "mettle_thread_sleep_ms", "usleep", "nanosleep",
      "sched_yield", "posix_yield", "SwitchToThread", NULL};
  int clocked = ii_extern_clock(machine, name, args, arg_count, result);
  if (clocked != II_EXTERN_UNHANDLED) {
    return clocked;
  }
  if (arg_count >= 6 && (strcmp(name, "CreateThread") == 0 ||
                         strcmp(name, "mettle_thread_create") == 0)) {
    return ii_cpu_start_thread(machine, name, args, result);
  }
  if (arg_count >= 4 && strcmp(name, "pthread_create") == 0) {
    return ii_cpu_start_thread(machine, name, args, result);
  }
  if (arg_count >= 1 && strcmp(name, "pthread_join") == 0) {
    IICpuThread *t = ii_cpu_thread(machine, (unsigned long long)ii_as_int(&args[0]));
    if (!t || ii_cpu_join(machine, t, II_WAIT_INFINITE, result) < 0) {
      return -1;
    }
    unsigned long long out = arg_count > 1 ? (unsigned long long)ii_as_int(&args[1]) : 0;
    if (out && !ii_mem_write(machine, out, 8, (unsigned long long)ii_as_int(&t->result))) {
      return -1;
    }
    *result = ii_int_value(0);
    return 1;
  }
  if (ii_cpu_name_is(name, kYield)) {
    ii_cpu_pass_time(machine, ii_cpu_sleep_ns(machine, name, args, arg_count));
    *result = ii_int_value(0);
    return ii_cpu_yield(machine) ? 1 : -1;
  }
  if (strcmp(name, "GetCurrentThreadId") == 0 ||
      strcmp(name, "mettle_thread_current_id") == 0 ||
      strcmp(name, "pthread_self") == 0) {
    *result = ii_int_value(ii_cpu_me(machine) + 1);
    return 1;
  }
  if (arg_count >= 5 && (strcmp(name, "mettle_parallel_range") == 0 ||
                         strcmp(name, "mettle_parallel_range_slots") == 0)) {
    *result = ii_int_value(0);
    return ii_cpu_parallel_range(machine, args,
                                 strcmp(name, "mettle_parallel_range_slots") == 0);
  }
  if (strcmp(name, "mettle_parallel_threads") == 0) {
    *result = ii_int_value(II_CPU_WORKERS);
    return 1;
  }
  if (arg_count >= 4 && strcmp(name, "mettle_parallel_fill") == 0) {
    return ii_cpu_fill(machine, args);
  }
  if (arg_count >= 3 && strcmp(name, "mettle_parallel_copy_bytes") == 0) {
    return ii_cpu_copy(machine, args);
  }
  int handled = ii_extern_cpu_locks(machine, name, args, arg_count, result);
  return handled != II_EXTERN_UNHANDLED
             ? handled
             : ii_extern_cpu_signals(machine, name, args, arg_count, result);
}

static void ii_cpu_destroy(IRInterpMachine *machine) {
  for (size_t i = 0; i < machine->cpu.count; i++) {
    IICpuThread *t = machine->cpu.threads[i];
    if (t->fiber) {
      ir_fiber_destroy(t->fiber);
    }
    free(t->effects);
    free(t);
  }
  free(machine->cpu.threads);
  free(machine->cpu.objects);
  if (machine->cpu.scheduler) {
    ir_fiber_scheduler_end(machine->cpu.scheduler);
  }
  memset(&machine->cpu, 0, sizeof(machine->cpu));
}
