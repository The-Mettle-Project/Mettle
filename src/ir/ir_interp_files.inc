#include "ir_host_io.h"

#define II_FILE_HANDLE_BASE 0x50000000ull

#if defined(__aarch64__)
#define II_SYS_PREAD 67
#define II_SYS_PWRITE 68
#else
#define II_SYS_PREAD 17
#define II_SYS_PWRITE 18
#endif

enum {
  II_HOST_FREE = 0,
  II_HOST_FILE,
  II_HOST_FIND,
  II_HOST_STREAM,
  II_HOST_MAPPING,
  II_HOST_VIEW
};

typedef struct {
  const char *name;
  const char *args;
  char result;
} IIWin32Spec;

static const IIWin32Spec kIIWin32[] = {
    {"CreateFileA", "siiNiiN", 'h'},
    {"ReadFile", "HBi4O", 'r'},
    {"WriteFile", "HBi4O", 'r'},
    {"FlushFileBuffers", "H", 'r'},
    {"GetFileSizeEx", "H8", 'r'},
    {"SetFileInformationByHandle", "HiZi", 'r'},
    {"GetFileInformationByHandle", "HY", 'r'},
    {"LockFileEx", "HiiiiO", 'r'},
    {"UnlockFileEx", "HiiiO", 'r'},
    {"GetFinalPathNameByHandleA", "HDii", 'r'},
    {"MoveFileExA", "ssi", 'r'},
    {"DeleteFileA", "s", 'r'},
    {"CreateDirectoryA", "sN", 'r'},
    {"RemoveDirectoryA", "s", 'r'},
    {"GetFileAttributesA", "s", 'r'},
    {"SetFileAttributesA", "si", 'r'},
    {"GetFileAttributesExA", "siA", 'r'},
    {"GetCurrentDirectoryA", "iC", 'r'},
    {"SetCurrentDirectoryA", "s", 'r'},
    {"GetTempPathA", "iC", 'r'},
    {"FindFirstFileA", "sF", 'f'},
    {"FindNextFileA", "HF", 'r'},
    {"CreateSymbolicLinkA", "ssi", 'r'},
};

static IIHostHandle *ii_host_handle(IRInterpMachine *machine,
                                    unsigned long long value) {
  if (value < II_FILE_HANDLE_BASE) {
    return NULL;
  }
  unsigned long long index = (value - II_FILE_HANDLE_BASE) / 4;
  if (index >= machine->files.count ||
      machine->files.handles[index].kind == II_HOST_FREE) {
    return NULL;
  }
  return &machine->files.handles[index];
}

static unsigned long long ii_host_wrap(IRInterpMachine *machine, int kind,
                                       unsigned long long host) {
  size_t index = machine->files.count;
  for (size_t i = 0; i < machine->files.count; i++) {
    if (machine->files.handles[i].kind == II_HOST_FREE) {
      index = i;
      break;
    }
  }
  if (index == machine->files.count) {
    if (machine->files.count == machine->files.capacity) {
      size_t grown = machine->files.capacity ? machine->files.capacity * 2 : 16;
      IIHostHandle *table = (IIHostHandle *)realloc(
          machine->files.handles, grown * sizeof(IIHostHandle));
      if (!table) {
        ii_fail(machine, IR_INTERP_TRAP, "out of memory for a file handle");
        return 0;
      }
      machine->files.handles = table;
      machine->files.capacity = grown;
    }
    machine->files.count++;
  }
  IIHostHandle *h = &machine->files.handles[index];
  memset(h, 0, sizeof(*h));
  h->kind = kind;
  h->host = host;
  return II_FILE_HANDLE_BASE + (unsigned long long)index * 4;
}

static void *ii_host_block(IRInterpMachine *machine, unsigned long long addr,
                           long long size, int *ok) {
  if (!addr || size <= 0) {
    return NULL;
  }
  long long offset = 0;
  IIBuffer *buf = ii_addr_to_buffer(machine, addr, size, &offset);
  if (!buf) {
    ii_fail(machine, IR_INTERP_TRAP, "a file call names memory out of bounds");
    *ok = 0;
    return NULL;
  }
  ii_mark_bytes(buf, offset, size, 1);
  return buf->data + offset;
}

static const char *ii_host_string(IRInterpMachine *machine,
                                  unsigned long long addr, int *ok) {
  long long offset = 0;
  IIBuffer *buf = addr ? ii_addr_to_buffer(machine, addr, 0, &offset) : NULL;
  if (!buf || !memchr(buf->data + offset, 0, (size_t)(buf->size - offset))) {
    ii_fail(machine, IR_INTERP_TRAP, "a file call names a path that is not "
                                     "a NUL-terminated string");
    *ok = 0;
    return NULL;
  }
  return (const char *)buf->data + offset;
}

static long long ii_win32_block_size(char spec, const long long *raw) {
  switch (spec) {
  case 'B':
  case 'D':
    return raw[2];
  case 'C':
    return raw[0];
  case 'Z':
    return raw[3];
  case '4':
    return 4;
  case '8':
    return 8;
  case 'O':
    return 32;
  case 'A':
    return 36;
  case 'Y':
    return 52;
  case 'F':
    return 320;
  default:
    return 0;
  }
}

static int ii_win32_translate(IRInterpMachine *machine, const IIWin32Spec *spec,
                              const IRInterpValue *args, size_t arg_count,
                              long long *host) {
  long long raw[8] = {0};
  size_t n = strlen(spec->args);
  if (arg_count < n || n > 8) {
    return 0;
  }
  for (size_t i = 0; i < n; i++) {
    raw[i] = ii_as_int(&args[i]);
  }
  int ok = 1;
  for (size_t i = 0; i < n && ok; i++) {
    char kind = spec->args[i];
    if (kind == 'i') {
      host[i] = raw[i];
    } else if (kind == 'N') {
      host[i] = 0;
    } else if (kind == 's') {
      host[i] = (long long)(size_t)ii_host_string(machine,
                                                  (unsigned long long)raw[i], &ok);
    } else if (kind == 'H') {
      IIHostHandle *h = ii_host_handle(machine, (unsigned long long)raw[i]);
      if (!h) {
        return 0;
      }
      host[i] = (long long)h->host;
    } else {
      host[i] = (long long)(size_t)ii_host_block(
          machine, (unsigned long long)raw[i], ii_win32_block_size(kind, raw),
          &ok);
    }
  }
  return ok ? 1 : -1;
}

static int ii_win32_call(IRInterpMachine *machine, const IIWin32Spec *spec,
                         const IRInterpValue *args, size_t arg_count,
                         IRInterpValue *result) {
  long long host[8] = {0};
  int translated = ii_win32_translate(machine, spec, args, arg_count, host);
  if (translated <= 0) {
    return translated < 0 ? -1 : II_EXTERN_UNHANDLED;
  }
  int known = 0;
  long long value =
      ir_host_win32(spec->name, host, &known, &machine->files.last_error);
  if (!known) {
    return II_EXTERN_UNHANDLED;
  }
  if (spec->result != 'r' && value != -1 && value != 0) {
    value = (long long)ii_host_wrap(
        machine, spec->result == 'h' ? II_HOST_FILE : II_HOST_FIND,
        (unsigned long long)value);
  }
  *result = ii_int_value(value);
  return 1;
}

static int ii_host_positioned(IRInterpMachine *machine, IIHostHandle *file,
                              void *data, long long length, long long offset,
                              int write) {
  unsigned char overlapped[32];
  unsigned got = 0;
  memset(overlapped, 0, sizeof(overlapped));
  uint32_t lo = (uint32_t)offset;
  uint32_t hi = (uint32_t)((unsigned long long)offset >> 32);
  memcpy(overlapped + 16, &lo, 4);
  memcpy(overlapped + 20, &hi, 4);
  long long host[6] = {(long long)file->host, (long long)(size_t)data, length,
                       (long long)(size_t)&got, (long long)(size_t)overlapped,
                       0};
  int known = 0;
  unsigned error = 0;
  long long ok =
      ir_host_win32(write ? "WriteFile" : "ReadFile", host, &known, &error);
  (void)machine;
  if (known) {
    return ok || error == 38;
  }
  long long sys[6] = {(long long)file->host, (long long)(size_t)data, length,
                      offset, 0, 0};
  long long moved = ir_host_syscall(write ? II_SYS_PWRITE : II_SYS_PREAD, sys,
                                    &known);
  return known && moved >= 0;
}

static int ii_view_sync(IRInterpMachine *machine, IIHostHandle *view) {
  IIHostHandle *file = ii_host_handle(machine, view->file);
  long long offset = 0;
  IIBuffer *buf = ii_addr_to_buffer(machine, view->view, view->length, &offset);
  return !view->writable ||
         (file && buf &&
          ii_host_positioned(machine, file, buf->data + offset, view->length,
                             view->offset, 1));
}

static IIHostHandle *ii_view_at(IRInterpMachine *machine,
                                unsigned long long addr) {
  for (size_t i = 0; i < machine->files.count; i++) {
    IIHostHandle *h = &machine->files.handles[i];
    if (h->kind == II_HOST_VIEW && h->view == addr) {
      return h;
    }
  }
  return NULL;
}

static int ii_map_file(IRInterpMachine *machine, unsigned long long file,
                       long long offset, long long length, int writable,
                       IRInterpValue *result) {
  IIHostHandle *f = ii_host_handle(machine, file);
  unsigned long long addr = length > 0 && f
                                ? ir_interp_add_buffer(machine, NULL, length)
                                : 0;
  long long at = 0;
  IIBuffer *buf = addr ? ii_addr_to_buffer(machine, addr, length, &at) : NULL;
  if (!buf || !ii_host_positioned(machine, f, buf->data + at, length, offset, 0)) {
    *result = ii_int_value(0);
    return 1;
  }
  unsigned long long handle = ii_host_wrap(machine, II_HOST_VIEW, 0);
  IIHostHandle *v = ii_host_handle(machine, handle);
  v->file = file;
  v->view = addr;
  v->offset = offset;
  v->length = length;
  v->writable = writable;
  *result = ii_int_value((long long)addr);
  return 1;
}

static int ii_extern_mapping(IRInterpMachine *machine, const char *name,
                             const IRInterpValue *args, size_t arg_count,
                             IRInterpValue *result) {
  if (arg_count >= 6 && strcmp(name, "CreateFileMappingA") == 0) {
    unsigned long long file = (unsigned long long)ii_as_int(&args[0]);
    if (!ii_host_handle(machine, file)) {
      return II_EXTERN_UNHANDLED;
    }
    unsigned long long handle = ii_host_wrap(machine, II_HOST_MAPPING, 0);
    IIHostHandle *m = ii_host_handle(machine, handle);
    m->file = file;
    m->length = (long long)(((unsigned long long)(uint32_t)ii_as_int(&args[3]) << 32) |
                            (uint32_t)ii_as_int(&args[4]));
    m->writable = (ii_as_int(&args[2]) & 0x04) != 0;
    *result = ii_int_value((long long)handle);
    return 1;
  }
  if (arg_count >= 5 && strcmp(name, "MapViewOfFile") == 0) {
    IIHostHandle *m = ii_host_handle(machine, (unsigned long long)ii_as_int(&args[0]));
    if (!m || m->kind != II_HOST_MAPPING) {
      return II_EXTERN_UNHANDLED;
    }
    long long offset =
        (long long)(((unsigned long long)(uint32_t)ii_as_int(&args[2]) << 32) |
                    (uint32_t)ii_as_int(&args[3]));
    long long length = ii_as_int(&args[4]) ? ii_as_int(&args[4])
                                           : m->length - offset;
    return ii_map_file(machine, m->file, offset, length,
                       m->writable && (ii_as_int(&args[1]) & 0x02) != 0, result);
  }
  if (arg_count >= 1 && (strcmp(name, "FlushViewOfFile") == 0 ||
                         strcmp(name, "UnmapViewOfFile") == 0)) {
    IIHostHandle *v = ii_view_at(machine, (unsigned long long)ii_as_int(&args[0]));
    if (!v) {
      return II_EXTERN_UNHANDLED;
    }
    int ok = ii_view_sync(machine, v);
    if (name[0] == 'U') {
      long long at = 0;
      IIBuffer *buf = ii_addr_to_buffer(machine, v->view, 0, &at);
      if (buf) {
        buf->freed = 1;
      }
      v->kind = II_HOST_FREE;
    }
    *result = ii_int_value(ok);
    return 1;
  }
  return II_EXTERN_UNHANDLED;
}

static int ii_extern_runtime_files(IRInterpMachine *machine, const char *name,
                                   const IRInterpValue *args, size_t arg_count,
                                   IRInterpValue *result) {
  static const char *const kPaths[] = {"mettle_file_exists", "mettle_dir_exists",
                                       "mettle_dir_create",
                                       "mettle_make_directory", NULL};
  int ok = 1;
  long long host[2] = {0, 0};
  if (arg_count >= 1 && ii_cpu_name_is(name, kPaths)) {
    host[0] = (long long)(size_t)ii_host_string(
        machine, (unsigned long long)ii_as_int(&args[0]), &ok);
  } else if (arg_count >= 2 && strcmp(name, "mettle_getcwd") == 0) {
    host[1] = ii_as_int(&args[1]);
    host[0] = (long long)(size_t)ii_host_block(
        machine, (unsigned long long)ii_as_int(&args[0]), host[1], &ok);
  } else {
    return II_EXTERN_UNHANDLED;
  }
  int known = 0;
  long long value = ok ? ir_host_runtime(name, host, &known) : 0;
  if (!ok || !known) {
    return ok ? II_EXTERN_UNHANDLED : -1;
  }
  *result = ii_int_value(value);
  return 1;
}

typedef struct {
  const char *name;
  const char *args;
} IIStdioSpec;

static const IIStdioSpec kIIStdio[] = {
    {"fopen", "ss"},  {"fclose", "F"},  {"fread", "BiiF"}, {"fwrite", "BiiF"},
    {"fgets", "GiF"}, {"fputs", "sF"},  {"fputc", "iF"},   {"putc", "iF"},
    {"fflush", "F"},  {"fseek", "Fii"}, {"ftell", "F"},    {"fgetc", "F"},
    {"getc", "F"},    {"feof", "F"},    {"ferror", "F"},   {"remove", "s"},
    {"unlink", "s"},  {"rename", "ss"}};

static int ii_stdio_args(IRInterpMachine *machine, const IIStdioSpec *spec,
                         const IRInterpValue *args, size_t arg_count,
                         long long *host, IIHostHandle **stream) {
  size_t needed = strlen(spec->args);
  const char *at = strchr(spec->args, 'F');
  if (arg_count < needed) {
    return 0;
  }
  if (at) {
    *stream = ii_host_handle(
        machine, (unsigned long long)ii_as_int(&args[at - spec->args]));
    if (!*stream || (*stream)->kind != II_HOST_STREAM) {
      return 0;
    }
  }
  int ok = 1;
  for (size_t i = 0; i < needed && ok; i++) {
    long long raw = ii_as_int(&args[i]);
    char kind = spec->args[i];
    if (kind == 'F') {
      host[i] = (long long)(*stream)->host;
    } else if (kind == 's') {
      host[i] = (long long)(size_t)ii_host_string(machine,
                                                  (unsigned long long)raw, &ok);
    } else if (kind == 'B' || kind == 'G') {
      long long size = kind == 'B' ? ii_as_int(&args[1]) * ii_as_int(&args[2])
                                   : ii_as_int(&args[1]);
      host[i] = (long long)(size_t)ii_host_block(
          machine, (unsigned long long)raw, size, &ok);
    } else {
      host[i] = raw;
    }
  }
  return ok ? 1 : -1;
}

static long long ii_stdio_byte(IIHostHandle *stream) {
  unsigned char byte = 0;
  long long host[4] = {(long long)(size_t)&byte, 1, 1, (long long)stream->host};
  int known = 0;
  if (ir_host_stdio("fread", host, &known) == 1) {
    return byte;
  }
  stream->eof = 1;
  return -1;
}

static int ii_extern_file_stream(IRInterpMachine *machine, const char *name,
                                 const IRInterpValue *args, size_t arg_count,
                                 IRInterpValue *result) {
  const IIStdioSpec *spec = NULL;
  for (size_t i = 0; i < sizeof(kIIStdio) / sizeof(kIIStdio[0]) && !spec; i++) {
    spec = strcmp(name, kIIStdio[i].name) == 0 ? &kIIStdio[i] : NULL;
  }
  long long host[4] = {0, 0, 0, 0};
  IIHostHandle *stream = NULL;
  int ready = spec ? ii_stdio_args(machine, spec, args, arg_count, host, &stream)
                   : 0;
  if (ready <= 0) {
    return ready < 0 ? -1 : II_EXTERN_UNHANDLED;
  }
  int known = 0;
  long long value = 0;
  if (strcmp(name, "fgetc") == 0 || strcmp(name, "getc") == 0) {
    value = ii_stdio_byte(stream);
  } else if (strcmp(name, "feof") == 0) {
    value = stream->eof;
  } else if (strcmp(name, "ferror") == 0) {
    value = 0;
  } else {
    value = ir_host_stdio(name, host, &known);
  }
  if (strcmp(name, "fopen") == 0) {
    value = value ? (long long)ii_host_wrap(machine, II_HOST_STREAM,
                                            (unsigned long long)value)
                  : 0;
  } else if (strcmp(name, "fclose") == 0) {
    stream->kind = II_HOST_FREE;
  } else if (strcmp(name, "fread") == 0) {
    stream->eof |= value < host[2];
  } else if (strcmp(name, "fgets") == 0) {
    stream->eof |= value == 0;
    value = value ? ii_as_int(&args[0]) : 0;
  } else if (strcmp(name, "fseek") == 0) {
    stream->eof = 0;
  }
  *result = ii_int_value(value);
  return 1;
}

static int ii_extern_files(IRInterpMachine *machine, const char *name,
                           const IRInterpValue *args, size_t arg_count,
                           IRInterpValue *result) {
  int runtime = ii_extern_runtime_files(machine, name, args, arg_count, result);
  if (runtime != II_EXTERN_UNHANDLED) {
    return runtime;
  }
  if (strcmp(name, "GetLastError") == 0) {
    *result = ii_int_value((long long)machine->files.last_error);
    return 1;
  }
  if (arg_count >= 1 && (strcmp(name, "CloseHandle") == 0 ||
                         strcmp(name, "FindClose") == 0)) {
    IIHostHandle *h = ii_host_handle(machine, (unsigned long long)ii_as_int(&args[0]));
    if (!h) {
      return II_EXTERN_UNHANDLED;
    }
    long long host[1] = {(long long)h->host};
    int known = 0;
    long long ok = h->kind == II_HOST_MAPPING
                       ? 1
                       : ir_host_win32(name, host, &known,
                                       &machine->files.last_error);
    h->kind = II_HOST_FREE;
    *result = ii_int_value(ok);
    return 1;
  }
  for (size_t i = 0; i < sizeof(kIIWin32) / sizeof(kIIWin32[0]); i++) {
    if (strcmp(name, kIIWin32[i].name) == 0) {
      return ii_win32_call(machine, &kIIWin32[i], args, arg_count, result);
    }
  }
  return ii_extern_mapping(machine, name, args, arg_count, result);
}

typedef struct {
  int number;
  const char *args;
} IISyscallSpec;

#if defined(__aarch64__)
static const IISyscallSpec kIISyscalls[] = {
    {63, "iBi"},  {64, "iBi"},  {57, "i"},    {80, "iS"},   {82, "i"},
    {83, "i"},    {46, "ii"},   {17, "Ci"},   {49, "s"},    {32, "ii"},
    {34, "isi"},  {79, "isSi"}, {56, "isii"}, {67, "iBii"}, {68, "iBii"},
    {78, "isDi"}, {38, "isis"}, {36, "sis"},  {35, "isi"},  {61, "iBi"},
    {62, "iii"}};
#define II_STAT_BYTES 128
#define II_SYS_MMAP 222
#define II_SYS_MUNMAP 215
#define II_SYS_MSYNC 227
#else
static const IISyscallSpec kIISyscalls[] = {
    {0, "iBi"},    {1, "iBi"},    {3, "i"},      {5, "iS"},     {74, "i"},
    {75, "i"},     {77, "ii"},    {79, "Ci"},    {80, "s"},     {73, "ii"},
    {258, "isi"},  {262, "isSi"}, {257, "isii"}, {17, "iBii"},  {18, "iBii"},
    {267, "isDi"}, {264, "isis"}, {266, "sis"},  {263, "isi"},  {217, "iBi"},
    {8, "iii"}};
#define II_STAT_BYTES 144
#define II_SYS_MMAP 9
#define II_SYS_MUNMAP 11
#define II_SYS_MSYNC 26
#endif

static long long ii_syscall_block(char kind, const long long *raw) {
  switch (kind) {
  case 'B':
  case 'D':
    return raw[2];
  case 'C':
    return raw[1];
  case 'S':
    return II_STAT_BYTES;
  default:
    return 0;
  }
}

static int ii_syscall_mapping(IRInterpMachine *machine, const long long *raw,
                              long long number, IRInterpValue *result) {
  if (number == II_SYS_MMAP) {
    unsigned long long file = ii_host_wrap(machine, II_HOST_FILE,
                                           (unsigned long long)raw[4]);
    int shared_write = (raw[3] & 1) && (raw[2] & 2);
    return ii_map_file(machine, file, raw[5], raw[1], shared_write, result);
  }
  IIHostHandle *v = ii_view_at(machine, (unsigned long long)raw[0]);
  if (!v) {
    *result = ii_int_value(-22);
    return 1;
  }
  int ok = ii_view_sync(machine, v);
  if (number == II_SYS_MUNMAP) {
    long long at = 0;
    IIBuffer *buf = ii_addr_to_buffer(machine, v->view, 0, &at);
    if (buf) {
      buf->freed = 1;
    }
    IIHostHandle *file = ii_host_handle(machine, v->file);
    if (file) {
      file->kind = II_HOST_FREE;
    }
    v->kind = II_HOST_FREE;
  }
  *result = ii_int_value(ok ? 0 : -5);
  return 1;
}

static int ii_syscall(IRInterpMachine *machine, const IRInterpValue *args,
                      size_t arg_count, IRInterpValue *result) {
  int threaded = ii_cpu_syscall(machine, args, arg_count, result);
  if (threaded != II_EXTERN_UNHANDLED) {
    return threaded;
  }
  long long raw[7] = {0};
  for (size_t i = 0; i < arg_count && i < 7; i++) {
    raw[i] = ii_as_int(&args[i]);
  }
  long long number = raw[0];
  if (number == II_SYS_MMAP || number == II_SYS_MUNMAP ||
      number == II_SYS_MSYNC) {
    return ii_syscall_mapping(machine, raw + 1, number, result);
  }
  const IISyscallSpec *spec = NULL;
  for (size_t i = 0; i < sizeof(kIISyscalls) / sizeof(kIISyscalls[0]); i++) {
    spec = kIISyscalls[i].number == number ? &kIISyscalls[i] : spec;
  }
  if (!spec) {
    char detail[64];
    snprintf(detail, sizeof(detail), "syscall %lld", number);
    ii_fail(machine, IR_INTERP_UNSUPPORTED, detail);
    return 0;
  }
  long long host[6] = {0};
  int ok = 1;
  for (size_t i = 0; spec->args[i] && ok; i++) {
    char kind = spec->args[i];
    host[i] = kind == 'i' ? raw[i + 1]
              : kind == 's'
                  ? (long long)(size_t)ii_host_string(
                        machine, (unsigned long long)raw[i + 1], &ok)
                  : (long long)(size_t)ii_host_block(
                        machine, (unsigned long long)raw[i + 1],
                        ii_syscall_block(kind, raw + 1), &ok);
  }
  int known = 0;
  long long value = ok ? ir_host_syscall(number, host, &known) : 0;
  if (!ok || !known) {
    if (ok) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "syscall on this host");
    }
    return 0;
  }
  *result = ii_int_value(value);
  return 1;
}

static void ii_files_destroy(IRInterpMachine *machine) {
  for (size_t i = 0; i < machine->files.count; i++) {
    IIHostHandle *h = &machine->files.handles[i];
    if (h->kind == II_HOST_STREAM) {
      long long host[1] = {(long long)h->host};
      int known = 0;
      (void)ir_host_stdio("fclose", host, &known);
    }
    if (h->kind == II_HOST_FILE || h->kind == II_HOST_FIND) {
      long long host[1] = {(long long)h->host};
      int known = 0;
      unsigned error = 0;
      (void)ir_host_win32(h->kind == II_HOST_FIND ? "FindClose" : "CloseHandle",
                          host, &known, &error);
    }
  }
  free(machine->files.handles);
  memset(&machine->files, 0, sizeof(machine->files));
}
