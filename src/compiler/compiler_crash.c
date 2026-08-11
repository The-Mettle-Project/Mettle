#include "compiler_crash.h"
#include "compiler_context.h"
#include "../runtime/crash_handler.h"
#include "../runtime/owned.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

#define MAX_BACKTRACE_FRAMES 64
#define MAX_SYM_NAME_LEN 1024

static int g_compiler_crash_installed = 0;
static int g_compiler_in_ice_handler = 0;
#if defined(_WIN32) || defined(_WIN64)
static int g_sym_initialized = 0;
static CONTEXT *g_compiler_crash_context = NULL;
#endif

#if defined(_WIN32) || defined(_WIN64)
static void mettle_compiler_sym_init(void) {
  HANDLE process = GetCurrentProcess();

  if (g_sym_initialized) {
    return;
  }

  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES |
                SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_INCLUDE_32BIT_MODULES);
  SymInitialize(process, NULL, TRUE);
  g_sym_initialized = 1;
}

static int mettle_compiler_frame_is_internal(const char *name) {
  if (!name || name[0] == '\0') {
    return 0;
  }

  return strstr(name, "mettle_compiler_write_backtrace") != NULL ||
         strstr(name, "mettle_compiler_write_frame") != NULL ||
         strstr(name, "mettle_compiler_ice_report") != NULL ||
         strstr(name, "mettle_compiler_unhandled_exception_filter") != NULL ||
         strstr(name, "CaptureStackBackTrace") != NULL ||
         strstr(name, "StackWalk64") != NULL ||
         strstr(name, "RtlCaptureStackBackTrace") != NULL ||
         strstr(name, "SymFunctionTableAccess64") != NULL ||
         strstr(name, "SymGetModuleBase64") != NULL;
}

static void mettle_compiler_write_module_offset(FILE *output, void *address) {
  HMODULE module = NULL;
  char module_path[MAX_PATH];
  const char *basename = NULL;
  DWORD64 module_base = 0;

  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCSTR)address, &module) ||
      !module) {
    fprintf(output, "%p", address);
    return;
  }

  if (GetModuleFileNameA(module, module_path, sizeof(module_path)) == 0) {
    fprintf(output, "%p", address);
    return;
  }

  basename = strrchr(module_path, '\\');
  basename = basename ? basename + 1 : module_path;
  module_base = (DWORD64)(uintptr_t)module;
  fprintf(output, "%s+0x%llX", basename,
          (unsigned long long)((DWORD64)(uintptr_t)address - module_base));
}

static void mettle_compiler_write_frame(FILE *output, HANDLE process,
                                        void *frame_address, size_t index) {
  DWORD64 address = (DWORD64)(uintptr_t)frame_address;
  DWORD64 lookup_address = address;
  char symbol_buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME_LEN * sizeof(TCHAR)];
  SYMBOL_INFO *symbol = (SYMBOL_INFO *)symbol_buffer;
  IMAGEHLP_LINE64 line = {0};
  DWORD64 symbol_displacement = 0;
  DWORD line_displacement = 0;
  int has_symbol = 0;

  if (address != 0) {
    lookup_address = address - 1;
  }

  symbol->MaxNameLen = MAX_SYM_NAME_LEN;
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  line.SizeOfStruct = sizeof(line);

  fprintf(output, "  #%zu ", index);

  if (SymFromAddr(process, lookup_address, &symbol_displacement, symbol)) {
    if (!mettle_compiler_frame_is_internal(symbol->Name)) {
      has_symbol = 1;
      fprintf(output, "%s", symbol->Name);
      if (symbol_displacement != 0) {
        fprintf(output, "+0x%llX", (unsigned long long)symbol_displacement);
      }
    }
  }

  if (!has_symbol) {
    mettle_compiler_write_module_offset(output, frame_address);
  }

  if (SymGetLineFromAddr64(process, lookup_address, &line_displacement, &line) &&
      line.FileName && line.LineNumber > 0) {
    fprintf(output, " at %s:%lu", line.FileName, line.LineNumber);
  }

  fprintf(output, " (%p)\n", frame_address);
}

static void mettle_compiler_write_backtrace_with_context(FILE *output,
                                                         CONTEXT *context) {
  HANDLE process = GetCurrentProcess();
  HANDLE thread = GetCurrentThread();
  size_t frame_index = 0;

  if (!output) {
    return;
  }

  fprintf(output, "\nCompiler backtrace:\n");
  mettle_compiler_sym_init();

  if (context) {
    STACKFRAME64 frame;
    memset(&frame, 0, sizeof(frame));
#if defined(_M_X64) || defined(__x86_64__)
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrPC.Offset = context->Rip;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context->Rbp;
    frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context->Rsp;

    for (size_t i = 0; i < MAX_BACKTRACE_FRAMES; i++) {
      if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame,
                       context, NULL, SymFunctionTableAccess64,
                       SymGetModuleBase64, NULL)) {
        break;
      }
      if (frame.AddrPC.Offset == 0) {
        break;
      }

      mettle_compiler_write_frame(
          output, process, (void *)(uintptr_t)frame.AddrPC.Offset, frame_index);
      frame_index++;
    }
#else
    (void)thread;
    (void)frame_index;
#endif
  } else {
    void *frames[MAX_BACKTRACE_FRAMES];
    USHORT frame_count =
        CaptureStackBackTrace(0, MAX_BACKTRACE_FRAMES, frames, NULL);

    for (USHORT i = 0; i < frame_count; i++) {
      char symbol_buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME_LEN * sizeof(TCHAR)];
      SYMBOL_INFO *symbol = (SYMBOL_INFO *)symbol_buffer;
      DWORD64 displacement = 0;
      int skip = 0;

      symbol->MaxNameLen = MAX_SYM_NAME_LEN;
      symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
      if (SymFromAddr(process, (DWORD64)(uintptr_t)frames[i] - 1, &displacement,
                      symbol) &&
          mettle_compiler_frame_is_internal(symbol->Name)) {
        skip = 1;
      }

      if (skip) {
        continue;
      }

      mettle_compiler_write_frame(output, process, frames[i], frame_index);
      frame_index++;
    }
  }
}
#else
static void mettle_compiler_write_frame(FILE *output, void *frame_address,
                                         size_t index) {
  fprintf(output, "  #%zu %p\n", index, frame_address);
}

static void mettle_compiler_write_backtrace_with_context(FILE *output,
                                                          void *context) {
  (void)context;
  if (!output) {
    return;
  }
  fprintf(output, "\nCompiler backtrace:\n");

  void **frame = (void **)__builtin_frame_address(0);
  for (size_t index = 0; frame && index < MAX_BACKTRACE_FRAMES; index++) {
    void **next = (void **)frame[0];
    void *return_address = frame[1];
    if (!return_address) {
      break;
    }
    if (index >= 2) {
      mettle_compiler_write_frame(output, return_address, index - 2);
    }
    if (!next || next <= frame || (uintptr_t)next - (uintptr_t)frame >
                                      16u * 1024u * 1024u ||
        ((uintptr_t)next & (sizeof(void *) - 1u)) != 0) {
      break;
    }
    frame = next;
  }
}
#endif

static void mettle_compiler_write_backtrace(FILE *output) {
#if defined(_WIN32) || defined(_WIN64)
  mettle_compiler_write_backtrace_with_context(output, g_compiler_crash_context);
#else
  mettle_compiler_write_backtrace_with_context(output, NULL);
#endif
}

void mettle_compiler_ice_report(const char *reason, const char *detail) {
  if (g_compiler_in_ice_handler) {
    fprintf(stderr, "Mettle internal compiler error (recursive)\n");
    return;
  }

  g_compiler_in_ice_handler = 1;
  mettle_compiler_ctx_write_snapshot();
  mettle_compiler_ctx_write_report(stderr, reason, detail);
  mettle_compiler_write_backtrace(stderr);
  g_compiler_in_ice_handler = 0;
}

void mettle_compiler_ice(const char *reason) {
  mettle_compiler_ice_report(reason, NULL);
  abort();
}

#if defined(_WIN32) || defined(_WIN64)
static LONG WINAPI mettle_compiler_unhandled_exception_filter(
    EXCEPTION_POINTERS *info) {
  char detail[64];

  if (g_compiler_in_ice_handler) {
    ExitProcess(3);
  }

  if (!info || !info->ExceptionRecord) {
    g_compiler_crash_context = info ? info->ContextRecord : NULL;
    mettle_compiler_ice_report("fatal exception", NULL);
    g_compiler_crash_context = NULL;
    ExitProcess(3);
  }

  snprintf(detail, sizeof(detail), "0x%08lX",
           (unsigned long)info->ExceptionRecord->ExceptionCode);
  g_compiler_crash_context = info->ContextRecord;
  mettle_compiler_ice_report(
      mettle_crash_exception_name(info->ExceptionRecord->ExceptionCode),
      detail);
  g_compiler_crash_context = NULL;
  ExitProcess(3);
}
#else
static void mettle_compiler_signal_handler(int signo, siginfo_t *info,
                                           void *ucontext_raw) {
  char detail[64];
  const char *reason = "fatal signal";

  (void)ucontext_raw;

  if (g_compiler_in_ice_handler) {
    _exit(128 + signo);
  }

  switch (signo) {
  case SIGSEGV:
    reason = "segmentation fault";
    break;
  case SIGBUS:
    reason = "bus error";
    break;
  case SIGFPE:
    reason = "floating-point exception";
    break;
  case SIGILL:
    reason = "illegal instruction";
    break;
  case SIGABRT:
    reason = "abort";
    break;
  default:
    break;
  }

  if (info && info->si_addr) {
    snprintf(detail, sizeof(detail), "at %p", info->si_addr);
    mettle_compiler_ice_report(reason, detail);
  } else {
    mettle_compiler_ice_report(reason, NULL);
  }
  _exit(128 + signo);
}
#endif

void mettle_compiler_crash_install(int argc, char **argv) {
  if (g_compiler_crash_installed) {
    return;
  }
  g_compiler_crash_installed = 1;
  (void)argc;
  (void)argv;

#if defined(_WIN32) || defined(_WIN64)
  SetUnhandledExceptionFilter(mettle_compiler_unhandled_exception_filter);
#else
  (void)mettle_install_signal_handler(
      SIGSEGV, (void (*)(int, void *, void *))mettle_compiler_signal_handler);
  (void)mettle_install_signal_handler(
      SIGBUS, (void (*)(int, void *, void *))mettle_compiler_signal_handler);
  (void)mettle_install_signal_handler(
      SIGFPE, (void (*)(int, void *, void *))mettle_compiler_signal_handler);
  (void)mettle_install_signal_handler(
      SIGILL, (void (*)(int, void *, void *))mettle_compiler_signal_handler);
  (void)mettle_install_signal_handler(
      SIGABRT, (void (*)(int, void *, void *))mettle_compiler_signal_handler);
#endif
}
