@echo off
REM Windows build script for Mettle
REM Usage: build.bat [gcc|clang] [--skip-tests] [--backend-only]
REM   Or set CC=clang before invoking (defaults to gcc).
REM
REM --backend-only stops after archiving bin\mtlc.lib: the libmtlc backend
REM alone, with none of the reference frontend. That is what a downstream
REM frontend needs -- the Mettle language repository fetches this tree and
REM builds the archive this way, then links its own driver against it.

setlocal

REM Select compiler: args override CC env var; default gcc.
set "SKIP_TESTS="
set "BACKEND_ONLY="
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
if /I "%~1"=="--backend-only" (
    set "BACKEND_ONLY=1"
    shift
    goto parse_args
)
if /I "%~1"=="--libmtlc-only" (
    set "BACKEND_ONLY=1"
    shift
    goto parse_args
)
echo Error: unknown argument '%~1'
echo Usage: build.bat [gcc^|clang] [--skip-tests] [--backend-only]
exit /b 1

:args_done
if not defined CC set "CC=gcc"
if defined METTLE_SKIP_TESTS set "SKIP_TESTS=1"
if defined METTLE_BACKEND_ONLY set "BACKEND_ONLY=1"

REM Every source file binds to the owned host ABI through host_redirect.h.
REM The compiler's own TUs keep Win64 unwind tables. StackWalk64 in the crash
REM handler unwinds x64 frames through .pdata/.xdata, so dropping them blinds
REM the ICE backtrace, and gcc 15.2 segfaults in the -gcodeview emitter when
REM -fno-asynchronous-unwind-tables removes them. .pdata is inert data: it
REM pulls in no unwinder, so the owned-runtime audit below is unaffected.
set CFLAGS=-Wall -Wextra -std=c99 -g -O2 -D_GNU_SOURCE -Isrc -Iinclude -fno-omit-frame-pointer -ffreestanding -fno-builtin -fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -include src/runtime/host_redirect.h
set RUNTIME_CFLAGS=-std=c99 -O2 -D_GNU_SOURCE -Isrc -ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -mno-stack-arg-probe -ffunction-sections -fdata-sections -fno-jump-tables
REM This build is mingw-ABI throughout: GNU ld, ar/nm, --entry, and the
REM __imp_-only archive audit. A stock LLVM install defaults to
REM x86_64-pc-windows-msvc, which emits _fltused, _tls_index and UCRT calls
REM the owned runtime does not provide, so pin clang to the GNU target.
REM On MSYS2 CLANG64 clang that is already the default and this is a no-op.
REM -femulated-tls picks the TLS model the owned runtime actually implements:
REM __thread through __emutls_get_address, which is what gcc emits on mingw by
REM default. Native Windows TLS instead wants _tls_index from the CRT's tlssup.
set "CCTARGET="
if /I "%CC%"=="clang" (
    set "CCTARGET=--target=x86_64-w64-windows-gnu"
    set "CFLAGS=%CFLAGS% --target=x86_64-w64-windows-gnu -femulated-tls -D_CRT_NONSTDC_NO_DEPRECATE -D_CRT_SECURE_NO_WARNINGS"
    set "RUNTIME_CFLAGS=%RUNTIME_CFLAGS% --target=x86_64-w64-windows-gnu -femulated-tls"
)
REM Release builds stamp the version via METTLE_VERSION (e.g. set by release.yml);
REM dev builds fall back to the default in main.c.
if defined METTLE_VERSION set "CFLAGS=%CFLAGS% -DMETTLE_VERSION_RAW=%METTLE_VERSION%"
REM CodeView debug info lets DbgHelp resolve ICE backtraces to file:line on Windows.
REM Opt out via METTLE_NO_CODEVIEW=1 (used by CI). The .pdb link flag is dropped
REM with it since there is no CodeView data to emit.
REM clang stays on DWARF: its CodeView emitter names the pre-emulation symbol of
REM every __thread variable, and under -femulated-tls nothing defines those, so
REM the archive audit below sees a screenful of undefined g_* symbols.
if defined METTLE_NO_CODEVIEW (
    set "LDFLAGS=-ldbghelp"
) else (
    if /I "%CC%"=="gcc" (
        set "CFLAGS=%CFLAGS% -gcodeview"
        set "LDFLAGS=-ldbghelp -Wl,--pdb,bin\mettle.pdb"
    ) else (
        set "LDFLAGS=-ldbghelp"
    )
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
if not exist obj\frontend mkdir obj\frontend
if not exist bin mkdir bin

REM Compile source files
echo Compiling common utilities...
%CC% %CFLAGS% -c src\common.c -o obj\common.o
if %ERRORLEVEL% NEQ 0 exit /b 1

REM Everything from here to :compile_ir is the reference frontend, which a
REM backend-only build has no use for.
if defined BACKEND_ONLY goto compile_ir

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

%CC% %CFLAGS% -c src\semantic\comptime_value.c -o obj\semantic\comptime_value.o
if %ERRORLEVEL% NEQ 0 exit /b 1

%CC% %CFLAGS% -c src\semantic\type_layout.c -o obj\semantic\type_layout.o
%CC% %CFLAGS% -c src\semantic\comptime_expand.c -o obj\semantic\comptime_expand.o
%CC% %CFLAGS% -c src\semantic\type_query.c -o obj\semantic\type_query.o
%CC% %CFLAGS% -c src\parser\ast_print.c -o obj\parser\ast_print.o
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

%CC% %CFLAGS% -c src\semantic\type_checker_aggregate.c -o obj\semantic\type_checker_aggregate.o
if %ERRORLEVEL% NEQ 0 exit /b 1

%CC% %CFLAGS% -c src\semantic\type_checker_tensor_epilogue.c -o obj\semantic\type_checker_tensor_epilogue.o
if %ERRORLEVEL% NEQ 0 exit /b 1

%CC% %CFLAGS% -c src\semantic\type_checker_memory.c -o obj\semantic\type_checker_memory.o
if %ERRORLEVEL% NEQ 0 exit /b 1

%CC% %CFLAGS% -c src\semantic\register_allocator.c -o obj\semantic\register_allocator.o
if %ERRORLEVEL% NEQ 0 exit /b 1

%CC% %CFLAGS% -c src\semantic\import_resolver.c -o obj\semantic\import_resolver.o
if %ERRORLEVEL% NEQ 0 exit /b 1

%CC% %CFLAGS% -c src\semantic\monomorphize.c -o obj\semantic\monomorphize.o
if %ERRORLEVEL% NEQ 0 exit /b 1

:compile_ir
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
for %%f in (src\codegen\binary_emitter.c src\codegen\code_generator.c src\codegen\elf_emitter.c src\codegen\ptx_emitter.c src\codegen\spirv_emitter.c) do (
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

echo Compiling required freestanding program runtime...
%CC% %RUNTIME_CFLAGS% -c src\runtime\freestanding.c -o obj\runtime\freestanding.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling owned host runtime and startup...
%CC% %RUNTIME_CFLAGS% -include src/runtime/host_prefix.h -c src\runtime\freestanding.c -o obj\runtime\host_runtime.o
if %ERRORLEVEL% NEQ 0 exit /b 1
%CC% %RUNTIME_CFLAGS% -c src\runtime\host_startup.c -o obj\runtime\host_startup.o
if %ERRORLEVEL% NEQ 0 exit /b 1

REM The language runtime, the Tracy shim and the driver's allocator all belong
REM to the frontend side; skip them for a backend-only build.
if defined BACKEND_ONLY goto compile_diagnostics

echo Compiling crash-handler runtime (opt-in: -d / -s / -g / IR trap)...
%CC% %RUNTIME_CFLAGS% -c src\runtime\crash_handler.c -o obj\runtime\crash_handler.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling memory-safety runtime (opt-in: --safe)...
%CC% %RUNTIME_CFLAGS% -c src\runtime\safety.c -o obj\runtime\safety.o
if %ERRORLEVEL% NEQ 0 exit /b 1

rem The swap runtime is written in Mettle and compiled by the compiler this
rem build produces, so it is staged after bin\mettle.exe exists.

echo Compiling atomics helpers (opt-in: std/thread)...
%CC% %RUNTIME_CFLAGS% -DMETTLE_ATOMICS_IN_FREESTANDING -c src\runtime\atomics.c -o obj\runtime\atomics.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling profile runtime (opt-in: --profile-runtime)...
%CC% %RUNTIME_CFLAGS% -c src\runtime\profile.c -o obj\runtime\profile.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling debug runtime (opt-in: --debug-hooks)...
%CC% %RUNTIME_CFLAGS% -c src\runtime\debug.c -o obj\runtime\debug.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling Tracy helper stubs (opt-in: std/tracy without --tracy)...
%CC% %RUNTIME_CFLAGS% -c stdlib\tracy_helpers.c -o obj\runtime\tracy_helpers.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling Tracy build support...
%CC% %CFLAGS% -c src\tracy_build.c -o obj\tracy_build.o
if %ERRORLEVEL% NEQ 0 exit /b 1

:compile_diagnostics
echo Compiling error reporter...
%CC% %CFLAGS% -c src\error\error_reporter.c -o obj\error\error_reporter.o
if %ERRORLEVEL% NEQ 0 exit /b 1
REM error_explain.c renders the driver's optimization report: frontend-side.
if not defined BACKEND_ONLY (
    %CC% %CFLAGS% -c src\error\error_explain.c -o obj\error\error_explain.o
    if errorlevel 1 exit /b 1
)

echo Compiling compiler diagnostics...
%CC% %CFLAGS% -c src\compiler\compiler_context.c -o obj\compiler\compiler_context.o
if %ERRORLEVEL% NEQ 0 exit /b 1
%CC% %CFLAGS% -c src\compiler\compiler_crash.c -o obj\compiler\compiler_crash.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling libmtlc public API...
%CC% %CFLAGS% -c src\mtlc_api.c -o obj\mtlc_api.o
if %ERRORLEVEL% NEQ 0 exit /b 1
%CC% %CFLAGS% -c src\mtlc_build.c -o obj\mtlc_build.o
if %ERRORLEVEL% NEQ 0 exit /b 1
%CC% %CFLAGS% -c src\mtlc_lib_fallbacks.c -o obj\mtlc_lib_fallbacks.o
if %ERRORLEVEL% NEQ 0 exit /b 1
%CC% %CFLAGS% -c src\mtlc_crash_fallback.c -o obj\mtlc_crash_fallback.o
if %ERRORLEVEL% NEQ 0 exit /b 1
%CC% %CFLAGS% -c src\runtime\verify_owned.c -o obj\runtime\verify_owned.o
if %ERRORLEVEL% NEQ 0 exit /b 1

if defined BACKEND_ONLY goto archive_libmtlc

echo Compiling frontend-to-backend type adapter...
%CC% %CFLAGS% -c src\frontend\mtlc_type_from_frontend.c -o obj\frontend\mtlc_type_from_frontend.o
if %ERRORLEVEL% NEQ 0 exit /b 1
%CC% %CFLAGS% -c src\frontend\mtlc_lower_module.c -o obj\frontend\mtlc_lower_module.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling main...
%CC% %CFLAGS% -c src\main.c -o obj\main.o
if %ERRORLEVEL% NEQ 0 exit /b 1

:archive_libmtlc

REM ---------------------------------------------------------------------------
REM Archive the standalone backend into libmtlc, then link the reference frontend
REM (this driver) against it. libmtlc = the IR core, optimizer + GNN, code
REM generators, and native linker. The AST->IR lowering TUs (ir_lowering,
REM ir_lower_*) are a FRONTEND concern and link into the driver, not the archive.
REM ar does not expand wildcards, so add objects via a cmd FOR loop (which does).
REM ---------------------------------------------------------------------------
set "AR=ar"
where %AR% >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    if /I "%CC%"=="clang" set "AR=llvm-ar"
)
where %AR% >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo Error: archiver '%AR%' not found on PATH ^(need binutils ar or llvm-ar^).
    exit /b 1
)

echo Archiving libmtlc ^(backend: IR core, optimizer, codegen, linker^)...
if exist bin\mtlc.lib del /Q bin\mtlc.lib
REM Backend IR core -- explicitly listed to EXCLUDE the lowering TUs below.
for %%o in (ir ir_comptime ir_debug_hooks ir_interp ir_optimize ir_pgo ir_profile ir_verify ml_gnn ml_obs ml_opt mtlc_type) do %AR% rcs bin\mtlc.lib obj\ir\%%o.o
for %%o in (obj\ir\optimizer\*.o) do %AR% rcs bin\mtlc.lib %%o
for %%o in (obj\codegen\binary_emitter.o obj\codegen\code_generator.o obj\codegen\elf_emitter.o obj\codegen\ptx_emitter.o obj\codegen\spirv_emitter.o) do %AR% rcs bin\mtlc.lib %%o
for %%o in (obj\codegen\binary\*.o) do %AR% rcs bin\mtlc.lib %%o
for %%o in (obj\linker\*.o) do %AR% rcs bin\mtlc.lib %%o
%AR% rcs bin\mtlc.lib obj\debug\debug_info.o
REM The diagnostics reporter is frontend-neutral (raw source text + SourceLocation,
REM no AST) and the backend comptime interpreter reports through it -> libmtlc.
%AR% rcs bin\mtlc.lib obj\error\error_reporter.o
for %%o in (obj\compiler\*.o) do %AR% rcs bin\mtlc.lib %%o
%AR% rcs bin\mtlc.lib obj\common.o obj\mtlc_api.o obj\mtlc_build.o obj\mtlc_lib_fallbacks.o obj\mtlc_crash_fallback.o obj\runtime\verify_owned.o
%AR% rcs bin\mtlc.lib obj\runtime\host_runtime.o
if not exist bin\mtlc.lib (
    echo Build failed: bin\mtlc.lib was not created.
    exit /b 1
)
ld -r --disable-runtime-pseudo-reloc --whole-archive bin\mtlc.lib --no-whole-archive -o obj\runtime\libmtlc-closure.o
if errorlevel 1 (
    echo Build failed: could not compute the libmtlc symbol closure.
    exit /b 1
)
nm -u obj\runtime\libmtlc-closure.o | findstr /V /C:"__imp_" >nul
if not errorlevel 1 (
    echo Build failed: libmtlc contains unresolved non-OS symbols.
    nm -u obj\runtime\libmtlc-closure.o
    exit /b 1
)
nm -u obj\runtime\libmtlc-closure.o | findstr /I /R /C:"__imp_malloc$" /C:"__imp_calloc$" /C:"__imp_realloc$" /C:"__imp_free$" /C:"__imp_memcpy$" /C:"__imp_memset$" /C:"__imp_printf$" /C:"__imp_fprintf$" /C:"__imp_strtod$" /C:"msvcrt" /C:"ucrt" /C:"vcruntime" /C:"libgcc" /C:"libwinpthread" >nul
if not errorlevel 1 (
    echo Build failed: libmtlc imports a C or compiler runtime symbol.
    nm -u obj\runtime\libmtlc-closure.o
    exit /b 1
)

REM A backend-only build is done here: the archive plus include\mtlc is
REM everything a frontend links against.
if defined BACKEND_ONLY (
    if exist bin\runtime rmdir /S /Q bin\runtime
    xcopy src\runtime bin\runtime\ /E /I /Y >nul
    copy /Y obj\runtime\freestanding.o bin\runtime\freestanding.o >nul
    copy /Y obj\runtime\freestanding.o bin\runtime\freestanding.obj >nul
    echo.
    echo libmtlc built: bin\mtlc.lib
    echo   headers: include\mtlc ^(public API^), src ^(backend internals^)
    echo   runtime: bin\runtime\freestanding.obj
    exit /b 0
)

echo Linking mettle ^(reference frontend^) against libmtlc...
set "LDFLAGS=%LDFLAGS% -Wl,--disable-runtime-pseudo-reloc"
%CC% %CCTARGET% -nostdlib -nostartfiles -nodefaultlibs -Wl,--entry,mettle_start -Wl,--subsystem,console obj\runtime\host_startup.o obj\lexer\lexer.o obj\parser\ast.o obj\parser\ast_print.o obj\parser\parser.o obj\semantic\symbol_table.o obj\semantic\comptime_value.o obj\semantic\type_layout.o obj\semantic\comptime_expand.o obj\semantic\type_query.o obj\semantic\type_checker.o obj\semantic\type_checker_types.o obj\semantic\type_checker_errors.o obj\semantic\type_checker_safety.o obj\semantic\type_checker_init_tracker.o obj\semantic\type_checker_decl.o obj\semantic\type_checker_match.o obj\semantic\type_checker_stmt.o obj\semantic\type_checker_expr.o obj\semantic\type_checker_aggregate.o obj\semantic\type_checker_tensor_epilogue.o obj\semantic\type_checker_memory.o obj\semantic\register_allocator.o obj\semantic\import_resolver.o obj\semantic\monomorphize.o obj\ir\ir_lowering.o obj\ir\ir_lower_address.o obj\ir\ir_lower_defer.o obj\ir\ir_lower_expr.o obj\ir\ir_lower_stmt.o obj\ir\ir_lower_support.o obj\ir\ir_lower_switch_match.o obj\ir\ir_lower_types.o obj\frontend\mtlc_type_from_frontend.o obj\frontend\mtlc_lower_module.o obj\error\error_explain.o obj\tracy_build.o obj\main.o bin\mtlc.lib -o bin\mettle.exe -lkernel32 -ldbghelp %LDFLAGS%

if %ERRORLEVEL% NEQ 0 (
    echo Build failed!
    exit /b 1
)
objdump -p bin\mettle.exe | findstr /I /C:"msvcrt" /C:"ucrt" /C:"vcruntime" /C:"api-ms-win-crt" /C:"libgcc" /C:"libwinpthread" >nul
if %ERRORLEVEL% EQU 0 (
    echo Build failed: bin\mettle.exe imports a forbidden C or compiler runtime.
    objdump -p bin\mettle.exe | findstr /I "DLL Name"
    exit /b 1
)

echo Bundling standard library into bin\stdlib...
if exist bin\stdlib rmdir /S /Q bin\stdlib
xcopy stdlib bin\stdlib\ /E /I /Y >nul

echo Bundling runtime into bin\runtime...
if exist bin\runtime rmdir /S /Q bin\runtime
xcopy src\runtime bin\runtime\ /E /I /Y >nul
copy /Y obj\runtime\freestanding.o bin\runtime\freestanding.o >nul
copy /Y obj\runtime\freestanding.o bin\runtime\freestanding.obj >nul
copy /Y obj\runtime\host_startup.o bin\runtime\host_startup.o >nul
copy /Y obj\runtime\host_startup.o bin\runtime\host_startup.obj >nul
copy /Y obj\runtime\crash_handler.o bin\runtime\crash_handler.o >nul
copy /Y obj\runtime\crash_handler.o bin\runtime\crash_handler.obj >nul
copy /Y obj\runtime\safety.o bin\runtime\safety.o >nul
copy /Y obj\runtime\safety.o bin\runtime\safety.obj >nul
echo Compiling swap runtime from Mettle source...
bin\mettle.exe --release --emit-obj src\runtime\swap.mettle -o bin\runtime\swap.o
if %ERRORLEVEL% NEQ 0 exit /b 1
copy /Y bin\runtime\swap.o bin\runtime\swap.obj >nul

echo Compiling string runtime from Mettle source...
bin\mettle.exe --release --emit-obj src\runtime\string.mettle -o bin\runtime\string.o
if %ERRORLEVEL% NEQ 0 exit /b 1
copy /Y bin\runtime\string.o bin\runtime\string.obj >nul
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
