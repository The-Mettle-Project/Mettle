@echo off
REM Windows build script for Mettle
REM Usage: build.bat [gcc|clang] [--skip-tests]
REM   Or set CC=clang before invoking (defaults to gcc).

setlocal

REM Select compiler: args override CC env var; default gcc.
set "SKIP_TESTS="
:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="clang" (
    set "CC=clang"
    shift
    goto parse_args
)
if /I "%~1"=="gcc" (
    set "CC=gcc"
    shift
    goto parse_args
)
if /I "%~1"=="--skip-tests" (
    set "SKIP_TESTS=1"
    shift
    goto parse_args
)
if /I "%~1"=="--no-tests" (
    set "SKIP_TESTS=1"
    shift
    goto parse_args
)
echo Error: unknown argument '%~1'
echo Usage: build.bat [gcc^|clang] [--skip-tests]
exit /b 1

:args_done
if not defined CC set "CC=gcc"
if defined METTLE_SKIP_TESTS set "SKIP_TESTS=1"

set CFLAGS=-Wall -Wextra -std=c99 -g -O2 -D_GNU_SOURCE -Isrc -fno-omit-frame-pointer
if /I "%CC%"=="clang" set "CFLAGS=%CFLAGS% -D_CRT_NONSTDC_NO_DEPRECATE -D_CRT_SECURE_NO_WARNINGS"
REM Release builds stamp the version via METTLE_VERSION (e.g. set by release.yml);
REM dev builds fall back to the default in main.c.
if defined METTLE_VERSION set "CFLAGS=%CFLAGS% -DMETTLE_VERSION_RAW=%METTLE_VERSION%"
REM CodeView debug info lets DbgHelp resolve ICE backtraces to file:line on Windows.
REM Some MinGW gcc builds ICE in the CodeView emitter on large functions, so allow
REM opting out via METTLE_NO_CODEVIEW=1 (used by CI). The .pdb link flag is dropped
REM with it since there is no CodeView data to emit.
if defined METTLE_NO_CODEVIEW (
    set "LDFLAGS=-ldbghelp"
) else (
    if /I "%CC%"=="gcc" set "CFLAGS=%CFLAGS% -gcodeview"
    if /I "%CC%"=="clang" set "CFLAGS=%CFLAGS% -gcodeview"
    set "LDFLAGS=-ldbghelp -Wl,--pdb,bin\mettle.pdb"
)

REM Check if selected compiler is available
where %CC% >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo Error: %CC% not found. Please install MinGW-w64, LLVM/Clang, or similar.
    echo You can download MinGW-w64 from: https://www.mingw-w64.org/downloads/
    echo Or LLVM from: https://releases.llvm.org/
    exit /b 1
)

echo Building with %CC%...

REM Start from a clean object tree so stale scratch objects cannot
REM accidentally participate in the final link.
if exist obj rmdir /S /Q obj

REM Create directories
if not exist obj mkdir obj
if not exist obj\lexer mkdir obj\lexer
if not exist obj\parser mkdir obj\parser
if not exist obj\semantic mkdir obj\semantic
if not exist obj\ir mkdir obj\ir
if not exist obj\ir\optimizer mkdir obj\ir\optimizer
if not exist obj\codegen mkdir obj\codegen
if not exist obj\codegen\binary mkdir obj\codegen\binary
if not exist obj\linker mkdir obj\linker
if not exist obj\debug mkdir obj\debug
if not exist obj\error mkdir obj\error
if not exist obj\compiler mkdir obj\compiler
if not exist obj\runtime mkdir obj\runtime
if not exist bin mkdir bin

REM Compile source files
echo Compiling common utilities...
%CC% %CFLAGS% -c src\common.c -o obj\common.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling lexer...
%CC% %CFLAGS% -c src\lexer\lexer.c -o obj\lexer\lexer.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling parser...
%CC% %CFLAGS% -c src\parser\ast.c -o obj\parser\ast.o
if %ERRORLEVEL% NEQ 0 exit /b 1

%CC% %CFLAGS% -c src\parser\parser.c -o obj\parser\parser.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling semantic analysis...
%CC% %CFLAGS% -c src\semantic\symbol_table.c -o obj\semantic\symbol_table.o
if %ERRORLEVEL% NEQ 0 exit /b 1

%CC% %CFLAGS% -c src\semantic\type_checker.c -o obj\semantic\type_checker.o
if %ERRORLEVEL% NEQ 0 exit /b 1

%CC% %CFLAGS% -c src\semantic\type_checker_types.c -o obj\semantic\type_checker_types.o
if %ERRORLEVEL% NEQ 0 exit /b 1

%CC% %CFLAGS% -c src\semantic\type_checker_errors.c -o obj\semantic\type_checker_errors.o
if %ERRORLEVEL% NEQ 0 exit /b 1

%CC% %CFLAGS% -c src\semantic\type_checker_safety.c -o obj\semantic\type_checker_safety.o
if %ERRORLEVEL% NEQ 0 exit /b 1

%CC% %CFLAGS% -c src\semantic\type_checker_init_tracker.c -o obj\semantic\type_checker_init_tracker.o
if %ERRORLEVEL% NEQ 0 exit /b 1

%CC% %CFLAGS% -c src\semantic\type_checker_decl.c -o obj\semantic\type_checker_decl.o
if %ERRORLEVEL% NEQ 0 exit /b 1

%CC% %CFLAGS% -c src\semantic\type_checker_match.c -o obj\semantic\type_checker_match.o
if %ERRORLEVEL% NEQ 0 exit /b 1

%CC% %CFLAGS% -c src\semantic\type_checker_stmt.c -o obj\semantic\type_checker_stmt.o
if %ERRORLEVEL% NEQ 0 exit /b 1

%CC% %CFLAGS% -c src\semantic\type_checker_expr.c -o obj\semantic\type_checker_expr.o
if %ERRORLEVEL% NEQ 0 exit /b 1

%CC% %CFLAGS% -c src\semantic\type_checker_memory.c -o obj\semantic\type_checker_memory.o
if %ERRORLEVEL% NEQ 0 exit /b 1

%CC% %CFLAGS% -c src\semantic\register_allocator.c -o obj\semantic\register_allocator.o
if %ERRORLEVEL% NEQ 0 exit /b 1

%CC% %CFLAGS% -c src\semantic\import_resolver.c -o obj\semantic\import_resolver.o
if %ERRORLEVEL% NEQ 0 exit /b 1

%CC% %CFLAGS% -c src\semantic\monomorphize.c -o obj\semantic\monomorphize.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling IR...
for %%f in (src\ir\*.c) do (
    echo   %%~nxf
    %CC% %CFLAGS% -c %%f -o obj\ir\%%~nf.o
    if errorlevel 1 exit /b 1
)
for %%f in (src\ir\optimizer\*.c) do (
    echo   optimizer\%%~nxf
    %CC% %CFLAGS% -c %%f -o obj\ir\optimizer\%%~nf.o
    if errorlevel 1 exit /b 1
)

echo Compiling code generator modules...
for %%f in (code_generator_calls code_generator_flow code_generator_inline_debug code_generator_ir code_generator_ops code_generator_stack code_generator_variables) do (
    if exist obj\codegen\%%f.o del /Q obj\codegen\%%f.o
)
for %%f in (src\codegen\binary_emitter.c src\codegen\code_generator.c src\codegen\elf_emitter.c src\codegen\program_entry.c src\codegen\ptx_emitter.c) do (
    echo   %%~nxf
    %CC% %CFLAGS% -c %%f -o obj\\codegen\\%%~nf.o
    if errorlevel 1 exit /b 1
)

echo Compiling binary object backend...
for %%f in (src\\codegen\\binary\\*.c) do (
    echo   binary\\%%~nxf
    %CC% %CFLAGS% -c %%f -o obj\\codegen\\binary\\%%~nf.o
    if errorlevel 1 exit /b 1
)

echo Compiling linker modules...
for %%f in (src\\linker\\*.c) do (
    echo   %%~nxf
    %CC% %CFLAGS% -c %%f -o obj\\linker\\%%~nf.o
    if errorlevel 1 exit /b 1
)

echo Compiling debug info...
%CC% %CFLAGS% -c src\debug\debug_info.c -o obj\debug\debug_info.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling crash-handler runtime (opt-in: -d / -s / -g / IR trap)...
%CC% %CFLAGS% -c src\runtime\crash_handler.c -o obj\runtime\crash_handler.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling atomics helpers (opt-in: std/thread)...
%CC% %CFLAGS% -c src\runtime\atomics.c -o obj\runtime\atomics.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling profile runtime (opt-in: --profile-runtime)...
%CC% %CFLAGS% -c src\runtime\profile.c -o obj\runtime\profile.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling debug runtime (opt-in: --debug-hooks)...
%CC% %CFLAGS% -c src\runtime\debug.c -o obj\runtime\debug.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling Tracy helper stubs (opt-in: std/tracy without --tracy)...
%CC% %CFLAGS% -c stdlib\tracy_helpers.c -o obj\runtime\tracy_helpers.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling Tracy build support...
%CC% %CFLAGS% -c src\tracy_build.c -o obj\tracy_build.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling error reporter...
%CC% %CFLAGS% -c src\error\error_reporter.c -o obj\error\error_reporter.o
if %ERRORLEVEL% NEQ 0 exit /b 1
%CC% %CFLAGS% -c src\error\error_explain.c -o obj\error\error_explain.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling compiler diagnostics...
%CC% %CFLAGS% -c src\compiler\compiler_context.c -o obj\compiler\compiler_context.o
if %ERRORLEVEL% NEQ 0 exit /b 1
%CC% %CFLAGS% -c src\compiler\compiler_crash.c -o obj\compiler\compiler_crash.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling main...
%CC% %CFLAGS% -c src\main.c -o obj\main.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Linking...
%CC% obj\common.o obj\lexer\lexer.o obj\parser\ast.o obj\parser\parser.o obj\semantic\symbol_table.o obj\semantic\type_checker.o obj\semantic\type_checker_types.o obj\semantic\type_checker_errors.o obj\semantic\type_checker_safety.o obj\semantic\type_checker_init_tracker.o obj\semantic\type_checker_decl.o obj\semantic\type_checker_match.o obj\semantic\type_checker_stmt.o obj\semantic\type_checker_expr.o obj\semantic\type_checker_memory.o obj\semantic\register_allocator.o obj\semantic\import_resolver.o obj\semantic\monomorphize.o obj\ir\*.o obj\ir\optimizer\*.o obj\codegen\binary_emitter.o obj\codegen\code_generator.o obj\codegen\elf_emitter.o obj\codegen\program_entry.o obj\codegen\ptx_emitter.o obj\\codegen\\binary\\*.o obj\\linker\\*.o obj\debug\debug_info.o obj\error\error_reporter.o obj\error\error_explain.o obj\compiler\compiler_context.o obj\compiler\compiler_crash.o obj\runtime\crash_handler.o obj\tracy_build.o obj\main.o -o bin\mettle.exe %LDFLAGS%

if %ERRORLEVEL% NEQ 0 (
    echo Build failed!
    exit /b 1
)

echo Bundling standard library into bin\stdlib...
if exist bin\stdlib rmdir /S /Q bin\stdlib
xcopy stdlib bin\stdlib\ /E /I /Y >nul

echo Bundling runtime into bin\runtime...
if exist bin\runtime rmdir /S /Q bin\runtime
xcopy src\runtime bin\runtime\ /E /I /Y >nul
copy /Y obj\runtime\crash_handler.o bin\runtime\crash_handler.o >nul
copy /Y obj\runtime\crash_handler.o bin\runtime\crash_handler.obj >nul
copy /Y obj\runtime\atomics.o bin\runtime\atomics.o >nul
copy /Y obj\runtime\atomics.o bin\runtime\atomics.obj >nul
copy /Y obj\runtime\profile.o bin\runtime\profile.o >nul
copy /Y obj\runtime\profile.o bin\runtime\profile.obj >nul
copy /Y obj\runtime\debug.o bin\runtime\debug.o >nul
copy /Y obj\runtime\debug.o bin\runtime\debug.obj >nul
copy /Y obj\runtime\tracy_helpers.o bin\runtime\tracy_helpers.o >nul
copy /Y obj\runtime\tracy_helpers.o bin\runtime\tracy_helpers.obj >nul

if exist installer\mettle-build.bat copy /Y installer\mettle-build.bat bin\mettle-build.bat >nul

echo Bundling ML optimizer model into bin\mlopt (used by --ml-opt)...
if exist bin\mlopt rmdir /S /Q bin\mlopt
mkdir bin\mlopt
if exist tools\mlopt\gnn_genius.bin copy /Y tools\mlopt\gnn_genius.bin bin\mlopt\gnn_genius.bin >nul
if exist tools\mlopt\bw_lib.txt copy /Y tools\mlopt\bw_lib.txt bin\mlopt\bw_lib.txt >nul
if exist tools\mlopt\gf2_lib1.txt copy /Y tools\mlopt\gf2_lib1.txt bin\mlopt\gf2_lib1.txt >nul

echo Rendering README.html for the installer docs shortcut...
where python >nul 2>&1 && python installer\render_readme.py

echo Build successful! Executable created at bin\mettle.exe
if defined SKIP_TESTS (
    echo Tests skipped.
    exit /b 0
)
echo.
echo Running tests...
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1
if %ERRORLEVEL% NEQ 0 (
    echo Tests failed!
    exit /b 1
)
echo All tests passed.
