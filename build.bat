@echo off
REM Windows build script for Mettle.
REM
REM Builds the Mettle compiler only. The backend -- IR optimization, code
REM generation, native linking -- is libmtlc, a separate project:
REM
REM     .\get-libmtlc.ps1     fetch libmtlc and build its archive
REM     .\build.bat           build bin\mettle.exe against it
REM
REM Usage: build.bat [gcc|clang] [--skip-tests]
REM   Or set CC=clang before invoking (defaults to gcc).
REM   Set LIBMTLC_DIR to build against a libmtlc checkout (default .\libmtlc).

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
if not defined LIBMTLC_DIR set "LIBMTLC_DIR=libmtlc"

REM ---------------------------------------------------------------------------
REM The libmtlc dependency has to be in place before anything else: the driver
REM includes the backend's headers and links its archive.
REM ---------------------------------------------------------------------------
if not exist "%LIBMTLC_DIR%\include\mtlc\mtlc.h" (
    echo Error: libmtlc not found in '%LIBMTLC_DIR%'.
    echo.
    echo Mettle is the frontend; libmtlc is the backend it compiles against.
    echo Fetch it with:
    echo     powershell -ExecutionPolicy Bypass -File get-libmtlc.ps1
    echo or set LIBMTLC_DIR to an existing libmtlc checkout.
    echo     https://github.com/The-Mettle-Project/libmtlc
    exit /b 1
)

REM A dist bundle drops the archive in lib\; building from source puts it in
REM bin\, which is what get-libmtlc.ps1 produces.
set "LIBMTLC_LIB="
if exist "%LIBMTLC_DIR%\lib\mtlc.lib" set "LIBMTLC_LIB=%LIBMTLC_DIR%\lib\mtlc.lib"
if not defined LIBMTLC_LIB if exist "%LIBMTLC_DIR%\lib\libmtlc.a" set "LIBMTLC_LIB=%LIBMTLC_DIR%\lib\libmtlc.a"
if not defined LIBMTLC_LIB if exist "%LIBMTLC_DIR%\bin\mtlc.lib" set "LIBMTLC_LIB=%LIBMTLC_DIR%\bin\mtlc.lib"
if not defined LIBMTLC_LIB if exist "%LIBMTLC_DIR%\bin\libmtlc.a" set "LIBMTLC_LIB=%LIBMTLC_DIR%\bin\libmtlc.a"
if not defined LIBMTLC_LIB (
    echo Error: no libmtlc archive found under '%LIBMTLC_DIR%'.
    echo Build it with:
    echo     powershell -ExecutionPolicy Bypass -File get-libmtlc.ps1
    echo or, from a libmtlc checkout:
    echo     build.bat --backend-only
    exit /b 1
)

REM METTLE_INTERNAL_ALLOC builds the driver against src\mettle_alloc.c
REM instead of the platform heap. Set METTLE_NO_INTERNAL_ALLOC=1 to fall
REM back to malloc (e.g. to attribute a regression). A sanitizer build needs
REM no variable: the allocator detects one and stands down.
REM
REM -Isrc comes FIRST so this repository's headers always win over libmtlc's
REM own copy of the frontend: libmtlc is a monorepo that carries the reference
REM frontend too, and its src\ has to be on the include path for the backend
REM headers the driver uses. See docs\mettle-and-libmtlc.md.
set CFLAGS=-Wall -Wextra -std=c99 -g -O2 -D_GNU_SOURCE -Isrc -I%LIBMTLC_DIR%\include -I%LIBMTLC_DIR%\src -fno-omit-frame-pointer
if not defined METTLE_NO_INTERNAL_ALLOC set "CFLAGS=%CFLAGS% -DMETTLE_INTERNAL_ALLOC"
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

echo Building Mettle with %CC% against %LIBMTLC_LIB%...

REM Start from a clean object tree so stale scratch objects cannot
REM accidentally participate in the final link.
if exist obj rmdir /S /Q obj

REM Create directories
if not exist obj mkdir obj
if not exist obj\lexer mkdir obj\lexer
if not exist obj\parser mkdir obj\parser
if not exist obj\semantic mkdir obj\semantic
if not exist obj\ir mkdir obj\ir
if not exist obj\error mkdir obj\error
if not exist obj\runtime mkdir obj\runtime
if not exist obj\frontend mkdir obj\frontend
if not exist bin mkdir bin

echo Compiling lexer...
%CC% %CFLAGS% -c src\lexer\lexer.c -o obj\lexer\lexer.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling parser...
%CC% %CFLAGS% -c src\parser\ast.c -o obj\parser\ast.o
if %ERRORLEVEL% NEQ 0 exit /b 1

%CC% %CFLAGS% -c src\parser\parser.c -o obj\parser\parser.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling semantic analysis...
for %%f in (symbol_table type_checker type_checker_types type_checker_errors type_checker_safety type_checker_init_tracker type_checker_decl type_checker_match type_checker_stmt type_checker_expr type_checker_aggregate type_checker_tensor_epilogue type_checker_memory register_allocator import_resolver monomorphize) do (
    echo   %%f.c
    %CC% %CFLAGS% -c src\semantic\%%f.c -o obj\semantic\%%f.o
    if errorlevel 1 exit /b 1
)

echo Compiling AST-to-IR lowering...
for %%f in (ir_lowering ir_lower_address ir_lower_defer ir_lower_expr ir_lower_stmt ir_lower_support ir_lower_switch_match ir_lower_types) do (
    echo   %%f.c
    %CC% %CFLAGS% -c src\ir\%%f.c -o obj\ir\%%f.o
    if errorlevel 1 exit /b 1
)

echo Compiling frontend-to-backend adapters...
%CC% %CFLAGS% -c src\frontend\mtlc_type_from_frontend.c -o obj\frontend\mtlc_type_from_frontend.o
if %ERRORLEVEL% NEQ 0 exit /b 1
%CC% %CFLAGS% -c src\frontend\mtlc_lower_module.c -o obj\frontend\mtlc_lower_module.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling optimization report renderer...
%CC% %CFLAGS% -c src\error\error_explain.c -o obj\error\error_explain.o
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

echo Compiling allocator...
%CC% %CFLAGS% -c src\mettle_alloc.c -o obj\mettle_alloc.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Compiling main...
%CC% %CFLAGS% -c src\main.c -o obj\main.o
if %ERRORLEVEL% NEQ 0 exit /b 1

echo Linking mettle against libmtlc...
%CC% obj\lexer\lexer.o obj\parser\ast.o obj\parser\parser.o obj\semantic\symbol_table.o obj\semantic\type_checker.o obj\semantic\type_checker_types.o obj\semantic\type_checker_errors.o obj\semantic\type_checker_safety.o obj\semantic\type_checker_init_tracker.o obj\semantic\type_checker_decl.o obj\semantic\type_checker_match.o obj\semantic\type_checker_stmt.o obj\semantic\type_checker_expr.o obj\semantic\type_checker_aggregate.o obj\semantic\type_checker_tensor_epilogue.o obj\semantic\type_checker_memory.o obj\semantic\register_allocator.o obj\semantic\import_resolver.o obj\semantic\monomorphize.o obj\ir\ir_lowering.o obj\ir\ir_lower_address.o obj\ir\ir_lower_defer.o obj\ir\ir_lower_expr.o obj\ir\ir_lower_stmt.o obj\ir\ir_lower_support.o obj\ir\ir_lower_switch_match.o obj\ir\ir_lower_types.o obj\frontend\mtlc_type_from_frontend.o obj\frontend\mtlc_lower_module.o obj\error\error_explain.o obj\runtime\crash_handler.o obj\tracy_build.o obj\mettle_alloc.o obj\main.o "%LIBMTLC_LIB%" -static -o bin\mettle.exe %LDFLAGS%

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
