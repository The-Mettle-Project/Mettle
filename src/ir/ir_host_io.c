#include "ir_host_io.h"

#include <stdio.h>
#include <string.h>

int mettle_file_exists(const char *path);
int mettle_dir_exists(const char *path);
int mettle_dir_create(const char *path);
int mettle_getcwd(char *buffer, int size);

long long ir_host_runtime(const char *name, const long long *args,
                          int *known) {
  const char *path = (const char *)(size_t)args[0];
  *known = 1;
  if (strcmp(name, "mettle_file_exists") == 0) {
    return mettle_file_exists(path);
  }
  if (strcmp(name, "mettle_dir_exists") == 0) {
    return mettle_dir_exists(path);
  }
  if (strcmp(name, "mettle_dir_create") == 0 ||
      strcmp(name, "mettle_make_directory") == 0) {
    return mettle_dir_create(path);
  }
  if (strcmp(name, "mettle_getcwd") == 0) {
    return mettle_getcwd((char *)(size_t)args[0], (int)args[1]);
  }
  *known = 0;
  return 0;
}

long long ir_host_stdio(const char *name, const long long *args, int *known) {
  FILE *fp = (FILE *)(size_t)args[0];
  void *buf = (void *)(size_t)args[0];
  *known = 1;
  if (strcmp(name, "remove") == 0 || strcmp(name, "unlink") == 0) {
    return remove((const char *)buf);
  }
  if (strcmp(name, "rename") == 0) {
    return rename((const char *)buf, (const char *)(size_t)args[1]);
  }
  if (strcmp(name, "fopen") == 0) {
    return (long long)(size_t)fopen((const char *)buf,
                                    (const char *)(size_t)args[1]);
  }
  if (strcmp(name, "fclose") == 0) {
    return fclose(fp);
  }
  if (strcmp(name, "fread") == 0) {
    return (long long)fread(buf, (size_t)args[1], (size_t)args[2],
                            (FILE *)(size_t)args[3]);
  }
  if (strcmp(name, "fwrite") == 0) {
    return (long long)fwrite(buf, (size_t)args[1], (size_t)args[2],
                             (FILE *)(size_t)args[3]);
  }
  if (strcmp(name, "fgets") == 0) {
    return (long long)(size_t)fgets((char *)buf, (int)args[1],
                                    (FILE *)(size_t)args[2]);
  }
  if (strcmp(name, "fputs") == 0) {
    return fputs((const char *)buf, (FILE *)(size_t)args[1]);
  }
  if (strcmp(name, "fputc") == 0 || strcmp(name, "putc") == 0) {
    return fputc((int)args[0], (FILE *)(size_t)args[1]);
  }
  if (strcmp(name, "fflush") == 0) {
    return fflush(fp);
  }
  if (strcmp(name, "fseek") == 0) {
    return fseek(fp, (long)args[1], (int)args[2]);
  }
  if (strcmp(name, "ftell") == 0) {
    return ftell(fp);
  }
  *known = 0;
  return 0;
}

#if defined(_WIN32)
#include <windows.h>

#define P(i) ((void *)(size_t)args[i])
#define S(i) ((const char *)(size_t)args[i])
#define H(i) ((HANDLE)(size_t)args[i])
#define U(i) ((DWORD)args[i])

static long long ir_host_win32_files(const char *name, const long long *args,
                                     int *known) {
  if (strcmp(name, "CreateFileA") == 0) {
    return (long long)(size_t)CreateFileA(S(0), U(1), U(2), NULL, U(4), U(5),
                                          NULL);
  }
  if (strcmp(name, "ReadFile") == 0) {
    return ReadFile(H(0), P(1), U(2), (DWORD *)P(3), (OVERLAPPED *)P(4));
  }
  if (strcmp(name, "WriteFile") == 0) {
    return WriteFile(H(0), P(1), U(2), (DWORD *)P(3), (OVERLAPPED *)P(4));
  }
  if (strcmp(name, "CloseHandle") == 0) {
    return CloseHandle(H(0));
  }
  if (strcmp(name, "FlushFileBuffers") == 0) {
    return FlushFileBuffers(H(0));
  }
  if (strcmp(name, "GetFileSizeEx") == 0) {
    return GetFileSizeEx(H(0), (LARGE_INTEGER *)P(1));
  }
  if (strcmp(name, "SetFileInformationByHandle") == 0) {
    return SetFileInformationByHandle(H(0), (FILE_INFO_BY_HANDLE_CLASS)args[1],
                                      P(2), U(3));
  }
  if (strcmp(name, "GetFileInformationByHandle") == 0) {
    return GetFileInformationByHandle(H(0),
                                      (BY_HANDLE_FILE_INFORMATION *)P(1));
  }
  if (strcmp(name, "LockFileEx") == 0) {
    return LockFileEx(H(0), U(1), U(2), U(3), U(4), (OVERLAPPED *)P(5));
  }
  if (strcmp(name, "UnlockFileEx") == 0) {
    return UnlockFileEx(H(0), U(1), U(2), U(3), (OVERLAPPED *)P(4));
  }
  if (strcmp(name, "GetFinalPathNameByHandleA") == 0) {
    return GetFinalPathNameByHandleA(H(0), (char *)P(1), U(2), U(3));
  }
  *known = 0;
  return 0;
}

static long long ir_host_win32_paths(const char *name, const long long *args,
                                     int *known) {
  if (strcmp(name, "MoveFileExA") == 0) {
    return MoveFileExA(S(0), S(1), U(2));
  }
  if (strcmp(name, "DeleteFileA") == 0) {
    return DeleteFileA(S(0));
  }
  if (strcmp(name, "CreateDirectoryA") == 0) {
    return CreateDirectoryA(S(0), NULL);
  }
  if (strcmp(name, "RemoveDirectoryA") == 0) {
    return RemoveDirectoryA(S(0));
  }
  if (strcmp(name, "GetFileAttributesA") == 0) {
    return (long long)GetFileAttributesA(S(0));
  }
  if (strcmp(name, "SetFileAttributesA") == 0) {
    return SetFileAttributesA(S(0), U(1));
  }
  if (strcmp(name, "GetFileAttributesExA") == 0) {
    return GetFileAttributesExA(S(0), (GET_FILEEX_INFO_LEVELS)args[1], P(2));
  }
  if (strcmp(name, "GetCurrentDirectoryA") == 0) {
    return (long long)GetCurrentDirectoryA(U(0), (char *)P(1));
  }
  if (strcmp(name, "SetCurrentDirectoryA") == 0) {
    return SetCurrentDirectoryA(S(0));
  }
  if (strcmp(name, "GetTempPathA") == 0) {
    return (long long)GetTempPathA(U(0), (char *)P(1));
  }
  if (strcmp(name, "FindFirstFileA") == 0) {
    return (long long)(size_t)FindFirstFileA(S(0), (WIN32_FIND_DATAA *)P(1));
  }
  if (strcmp(name, "FindNextFileA") == 0) {
    return FindNextFileA(H(0), (WIN32_FIND_DATAA *)P(1));
  }
  if (strcmp(name, "FindClose") == 0) {
    return FindClose(H(0));
  }
  if (strcmp(name, "CreateSymbolicLinkA") == 0) {
    return CreateSymbolicLinkA(S(0), S(1), U(2));
  }
  *known = 0;
  return 0;
}

long long ir_host_win32(const char *name, const long long *args, int *known,
                        unsigned *error) {
  *known = 1;
  SetLastError(0);
  long long result = ir_host_win32_files(name, args, known);
  if (!*known) {
    *known = 1;
    result = ir_host_win32_paths(name, args, known);
  }
  *error = (unsigned)GetLastError();
  return result;
}

long long ir_host_syscall(long long number, const long long *args,
                          int *known) {
  (void)number;
  (void)args;
  *known = 0;
  return -38;
}

#else

long long mettle_host_syscall6(long long number, long long a1, long long a2,
                               long long a3, long long a4, long long a5,
                               long long a6);

long long ir_host_win32(const char *name, const long long *args, int *known,
                        unsigned *error) {
  (void)name;
  (void)args;
  *known = 0;
  *error = 0;
  return 0;
}

long long ir_host_syscall(long long number, const long long *args,
                          int *known) {
  *known = 1;
  return mettle_host_syscall6(number, args[0], args[1], args[2], args[3],
                              args[4], args[5]);
}

#endif
