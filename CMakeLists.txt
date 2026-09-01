cmake_minimum_required(VERSION 3.16)

project(mettle C)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Release Debug)

set(CMAKE_C_FLAGS "")

set(CMAKE_C_FLAGS_DEBUG "")

set(CMAKE_C_FLAGS_RELEASE "")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(ProcessorCount)

ProcessorCount(NCORES)

if(NCORES EQUAL 0)
  set(NCORES 4)
endif()

if(WIN32 AND NOT MINGW AND NOT CYGWIN)
  message(FATAL_ERROR "Native MSVC cannot link this freestanding image. Use MinGW-w64, MSYS2, or WSL.")
endif()

if(NOT DEFINED PYTHON)
  set(PYTHON python3 CACHE STRING "")
endif()

set(EXTRA_CFLAGS "" CACHE STRING "")

option(DGX_SPARK "DGX Spark profile" OFF)

set(DGX_SPARK_CFLAGS "-march=armv9.2-a" CACHE STRING "")

find_program(AR_EXECUTABLE NAMES $ENV{AR} ar REQUIRED)

find_program(LD_EXECUTABLE NAMES $ENV{LD} ld REQUIRED)

find_program(NM_EXECUTABLE NAMES $ENV{NM} nm REQUIRED)

find_program(READELF_EXECUTABLE NAMES readelf llvm-readelf)

find_program(STRIP_EXECUTABLE NAMES strip llvm-strip)

find_program(PWSH_EXECUTABLE NAMES pwsh pwsh.exe powershell)

set(SRCDIR ${CMAKE_SOURCE_DIR}/src)

set(OBJDIR ${CMAKE_BINARY_DIR}/obj)

set(BINDIR ${CMAKE_BINARY_DIR}/bin)

set(STDLIBDIR ${CMAKE_SOURCE_DIR}/stdlib)

set(RUNTIMEDIR ${CMAKE_SOURCE_DIR}/src/runtime)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "/usr/local" CACHE PATH "" FORCE)
endif()

set(LIBMTLC_VERSION 0.2.0)

set(LIBMTLC ${BINDIR}/libmtlc${CMAKE_STATIC_LIBRARY_SUFFIX})

set(TARGET ${BINDIR}/mettle${CMAKE_EXECUTABLE_SUFFIX})

set(ARCH_CFLAGS "")

execute_process(COMMAND ${CMAKE_C_COMPILER} -dumpmachine
                OUTPUT_VARIABLE _triple OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

if(_triple MATCHES "aarch64|arm64")
  execute_process(COMMAND ${CMAKE_C_COMPILER} -mno-outline-atomics -E -x c /dev/null
                  RESULT_VARIABLE _ok OUTPUT_QUIET ERROR_QUIET)
  if(_ok EQUAL 0)
    set(ARCH_CFLAGS -mno-outline-atomics)
  endif()

endif()

set(COMMON_CFLAGS -std=c99 -D_GNU_SOURCE ${ARCH_CFLAGS})

set(HOST_BACKEND_CFLAGS
  -ffreestanding -fno-builtin -fno-stack-protector
  -fno-asynchronous-unwind-tables -fno-unwind-tables
  -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
  -include ${RUNTIMEDIR}/host_redirect.h)


set(FREESTANDING_CFLAGS
  -std=c99 -O2 -ffreestanding -fno-builtin -fno-ident
  -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables
  -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
  -ffunction-sections -fdata-sections -fno-jump-tables ${ARCH_CFLAGS})

set(RUNTIME_OBJ_CFLAGS ${FREESTANDING_CFLAGS} -D_GNU_SOURCE -I${SRCDIR})

set(MTLC_CFLAGS ${COMMON_CFLAGS})

set(LD_EXTRA "")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  list(APPEND MTLC_CFLAGS -Wall -Wextra -g3 -O0 -DDEBUG -fno-omit-frame-pointer)
else()

  list(APPEND MTLC_CFLAGS -Oz -DNDEBUG -fomit-frame-pointer
                          -ffunction-sections -fdata-sections
                          -fno-asynchronous-unwind-tables -fno-unwind-tables
                          -fno-ident)
  set(LD_EXTRA -s)
endif()

if(EXTRA_CFLAGS)
  separate_arguments(_extra NATIVE_COMMAND "${EXTRA_CFLAGS}")
  list(APPEND MTLC_CFLAGS ${_extra})
endif()

if(DGX_SPARK)
  separate_arguments(_dgx NATIVE_COMMAND "${DGX_SPARK_CFLAGS}")
  list(APPEND MTLC_CFLAGS ${_dgx})
endif()

file(GLOB LEXER_SOURCES CONFIGURE_DEPENDS ${SRCDIR}/lexer/*.c)

file(GLOB PARSER_SOURCES CONFIGURE_DEPENDS ${SRCDIR}/parser/*.c)

file(GLOB SEMANTIC_SOURCES CONFIGURE_DEPENDS ${SRCDIR}/semantic/*.c)

file(GLOB _IR_C CONFIGURE_DEPENDS ${SRCDIR}/ir/*.c)

file(GLOB _IR_OPT_C CONFIGURE_DEPENDS ${SRCDIR}/ir/optimizer/*.c)

file(GLOB _CG_ASM CONFIGURE_DEPENDS ${SRCDIR}/codegen/asm/*.c)

file(GLOB _CG_BIN CONFIGURE_DEPENDS ${SRCDIR}/codegen/binary/*.c)

file(GLOB LINKER_SOURCES CONFIGURE_DEPENDS ${SRCDIR}/linker/*.c)

set(LOWERING_SOURCES
  ${SRCDIR}/ir/ir_lowering.c ${SRCDIR}/ir/ir_lower_address.c
  ${SRCDIR}/ir/ir_lower_defer.c ${SRCDIR}/ir/ir_lower_expr.c
  ${SRCDIR}/ir/ir_lower_stmt.c ${SRCDIR}/ir/ir_lower_support.c
  ${SRCDIR}/ir/ir_lower_switch_match.c ${SRCDIR}/ir/ir_lower_types.c)

set(IR_CORE_SOURCES ${_IR_C} ${_IR_OPT_C})

list(REMOVE_ITEM IR_CORE_SOURCES ${LOWERING_SOURCES})

set(CODEGEN_SOURCES
  ${SRCDIR}/codegen/binary_emitter.c ${SRCDIR}/codegen/code_generator.c
  ${SRCDIR}/codegen/elf_emitter.c ${SRCDIR}/codegen/gpu_detect.c
  ${SRCDIR}/codegen/ptx_emitter.c ${SRCDIR}/codegen/spirv_emitter.c
  ${SRCDIR}/codegen/target.c ${SRCDIR}/codegen/flat_emitter.c
  ${_CG_ASM} ${_CG_BIN})

set(BACKEND_SOURCES
  ${SRCDIR}/common.c ${IR_CORE_SOURCES} ${CODEGEN_SOURCES} ${LINKER_SOURCES}
  ${SRCDIR}/debug/debug_info.c
  ${SRCDIR}/error/error_reporter.c ${SRCDIR}/error/diag_style.c
  ${SRCDIR}/compiler/compiler_context.c ${SRCDIR}/compiler/compiler_crash.c
  ${SRCDIR}/compiler/compiler_self_profile.c
  ${SRCDIR}/mtlc_api.c ${SRCDIR}/mtlc_build.c
  ${SRCDIR}/mtlc_lib_fallbacks.c ${SRCDIR}/mtlc_crash_fallback.c
  ${RUNTIMEDIR}/verify_owned.c)

set(FRONTEND_SOURCES
  ${LEXER_SOURCES} ${PARSER_SOURCES} ${SEMANTIC_SOURCES} ${LOWERING_SOURCES}
  ${SRCDIR}/frontend/mtlc_type_from_frontend.c
  ${SRCDIR}/frontend/mtlc_lower_module.c
  ${SRCDIR}/error/error_explain.c ${SRCDIR}/main.c ${SRCDIR}/tracy_build.c)

function(mtlc_objects name)
  add_library(${name} OBJECT ${ARGN})
  target_include_directories(${name} PRIVATE ${SRCDIR} ${CMAKE_SOURCE_DIR}/include)
  target_compile_options(${name} PRIVATE ${MTLC_CFLAGS} ${HOST_BACKEND_CFLAGS})
  set_target_properties(${name} PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE OFF
    ARCHIVE_OUTPUT_DIRECTORY ${OBJDIR}
    COMPILE_PDB_OUTPUT_DIRECTORY ${OBJDIR})
endfunction()

mtlc_objects(mtlc_backend ${BACKEND_SOURCES})

mtlc_objects(mtlc_frontend ${FRONTEND_SOURCES})

add_library(host_runtime OBJECT ${RUNTIMEDIR}/freestanding.c)

target_compile_options(host_runtime PRIVATE ${FREESTANDING_CFLAGS} -include ${RUNTIMEDIR}/host_prefix.h)

add_library(host_startup OBJECT ${RUNTIMEDIR}/host_startup.c)

target_compile_options(host_startup PRIVATE ${FREESTANDING_CFLAGS})

file(MAKE_DIRECTORY ${BINDIR} ${OBJDIR}/runtime)

add_custom_command(OUTPUT ${LIBMTLC}
  COMMAND ${CMAKE_COMMAND} -E rm -f ${LIBMTLC}
  COMMAND ${AR_EXECUTABLE} rcs ${LIBMTLC}
          $<TARGET_OBJECTS:mtlc_backend> $<TARGET_OBJECTS:host_runtime>
  COMMAND ${LD_EXECUTABLE} -r --whole-archive ${LIBMTLC} --no-whole-archive
          -o ${OBJDIR}/runtime/libmtlc-closure.o
  COMMAND ${CMAKE_COMMAND}
          -DNM=${NM_EXECUTABLE}
          -DOBJ=${OBJDIR}/runtime/libmtlc-closure.o
          -DLABEL=${LIBMTLC}
          -P ${CMAKE_BINARY_DIR}/_check_undef.cmake
  DEPENDS mtlc_backend host_runtime
  COMMAND_EXPAND_LISTS
  VERBATIM)

add_custom_target(libmtlc ALL DEPENDS ${LIBMTLC})

set(LD_IMAGE ${LD_EXECUTABLE} -static --gc-sections -e _start ${LD_EXTRA})

if(NOT WIN32)
  list(INSERT LD_IMAGE 2 -z noexecstack)
endif()

add_custom_command(OUTPUT ${TARGET}
  COMMAND ${LD_IMAGE} -o ${TARGET}
          $<TARGET_OBJECTS:host_startup> $<TARGET_OBJECTS:mtlc_frontend>
          --start-group ${LIBMTLC} --end-group
  COMMAND ${CMAKE_COMMAND}
          -DREADELF=${READELF_EXECUTABLE}
          -DNM=${NM_EXECUTABLE}
          -DSTRIP=$<IF:$<AND:$<NOT:$<STREQUAL:${CMAKE_BUILD_TYPE},Debug>>,$<BOOL:${STRIP_EXECUTABLE}>>,${STRIP_EXECUTABLE},>
          -DBIN=${TARGET}
          -P ${CMAKE_BINARY_DIR}/_check_image.cmake
  DEPENDS host_startup mtlc_frontend ${LIBMTLC}
  COMMAND_EXPAND_LISTS
  VERBATIM)

add_custom_target(mettle ALL DEPENDS ${TARGET})

add_dependencies(mettle libmtlc)

file(WRITE ${CMAKE_BINARY_DIR}/_check_undef.cmake [=[
execute_process(COMMAND ${NM} -u ${OBJ} OUTPUT_VARIABLE u ERROR_QUIET)
string(REGEX REPLACE "[^\n]*_GLOBAL_OFFSET_TABLE_[^\n]*\n?" "" u "${u}")
string(STRIP "${u}" u)
if(u)
  execute_process(COMMAND ${NM} -u ${OBJ})
  message(FATAL_ERROR "error: ${LABEL} has unresolved host symbols")
endif()
]=])

file(WRITE ${CMAKE_BINARY_DIR}/_check_image.cmake [=[
if(READELF)
  execute_process(COMMAND ${READELF} -l ${BIN} OUTPUT_VARIABLE ph ERROR_VARIABLE e)
  if("${ph}${e}" MATCHES "INTERP")
    message(FATAL_ERROR "error: ${BIN} contains a dynamic loader")
  endif()
  execute_process(COMMAND ${READELF} -d ${BIN} OUTPUT_VARIABLE dy ERROR_VARIABLE de)
  if("${dy}${de}" MATCHES "NEEDED")
    message(FATAL_ERROR "error: ${BIN} contains a shared library dependency")
  endif()
endif()
execute_process(COMMAND ${NM} -u ${BIN} OUTPUT_VARIABLE u ERROR_QUIET)
string(STRIP "${u}" u)
if(u)
  execute_process(COMMAND ${NM} -u ${BIN})
  message(FATAL_ERROR "error: ${BIN} contains unresolved symbols")
endif()
if(STRIP)
  execute_process(COMMAND ${STRIP} --strip-all --remove-section=.comment ${BIN} RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    execute_process(COMMAND ${STRIP} --strip-all ${BIN})
  endif()
endif()
]=])

file(WRITE ${CMAKE_BINARY_DIR}/_expect1.cmake [=[
execute_process(COMMAND ${CMD} OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE rc)
if(NOT rc EQUAL 1)
  message(FATAL_ERROR "${CMD} exited ${rc}, expected 1")
endif()
]=])

file(WRITE ${CMAKE_BINARY_DIR}/_check_products.cmake [=[
foreach(p ${PRODUCTS})
  if(READELF)
    execute_process(COMMAND ${READELF} -l ${p} OUTPUT_VARIABLE ph ERROR_VARIABLE e)
    if("${ph}${e}" MATCHES "INTERP")
      message(FATAL_ERROR "error: ${p} has a dynamic loader")
    endif()
    execute_process(COMMAND ${READELF} -d ${p} OUTPUT_VARIABLE dy ERROR_VARIABLE de)
    if("${dy}${de}" MATCHES "NEEDED")
      message(FATAL_ERROR "error: ${p} has a shared dependency")
    endif()
  endif()
  execute_process(COMMAND ${NM} -u ${p} OUTPUT_VARIABLE u ERROR_QUIET)
  string(STRIP "${u}" u)
  if(u)
    message(FATAL_ERROR "error: ${p} has unresolved symbols")
  endif()
endforeach()
]=])

add_custom_target(bundle-stdlib ALL
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${BINDIR}/stdlib
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${STDLIBDIR} ${BINDIR}/stdlib)

add_custom_target(bundle-runtime ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${OBJDIR}/runtime ${BINDIR}
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${BINDIR}/runtime
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${RUNTIMEDIR} ${BINDIR}/runtime
  COMMAND ${CMAKE_C_COMPILER} ${FREESTANDING_CFLAGS} -Os -c ${RUNTIMEDIR}/freestanding.c -o ${OBJDIR}/runtime/freestanding.o
  COMMAND ${CMAKE_COMMAND} -E copy ${OBJDIR}/runtime/freestanding.o ${BINDIR}/runtime/freestanding.o
  COMMAND ${CMAKE_C_COMPILER} ${FREESTANDING_CFLAGS} -Os -DMT_SHARED_RUNTIME -c ${RUNTIMEDIR}/freestanding.c -o ${OBJDIR}/runtime/freestanding_shared.o
  COMMAND ${CMAKE_COMMAND} -E copy ${OBJDIR}/runtime/freestanding_shared.o ${BINDIR}/runtime/freestanding_shared.o
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_OBJECTS:host_startup> ${BINDIR}/runtime/host_startup.o
  COMMAND ${CMAKE_C_COMPILER} ${RUNTIME_OBJ_CFLAGS} -c ${STDLIBDIR}/tracy_helpers.c -o ${OBJDIR}/runtime/tracy_helpers.o
  COMMAND ${CMAKE_COMMAND} -E copy ${OBJDIR}/runtime/tracy_helpers.o ${BINDIR}/runtime/tracy_helpers.o
  COMMAND ${CMAKE_COMMAND} -E copy ${OBJDIR}/runtime/tracy_helpers.o ${BINDIR}/runtime/tracy_helpers.obj
  COMMAND ${CMAKE_C_COMPILER} ${RUNTIME_OBJ_CFLAGS} -DMETTLE_ATOMICS_IN_FREESTANDING -c ${RUNTIMEDIR}/atomics.c -o ${OBJDIR}/runtime/atomics.o
  COMMAND ${CMAKE_C_COMPILER} ${RUNTIME_OBJ_CFLAGS} -c ${RUNTIMEDIR}/crash_handler.c -o ${OBJDIR}/runtime/crash_handler.o
  COMMAND ${CMAKE_C_COMPILER} ${RUNTIME_OBJ_CFLAGS} -c ${RUNTIMEDIR}/safety.c -o ${OBJDIR}/runtime/safety.o
  COMMAND ${CMAKE_C_COMPILER} ${RUNTIME_OBJ_CFLAGS} -DMT_SHARED_RUNTIME -c ${RUNTIMEDIR}/safety.c -o ${OBJDIR}/runtime/safety_shared.o
  COMMAND ${CMAKE_COMMAND} -E copy ${OBJDIR}/runtime/safety_shared.o ${BINDIR}/runtime/safety_shared.o
  COMMAND ${CMAKE_C_COMPILER} ${RUNTIME_OBJ_CFLAGS} -c ${RUNTIMEDIR}/debug.c -o ${OBJDIR}/runtime/debug.o
  COMMAND ${CMAKE_C_COMPILER} ${RUNTIME_OBJ_CFLAGS} -c ${RUNTIMEDIR}/profile.c -o ${OBJDIR}/runtime/profile.o
  COMMAND ${CMAKE_COMMAND} -E copy ${OBJDIR}/runtime/atomics.o ${BINDIR}/runtime/atomics.o
  COMMAND ${CMAKE_COMMAND} -E copy ${OBJDIR}/runtime/crash_handler.o ${BINDIR}/runtime/crash_handler.o
  COMMAND ${CMAKE_COMMAND} -E copy ${OBJDIR}/runtime/safety.o ${BINDIR}/runtime/safety.o
  COMMAND ${CMAKE_COMMAND} -E copy ${OBJDIR}/runtime/debug.o ${BINDIR}/runtime/debug.o
  COMMAND ${TARGET} --release --emit-obj ${RUNTIMEDIR}/swap.mettle -o ${BINDIR}/runtime/swap.o
  COMMAND ${TARGET} --release --emit-obj ${RUNTIMEDIR}/string.mettle -o ${BINDIR}/runtime/string.o
  COMMAND ${CMAKE_COMMAND} -E copy ${OBJDIR}/runtime/profile.o ${BINDIR}/runtime/profile.o
  DEPENDS ${TARGET} host_startup
  VERBATIM)

add_dependencies(bundle-runtime mettle)

add_custom_target(test
  COMMAND ${CMAKE_COMMAND} -E echo "Running owned runtime tests..."
  COMMAND ${TARGET} --build --native-heap ${CMAKE_SOURCE_DIR}/tests/test_native_heap_threads.mettle -o ${BINDIR}/owned-threads-test${CMAKE_EXECUTABLE_SUFFIX}
  COMMAND ${BINDIR}/owned-threads-test${CMAKE_EXECUTABLE_SUFFIX}
  COMMAND ${TARGET} --build ${CMAKE_SOURCE_DIR}/tests/test_thread_posix_owned.mettle -o ${BINDIR}/owned-pthread-test${CMAKE_EXECUTABLE_SUFFIX}
  COMMAND ${BINDIR}/owned-pthread-test${CMAKE_EXECUTABLE_SUFFIX}
  COMMAND ${TARGET} --build ${CMAKE_SOURCE_DIR}/tests/test_owned_dir.mettle -o ${BINDIR}/owned-dir-test${CMAKE_EXECUTABLE_SUFFIX}
  COMMAND ${BINDIR}/owned-dir-test${CMAKE_EXECUTABLE_SUFFIX}
  COMMAND ${TARGET} --build -s ${CMAKE_SOURCE_DIR}/tests/test_runtime_null_deref_check.mettle -o ${BINDIR}/owned-crash-test${CMAKE_EXECUTABLE_SUFFIX}
  COMMAND ${CMAKE_COMMAND} -DCMD=${BINDIR}/owned-crash-test${CMAKE_EXECUTABLE_SUFFIX} -P ${CMAKE_BINARY_DIR}/_expect1.cmake
  COMMAND ${CMAKE_COMMAND}
          -DREADELF=${READELF_EXECUTABLE} -DNM=${NM_EXECUTABLE}
          "-DPRODUCTS=${TARGET};${BINDIR}/owned-threads-test${CMAKE_EXECUTABLE_SUFFIX};${BINDIR}/owned-pthread-test${CMAKE_EXECUTABLE_SUFFIX};${BINDIR}/owned-dir-test${CMAKE_EXECUTABLE_SUFFIX};${BINDIR}/owned-crash-test${CMAKE_EXECUTABLE_SUFFIX}"
          -P ${CMAKE_BINARY_DIR}/_check_products.cmake
  DEPENDS mettle bundle-runtime
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} VERBATIM)

if(PWSH_EXECUTABLE)
  add_custom_target(check
    COMMAND ${PWSH_EXECUTABLE} -NoProfile -File ${CMAKE_SOURCE_DIR}/tests/run_tests.ps1 -CompilerPath ${TARGET}
    DEPENDS mettle bundle-stdlib bundle-runtime
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} VERBATIM)
else()
  add_custom_target(check
    COMMAND ${CMAKE_COMMAND} -E echo "error: pwsh is required for the test suite"
    COMMAND ${CMAKE_COMMAND} -E echo "  install PowerShell Core: https://aka.ms/powershell"
    COMMAND ${CMAKE_COMMAND} -E false)
endif()

add_custom_target(complexity
  COMMAND ${PYTHON} ${CMAKE_SOURCE_DIR}/tools/ci/complexity_audit.py --self-test
  COMMAND ${PYTHON} ${CMAKE_SOURCE_DIR}/tools/ci/complexity_audit.py
  COMMAND ${PYTHON} ${CMAKE_SOURCE_DIR}/tools/ci/complexity_audit.py --check
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} VERBATIM)

install(CODE "
  set(_root \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}\")
  file(MAKE_DIRECTORY \"\${_root}/bin\" \"\${_root}/stdlib\" \"\${_root}/runtime\")
  file(COPY \"${TARGET}\" DESTINATION \"\${_root}/bin\")
  file(COPY \"${BINDIR}/stdlib/\" DESTINATION \"\${_root}/stdlib\")
  file(COPY \"${BINDIR}/runtime/\" DESTINATION \"\${_root}/runtime\")
")

add_custom_target(install-libmtlc
  COMMAND ${CMAKE_COMMAND}
          -DPREFIX=${CMAKE_INSTALL_PREFIX}
          -DLIBMTLC_VERSION=${LIBMTLC_VERSION}
          -DLIBMTLC=${LIBMTLC}
          -DHOST_STARTUP=$<TARGET_OBJECTS:host_startup>
          -DFREESTANDING=${BINDIR}/runtime/freestanding.o
          -DRUNTIMEDIR=${RUNTIMEDIR}
          -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
          -P ${CMAKE_BINARY_DIR}/_install_libmtlc.cmake
  DEPENDS libmtlc bundle-runtime VERBATIM)
file(WRITE ${CMAKE_BINARY_DIR}/_install_libmtlc.cmake [=[
set(root "$ENV{DESTDIR}${PREFIX}")
file(MAKE_DIRECTORY ${root}/include/mtlc ${root}/lib/pkgconfig ${root}/lib/libmtlc/runtime)
file(GLOB hdrs "${SOURCE_DIR}/include/mtlc/*.h")
file(COPY ${hdrs} DESTINATION ${root}/include/mtlc)
file(COPY ${RUNTIMEDIR}/host_redirect.h ${RUNTIMEDIR}/host_prefix.h DESTINATION ${root}/include/mtlc)
file(COPY ${LIBMTLC} DESTINATION ${root}/lib)
file(COPY ${FREESTANDING} DESTINATION ${root}/lib/libmtlc/runtime)
file(COPY ${HOST_STARTUP} DESTINATION ${root}/lib/libmtlc/runtime)
file(WRITE ${root}/lib/pkgconfig/libmtlc.pc
"prefix=${PREFIX}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libmtlc
Description: Freestanding native compiler backend
Version: ${LIBMTLC_VERSION}
Libs: -nostdlib -nostartfiles -nodefaultlibs -no-pie -static -Wl,-e,_start,--gc-sections \${libdir}/libmtlc/runtime/host_startup.o -L\${libdir} -lmtlc
Cflags: -I\${includedir} -include mtlc/host_redirect.h -ffreestanding -fno-builtin -fno-stack-protector
")
message(STATUS "installed libmtlc ${LIBMTLC_VERSION} to ${root} (include/mtlc, libmtlc.a, libmtlc.pc)")
]=])

add_custom_target(dist-libmtlc
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_SOURCE_DIR}/dist/libmtlc
  COMMAND ${CMAKE_COMMAND} -E make_directory
          ${CMAKE_SOURCE_DIR}/dist/libmtlc/include/mtlc
          ${CMAKE_SOURCE_DIR}/dist/libmtlc/lib
          ${CMAKE_SOURCE_DIR}/dist/libmtlc/runtime
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/include/mtlc
          ${CMAKE_SOURCE_DIR}/dist/libmtlc/include/mtlc
  COMMAND ${CMAKE_COMMAND} -E copy ${LIBMTLC} ${CMAKE_SOURCE_DIR}/dist/libmtlc/lib/
  COMMAND ${CMAKE_COMMAND} -E copy ${BINDIR}/runtime/freestanding.o
          ${CMAKE_SOURCE_DIR}/dist/libmtlc/runtime/
  COMMAND ${CMAKE_COMMAND} -E echo "staged dist/libmtlc (link with: cc -Idist/libmtlc/include app.c dist/libmtlc/lib/libmtlc.a)"
  DEPENDS libmtlc bundle-runtime)

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}  jobs: ${NCORES}  generator: ${CMAKE_GENERATOR}")

message(STATUS "Build with: cmake --build ${CMAKE_BINARY_DIR} --parallel ${NCORES}")
