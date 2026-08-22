/* public_api_test.c - proves the FULL public libmtlc surface end to end, using
 * only <mtlc/...> headers + the static library (no internal headers, no Mettle
 * frontend). Driven by tests/run_tests.ps1 (the `public_api` gate).
 *
 *   public_api_test <outdir>
 *
 * Builds six module families through mtlc/build.h and emits through
 * mtlc/pipeline.h:
 *   1. native:  globals (read+write), owned malloc + putchar, pointer
 *      load/store, address-of local, float
 *      arithmetic + cast, optimizer on -> <outdir>/pubapi_native.exe
 *      (the harness runs it: expects exit code 42 and stdout "OK")
 *   2. gpu:     a float32* kernel -> PTX text (checked: ".entry") and a SPIR-V
 *      binary (checked: magic 0x07230203)
 *   3. arm64:   globals + an 11-argument external call -> relocatable AArch64
 *      ELF (checked: machine/type and AAELF64 address/call relocations)
 *   4. launch:  frontend-neutral semantic GPU launch -> native object with the
 *      checked runtime-provider ABI (never executed; no GPU is required)
 *   5. tensor:  frontend-neutral cooperative matrix descriptors spanning the
 *      stable PTX WMMA shape/type family -> GB10 PTX
 *   6. transfer: frontend-neutral multidimensional workgroup transfers ->
 *      portable cooperative replay and native GB10 TMA PTX
 *   7. ergonomics: the convenience surface -- composite type constructors,
 *      element/field addressing, typed operator enums, allocated labels,
 *      builder and context diagnostics -> <outdir>/pubapi_ergonomics.exe
 *      (the harness runs it: expects exit code 77)
 *
 * Exit code 0 = everything emitted and structurally verified. */
#include <mtlc/build.h>
#include <mtlc/mtlc.h>
#include <mtlc/pipeline.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *what) {
  fprintf(stderr, "public_api_test: FAIL: %s\n", what);
  return 1;
}

static char *path_join(const char *dir, const char *name) {
  size_t n = strlen(dir) + strlen(name) + 2;
  char *p = malloc(n);
  snprintf(p, n, "%s/%s", dir, name);
  return p;
}

static uint16_t read_le16(const unsigned char *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t read_le32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t read_le64(const unsigned char *p) {
  return (uint64_t)read_le32(p) | ((uint64_t)read_le32(p + 4) << 32);
}

static int is_arm64_relocatable_with_native_relocs(const char *path,
                                                    int require_address) {
  FILE *file = fopen(path, "rb");
  unsigned char *bytes = NULL;
  long length = 0;
  int page = 0, lo12 = 0, call = 0;
  if (!file || fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 64 ||
      fseek(file, 0, SEEK_SET) != 0) {
    if (file) fclose(file);
    return 0;
  }
  bytes = malloc((size_t)length);
  if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
    free(bytes);
    fclose(file);
    return 0;
  }
  fclose(file);
  if (memcmp(bytes, "\x7f" "ELF", 4) != 0 || bytes[4] != 2 ||
      bytes[5] != 1 || read_le16(bytes + 16) != 1 ||
      read_le16(bytes + 18) != 183) {
    free(bytes);
    return 0;
  }
  uint64_t shoff = read_le64(bytes + 40);
  uint16_t shentsize = read_le16(bytes + 58);
  uint16_t shnum = read_le16(bytes + 60);
  if (shentsize < 64 || shoff + (uint64_t)shentsize * shnum >
                             (uint64_t)length) {
    free(bytes);
    return 0;
  }
  for (uint16_t i = 0; i < shnum; i++) {
    const unsigned char *section = bytes + shoff + (uint64_t)i * shentsize;
    if (read_le32(section + 4) != 4) continue; /* SHT_RELA */
    uint64_t offset = read_le64(section + 24);
    uint64_t size = read_le64(section + 32);
    uint64_t entsize = read_le64(section + 56);
    if (entsize < 24 || offset + size > (uint64_t)length) continue;
    for (uint64_t at = offset; at + 24 <= offset + size; at += entsize) {
      uint32_t type = (uint32_t)read_le64(bytes + at + 8);
      page |= type == 275; /* R_AARCH64_ADR_PREL_PG_HI21 */
      lo12 |= type == 277; /* R_AARCH64_ADD_ABS_LO12_NC */
      call |= type == 283; /* R_AARCH64_CALL26 */
    }
  }
  free(bytes);
  return call && (!require_address || (page && lo12));
}

/* Inspect enough SPIR-V to prove the atomic's semantic operands, even on
 * developer machines without spirv-dis/spirv-val. OpConstant = 43 and
 * OpAtomicIAdd = 234 in SPIR-V 1.0. */
static int spirv_has_atomic_contract(const char *path, uint32_t expected_scope,
                                     uint32_t expected_semantics) {
  FILE *f = fopen(path, "rb");
  uint32_t *words = NULL;
  uint32_t *constants = NULL;
  long bytes;
  size_t count;
  int found = 0;
  if (!f || fseek(f, 0, SEEK_END) != 0 || (bytes = ftell(f)) < 20 ||
      fseek(f, 0, SEEK_SET) != 0 || (bytes % 4) != 0) {
    if (f) fclose(f);
    return 0;
  }
  count = (size_t)bytes / 4;
  words = malloc(count * sizeof(*words));
  if (!words || fread(words, sizeof(*words), count, f) != count ||
      words[0] != 0x07230203u || words[3] == 0) {
    free(words);
    fclose(f);
    return 0;
  }
  fclose(f);
  constants = calloc(words[3], sizeof(*constants));
  if (!constants) {
    free(words);
    return 0;
  }
  for (size_t i = 5; i < count;) {
    uint32_t wc = words[i] >> 16, op = words[i] & 0xffffu;
    if (wc == 0 || i + wc > count) goto done;
    if (op == 43 && wc == 4 && words[i + 2] < words[3]) {
      constants[words[i + 2]] = words[i + 3];
    }
    i += wc;
  }
  for (size_t i = 5; i < count;) {
    uint32_t wc = words[i] >> 16, op = words[i] & 0xffffu;
    if (wc == 0 || i + wc > count) goto done;
    if (op == 234 && wc == 7 && words[i + 4] < words[3] &&
        words[i + 5] < words[3] &&
        constants[words[i + 4]] == expected_scope &&
        constants[words[i + 5]] == expected_semantics) {
      found = 1;
      break;
    }
    i += wc;
  }
done:
  free(constants);
  free(words);
  return found;
}

static int spirv_has_compare_exchange_contract(
    const char *path, uint32_t expected_scope, uint32_t expected_success,
    uint32_t expected_failure) {
  FILE *f = fopen(path, "rb");
  uint32_t *words = NULL, *constants = NULL;
  long bytes;
  size_t count;
  int found = 0;
  if (!f || fseek(f, 0, SEEK_END) != 0 || (bytes = ftell(f)) < 20 ||
      fseek(f, 0, SEEK_SET) != 0 || (bytes % 4) != 0) {
    if (f) fclose(f);
    return 0;
  }
  count = (size_t)bytes / 4;
  words = malloc(count * sizeof(*words));
  if (!words || fread(words, sizeof(*words), count, f) != count ||
      words[0] != 0x07230203u || words[3] == 0) {
    free(words);
    fclose(f);
    return 0;
  }
  fclose(f);
  constants = calloc(words[3], sizeof(*constants));
  if (!constants) {
    free(words);
    return 0;
  }
  for (size_t i = 5; i < count;) {
    uint32_t wc = words[i] >> 16, op = words[i] & 0xffffu;
    if (!wc || i + wc > count) goto done;
    if (op == 43 && wc == 4 && words[i + 2] < words[3])
      constants[words[i + 2]] = words[i + 3];
    i += wc;
  }
  for (size_t i = 5; i < count;) {
    uint32_t wc = words[i] >> 16, op = words[i] & 0xffffu;
    if (!wc || i + wc > count) goto done;
    if (op == 230 && wc == 9 && words[i + 4] < words[3] &&
        words[i + 5] < words[3] && words[i + 6] < words[3] &&
        constants[words[i + 4]] == expected_scope &&
        constants[words[i + 5]] == expected_success &&
        constants[words[i + 6]] == expected_failure) {
      found = 1;
      break;
    }
    i += wc;
  }
done:
  free(constants);
  free(words);
  return found;
}

static int spirv_has_barrier_contract(const char *path,
                                      uint32_t expected_semantics) {
  FILE *f = fopen(path, "rb");
  uint32_t *words = NULL, *constants = NULL;
  long bytes;
  size_t count;
  int found = 0;
  if (!f || fseek(f, 0, SEEK_END) != 0 || (bytes = ftell(f)) < 20 ||
      fseek(f, 0, SEEK_SET) != 0 || (bytes % 4) != 0) {
    if (f) fclose(f);
    return 0;
  }
  count = (size_t)bytes / 4;
  words = malloc(count * sizeof(*words));
  if (!words || fread(words, sizeof(*words), count, f) != count ||
      words[0] != 0x07230203u || words[3] == 0) {
    free(words);
    fclose(f);
    return 0;
  }
  fclose(f);
  constants = calloc(words[3], sizeof(*constants));
  if (!constants) goto done;
  for (size_t i = 5; i < count;) {
    uint32_t wc = words[i] >> 16, op = words[i] & 0xffffu;
    if (wc == 0 || i + wc > count) goto done;
    if (op == 43u && wc == 4u && words[i + 2] < words[3])
      constants[words[i + 2]] = words[i + 3];
    i += wc;
  }
  for (size_t i = 5; i < count;) {
    uint32_t wc = words[i] >> 16, op = words[i] & 0xffffu;
    if (wc == 0 || i + wc > count) goto done;
    if (op == 224u && wc == 4u && words[i + 1] < words[3] &&
        words[i + 2] < words[3] && words[i + 3] < words[3] &&
        constants[words[i + 1]] == 2u && constants[words[i + 2]] == 2u &&
        constants[words[i + 3]] == expected_semantics) {
      found = 1;
      break;
    }
    i += wc;
  }
done:
  free(constants);
  free(words);
  return found;
}

static size_t spirv_count_opcode(const char *path, uint32_t expected_opcode) {
  FILE *f = fopen(path, "rb");
  uint32_t *words = NULL;
  long bytes;
  size_t count, found = 0;
  if (!f || fseek(f, 0, SEEK_END) != 0 || (bytes = ftell(f)) < 20 ||
      fseek(f, 0, SEEK_SET) != 0 || (bytes % 4) != 0) {
    if (f) fclose(f);
    return 0;
  }
  count = (size_t)bytes / 4;
  words = malloc(count * sizeof(*words));
  if (!words || fread(words, sizeof(*words), count, f) != count ||
      words[0] != 0x07230203u) {
    free(words);
    fclose(f);
    return 0;
  }
  fclose(f);
  for (size_t i = 5; i < count;) {
    uint32_t wc = words[i] >> 16, op = words[i] & 0xffffu;
    if (wc == 0 || i + wc > count) {
      found = 0;
      break;
    }
    if (op == expected_opcode) found++;
    i += wc;
  }
  free(words);
  return found;
}

static size_t spirv_count_group_operation(const char *path,
                                          uint32_t expected_opcode,
                                          uint32_t expected_operation) {
  FILE *f = fopen(path, "rb");
  uint32_t *words = NULL;
  long bytes;
  size_t count, found = 0;
  if (!f || fseek(f, 0, SEEK_END) != 0 || (bytes = ftell(f)) < 20 ||
      fseek(f, 0, SEEK_SET) != 0 || (bytes % 4) != 0) {
    if (f) fclose(f);
    return 0;
  }
  count = (size_t)bytes / 4;
  words = malloc(count * sizeof(*words));
  if (!words || fread(words, sizeof(*words), count, f) != count ||
      words[0] != 0x07230203u) {
    free(words);
    fclose(f);
    return 0;
  }
  fclose(f);
  for (size_t i = 5; i < count;) {
    uint32_t wc = words[i] >> 16, op = words[i] & 0xffffu;
    if (wc == 0 || i + wc > count) {
      found = 0;
      break;
    }
    if (op == expected_opcode && wc >= 6u &&
        words[i + 4] == expected_operation) {
      found++;
    }
    i += wc;
  }
  free(words);
  return found;
}

static int spirv_has_capability(const char *path,
                                uint32_t expected_capability) {
  FILE *f = fopen(path, "rb");
  uint32_t *words = NULL;
  long bytes;
  size_t count;
  int found = 0;
  if (!f || fseek(f, 0, SEEK_END) != 0 || (bytes = ftell(f)) < 20 ||
      fseek(f, 0, SEEK_SET) != 0 || (bytes % 4) != 0) {
    if (f) fclose(f);
    return 0;
  }
  count = (size_t)bytes / 4;
  words = malloc(count * sizeof(*words));
  if (!words || fread(words, sizeof(*words), count, f) != count ||
      words[0] != 0x07230203u) {
    free(words);
    fclose(f);
    return 0;
  }
  fclose(f);
  for (size_t i = 5; i < count;) {
    uint32_t wc = words[i] >> 16, op = words[i] & 0xffffu;
    if (wc == 0 || i + wc > count) break;
    if (op == 17u && wc == 2u && words[i + 1] == expected_capability) {
      found = 1;
      break;
    }
    i += wc;
  }
  free(words);
  return found;
}

/* Count OpVariables whose pointer pointee is an OpTypeArray and whose final
 * operand is the requested Storage Class. This excludes ordinary Function
 * shadow variables while proving neutral workgroup/private arrays survive. */
static size_t spirv_count_array_variable_storage(
    const char *path, uint32_t expected_storage) {
  FILE *f = fopen(path, "rb");
  uint32_t *words = NULL;
  uint32_t *pointer_pointee = NULL;
  unsigned char *array_types = NULL;
  long bytes;
  size_t count, found = 0;
  if (!f || fseek(f, 0, SEEK_END) != 0 || (bytes = ftell(f)) < 20 ||
      fseek(f, 0, SEEK_SET) != 0 || (bytes % 4) != 0) {
    if (f) fclose(f);
    return 0;
  }
  count = (size_t)bytes / 4;
  words = malloc(count * sizeof(*words));
  if (!words || fread(words, sizeof(*words), count, f) != count ||
      words[0] != 0x07230203u || words[3] == 0) {
    free(words);
    fclose(f);
    return 0;
  }
  fclose(f);
  pointer_pointee = calloc(words[3], sizeof(*pointer_pointee));
  array_types = calloc(words[3], sizeof(*array_types));
  if (!pointer_pointee || !array_types) {
    free(pointer_pointee);
    free(array_types);
    free(words);
    return 0;
  }
  for (size_t i = 5; i < count;) {
    uint32_t wc = words[i] >> 16, op = words[i] & 0xffffu;
    if (wc == 0 || i + wc > count) goto invalid;
    if (op == 28u && wc == 4u && words[i + 1] < words[3])
      array_types[words[i + 1]] = 1;
    if (op == 32u && wc == 4u && words[i + 1] < words[3])
      pointer_pointee[words[i + 1]] = words[i + 3];
    i += wc;
  }
  for (size_t i = 5; i < count;) {
    uint32_t wc = words[i] >> 16, op = words[i] & 0xffffu;
    if (wc == 0 || i + wc > count) {
      goto invalid;
    }
    if (op == 59u && wc == 4u && words[i + 1] < words[3] &&
        words[i + 3] == expected_storage &&
        pointer_pointee[words[i + 1]] < words[3] &&
        array_types[pointer_pointee[words[i + 1]]])
      found++;
    i += wc;
  }
  free(pointer_pointee);
  free(array_types);
  free(words);
  return found;
invalid:
  free(pointer_pointee);
  free(array_types);
  free(words);
  return 0;
}

/* OpTypePointer = 32 and OpFunctionParameter = 55. Dynamic workgroup memory
 * must cross the OpenCL kernel ABI as exactly one Workgroup pointer parameter,
 * not as a fixed OpVariable or a global pointer. */
static size_t spirv_count_pointer_parameters(const char *path,
                                             uint32_t expected_storage) {
  FILE *f = fopen(path, "rb");
  uint32_t *words = NULL, *pointer_storage = NULL;
  long bytes;
  size_t count, found = 0;
  if (!f || fseek(f, 0, SEEK_END) != 0 || (bytes = ftell(f)) < 20 ||
      fseek(f, 0, SEEK_SET) != 0 || (bytes % 4) != 0) {
    if (f) fclose(f);
    return 0;
  }
  count = (size_t)bytes / 4;
  words = malloc(count * sizeof(*words));
  if (!words || fread(words, sizeof(*words), count, f) != count ||
      words[0] != 0x07230203u || words[3] == 0) {
    free(words);
    fclose(f);
    return 0;
  }
  fclose(f);
  pointer_storage = calloc(words[3], sizeof(*pointer_storage));
  if (!pointer_storage) {
    free(words);
    return 0;
  }
  for (size_t i = 5; i < count;) {
    uint32_t wc = words[i] >> 16, op = words[i] & 0xffffu;
    if (wc == 0 || i + wc > count) goto done;
    if (op == 32u && wc == 4u && words[i + 1] < words[3])
      pointer_storage[words[i + 1]] = words[i + 2] + 1u;
    i += wc;
  }
  for (size_t i = 5; i < count;) {
    uint32_t wc = words[i] >> 16, op = words[i] & 0xffffu;
    if (wc == 0 || i + wc > count) {
      found = 0;
      break;
    }
    if (op == 55u && wc == 3u && words[i + 1] < words[3] &&
        pointer_storage[words[i + 1]] == expected_storage + 1u)
      found++;
    i += wc;
  }
done:
  free(pointer_storage);
  free(words);
  return found;
}

/* ---- module 1: the native program (exit 42, prints "OK") ---- */
static MtlcModule *build_native_module(void) {
  const MtlcType *i64 = mtlc_type_scalar(MTLC_TYPE_INT64);
  const MtlcType *i32 = mtlc_type_scalar(MTLC_TYPE_INT32);
  const MtlcType *f64 = mtlc_type_scalar(MTLC_TYPE_FLOAT64);
  const MtlcType *pi64 = mtlc_type_pointer(i64);
  MtlcBuilder *b = mtlc_builder_create();

  /* module global, read AND written below */
  mtlc_builder_global(b, "counter", i64, 40, /*extern=*/0);

  /* Runtime externs resolved by libmtlc's freestanding program object. */
  {
    const char *pn[] = {"size"};
    const MtlcType *pt[] = {i64};
    mtlc_builder_function(b, "malloc", pi64, pn, pt, 1, /*extern=*/1);
  }
  {
    const char *pn[] = {"c"};
    const MtlcType *pt[] = {i32};
    mtlc_builder_function(b, "putchar", i32, pn, pt, 1, /*extern=*/1);
  }

  /* The inliner must not alias this address-taken parameter to caller storage. */
  {
    const char *pn[] = {"value"};
    const MtlcType *pt[] = {i64};
    MtlcFn *touch =
        mtlc_builder_function(b, "touch_copy", i64, pn, pt, 1, 0);
    MtlcValue value = mtlc_fn_param(touch, 0);
    MtlcValue address = mtlc_address_of(touch, value, pi64);
    mtlc_store(touch, address, mtlc_const_int(touch, i64, 1), i64);
    mtlc_return(touch, mtlc_const_int(touch, i64, 0));
  }

  MtlcFn *m = mtlc_builder_function(b, "main", i64, NULL, NULL, 0, 0);

  /* heap memory through an extern + pointer store/load:
   *   p = malloc(8); *p = 41; x = *p + 1;              -> x = 42 */
  MtlcValue eight = mtlc_const_int(m, i64, 8);
  MtlcValue p = mtlc_call(m, "malloc", &eight, 1, pi64);
  MtlcValue c41 = mtlc_const_int(m, i64, 41);
  mtlc_store(m, p, c41, i64);
  MtlcValue loaded = mtlc_load(m, p, i64);
  MtlcValue x = mtlc_local(m, "x", i64);
  mtlc_assign(m, x, mtlc_binary(m, "+", loaded, mtlc_const_int(m, i64, 1), i64));

  /* global read: x += counter - 40                      (+0) */
  MtlcValue g = mtlc_global_ref(m, "counter");
  MtlcValue gdiff = mtlc_binary(m, "-", g, mtlc_const_int(m, i64, 40), i64);
  mtlc_assign(m, x, mtlc_binary(m, "+", x, gdiff, i64));

  /* float arithmetic + cast: (2.5 * 4.0) -> 10.0 -> 10; x += 10 - 10  (+0) */
  MtlcValue fprod = mtlc_binary(m, "*", mtlc_const_float(m, f64, 2.5),
                                mtlc_const_float(m, f64, 4.0), f64);
  MtlcValue fi = mtlc_cast(m, fprod, i64);
  MtlcValue fdiff = mtlc_binary(m, "-", fi, mtlc_const_int(m, i64, 10), i64);
  mtlc_assign(m, x, mtlc_binary(m, "+", x, fdiff, i64));

  /* touch_copy mutates only its by-value parameter; x must remain 42. */
  mtlc_call(m, "touch_copy", &x, 1, i64);

  /* address-of a local + store/load through the pointer: l=7; *&l=8; (+0) */
  MtlcValue l = mtlc_local(m, "l", i64);
  mtlc_assign(m, l, mtlc_const_int(m, i64, 7));
  MtlcValue pl = mtlc_address_of(m, l, pi64);
  mtlc_store(m, pl, mtlc_const_int(m, i64, 8), i64);
  MtlcValue lv = mtlc_load(m, pl, i64);
  MtlcValue ldiff = mtlc_binary(m, "-", lv, mtlc_const_int(m, i64, 8), i64);
  mtlc_assign(m, x, mtlc_binary(m, "+", x, ldiff, i64));

  /* global WRITE + read-back: counter = x; x = counter */
  mtlc_assign(m, g, x);
  mtlc_assign(m, x, mtlc_global_ref(m, "counter"));

  /* print "OK\n" through the putchar extern */
  MtlcValue cO = mtlc_const_int(m, i32, 'O');
  mtlc_call(m, "putchar", &cO, 1, i32);
  MtlcValue cK = mtlc_const_int(m, i32, 'K');
  mtlc_call(m, "putchar", &cK, 1, i32);
  MtlcValue nl = mtlc_const_int(m, i32, '\n');
  mtlc_call(m, "putchar", &nl, 1, i32);

  mtlc_return(m, x); /* 42 */
  return mtlc_builder_finish(b);
}

/* ---- module 2: a GPU kernel (for PTX + SPIR-V) ---- */
static MtlcModule *build_gpu_module(void) {
  const MtlcType *f32 = mtlc_type_scalar(MTLC_TYPE_FLOAT32);
  const MtlcType *boolean = mtlc_type_scalar(MTLC_TYPE_BOOL);
  const MtlcType *voidt = mtlc_type_scalar(MTLC_TYPE_VOID);
  const MtlcType *i8 = mtlc_type_scalar(MTLC_TYPE_INT8);
  const MtlcType *u16 = mtlc_type_scalar(MTLC_TYPE_UINT16);
  const MtlcType *u32 = mtlc_type_scalar(MTLC_TYPE_UINT32);
  const MtlcType *u64 = mtlc_type_scalar(MTLC_TYPE_UINT64);
  const MtlcType *pf32 =
      mtlc_type_pointer_in(f32, MTLC_ADDRESS_SPACE_GLOBAL);
  const MtlcType *pu32 =
      mtlc_type_pointer_in(u32, MTLC_ADDRESS_SPACE_GLOBAL);
  const MtlcType *pu64 =
      mtlc_type_pointer_in(u64, MTLC_ADDRESS_SPACE_GLOBAL);
  MtlcBuilder *b = mtlc_builder_create();
  const char *pn[] = {"a", "counter", "counter64"};
  const MtlcType *pt[] = {pf32, pu32, pu64};
  /* An ordinary function must remain ordinary in a GPU module. This is the
   * public-API proof that libmtlc's entry-point decision does not depend on the
   * Mettle parser/AST. */
  MtlcFn *helper =
      mtlc_builder_function(b, "ordinary_not_entry", f32, NULL, NULL, 0, 0);
  mtlc_return(helper, mtlc_const_float(helper, f32, 1.0));

  /* Ordinary functions become device helpers only through kernel reachability.
   * There is no PTX/SPIR-V annotation in the public neutral IR. */
  const char *helper_names[] = {"x"};
  const MtlcType *helper_types[] = {f32};
  MtlcFn *scale_value = mtlc_builder_function(
      b, "scale_value", f32, helper_names, helper_types, 1, 0);
  mtlc_return(scale_value,
              mtlc_binary(scale_value, "*", mtlc_fn_param(scale_value, 0),
                          mtlc_const_float(scale_value, f32, 2.0), f32));
  /* Asserted below to reach PTX as a device function with a real call
   * site, so the case has to say it must stay out of line. */
  mtlc_fn_set_noinline(scale_value);

  const char *load_names[] = {"p"};
  const MtlcType *load_types[] = {pf32};
  MtlcFn *load_scaled = mtlc_builder_function(
      b, "load_scaled", f32, load_names, load_types, 1, 0);
  MtlcValue loaded = mtlc_load(load_scaled, mtlc_fn_param(load_scaled, 0), f32);
  mtlc_return(load_scaled,
              mtlc_call(load_scaled, "scale_value", &loaded, 1, f32));
  /* Asserted below to reach PTX as a device function with a real call
   * site, so the case has to say it must stay out of line. */
  mtlc_fn_set_noinline(load_scaled);

  const char *store_names[] = {"p", "x"};
  const MtlcType *store_types[] = {pf32, f32};
  MtlcFn *store_value = mtlc_builder_function(
      b, "store_value", voidt, store_names, store_types, 2, 0);
  mtlc_store(store_value, mtlc_fn_param(store_value, 0),
             mtlc_fn_param(store_value, 1), f32);
  mtlc_return(store_value, MTLC_NO_VALUE);
  /* Asserted below to reach PTX as a device function with a real call
   * site, so the case has to say it must stay out of line. */
  mtlc_fn_set_noinline(store_value);

  const char *narrow_names[] = {"x"};
  const MtlcType *i8_types[] = {i8};
  MtlcFn *identity_i8 = mtlc_builder_function(
      b, "identity_i8", i8, narrow_names, i8_types, 1, 0);
  mtlc_return(identity_i8, mtlc_fn_param(identity_i8, 0));
  /* Asserted below to reach PTX as a device function with a real call
   * site, so the case has to say it must stay out of line. */
  mtlc_fn_set_noinline(identity_i8);
  const MtlcType *u16_types[] = {u16};
  MtlcFn *identity_u16 = mtlc_builder_function(
      b, "identity_u16", u16, narrow_names, u16_types, 1, 0);
  mtlc_return(identity_u16, mtlc_fn_param(identity_u16, 0));
  /* Asserted below to reach PTX as a device function with a real call
   * site, so the case has to say it must stay out of line. */
  mtlc_fn_set_noinline(identity_u16);

  /* A collective inside an ordinary helper remains frontend-neutral. The
   * shared call-graph verifier propagates the uniform flag from every call
   * site while allowing the reduced value itself to vary by lane. */
  const char *collective_names[] = {"value", "uniform_flag"};
  const MtlcType *collective_types[] = {u32, u32};
  MtlcFn *conditional_reduce = mtlc_builder_function(
      b, "conditional_reduce", voidt, collective_names, collective_types, 2, 0);
  mtlc_branch_if_zero(conditional_reduce,
                      mtlc_fn_param(conditional_reduce, 1),
                      "collective_skip");
  MtlcValue reduced_value = mtlc_fn_param(conditional_reduce, 0);
  mtlc_intrinsic(conditional_reduce,
                 MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_U32,
                 &reduced_value, 1, u32);
  mtlc_label(conditional_reduce, "collective_skip");
  mtlc_return(conditional_reduce, MTLC_NO_VALUE);
  /* Asserted below to reach PTX as a device function with a real call
   * site, so the case has to say it must stay out of line. */
  mtlc_fn_set_noinline(conditional_reduce);

  MtlcFn *k = mtlc_builder_kernel(b, "scale2", pn, pt, 3);
  /* Explicit semantic intrinsic: proves a non-Mettle frontend never has to
   * manufacture the legacy source spelling "gpu_tid_x". */
  mtlc_intrinsic(k, MTLC_INTRINSIC_GPU_LOCAL_ID_X, NULL, 0, u32);
  MtlcValue a = mtlc_fn_param(k, 0);
  MtlcValue v2 = mtlc_call(k, "load_scaled", &a, 1, f32);
  MtlcValue subgroup_lane = mtlc_intrinsic(
      k, MTLC_INTRINSIC_GPU_SUBGROUP_LOCAL_ID, NULL, 0, u32);
  MtlcValue subgroup_width = mtlc_intrinsic(
      k, MTLC_INTRINSIC_GPU_SUBGROUP_SIZE, NULL, 0, u32);
  MtlcValue lane_zero = mtlc_const_int(k, u32, 0);
  MtlcValue broadcast_f32_args[] = {v2, lane_zero};
  MtlcValue broadcast_u32_args[] = {subgroup_width, lane_zero};
  mtlc_intrinsic(k, MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_F32,
                 broadcast_f32_args, 2, f32);
  mtlc_intrinsic(k, MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_U32,
                 broadcast_u32_args, 2, u32);
  mtlc_intrinsic(k, MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_F32, &v2, 1,
                 f32);
  mtlc_intrinsic(k, MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_U32,
                 &subgroup_lane, 1, u32);
  mtlc_intrinsic(k, MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_F32, &v2, 1,
                 f32);
  mtlc_intrinsic(k, MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_U32,
                 &subgroup_lane, 1, u32);
  mtlc_intrinsic(k, MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_F32, &v2, 1,
                 f32);
  mtlc_intrinsic(k, MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_U32,
                 &subgroup_lane, 1, u32);
  mtlc_intrinsic(k, MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_F32,
                 &v2, 1, f32);
  mtlc_intrinsic(k, MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_U32,
                 &subgroup_lane, 1, u32);
  mtlc_intrinsic(k, MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_F32,
                 &v2, 1, f32);
  mtlc_intrinsic(k, MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_U32,
                 &subgroup_lane, 1, u32);
  MtlcValue active_lane = mtlc_binary(
      k, "<", subgroup_lane, subgroup_width, boolean);
  MtlcValue ballot_args[] = {active_lane, lane_zero};
  mtlc_intrinsic(k, MTLC_INTRINSIC_GPU_SUBGROUP_BALLOT_WORD,
                 ballot_args, 2, u32);
  mtlc_intrinsic(k, MTLC_INTRINSIC_GPU_SUBGROUP_ANY,
                 &active_lane, 1, boolean);
  mtlc_intrinsic(k, MTLC_INTRINSIC_GPU_SUBGROUP_ALL,
                 &active_lane, 1, boolean);
  MtlcValue conditional_args[] = {subgroup_lane, lane_zero};
  mtlc_call(k, "conditional_reduce", conditional_args, 2, voidt);
  /* Static memory is target-neutral at the public boundary. Both backends
   * must preserve the exact workgroup/private address space through stores
   * and loads; the barrier makes the shared-memory intent explicit. */
  MtlcValue tile = mtlc_address_space_alloc(
      k, "tile", f32, 32, MTLC_ADDRESS_SPACE_WORKGROUP);
  MtlcValue scratch = mtlc_address_space_alloc(
      k, "scratch", u32, 4, MTLC_ADDRESS_SPACE_PRIVATE);
  MtlcValue dynamic_values =
      mtlc_dynamic_workgroup_view(k, "dynamic_values", f32);
  MtlcValue dynamic_metadata =
      mtlc_dynamic_workgroup_view(k, "dynamic_metadata", u32);
  MtlcValue dynamic_counters =
      mtlc_dynamic_workgroup_view(k, "dynamic_counters", u64);
  /* The public builder emits the same neutral async-copy contract as the
   * reference frontend. GB10 lowers this to cp.async; the portable PTX and
   * SPIR-V profiles replay the exact copy synchronously. */
  mtlc_async_copy_workgroup(k, tile, a, f32, 1, 4,
                            MTLC_ASYNC_CACHE_ALL);
  mtlc_async_copy_commit(k);
  mtlc_async_copy_wait(k, 0);
  mtlc_store(k, tile, v2, f32);
  mtlc_store(k, dynamic_values, v2, f32);
  mtlc_store(k, dynamic_metadata, subgroup_lane, u32);
  mtlc_workgroup_barrier(k, MTLC_MEMORY_ORDER_ACQ_REL,
                         MTLC_MEMORY_REGION_WORKGROUP |
                             MTLC_MEMORY_REGION_GLOBAL);
  MtlcValue tile_value = mtlc_load(k, tile, f32);
  mtlc_load(k, dynamic_values, f32);
  mtlc_load(k, dynamic_metadata, u32);
  mtlc_load(k, dynamic_counters, u64);
  mtlc_store(k, scratch, mtlc_const_int(k, u32, 7), u32);
  mtlc_load(k, scratch, u32);
  MtlcValue store_args[] = {a, tile_value};
  mtlc_call(k, "store_value", store_args, 2, voidt);
  MtlcValue i8_arg = mtlc_const_int(k, i8, -1);
  MtlcValue u16_arg = mtlc_const_int(k, u16, 65535);
  mtlc_call(k, "identity_i8", &i8_arg, 1, i8);
  mtlc_call(k, "identity_u16", &u16_arg, 1, u16);
  /* Exact memory semantics are public neutral IR, not strings understood only
   * by one frontend or backend. The PTX backend uses NVIDIA's seq-cst ABI
   * sequence; SPIR-V receives the matching scope/order/memory-class bits. */
  {
    static const MtlcMemoryOrder orders[] = {
        MTLC_MEMORY_ORDER_RELAXED, MTLC_MEMORY_ORDER_ACQUIRE,
        MTLC_MEMORY_ORDER_RELEASE, MTLC_MEMORY_ORDER_ACQ_REL,
        MTLC_MEMORY_ORDER_SEQ_CST};
    static const MtlcMemoryScope scopes[] = {
        MTLC_MEMORY_SCOPE_WORK_ITEM, MTLC_MEMORY_SCOPE_SUBGROUP,
        MTLC_MEMORY_SCOPE_WORKGROUP, MTLC_MEMORY_SCOPE_SYSTEM,
        MTLC_MEMORY_SCOPE_DEVICE};
    for (size_t i = 0; i < sizeof(orders) / sizeof(orders[0]); i++) {
      MtlcValue atomic_args[3] = {mtlc_fn_param(k, 1),
                                  mtlc_const_int(k, u32, 0),
                                  mtlc_const_int(k, u32, (long long)i + 1)};
      mtlc_intrinsic_memory(k, MTLC_INTRINSIC_GPU_ATOMIC_ADD_U32, atomic_args,
                            3, u32, MTLC_ADDRESS_SPACE_GLOBAL, orders[i],
                            scopes[i]);
    }
    static const MtlcIntrinsic rmw32[] = {
        MTLC_INTRINSIC_GPU_ATOMIC_SUB_U32,
        MTLC_INTRINSIC_GPU_ATOMIC_MIN_U32,
        MTLC_INTRINSIC_GPU_ATOMIC_MAX_U32,
        MTLC_INTRINSIC_GPU_ATOMIC_AND_U32,
        MTLC_INTRINSIC_GPU_ATOMIC_OR_U32,
        MTLC_INTRINSIC_GPU_ATOMIC_XOR_U32,
        MTLC_INTRINSIC_GPU_ATOMIC_EXCHANGE_U32};
    static const MtlcIntrinsic rmw64[] = {
        MTLC_INTRINSIC_GPU_ATOMIC_ADD_U64,
        MTLC_INTRINSIC_GPU_ATOMIC_SUB_U64,
        MTLC_INTRINSIC_GPU_ATOMIC_MIN_U64,
        MTLC_INTRINSIC_GPU_ATOMIC_MAX_U64,
        MTLC_INTRINSIC_GPU_ATOMIC_AND_U64,
        MTLC_INTRINSIC_GPU_ATOMIC_OR_U64,
        MTLC_INTRINSIC_GPU_ATOMIC_XOR_U64,
        MTLC_INTRINSIC_GPU_ATOMIC_EXCHANGE_U64};
    for (size_t i = 0; i < sizeof(rmw32) / sizeof(rmw32[0]); i++) {
      MtlcValue atomic_args[3] = {mtlc_fn_param(k, 1),
                                  mtlc_const_int(k, u64, 0),
                                  mtlc_const_int(k, u32, (long long)i + 1)};
      mtlc_intrinsic_memory(k, rmw32[i], atomic_args, 3, u32,
                            MTLC_ADDRESS_SPACE_GLOBAL,
                            MTLC_MEMORY_ORDER_RELAXED,
                            MTLC_MEMORY_SCOPE_DEVICE);
    }
    for (size_t i = 0; i < sizeof(rmw64) / sizeof(rmw64[0]); i++) {
      MtlcValue atomic_args[3] = {mtlc_fn_param(k, 2),
                                  mtlc_const_int(k, u64, 0),
                                  mtlc_const_int(k, u64, (long long)i + 1)};
      mtlc_intrinsic_memory(k, rmw64[i], atomic_args, 3, u64,
                            MTLC_ADDRESS_SPACE_GLOBAL,
                            MTLC_MEMORY_ORDER_RELAXED,
                            MTLC_MEMORY_SCOPE_DEVICE);
    }
    MtlcValue cas32[4] = {mtlc_fn_param(k, 1),
                          mtlc_const_int(k, u64, 0),
                          mtlc_const_int(k, u32, 0),
                          mtlc_const_int(k, u32, 1)};
    MtlcValue cas64[4] = {mtlc_fn_param(k, 2),
                          mtlc_const_int(k, u64, 0),
                          mtlc_const_int(k, u64, 0),
                          mtlc_const_int(k, u64, 1)};
    mtlc_atomic_compare_exchange(
        k, MTLC_INTRINSIC_GPU_ATOMIC_COMPARE_EXCHANGE_U32, cas32, u32,
        MTLC_ADDRESS_SPACE_GLOBAL, MTLC_MEMORY_ORDER_ACQ_REL,
        MTLC_MEMORY_ORDER_ACQUIRE, MTLC_MEMORY_SCOPE_DEVICE);
    mtlc_atomic_compare_exchange(
        k, MTLC_INTRINSIC_GPU_ATOMIC_COMPARE_EXCHANGE_U64, cas64, u64,
        MTLC_ADDRESS_SPACE_GLOBAL, MTLC_MEMORY_ORDER_SEQ_CST,
        MTLC_MEMORY_ORDER_SEQ_CST, MTLC_MEMORY_SCOPE_SYSTEM);

    MtlcValue load32[2] = {mtlc_fn_param(k, 1),
                           mtlc_const_int(k, u64, 0)};
    MtlcValue load64[2] = {mtlc_fn_param(k, 2),
                           mtlc_const_int(k, u64, 0)};
    MtlcValue store32[3] = {mtlc_fn_param(k, 1),
                            mtlc_const_int(k, u64, 0),
                            mtlc_const_int(k, u32, 7)};
    MtlcValue store64[3] = {mtlc_fn_param(k, 2),
                            mtlc_const_int(k, u64, 0),
                            mtlc_const_int(k, u64, 9)};
    mtlc_intrinsic_memory(k, MTLC_INTRINSIC_GPU_ATOMIC_LOAD_U32, load32, 2,
                          u32, MTLC_ADDRESS_SPACE_GLOBAL,
                          MTLC_MEMORY_ORDER_ACQUIRE,
                          MTLC_MEMORY_SCOPE_DEVICE);
    mtlc_intrinsic_memory(k, MTLC_INTRINSIC_GPU_ATOMIC_LOAD_U64, load64, 2,
                          u64, MTLC_ADDRESS_SPACE_GLOBAL,
                          MTLC_MEMORY_ORDER_SEQ_CST,
                          MTLC_MEMORY_SCOPE_SYSTEM);
    mtlc_intrinsic_memory(k, MTLC_INTRINSIC_GPU_ATOMIC_STORE_U32, store32, 3,
                          voidt, MTLC_ADDRESS_SPACE_GLOBAL,
                          MTLC_MEMORY_ORDER_RELEASE,
                          MTLC_MEMORY_SCOPE_DEVICE);
    mtlc_intrinsic_memory(k, MTLC_INTRINSIC_GPU_ATOMIC_STORE_U64, store64, 3,
                          voidt, MTLC_ADDRESS_SPACE_GLOBAL,
                          MTLC_MEMORY_ORDER_SEQ_CST,
                          MTLC_MEMORY_SCOPE_SYSTEM);

    MtlcValue shared_load32[2] = {dynamic_metadata,
                                  mtlc_const_int(k, u64, 0)};
    MtlcValue shared_load64[2] = {dynamic_counters,
                                  mtlc_const_int(k, u64, 0)};
    MtlcValue shared_store32[3] = {dynamic_metadata,
                                   mtlc_const_int(k, u64, 0),
                                   mtlc_const_int(k, u32, 11)};
    MtlcValue shared_store64[3] = {dynamic_counters,
                                   mtlc_const_int(k, u64, 0),
                                   mtlc_const_int(k, u64, 13)};
    mtlc_intrinsic_memory(k, MTLC_INTRINSIC_GPU_ATOMIC_LOAD_U32,
                          shared_load32, 2, u32,
                          MTLC_ADDRESS_SPACE_WORKGROUP,
                          MTLC_MEMORY_ORDER_RELAXED,
                          MTLC_MEMORY_SCOPE_WORKGROUP);
    mtlc_intrinsic_memory(k, MTLC_INTRINSIC_GPU_ATOMIC_LOAD_U64,
                          shared_load64, 2, u64,
                          MTLC_ADDRESS_SPACE_WORKGROUP,
                          MTLC_MEMORY_ORDER_ACQUIRE,
                          MTLC_MEMORY_SCOPE_WORKGROUP);
    mtlc_intrinsic_memory(k, MTLC_INTRINSIC_GPU_ATOMIC_STORE_U32,
                          shared_store32, 3, voidt,
                          MTLC_ADDRESS_SPACE_WORKGROUP,
                          MTLC_MEMORY_ORDER_RELAXED,
                          MTLC_MEMORY_SCOPE_WORKGROUP);
    mtlc_intrinsic_memory(k, MTLC_INTRINSIC_GPU_ATOMIC_STORE_U64,
                          shared_store64, 3, voidt,
                          MTLC_ADDRESS_SPACE_WORKGROUP,
                          MTLC_MEMORY_ORDER_RELEASE,
                          MTLC_MEMORY_SCOPE_WORKGROUP);
  }
  mtlc_return(k, MTLC_NO_VALUE);

  /* A second, completely ordinary public-builder kernel proves automatic
   * staging is owned by the shared optimizer rather than the Mettle frontend.
   * No async API is used here. */
  {
    const char *auto_names[] = {"source", "output"};
    const MtlcType *auto_types[] = {pf32, pf32};
    MtlcFn *auto_kernel =
        mtlc_builder_kernel(b, "auto_stage_public", auto_names, auto_types, 2);
    MtlcValue auto_tile = mtlc_address_space_alloc(
        auto_kernel, "tile", f32, 1, MTLC_ADDRESS_SPACE_WORKGROUP);
    MtlcValue auto_value =
        mtlc_load(auto_kernel, mtlc_fn_param(auto_kernel, 0), f32);
    mtlc_store(auto_kernel, auto_tile, auto_value, f32);
    mtlc_workgroup_barrier(auto_kernel, MTLC_MEMORY_ORDER_ACQ_REL,
                           MTLC_MEMORY_REGION_WORKGROUP);
    MtlcValue staged = mtlc_load(auto_kernel, auto_tile, f32);
    mtlc_store(auto_kernel, mtlc_fn_param(auto_kernel, 1), staged, f32);
    mtlc_return(auto_kernel, MTLC_NO_VALUE);
  }
  return mtlc_builder_finish(b);
}

static MtlcModule *build_subgroup_shuffle_module(void) {
  const MtlcType *u32 = mtlc_type_scalar(MTLC_TYPE_UINT32);
  const MtlcType *f32 = mtlc_type_scalar(MTLC_TYPE_FLOAT32);
  MtlcBuilder *builder = mtlc_builder_create();
  MtlcFn *kernel =
      mtlc_builder_kernel(builder, "subgroup_shuffle_public", NULL, NULL, 0);
  MtlcValue lane = mtlc_intrinsic(
      kernel, MTLC_INTRINSIC_GPU_SUBGROUP_LOCAL_ID, NULL, 0, u32);
  MtlcValue integer_args[] = {lane, lane};
  MtlcValue float_value = mtlc_const_float(kernel, f32, 1.0);
  MtlcValue float_args[] = {float_value, lane};
  mtlc_intrinsic(kernel, MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_U32,
                 integer_args, 2, u32);
  mtlc_intrinsic(kernel, MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_F32,
                 float_args, 2, f32);
  mtlc_return(kernel, MTLC_NO_VALUE);
  return mtlc_builder_finish(builder);
}

/* ---- rank-aware tensor movement (portable contract + PTX TMA) ---- */
static MtlcModule *build_tensor_transfer_module(void) {
  const MtlcType *voidt = mtlc_type_scalar(MTLC_TYPE_VOID);
  const MtlcType *f32 = mtlc_type_scalar(MTLC_TYPE_FLOAT32);
  const MtlcType *u8 = mtlc_type_scalar(MTLC_TYPE_UINT8);
  const MtlcType *i32 = mtlc_type_scalar(MTLC_TYPE_INT32);
  const MtlcType *pf32_global =
      mtlc_type_pointer_in(f32, MTLC_ADDRESS_SPACE_GLOBAL);
  const MtlcType *pu8_global =
      mtlc_type_pointer_in(u8, MTLC_ADDRESS_SPACE_GLOBAL);
  MtlcBuilder *builder = mtlc_builder_create();

  {
    const char *names[] = {"source", "prepared_view", "x", "y"};
    const MtlcType *types[] = {pf32_global, pu8_global, i32, i32};
    MtlcFn *kernel = mtlc_builder_kernel(
        builder, "tensor_transfer_load_2d", names, types, 4);
    MtlcValue tile = mtlc_address_space_alloc(
        kernel, "tile", f32, 256, MTLC_ADDRESS_SPACE_WORKGROUP);
    MtlcTensorTransferDesc desc = {0};
    desc.rank = 2;
    desc.direction = MTLC_TENSOR_TRANSFER_GLOBAL_TO_WORKGROUP;
    desc.element = MTLC_TENSOR_ELEMENT_FLOAT32;
    desc.packing = MTLC_TENSOR_PACKING_LOGICAL;
    desc.bounds = MTLC_TENSOR_BOUNDS_ZERO;
    desc.scope = MTLC_MEMORY_SCOPE_WORKGROUP;
    desc.global_extent[0] = 128;
    desc.global_extent[1] = 96;
    desc.global_stride_bytes[0] = 4;
    desc.global_stride_bytes[1] = 128 * 4;
    desc.tile_extent[0] = 16;
    desc.tile_extent[1] = 16;
    desc.element_stride[0] = 1;
    desc.element_stride[1] = 1;
    MtlcTensorTransferOperands operands = {0};
    operands.destination = tile;
    operands.source = mtlc_fn_param(kernel, 0);
    operands.prepared_view = mtlc_fn_param(kernel, 1);
    operands.coordinates[0] = mtlc_fn_param(kernel, 2);
    operands.coordinates[1] = mtlc_fn_param(kernel, 3);
    mtlc_tensor_transfer_workgroup(kernel, &desc, &operands);
    mtlc_return(kernel, MTLC_NO_VALUE);
  }

  {
    const char *names[] = {"destination", "prepared_view", "c0", "c1",
                           "c2", "c3", "c4"};
    const MtlcType *types[] = {pf32_global, pu8_global, i32, i32,
                               i32, i32, i32};
    MtlcFn *kernel = mtlc_builder_kernel(
        builder, "tensor_transfer_store_5d", names, types, 7);
    MtlcValue tile = mtlc_address_space_alloc(
        kernel, "tile", f32, 128, MTLC_ADDRESS_SPACE_WORKGROUP);
    MtlcTensorTransferDesc desc = {0};
    desc.rank = 5;
    desc.direction = MTLC_TENSOR_TRANSFER_WORKGROUP_TO_GLOBAL;
    desc.element = MTLC_TENSOR_ELEMENT_FLOAT32;
    desc.packing = MTLC_TENSOR_PACKING_LOGICAL;
    desc.bounds = MTLC_TENSOR_BOUNDS_ZERO;
    desc.scope = MTLC_MEMORY_SCOPE_WORKGROUP;
    const uint64_t extents[5] = {32, 24, 8, 4, 3};
    const uint32_t tiles[5] = {4, 4, 2, 2, 2};
    uint64_t stride = 4;
    for (size_t dimension = 0; dimension < 5; dimension++) {
      desc.global_extent[dimension] = extents[dimension];
      desc.global_stride_bytes[dimension] = stride;
      desc.tile_extent[dimension] = tiles[dimension];
      desc.element_stride[dimension] = 1;
      stride *= extents[dimension];
    }
    MtlcTensorTransferOperands operands = {0};
    operands.destination = mtlc_fn_param(kernel, 0);
    operands.source = tile;
    operands.prepared_view = mtlc_fn_param(kernel, 1);
    for (size_t dimension = 0; dimension < 5; dimension++)
      operands.coordinates[dimension] = mtlc_fn_param(kernel, dimension + 2);
    mtlc_tensor_transfer_workgroup(kernel, &desc, &operands);
    mtlc_return(kernel, MTLC_NO_VALUE);
  }
  (void)voidt;
  return mtlc_builder_finish(builder);
}

static int tensor_transfer_descriptor_validation(void) {
  MtlcTensorTransferDesc desc = {0};
  desc.rank = 1;
  desc.direction = MTLC_TENSOR_TRANSFER_GLOBAL_TO_WORKGROUP;
  desc.element = MTLC_TENSOR_ELEMENT_UINT8;
  desc.packing = MTLC_TENSOR_PACKING_LOGICAL;
  desc.bounds = MTLC_TENSOR_BOUNDS_ZERO;
  desc.scope = MTLC_MEMORY_SCOPE_WORKGROUP;
  desc.global_extent[0] = 1024;
  desc.global_stride_bytes[0] = 1;
  desc.tile_extent[0] = 256;
  desc.element_stride[0] = 1;
  if (!mtlc_tensor_transfer_desc_is_valid(&desc)) return 0;
  desc.rank = 6;
  if (mtlc_tensor_transfer_desc_is_valid(&desc)) return 0;
  desc.rank = 1;
  desc.packing = MTLC_TENSOR_PACKING_DENSE_SUBBYTE;
  if (mtlc_tensor_transfer_desc_is_valid(&desc)) return 0;
  desc.packing = MTLC_TENSOR_PACKING_LOGICAL;
  desc.global_extent[4] = 1;
  if (mtlc_tensor_transfer_desc_is_valid(&desc)) return 0;
  desc.global_extent[4] = 0;
  desc.tile_extent[0] = 65537;
  return !mtlc_tensor_transfer_desc_is_valid(&desc);
}

/* ---- module 3: native AArch64 object ABI ---- */
static MtlcModule *build_arm64_module(void) {
  const MtlcType *i64 = mtlc_type_scalar(MTLC_TYPE_INT64);
  const MtlcType *pi64 = mtlc_type_pointer(i64);
  MtlcBuilder *b = mtlc_builder_create();
  mtlc_builder_global(b, "arm_counter", i64, 40, 0);
  const char *pn[] = {"p", "a", "b", "c", "d", "e",
                      "f", "g", "h", "i", "j"};
  const MtlcType *pt[] = {pi64, i64, i64, i64, i64, i64,
                          i64,  i64, i64, i64, i64};
  mtlc_builder_function(b, "arm_external_eleven", i64, pn, pt, 11, 1);
  const char *sn[] = {"a", "b", "c", "d", "e", "f",
                      "g", "h", "i", "j", "k"};
  const MtlcType *st[] = {i64, i64, i64, i64, i64, i64,
                          i64, i64, i64, i64, i64};
  MtlcFn *sum = mtlc_builder_function(b, "arm_stack_eleven", i64, sn, st,
                                      11, 0);
  MtlcValue tail = mtlc_binary(sum, "+", mtlc_fn_param(sum, 8),
                               mtlc_fn_param(sum, 9), i64);
  mtlc_return(sum, mtlc_binary(sum, "+", tail, mtlc_fn_param(sum, 10), i64));
  MtlcFn *m = mtlc_builder_function(b, "main", i64, NULL, NULL, 0, 0);
  MtlcValue global = mtlc_global_ref(m, "arm_counter");
  MtlcValue args[11] = {mtlc_address_of(m, global, pi64)};
  MtlcValue stack_args[11];
  for (int i = 0; i < 11; i++)
    stack_args[i] = mtlc_const_int(m, i64, i + 1);
  args[1] = mtlc_call(m, "arm_stack_eleven", stack_args, 11, i64);
  for (int i = 2; i < 11; i++) args[i] = mtlc_const_int(m, i64, i);
  mtlc_return(m, mtlc_call(m, "arm_external_eleven", args, 11, i64));
  return mtlc_builder_finish(b);
}

/* ---- module 4: semantic host launch (for native object lowering) ---- */
static MtlcModule *build_launch_module(void) {
  const MtlcType *v = mtlc_type_scalar(MTLC_TYPE_VOID);
  const MtlcType *i64 = mtlc_type_scalar(MTLC_TYPE_INT64);
  const MtlcType *i32 = mtlc_type_scalar(MTLC_TYPE_INT32);
  const MtlcType *f32 = mtlc_type_scalar(MTLC_TYPE_FLOAT32);
  const MtlcType *pi64 = mtlc_type_pointer(i64);
  MtlcBuilder *b = mtlc_builder_create();
  {
    const char *pn[] = {"f",  "gx", "gy", "gz", "bx", "by",
                        "bz", "sm", "s",  "p",  "n"};
    const MtlcType *pt[] = {i64, i32, i32, i32, i32, i32,
                            i32, i32, i64, pi64, i32};
    mtlc_builder_function(b, "mtlc_gpu_launch_checked", v, pn, pt, 11, 1);
  }
  MtlcFn *m = mtlc_builder_function(b, "main", i64, NULL, NULL, 0, 0);
  MtlcValue one = mtlc_const_int(m, i32, 1);
  MtlcValue zero64 = mtlc_const_int(m, i64, 0);
  MtlcValue kernel_args[] = {mtlc_const_int(m, i32, 17),
                             mtlc_const_float(m, f32, 1.25)};
  const MtlcType *kernel_types[] = {i32, f32};
  MtlcDim3 grid = {mtlc_const_int(m, i32, 7),
                   mtlc_const_int(m, i32, 3),
                   mtlc_const_int(m, i32, 2)};
  MtlcDim3 block = {mtlc_const_int(m, i32, 32),
                    mtlc_const_int(m, i32, 4), one};
  mtlc_gpu_launch(m, zero64, grid, block,
                  mtlc_const_int(m, i32, 4096),
                  mtlc_const_int(m, i64, 99), kernel_args,
                  kernel_types, 2);
  mtlc_return(m, zero64);
  return mtlc_builder_finish(b);
}

static void add_tensor_kernel(MtlcBuilder *builder, const char *name,
                              const MtlcTensorMmaDesc *desc,
                              const MtlcType *a_type,
                              const MtlcType *b_type,
                              const MtlcType *c_type,
                              const MtlcType *d_type) {
  const char *names[] = {"a", "b", "c", "d"};
  const MtlcType *types[] = {a_type, b_type, c_type, d_type};
  MtlcFn *kernel = mtlc_builder_kernel(builder, name, names, types, 4);
  mtlc_tensor_mma(kernel, desc, mtlc_fn_param(kernel, 0),
                  mtlc_fn_param(kernel, 1), mtlc_fn_param(kernel, 2),
                  mtlc_fn_param(kernel, 3));
  mtlc_return(kernel, MTLC_NO_VALUE);
}

static void add_tensor_matmul_kernel(MtlcBuilder *builder,
                                     const char *name,
                                     const MtlcTensorMmaDesc *base_desc,
                                     const MtlcType *a_type,
                                     const MtlcType *b_type,
                                     const MtlcType *c_type,
                                     const MtlcType *d_type,
                                     const MtlcType *u32) {
  const char *names[] = {"a",   "b",   "c",   "d",   "row",
                         "col", "m",   "n",   "k",   "lda",
                         "ldb", "ldc", "ldd"};
  const MtlcType *types[] = {a_type, b_type, c_type, d_type, u32,
                             u32,    u32,    u32,    u32,    u32,
                             u32,    u32,    u32};
  MtlcTensorMmaDesc desc = *base_desc;
  desc.a_leading_dimension = 0;
  desc.b_leading_dimension = 0;
  desc.c_leading_dimension = 0;
  desc.d_leading_dimension = 0;
  MtlcFn *kernel =
      mtlc_builder_kernel(builder, name, names, types, 13);
  MtlcTensorMatmulOperands operands = {0};
  operands.matrix.a = mtlc_fn_param(kernel, 0);
  operands.matrix.b = mtlc_fn_param(kernel, 1);
  operands.matrix.c = mtlc_fn_param(kernel, 2);
  operands.matrix.d = mtlc_fn_param(kernel, 3);
  operands.matrix.metadata = MTLC_NO_VALUE;
  operands.matrix.a_scale = MTLC_NO_VALUE;
  operands.matrix.b_scale = MTLC_NO_VALUE;
  operands.matrix.runtime_stride_mask = MTLC_TENSOR_RUNTIME_STRIDE_ALL;
  operands.matrix.a_leading_dimension = mtlc_fn_param(kernel, 9);
  operands.matrix.b_leading_dimension = mtlc_fn_param(kernel, 10);
  operands.matrix.c_leading_dimension = mtlc_fn_param(kernel, 11);
  operands.matrix.d_leading_dimension = mtlc_fn_param(kernel, 12);
  operands.row_origin = mtlc_fn_param(kernel, 4);
  operands.column_origin = mtlc_fn_param(kernel, 5);
  operands.problem_m = mtlc_fn_param(kernel, 6);
  operands.problem_n = mtlc_fn_param(kernel, 7);
  operands.problem_k = mtlc_fn_param(kernel, 8);
  mtlc_tensor_matmul(kernel, &desc, &operands);
  mtlc_return(kernel, MTLC_NO_VALUE);
}

static void add_scaled_tensor_matmul_kernel(
    MtlcBuilder *builder, const char *name,
    const MtlcTensorMmaDesc *base_desc, const MtlcType *a_type,
    const MtlcType *b_type, const MtlcType *c_type,
    const MtlcType *d_type, const MtlcType *scale_type,
    const MtlcType *u32) {
  const char *names[] = {"a",   "b",   "c",   "d",   "a_scale",
                         "b_scale", "row", "col", "m", "n",
                         "k",   "lda", "ldb", "ldc", "ldd"};
  const MtlcType *types[] = {
      a_type, b_type, c_type, d_type, scale_type, scale_type, u32, u32,
      u32,    u32,    u32,    u32,    u32,        u32,        u32};
  MtlcTensorMmaDesc desc = *base_desc;
  desc.a_leading_dimension = 0;
  desc.b_leading_dimension = 0;
  desc.c_leading_dimension = 0;
  desc.d_leading_dimension = 0;
  MtlcFn *kernel =
      mtlc_builder_kernel(builder, name, names, types, 15);
  MtlcTensorMatmulOperands operands = {0};
  operands.matrix.a = mtlc_fn_param(kernel, 0);
  operands.matrix.b = mtlc_fn_param(kernel, 1);
  operands.matrix.c = mtlc_fn_param(kernel, 2);
  operands.matrix.d = mtlc_fn_param(kernel, 3);
  operands.matrix.metadata = MTLC_NO_VALUE;
  operands.matrix.a_scale = mtlc_fn_param(kernel, 4);
  operands.matrix.b_scale = mtlc_fn_param(kernel, 5);
  operands.matrix.runtime_stride_mask = MTLC_TENSOR_RUNTIME_STRIDE_ALL;
  operands.matrix.a_leading_dimension = mtlc_fn_param(kernel, 11);
  operands.matrix.b_leading_dimension = mtlc_fn_param(kernel, 12);
  operands.matrix.c_leading_dimension = mtlc_fn_param(kernel, 13);
  operands.matrix.d_leading_dimension = mtlc_fn_param(kernel, 14);
  operands.row_origin = mtlc_fn_param(kernel, 6);
  operands.column_origin = mtlc_fn_param(kernel, 7);
  operands.problem_m = mtlc_fn_param(kernel, 8);
  operands.problem_n = mtlc_fn_param(kernel, 9);
  operands.problem_k = mtlc_fn_param(kernel, 10);
  mtlc_tensor_matmul(kernel, &desc, &operands);
  mtlc_return(kernel, MTLC_NO_VALUE);
}

static void add_sparse_tensor_matmul_kernel(
    MtlcBuilder *builder, const char *name,
    const MtlcTensorMmaDesc *base_desc, const MtlcType *a_type,
    const MtlcType *b_type, const MtlcType *c_type,
    const MtlcType *d_type, const MtlcType *metadata_type,
    const MtlcType *u32) {
  const char *names[] = {"a",   "b",   "c",   "d",   "metadata",
                         "row", "col", "m",   "n",   "k",
                         "lda", "ldb", "ldc", "ldd"};
  const MtlcType *types[] = {
      a_type, b_type, c_type, d_type, metadata_type, u32, u32,
      u32,    u32,    u32,    u32,    u32,           u32, u32};
  MtlcTensorMmaDesc desc = *base_desc;
  desc.a_leading_dimension = 0;
  desc.b_leading_dimension = 0;
  desc.c_leading_dimension = 0;
  desc.d_leading_dimension = 0;
  MtlcFn *kernel =
      mtlc_builder_kernel(builder, name, names, types, 14);
  MtlcTensorMatmulOperands operands = {0};
  operands.matrix.a = mtlc_fn_param(kernel, 0);
  operands.matrix.b = mtlc_fn_param(kernel, 1);
  operands.matrix.c = mtlc_fn_param(kernel, 2);
  operands.matrix.d = mtlc_fn_param(kernel, 3);
  operands.matrix.metadata = mtlc_fn_param(kernel, 4);
  operands.matrix.a_scale = MTLC_NO_VALUE;
  operands.matrix.b_scale = MTLC_NO_VALUE;
  operands.matrix.runtime_stride_mask = MTLC_TENSOR_RUNTIME_STRIDE_ALL;
  operands.matrix.a_leading_dimension = mtlc_fn_param(kernel, 10);
  operands.matrix.b_leading_dimension = mtlc_fn_param(kernel, 11);
  operands.matrix.c_leading_dimension = mtlc_fn_param(kernel, 12);
  operands.matrix.d_leading_dimension = mtlc_fn_param(kernel, 13);
  operands.row_origin = mtlc_fn_param(kernel, 5);
  operands.column_origin = mtlc_fn_param(kernel, 6);
  operands.problem_m = mtlc_fn_param(kernel, 7);
  operands.problem_n = mtlc_fn_param(kernel, 8);
  operands.problem_k = mtlc_fn_param(kernel, 9);
  mtlc_tensor_matmul(kernel, &desc, &operands);
  mtlc_return(kernel, MTLC_NO_VALUE);
}

static void add_scaled_tensor_kernel(MtlcBuilder *builder, const char *name,
                                     const MtlcTensorMmaDesc *desc,
                                     const MtlcType *a_type,
                                     const MtlcType *b_type,
                                     const MtlcType *c_type,
                                     const MtlcType *d_type,
                                     const MtlcType *scale_type) {
  const char *names[] = {"a", "b", "c", "d", "a_scale", "b_scale"};
  const MtlcType *types[] = {a_type, b_type, c_type, d_type,
                             scale_type, scale_type};
  MtlcFn *kernel = mtlc_builder_kernel(builder, name, names, types, 6);
  MtlcTensorMmaOperands operands = {0};
  operands.a = mtlc_fn_param(kernel, 0);
  operands.b = mtlc_fn_param(kernel, 1);
  operands.c = mtlc_fn_param(kernel, 2);
  operands.d = mtlc_fn_param(kernel, 3);
  operands.metadata = MTLC_NO_VALUE;
  operands.a_scale = mtlc_fn_param(kernel, 4);
  operands.b_scale = mtlc_fn_param(kernel, 5);
  mtlc_tensor_mma_ex(kernel, desc, &operands);
  mtlc_return(kernel, MTLC_NO_VALUE);
}

static void add_sparse_tensor_kernel(MtlcBuilder *builder, const char *name,
                                     const MtlcTensorMmaDesc *desc,
                                     const MtlcType *a_type,
                                     const MtlcType *b_type,
                                     const MtlcType *c_type,
                                     const MtlcType *d_type,
                                     const MtlcType *metadata_type) {
  const char *names[] = {"a", "b", "c", "d", "metadata"};
  const MtlcType *types[] = {a_type, b_type, c_type, d_type, metadata_type};
  MtlcFn *kernel = mtlc_builder_kernel(builder, name, names, types, 5);
  MtlcTensorMmaOperands operands = {0};
  operands.a = mtlc_fn_param(kernel, 0);
  operands.b = mtlc_fn_param(kernel, 1);
  operands.c = mtlc_fn_param(kernel, 2);
  operands.d = mtlc_fn_param(kernel, 3);
  operands.metadata = mtlc_fn_param(kernel, 4);
  operands.a_scale = MTLC_NO_VALUE;
  operands.b_scale = MTLC_NO_VALUE;
  mtlc_tensor_mma_ex(kernel, desc, &operands);
  mtlc_return(kernel, MTLC_NO_VALUE);
}

static void add_scaled_tensor_chain_kernel(
    MtlcBuilder *builder, const char *name, const MtlcTensorMmaDesc *desc,
    const MtlcType *a_type, const MtlcType *b_type,
    const MtlcType *c_type, const MtlcType *d_type,
    const MtlcType *scale_type) {
  const char *names[] = {"a", "b", "c", "d", "a_scale", "b_scale"};
  const MtlcType *types[] = {a_type, b_type, c_type, d_type,
                             scale_type, scale_type};
  MtlcFn *kernel = mtlc_builder_kernel(builder, name, names, types, 6);
  MtlcTensorMmaOperands tiles[3] = {{0}};
  for (size_t tile = 0; tile < 3; tile++) {
    tiles[tile].a = mtlc_fn_param(kernel, 0);
    tiles[tile].b = mtlc_fn_param(kernel, 1);
    tiles[tile].c = mtlc_fn_param(kernel, tile == 0 ? 2 : 3);
    tiles[tile].d = mtlc_fn_param(kernel, 3);
    tiles[tile].metadata = MTLC_NO_VALUE;
    tiles[tile].a_scale = mtlc_fn_param(kernel, 4);
    tiles[tile].b_scale = mtlc_fn_param(kernel, 5);
  }
  mtlc_tensor_mma_chain(kernel, desc, tiles, 3);
  mtlc_return(kernel, MTLC_NO_VALUE);
}

static void add_scaled_tensor_loop_kernel(
    MtlcBuilder *builder, const char *name,
    const MtlcTensorMmaDesc *base_desc, const MtlcType *packed_type,
    const MtlcType *output_type, const MtlcType *scale_type,
    uint32_t k_step, uint32_t packed_numerator,
    uint32_t packed_denominator, uint32_t scale_block) {
  const MtlcType *i32 = mtlc_type_scalar(MTLC_TYPE_INT32);
  const MtlcType *boolean = mtlc_type_scalar(MTLC_TYPE_BOOL);
  const char *names[] = {"a", "b", "c", "d", "a_scale", "b_scale",
                         "k", "lda", "ldb", "ldc", "ldd"};
  const MtlcType *types[] = {packed_type, packed_type, output_type,
                             output_type, scale_type, scale_type, i32,
                             i32, i32, i32, i32};
  MtlcTensorMmaDesc desc = *base_desc;
  desc.a_leading_dimension = 0;
  desc.b_leading_dimension = 0;
  desc.c_leading_dimension = 0;
  desc.d_leading_dimension = 0;
  MtlcFn *kernel = mtlc_builder_kernel(builder, name, names, types, 11);
  MtlcValue values[11];
  for (size_t i = 0; i < 11; i++) values[i] = mtlc_fn_param(kernel, i);

  MtlcTensorMmaOperands operands = {0};
  operands.a = values[0];
  operands.b = values[1];
  operands.c = values[2];
  operands.d = values[3];
  operands.metadata = MTLC_NO_VALUE;
  operands.a_scale = values[4];
  operands.b_scale = values[5];
  operands.runtime_stride_mask = MTLC_TENSOR_RUNTIME_STRIDE_ALL;
  operands.a_leading_dimension = values[7];
  operands.b_leading_dimension = values[8];
  operands.c_leading_dimension = values[9];
  operands.d_leading_dimension = values[10];
  mtlc_tensor_mma_ex(kernel, &desc, &operands);

  MtlcValue inner = mtlc_local(kernel, "inner", i32);
  mtlc_assign(kernel, inner, mtlc_const_int(kernel, i32, k_step));
  mtlc_label(kernel, "scaled_tensor_k_loop");
  MtlcValue active = mtlc_binary(kernel, "<", inner, values[6], boolean);
  mtlc_branch_if_zero(kernel, active, "scaled_tensor_k_done");
  MtlcValue packed_offset = inner;
  if (packed_numerator != 1)
    packed_offset = mtlc_binary(
        kernel, "*", packed_offset,
        mtlc_const_int(kernel, i32, packed_numerator), i32);
  if (packed_denominator != 1)
    packed_offset = mtlc_binary(
        kernel, "/", packed_offset,
        mtlc_const_int(kernel, i32, packed_denominator), i32);
  MtlcValue scale_offset =
      mtlc_binary(kernel, "/", inner,
                  mtlc_const_int(kernel, i32, scale_block), i32);
  operands.a = mtlc_binary(kernel, "+", values[0], packed_offset,
                           packed_type);
  operands.b = mtlc_binary(kernel, "+", values[1], packed_offset,
                           packed_type);
  operands.c = operands.d = values[3];
  operands.a_scale =
      mtlc_binary(kernel, "+", values[4], scale_offset, scale_type);
  operands.b_scale =
      mtlc_binary(kernel, "+", values[5], scale_offset, scale_type);
  operands.c_leading_dimension = values[10];
  mtlc_tensor_mma_ex(kernel, &desc, &operands);
  mtlc_assign(kernel, inner,
               mtlc_binary(kernel, "+", inner,
                           mtlc_const_int(kernel, i32, k_step), i32));
  mtlc_jump(kernel, "scaled_tensor_k_loop");
  mtlc_label(kernel, "scaled_tensor_k_done");
  mtlc_return(kernel, MTLC_NO_VALUE);
}

static void add_tensor_chain_kernel(MtlcBuilder *builder,
                                    const char *name,
                                    const MtlcTensorMmaDesc *desc,
                                    const MtlcType *a_type,
                                    const MtlcType *b_type,
                                    const MtlcType *c_type,
                                    const MtlcType *d_type) {
  const char *names[] = {"a", "b", "c", "d"};
  const MtlcType *types[] = {a_type, b_type, c_type, d_type};
  MtlcFn *kernel = mtlc_builder_kernel(builder, name, names, types, 4);
  MtlcTensorMmaOperands tiles[3] = {{0}};
  for (size_t i = 0; i < 3; i++) {
    tiles[i].a = mtlc_fn_param(kernel, 0);
    tiles[i].b = mtlc_fn_param(kernel, 1);
    tiles[i].c = mtlc_fn_param(kernel, i == 0 ? 2 : 3);
    tiles[i].d = mtlc_fn_param(kernel, 3);
    tiles[i].metadata = MTLC_NO_VALUE;
    tiles[i].a_scale = MTLC_NO_VALUE;
    tiles[i].b_scale = MTLC_NO_VALUE;
  }
  mtlc_tensor_mma_chain(kernel, desc, tiles, 3);
  mtlc_return(kernel, MTLC_NO_VALUE);
}

static void add_strided_tensor_chain_kernel(
    MtlcBuilder *builder, const MtlcTensorMmaDesc *base_desc,
    const MtlcType *a_type, const MtlcType *b_type,
    const MtlcType *c_type, const MtlcType *d_type) {
  const MtlcType *i32 = mtlc_type_scalar(MTLC_TYPE_INT32);
  const char *names[] = {"a", "b", "c", "d", "lda", "ldb", "ldc", "ldd"};
  const MtlcType *types[] = {a_type, b_type, c_type, d_type,
                             i32, i32, i32, i32};
  MtlcTensorMmaDesc desc = *base_desc;
  desc.a_leading_dimension = 0;
  desc.b_leading_dimension = 0;
  desc.c_leading_dimension = 0;
  desc.d_leading_dimension = 0;
  MtlcFn *kernel = mtlc_builder_kernel(
      builder, "tensor_f16_f32_strided_chain3", names, types, 8);
  MtlcValue values[8];
  for (size_t i = 0; i < 8; i++) values[i] = mtlc_fn_param(kernel, i);
  MtlcTensorMmaOperands tiles[3] = {{0}};
  for (size_t i = 0; i < 3; i++) {
    tiles[i].a = values[0];
    tiles[i].b = values[1];
    tiles[i].c = values[i == 0 ? 2 : 3];
    tiles[i].d = values[3];
    tiles[i].metadata = MTLC_NO_VALUE;
    tiles[i].a_scale = MTLC_NO_VALUE;
    tiles[i].b_scale = MTLC_NO_VALUE;
    tiles[i].runtime_stride_mask = MTLC_TENSOR_RUNTIME_STRIDE_A |
                                   MTLC_TENSOR_RUNTIME_STRIDE_B |
                                   MTLC_TENSOR_RUNTIME_STRIDE_C |
                                   MTLC_TENSOR_RUNTIME_STRIDE_D;
    tiles[i].a_leading_dimension = values[4];
    tiles[i].b_leading_dimension = values[5];
    tiles[i].c_leading_dimension = values[i == 0 ? 6 : 7];
    tiles[i].d_leading_dimension = values[7];
  }
  mtlc_tensor_mma_chain(kernel, &desc, tiles, 3);
  mtlc_return(kernel, MTLC_NO_VALUE);
}

static void add_strided_tensor_kernel(MtlcBuilder *builder, const char *name,
                                      const MtlcTensorMmaDesc *base_desc,
                                      const MtlcType *a_type,
                                      const MtlcType *b_type,
                                      const MtlcType *c_type,
                                      const MtlcType *d_type) {
  const MtlcType *i32 = mtlc_type_scalar(MTLC_TYPE_INT32);
  const char *names[] = {"a", "b", "c", "d", "lda", "ldb", "ldc", "ldd"};
  const MtlcType *types[] = {a_type, b_type, c_type, d_type,
                             i32, i32, i32, i32};
  MtlcTensorMmaDesc desc = *base_desc;
  desc.a_leading_dimension = 0;
  desc.b_leading_dimension = 0;
  desc.c_leading_dimension = 0;
  desc.d_leading_dimension = 0;
  MtlcFn *kernel = mtlc_builder_kernel(builder, name, names, types, 8);
  mtlc_tensor_mma_strided(
      kernel, &desc, mtlc_fn_param(kernel, 0), mtlc_fn_param(kernel, 1),
      mtlc_fn_param(kernel, 2), mtlc_fn_param(kernel, 3),
      mtlc_fn_param(kernel, 4), mtlc_fn_param(kernel, 5),
      mtlc_fn_param(kernel, 6), mtlc_fn_param(kernel, 7));
  mtlc_return(kernel, MTLC_NO_VALUE);
}

static void add_strided_tensor_loop_kernel(
    MtlcBuilder *builder, const char *name,
    const MtlcTensorMmaDesc *base_desc, int k_step, int element_size_shift,
    const MtlcType *a_type, const MtlcType *b_type,
    const MtlcType *c_type, const MtlcType *d_type) {
  const MtlcType *i32 = mtlc_type_scalar(MTLC_TYPE_INT32);
  const MtlcType *boolean = mtlc_type_scalar(MTLC_TYPE_BOOL);
  const char *names[] = {"a", "b", "c", "d", "k",
                         "lda", "ldb", "ldc", "ldd"};
  const MtlcType *types[] = {a_type, b_type, c_type, d_type, i32,
                             i32, i32, i32, i32};
  MtlcTensorMmaDesc desc = *base_desc;
  desc.a_leading_dimension = 0;
  desc.b_leading_dimension = 0;
  desc.c_leading_dimension = 0;
  desc.d_leading_dimension = 0;
  MtlcFn *kernel =
      mtlc_builder_kernel(builder, name, names, types, 9);
  MtlcValue values[9];
  for (size_t i = 0; i < 9; i++) values[i] = mtlc_fn_param(kernel, i);

  mtlc_tensor_mma_strided(kernel, &desc, values[0], values[1], values[2],
                          values[3], values[5], values[6], values[7],
                          values[8]);
  MtlcValue inner = mtlc_local(kernel, "inner", i32);
  mtlc_assign(kernel, inner, mtlc_const_int(kernel, i32, k_step));
  mtlc_label(kernel, "tensor_k_loop");
  MtlcValue active = mtlc_binary(kernel, "<", inner, values[4], boolean);
  mtlc_branch_if_zero(kernel, active, "tensor_k_done");

  /* Public IR pointer arithmetic is expressed in bytes, as in the frontend's
   * neutral lowering. Both f16 input pointers share the same scaled offset. */
  MtlcValue byte_offset =
      element_size_shift == 0
          ? inner
          : mtlc_binary(kernel, "<<", inner,
                        mtlc_const_int(kernel, i32, element_size_shift), i32);
  MtlcValue next_a =
      mtlc_binary(kernel, "+", values[0], byte_offset, a_type);
  MtlcValue next_b =
      mtlc_binary(kernel, "+", values[1], byte_offset, b_type);
  mtlc_tensor_mma_strided(kernel, &desc, next_a, next_b, values[3],
                          values[3], values[5], values[6], values[8],
                          values[8]);
  mtlc_assign(kernel, inner,
              mtlc_binary(kernel, "+", inner,
                          mtlc_const_int(kernel, i32, k_step), i32));
  mtlc_jump(kernel, "tensor_k_loop");
  mtlc_label(kernel, "tensor_k_done");
  mtlc_return(kernel, MTLC_NO_VALUE);
}

static void add_tensor_pipeline_kernel(
    MtlcBuilder *builder, const MtlcTensorMmaDesc *desc,
    const MtlcType *u16, const MtlcType *u32,
    const MtlcType *global_u16, const MtlcType *global_f32) {
  const MtlcType *workgroup_u16 =
      mtlc_type_pointer_in(u16, MTLC_ADDRESS_SPACE_WORKGROUP);
  const char *names[] = {"a", "b", "c", "d"};
  const MtlcType *types[] = {global_u16, global_u16,
                             global_f32, global_f32};
  MtlcFn *kernel = mtlc_builder_kernel(
      builder, "tensor_pipeline_public", names, types, 4);
  MtlcValue a_stage = mtlc_address_space_alloc(
      kernel, "a_stage", u16, 1024, MTLC_ADDRESS_SPACE_WORKGROUP);
  MtlcValue b_stage = mtlc_address_space_alloc(
      kernel, "b_stage", u16, 1024, MTLC_ADDRESS_SPACE_WORKGROUP);
  MtlcValue lane = mtlc_intrinsic(
      kernel, MTLC_INTRINSIC_GPU_LOCAL_ID_X, NULL, 0, u32);
  /* Public pointer arithmetic is byte-based. Eight binary16 elements per lane
   * therefore advance by sixteen bytes; consecutive tiles are 512 bytes. */
  MtlcValue lane_bytes = mtlc_binary(
      kernel, "<<", lane, mtlc_const_int(kernel, u32, 4), u32);
  MtlcValue a_tiles[4];
  MtlcValue b_tiles[4];
  for (uint32_t stage = 0; stage < 4; stage++) {
    MtlcValue tile_offset =
        mtlc_const_int(kernel, u32, (int64_t)stage * 512);
    MtlcValue lane_offset =
        mtlc_binary(kernel, "+", tile_offset, lane_bytes, u32);
    a_tiles[stage] = mtlc_binary(
        kernel, "+", a_stage, tile_offset, workgroup_u16);
    b_tiles[stage] = mtlc_binary(
        kernel, "+", b_stage, tile_offset, workgroup_u16);
    MtlcValue a_stage_lane = mtlc_binary(
        kernel, "+", a_stage, lane_offset, workgroup_u16);
    MtlcValue b_stage_lane = mtlc_binary(
        kernel, "+", b_stage, lane_offset, workgroup_u16);
    MtlcValue a_global_lane = mtlc_binary(
        kernel, "+", mtlc_fn_param(kernel, 0), lane_offset, global_u16);
    MtlcValue b_global_lane = mtlc_binary(
        kernel, "+", mtlc_fn_param(kernel, 1), lane_offset, global_u16);
    mtlc_async_copy_workgroup(kernel, a_stage_lane, a_global_lane, u16, 8, 16,
                              MTLC_ASYNC_CACHE_GLOBAL);
    mtlc_async_copy_workgroup(kernel, b_stage_lane, b_global_lane, u16, 8, 16,
                              MTLC_ASYNC_CACHE_GLOBAL);
    mtlc_async_copy_commit(kernel);
  }
  for (uint32_t stage = 0; stage < 4; stage++) {
    mtlc_async_copy_wait(kernel, 3 - stage);
    mtlc_workgroup_barrier(kernel, MTLC_MEMORY_ORDER_ACQ_REL,
                           MTLC_MEMORY_REGION_WORKGROUP);
    MtlcValue accumulator = mtlc_fn_param(kernel, stage == 0 ? 2 : 3);
    mtlc_tensor_mma(kernel, desc, a_tiles[stage], b_tiles[stage], accumulator,
                    mtlc_fn_param(kernel, 3));
  }
  mtlc_return(kernel, MTLC_NO_VALUE);
}

/* ---- module 5: broad target-neutral tensor descriptors ---- */
static const MtlcTensorElement k_mxf8f6f4_elements[] = {
    MTLC_TENSOR_ELEMENT_FLOAT8_E4M3,
    MTLC_TENSOR_ELEMENT_FLOAT8_E5M2,
    MTLC_TENSOR_ELEMENT_FLOAT6_E2M3,
    MTLC_TENSOR_ELEMENT_FLOAT6_E3M2,
    MTLC_TENSOR_ELEMENT_FLOAT4_E2M1};
static const char *const k_mxf8f6f4_element_names[] = {
    "e4m3", "e5m2", "e2m3", "e3m2", "e2m1"};

static void add_tensor_epilogue_kernel(MtlcBuilder *builder,
                                       const MtlcType *pf32,
                                       const MtlcType *f32,
                                       const MtlcType *i32) {
  const char *names[] = {"d",       "bias", "alpha", "beta",
                         "minimum", "maximum", "ldd", "ldbias"};
  const MtlcType *types[] = {pf32, pf32, f32, f32, f32, f32, i32, i32};
  MtlcFn *kernel = mtlc_builder_kernel(
      builder, "tensor_epilogue_public", names, types, 8);
  MtlcTensorEpilogueDesc desc = {0};
  desc.m = 19;
  desc.n = 37;
  desc.element = MTLC_TENSOR_ELEMENT_FLOAT32;
  desc.layout = MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
  desc.bias_mode = MTLC_TENSOR_BIAS_MATRIX;
  desc.bias_layout = MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  desc.activation = MTLC_TENSOR_ACTIVATION_CLAMP;
  desc.scale_output = 1;
  desc.scale_bias = 1;
  desc.scope = MTLC_MEMORY_SCOPE_WORKGROUP;
  MtlcTensorEpilogueOperands operands = {0};
  operands.destination = mtlc_fn_param(kernel, 0);
  operands.bias = mtlc_fn_param(kernel, 1);
  operands.alpha = mtlc_fn_param(kernel, 2);
  operands.beta = mtlc_fn_param(kernel, 3);
  operands.clamp_min = mtlc_fn_param(kernel, 4);
  operands.clamp_max = mtlc_fn_param(kernel, 5);
  operands.leading_dimension = mtlc_fn_param(kernel, 6);
  operands.bias_leading_dimension = mtlc_fn_param(kernel, 7);
  mtlc_tensor_epilogue(kernel, &desc, &operands);
  mtlc_return(kernel, MTLC_NO_VALUE);
}

static void add_tensor_epilogue_fused_kernel(
    MtlcBuilder *builder, const MtlcTensorMmaDesc *mma_desc,
    const MtlcType *pu16, const MtlcType *pf32, const MtlcType *f32) {
  const char *names[] = {"a", "b", "c", "d", "alpha"};
  const MtlcType *types[] = {pu16, pu16, pf32, pf32, f32};
  MtlcFn *kernel = mtlc_builder_kernel(
      builder, "tensor_epilogue_fused_public", names, types, 5);
  mtlc_tensor_mma(kernel, mma_desc, mtlc_fn_param(kernel, 0),
                  mtlc_fn_param(kernel, 1), mtlc_fn_param(kernel, 2),
                  mtlc_fn_param(kernel, 3));
  MtlcTensorEpilogueDesc epilogue = {0};
  epilogue.m = 16;
  epilogue.n = 16;
  epilogue.element = MTLC_TENSOR_ELEMENT_FLOAT32;
  epilogue.layout = MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  epilogue.leading_dimension = 16;
  epilogue.bias_mode = MTLC_TENSOR_BIAS_NONE;
  epilogue.bias_layout = MTLC_TENSOR_LAYOUT_INVALID;
  epilogue.activation = MTLC_TENSOR_ACTIVATION_RELU;
  epilogue.scale_output = 1;
  epilogue.scope = MTLC_MEMORY_SCOPE_SUBGROUP;
  MtlcTensorEpilogueOperands operands = {
      .destination = mtlc_fn_param(kernel, 3),
      .bias = MTLC_NO_VALUE,
      .alpha = mtlc_fn_param(kernel, 4),
      .beta = MTLC_NO_VALUE,
      .clamp_min = MTLC_NO_VALUE,
      .clamp_max = MTLC_NO_VALUE,
      .leading_dimension = MTLC_NO_VALUE,
      .bias_leading_dimension = MTLC_NO_VALUE};
  mtlc_tensor_epilogue(kernel, &epilogue, &operands);
  mtlc_return(kernel, MTLC_NO_VALUE);
}

static int tensor_epilogue_descriptor_validation(void) {
  MtlcTensorEpilogueDesc desc = {0};
  desc.m = 16;
  desc.n = 32;
  desc.element = MTLC_TENSOR_ELEMENT_FLOAT32;
  desc.layout = MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  desc.leading_dimension = 32;
  desc.bias_mode = MTLC_TENSOR_BIAS_NONE;
  desc.bias_layout = MTLC_TENSOR_LAYOUT_INVALID;
  desc.activation = MTLC_TENSOR_ACTIVATION_RELU;
  desc.scope = MTLC_MEMORY_SCOPE_SUBGROUP;
  if (!mtlc_tensor_epilogue_desc_is_valid(&desc)) return 0;
  desc.scale_bias = 1;
  if (mtlc_tensor_epilogue_desc_is_valid(&desc)) return 0;
  desc.scale_bias = 0;
  desc.element = MTLC_TENSOR_ELEMENT_FLOAT8_E4M3;
  if (mtlc_tensor_epilogue_desc_is_valid(&desc)) return 0;
  desc.element = MTLC_TENSOR_ELEMENT_FLOAT32;
  desc.leading_dimension = 31;
  return !mtlc_tensor_epilogue_desc_is_valid(&desc);
}

static MtlcModule *build_tensor_module(void) {
  const MtlcType *u8 = mtlc_type_scalar(MTLC_TYPE_UINT8);
  const MtlcType *i8 = mtlc_type_scalar(MTLC_TYPE_INT8);
  const MtlcType *u16 = mtlc_type_scalar(MTLC_TYPE_UINT16);
  const MtlcType *u32 = mtlc_type_scalar(MTLC_TYPE_UINT32);
  const MtlcType *i32 = mtlc_type_scalar(MTLC_TYPE_INT32);
  const MtlcType *f32 = mtlc_type_scalar(MTLC_TYPE_FLOAT32);
  const MtlcType *f64 = mtlc_type_scalar(MTLC_TYPE_FLOAT64);
  const MtlcType *pu8 = mtlc_type_pointer_in(u8, MTLC_ADDRESS_SPACE_GLOBAL);
  const MtlcType *pi8 = mtlc_type_pointer_in(i8, MTLC_ADDRESS_SPACE_GLOBAL);
  const MtlcType *pu16 = mtlc_type_pointer_in(u16, MTLC_ADDRESS_SPACE_GLOBAL);
  const MtlcType *pi32 = mtlc_type_pointer_in(i32, MTLC_ADDRESS_SPACE_GLOBAL);
  const MtlcType *pf32 = mtlc_type_pointer_in(f32, MTLC_ADDRESS_SPACE_GLOBAL);
  const MtlcType *pf64 = mtlc_type_pointer_in(f64, MTLC_ADDRESS_SPACE_GLOBAL);
  MtlcBuilder *builder = mtlc_builder_create();
  add_tensor_epilogue_kernel(builder, pf32, f32, i32);
  MtlcTensorMmaDesc desc = mtlc_tensor_mma_f16_f32_m16n16k16_desc();
  add_tensor_matmul_kernel(builder, "tensor_matmul_public", &desc, pu16,
                           pu16, pf32, pf32, u32);
  MtlcTensorMmaDesc transpose_desc = desc;
  transpose_desc.transpose_a = 1;
  transpose_desc.transpose_b = 1;
  add_tensor_matmul_kernel(builder, "tensor_matmul_transpose_public",
                           &transpose_desc, pu16, pu16, pf32, pf32, u32);
  add_tensor_epilogue_fused_kernel(builder, &desc, pu16, pf32, f32);
  add_tensor_kernel(builder, "tensor_f16_f32_m16n16k16", &desc, pu16, pu16,
                    pf32, pf32);
  add_tensor_chain_kernel(builder, "tensor_f16_f32_chain3", &desc, pu16,
                          pu16, pf32, pf32);
  add_strided_tensor_chain_kernel(builder, &desc, pu16, pu16, pf32, pf32);
  add_strided_tensor_kernel(builder, "tensor_f16_f32_strided", &desc, pu16,
                            pu16, pf32, pf32);
  add_strided_tensor_loop_kernel(builder, "tensor_f16_f32_runtime_k", &desc,
                                 16, 1, pu16, pu16, pf32, pf32);
  add_tensor_pipeline_kernel(builder, &desc, u16, u32, pu16, pf32);

  MtlcTensorMmaDesc tiled_desc = desc;
  tiled_desc.m = 32;
  tiled_desc.n = 32;
  tiled_desc.a_leading_dimension = 16;
  tiled_desc.b_leading_dimension = 16;
  tiled_desc.c_leading_dimension = 32;
  tiled_desc.d_leading_dimension = 32;
  add_tensor_kernel(builder, "tensor_tiled_f16_m32n32", &tiled_desc, pu16,
                    pu16, pf32, pf32);
  add_tensor_chain_kernel(builder, "tensor_tiled_f16_chain3_m32n32",
                          &tiled_desc, pu16, pu16, pf32, pf32);

  /* Structured sparsity remains a neutral compressed-A + uint8 group-mask
   * contract. Only the PTX backend translates that representation into its
   * ordered warp metadata and mma.sp fragments. */
  desc.sparsity = MTLC_TENSOR_SPARSITY_STRUCTURED_2_TO_4;
  desc.a_leading_dimension = 8;
  add_sparse_tensor_kernel(builder, "tensor_sparse_f16_2to4", &desc, pu16,
                           pu16, pf32, pf32, pu8);
  add_sparse_tensor_matmul_kernel(
      builder, "tensor_matmul_sparse_f16_2to4_public", &desc, pu16, pu16,
      pf32, pf32, pu8, u32);

  /* Native low-precision support is expressed through the same neutral
   * descriptor used by every frontend. The PTX backend alone owns register
   * fragment packing, m16n8 subdivision, and the sm_89+ instruction choice. */
  desc = (MtlcTensorMmaDesc){0};
  desc.m = 16;
  desc.n = 16;
  desc.k = 32;
  desc.a_element = MTLC_TENSOR_ELEMENT_FLOAT8_E4M3;
  desc.b_element = MTLC_TENSOR_ELEMENT_FLOAT8_E5M2;
  desc.accumulator_element = desc.result_element =
      MTLC_TENSOR_ELEMENT_FLOAT32;
  desc.a_layout = desc.c_layout = desc.d_layout =
      MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  desc.b_layout = MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
  desc.a_leading_dimension = desc.b_leading_dimension = 32;
  desc.c_leading_dimension = desc.d_leading_dimension = 16;
  desc.scope = MTLC_MEMORY_SCOPE_SUBGROUP;
  add_tensor_matmul_kernel(builder, "tensor_matmul_fp8_public", &desc, pu8,
                           pu8, pf32, pf32, u32);
  add_tensor_kernel(builder, "tensor_fp8_m16n16k32", &desc, pu8, pu8, pf32,
                    pf32);
  add_tensor_chain_kernel(builder, "tensor_fp8_chain3_m16n16k32", &desc,
                          pu8, pu8, pf32, pf32);
  add_strided_tensor_loop_kernel(
      builder, "tensor_fp8_runtime_k_m16n16k32", &desc, 32, 0, pu8, pu8,
      pf32, pf32);

  desc = (MtlcTensorMmaDesc){0};
  desc.m = 32;
  desc.n = 24;
  desc.k = 16;
  desc.a_element = MTLC_TENSOR_ELEMENT_FLOAT8_E5M2;
  desc.b_element = MTLC_TENSOR_ELEMENT_FLOAT8_E4M3;
  desc.accumulator_element = desc.result_element =
      MTLC_TENSOR_ELEMENT_FLOAT32;
  desc.a_layout = desc.c_layout = desc.d_layout =
      MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
  desc.b_layout = MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  desc.transpose_a = desc.transpose_b = 1;
  desc.scope = MTLC_MEMORY_SCOPE_SUBGROUP;
  add_strided_tensor_kernel(builder,
                            "tensor_fp8_m32n24k16_transposed", &desc, pu8,
                            pu8, pf32, pf32);

  /* Packed storage and block scales are descriptor semantics, not PTX
   * intrinsics. This exact module is built entirely through libmtlc. */
  desc = (MtlcTensorMmaDesc){0};
  desc.m = 16;
  desc.n = 16;
  desc.k = 64;
  desc.a_element = desc.b_element = MTLC_TENSOR_ELEMENT_FLOAT4_E2M1;
  desc.accumulator_element = desc.result_element =
      MTLC_TENSOR_ELEMENT_FLOAT32;
  desc.a_layout = desc.c_layout = desc.d_layout =
      MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  desc.b_layout = MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
  desc.a_leading_dimension = desc.b_leading_dimension = 72;
  desc.c_leading_dimension = 20;
  desc.d_leading_dimension = 24;
  desc.a_scale_mode = desc.b_scale_mode = MTLC_TENSOR_SCALE_BLOCK_32;
  desc.a_scale_element = desc.b_scale_element =
      MTLC_TENSOR_ELEMENT_SCALE_UE8M0;
  desc.a_packing = desc.b_packing = MTLC_TENSOR_PACKING_DENSE_SUBBYTE;
  desc.a_scale_leading_dimension = desc.b_scale_leading_dimension = 3;
  desc.scope = MTLC_MEMORY_SCOPE_SUBGROUP;
  add_scaled_tensor_kernel(builder, "tensor_mxfp4_m16n16k64", &desc, pu8,
                           pu8, pf32, pf32, pu8);
  desc.c_leading_dimension = desc.d_leading_dimension = 24;
  add_scaled_tensor_chain_kernel(
      builder, "tensor_mxfp4_chain3_m16n16k64", &desc, pu8, pu8, pf32,
      pf32, pu8);
  desc.a_scale_leading_dimension = desc.b_scale_leading_dimension = 7;
  add_scaled_tensor_loop_kernel(
      builder, "tensor_mxfp4_runtime_k_m16n16k64", &desc, pu8, pf32, pu8,
      64, 1, 2, 32);

  /* The same public operation selects NVFP4 when its semantic scale grid is
   * UE4M3 block16. No PTX kind or selector leaks into the builder. */
  desc = (MtlcTensorMmaDesc){0};
  desc.m = 16;
  desc.n = 16;
  desc.k = 64;
  desc.a_element = desc.b_element = MTLC_TENSOR_ELEMENT_FLOAT4_E2M1;
  desc.accumulator_element = desc.result_element =
      MTLC_TENSOR_ELEMENT_FLOAT32;
  desc.a_layout = desc.c_layout = desc.d_layout =
      MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  desc.b_layout = MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
  desc.a_leading_dimension = desc.b_leading_dimension = 72;
  desc.c_leading_dimension = 20;
  desc.d_leading_dimension = 24;
  desc.a_scale_mode = desc.b_scale_mode = MTLC_TENSOR_SCALE_BLOCK_16;
  desc.a_scale_element = desc.b_scale_element =
      MTLC_TENSOR_ELEMENT_SCALE_UE4M3;
  desc.a_packing = desc.b_packing = MTLC_TENSOR_PACKING_DENSE_SUBBYTE;
  desc.a_scale_leading_dimension = desc.b_scale_leading_dimension = 5;
  desc.scope = MTLC_MEMORY_SCOPE_SUBGROUP;
  add_scaled_tensor_kernel(builder, "tensor_nvfp4_m16n16k64", &desc, pu8,
                           pu8, pf32, pf32, pu8);
  desc.c_leading_dimension = desc.d_leading_dimension = 24;
  add_scaled_tensor_chain_kernel(
      builder, "tensor_nvfp4_chain3_m16n16k64", &desc, pu8, pu8, pf32,
      pf32, pu8);
  desc.a_scale_leading_dimension = desc.b_scale_leading_dimension = 13;
  add_scaled_tensor_loop_kernel(
      builder, "tensor_nvfp4_runtime_k_m16n16k64", &desc, pu8, pf32, pu8,
      64, 1, 2, 16);

  /* Dense FP6 and mixed FP6 formats share the neutral packing/scale surface.
   * libmtlc describes six-bit logical values; PTX privately expands each
   * fragment into byte containers for Blackwell's mxf8f6f4 family. */
  desc = (MtlcTensorMmaDesc){0};
  desc.m = 16;
  desc.n = 16;
  desc.k = 32;
  desc.a_element = MTLC_TENSOR_ELEMENT_FLOAT6_E3M2;
  desc.b_element = MTLC_TENSOR_ELEMENT_FLOAT6_E2M3;
  desc.accumulator_element = desc.result_element =
      MTLC_TENSOR_ELEMENT_FLOAT32;
  desc.a_layout = desc.c_layout = desc.d_layout =
      MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  desc.b_layout = MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
  desc.a_leading_dimension = desc.b_leading_dimension = 36;
  desc.c_leading_dimension = 20;
  desc.d_leading_dimension = 24;
  desc.a_scale_mode = desc.b_scale_mode = MTLC_TENSOR_SCALE_BLOCK_32;
  desc.a_scale_element = desc.b_scale_element =
      MTLC_TENSOR_ELEMENT_SCALE_UE8M0;
  desc.a_packing = desc.b_packing = MTLC_TENSOR_PACKING_DENSE_SUBBYTE;
  desc.a_scale_leading_dimension = desc.b_scale_leading_dimension = 2;
  desc.scope = MTLC_MEMORY_SCOPE_SUBGROUP;
  add_scaled_tensor_kernel(builder, "tensor_mxfp6_m16n16k32", &desc, pu8,
                           pu8, pf32, pf32, pu8);
  desc.c_leading_dimension = desc.d_leading_dimension = 24;
  add_scaled_tensor_chain_kernel(
      builder, "tensor_mxfp6_chain3_m16n16k32", &desc, pu8, pu8, pf32,
      pf32, pu8);
  desc.a_scale_leading_dimension = desc.b_scale_leading_dimension = 4;
  add_scaled_tensor_loop_kernel(
      builder, "tensor_mxfp6_runtime_k_m16n16k32", &desc, pu8, pf32, pu8,
      32, 3, 4, 32);

  /* Assemble the full documented 5x5 A/B mxf8f6f4 type matrix. This guards
   * independent operand selection rather than extrapolating from one mixed
   * pair; all entries still use the same public neutral operation. */
  for (size_t a_kind = 0; a_kind < 5; a_kind++) {
    for (size_t b_kind = 0; b_kind < 5; b_kind++) {
      char name[96], matmul_name[112];
      snprintf(name, sizeof(name), "tensor_mxf8f6f4_%s_%s_m16n8k32",
               k_mxf8f6f4_element_names[a_kind],
               k_mxf8f6f4_element_names[b_kind]);
      snprintf(matmul_name, sizeof(matmul_name),
               "tensor_matmul_mxf8f6f4_%s_%s_public",
               k_mxf8f6f4_element_names[a_kind],
               k_mxf8f6f4_element_names[b_kind]);
      desc = (MtlcTensorMmaDesc){0};
      desc.m = 16;
      desc.n = 8;
      desc.k = 32;
      desc.a_element = k_mxf8f6f4_elements[a_kind];
      desc.b_element = k_mxf8f6f4_elements[b_kind];
      desc.accumulator_element = desc.result_element =
          MTLC_TENSOR_ELEMENT_FLOAT32;
      desc.a_layout = desc.c_layout = desc.d_layout =
          MTLC_TENSOR_LAYOUT_ROW_MAJOR;
      desc.b_layout = MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
      desc.a_leading_dimension = desc.b_leading_dimension = 32;
      desc.c_leading_dimension = desc.d_leading_dimension = 8;
      desc.a_scale_mode = desc.b_scale_mode = MTLC_TENSOR_SCALE_BLOCK_32;
      desc.a_scale_element = desc.b_scale_element =
          MTLC_TENSOR_ELEMENT_SCALE_UE8M0;
      desc.a_packing = a_kind >= 2 ? MTLC_TENSOR_PACKING_DENSE_SUBBYTE
                                  : MTLC_TENSOR_PACKING_LOGICAL;
      desc.b_packing = b_kind >= 2 ? MTLC_TENSOR_PACKING_DENSE_SUBBYTE
                                  : MTLC_TENSOR_PACKING_LOGICAL;
      desc.a_scale_leading_dimension = desc.b_scale_leading_dimension = 1;
      desc.scope = MTLC_MEMORY_SCOPE_SUBGROUP;
      add_scaled_tensor_kernel(builder, name, &desc, pu8, pu8, pf32, pf32,
                               pu8);
      MtlcTensorMmaDesc matmul_desc = desc;
      matmul_desc.a_scale_leading_dimension = 9;
      matmul_desc.b_scale_leading_dimension = 11;
      add_scaled_tensor_matmul_kernel(builder, matmul_name, &matmul_desc, pu8,
                                      pu8, pf32, pf32, pu8, u32);
    }
  }

  desc = mtlc_tensor_mma_f16_f32_m16n16k16_desc();
  desc.m = 8;
  desc.n = 32;
  desc.k = 16;
  desc.a_layout = MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
  desc.b_layout = MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  desc.c_layout = MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
  desc.d_layout = MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
  desc.a_leading_dimension = 8;
  desc.b_leading_dimension = 32;
  desc.c_leading_dimension = 8;
  desc.d_leading_dimension = 8;
  add_tensor_kernel(builder, "tensor_f16_f32_m8n32k16", &desc, pu16, pu16,
                    pf32, pf32);

  desc = mtlc_tensor_mma_f16_f32_m16n16k16_desc();
  desc.accumulator_element = MTLC_TENSOR_ELEMENT_FLOAT16;
  desc.result_element = MTLC_TENSOR_ELEMENT_FLOAT16;
  add_tensor_kernel(builder, "tensor_f16_f16_m16n16k16", &desc, pu16, pu16,
                    pu16, pu16);

  desc = mtlc_tensor_mma_f16_f32_m16n16k16_desc();
  desc.accumulator_element = MTLC_TENSOR_ELEMENT_FLOAT16;
  add_tensor_kernel(builder, "tensor_f16_acc_f32_result_m16n16k16", &desc,
                    pu16, pu16, pu16, pf32);

  desc = mtlc_tensor_mma_f16_f32_m16n16k16_desc();
  desc.result_element = MTLC_TENSOR_ELEMENT_FLOAT16;
  add_tensor_kernel(builder, "tensor_f32_acc_f16_result_m16n16k16", &desc,
                    pu16, pu16, pf32, pu16);

  desc = mtlc_tensor_mma_f16_f32_m16n16k16_desc();
  desc.m = 32;
  desc.n = 8;
  desc.a_element = MTLC_TENSOR_ELEMENT_BFLOAT16;
  desc.b_element = MTLC_TENSOR_ELEMENT_BFLOAT16;
  desc.a_layout = MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
  desc.b_layout = MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  desc.d_layout = MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
  desc.a_leading_dimension = 32;
  desc.b_leading_dimension = 8;
  desc.c_leading_dimension = 8;
  desc.d_leading_dimension = 32;
  add_tensor_kernel(builder, "tensor_bf16_f32_m32n8k16", &desc, pu16, pu16,
                    pf32, pf32);

  desc = mtlc_tensor_mma_f16_f32_m16n16k16_desc();
  desc.k = 8;
  desc.a_element = MTLC_TENSOR_ELEMENT_TFLOAT32;
  desc.b_element = MTLC_TENSOR_ELEMENT_TFLOAT32;
  desc.a_leading_dimension = 8;
  add_tensor_kernel(builder, "tensor_tf32_f32_m16n16k8", &desc, pf32, pf32,
                    pf32, pf32);

  desc = (MtlcTensorMmaDesc){0};
  desc.m = desc.n = 8;
  desc.k = 4;
  desc.a_element = desc.b_element = desc.accumulator_element =
      desc.result_element = MTLC_TENSOR_ELEMENT_FLOAT64;
  desc.a_layout = desc.c_layout = desc.d_layout =
      MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  desc.b_layout = MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
  desc.a_leading_dimension = 4;
  desc.b_leading_dimension = desc.c_leading_dimension =
      desc.d_leading_dimension = 8;
  desc.rounding = MTLC_TENSOR_ROUND_NEAREST_EVEN;
  desc.scope = MTLC_MEMORY_SCOPE_SUBGROUP;
  add_tensor_kernel(builder, "tensor_f64_m8n8k4", &desc, pf64, pf64, pf64,
                    pf64);

  desc = mtlc_tensor_mma_f16_f32_m16n16k16_desc();
  desc.a_element = desc.b_element = MTLC_TENSOR_ELEMENT_INT8;
  desc.accumulator_element = desc.result_element = MTLC_TENSOR_ELEMENT_INT32;
  desc.overflow = MTLC_TENSOR_OVERFLOW_SATURATE_FINITE;
  add_tensor_kernel(builder, "tensor_i8_i32_m16n16k16", &desc, pi8, pi8, pi32,
                    pi32);

  desc = (MtlcTensorMmaDesc){0};
  desc.m = desc.n = 8;
  desc.k = 32;
  desc.a_element = desc.b_element = MTLC_TENSOR_ELEMENT_UINT4;
  desc.accumulator_element = desc.result_element = MTLC_TENSOR_ELEMENT_INT32;
  desc.a_layout = desc.c_layout = desc.d_layout =
      MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  desc.b_layout = MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
  desc.a_leading_dimension = 32;
  desc.b_leading_dimension = 32;
  desc.c_leading_dimension = desc.d_leading_dimension = 8;
  desc.scope = MTLC_MEMORY_SCOPE_SUBGROUP;
  add_tensor_kernel(builder, "tensor_u4_i32_m8n8k32", &desc, pu8, pu8, pi32,
                    pi32);

  desc.k = 128;
  desc.a_element = desc.b_element = MTLC_TENSOR_ELEMENT_BIT1;
  desc.math_mode = MTLC_TENSOR_MATH_XOR_POPCOUNT;
  desc.a_leading_dimension = 128;
  desc.b_leading_dimension = 128;
  add_tensor_kernel(builder, "tensor_b1_xor_m8n8k128", &desc, pu8, pu8, pi32,
                    pi32);
  return mtlc_builder_finish(builder);
}

static int tensor_chain_rejects_invalid_connectivity(void) {
  const MtlcType *u16 = mtlc_type_scalar(MTLC_TYPE_UINT16);
  const MtlcType *f32 = mtlc_type_scalar(MTLC_TYPE_FLOAT32);
  const MtlcType *pu16 = mtlc_type_pointer_in(u16, MTLC_ADDRESS_SPACE_GLOBAL);
  const MtlcType *pf32 = mtlc_type_pointer_in(f32, MTLC_ADDRESS_SPACE_GLOBAL);
  const char *names[] = {"a", "b", "c", "d", "other"};
  const MtlcType *types[] = {pu16, pu16, pf32, pf32, pf32};
  MtlcTensorMmaDesc desc = mtlc_tensor_mma_f16_f32_m16n16k16_desc();
  MtlcBuilder *builder = mtlc_builder_create();
  MtlcFn *kernel = mtlc_builder_kernel(
      builder, "invalid_tensor_chain", names, types, 5);
  MtlcValue values[5];
  for (size_t i = 0; i < 5; i++) values[i] = mtlc_fn_param(kernel, i);
  MtlcTensorMmaOperands tiles[2] = {{0}};
  for (size_t i = 0; i < 2; i++) {
    tiles[i].a = values[0];
    tiles[i].b = values[1];
    tiles[i].c = values[i == 0 ? 2 : 3];
    tiles[i].d = values[i == 0 ? 3 : 4];
    tiles[i].metadata = MTLC_NO_VALUE;
    tiles[i].a_scale = MTLC_NO_VALUE;
    tiles[i].b_scale = MTLC_NO_VALUE;
  }
  mtlc_tensor_mma_chain(kernel, &desc, tiles, 2);
  mtlc_return(kernel, MTLC_NO_VALUE);
  return mtlc_builder_finish(builder) == NULL;
}

static int tensor_descriptor_rejects_invalid_scaling(void) {
  MtlcTensorMmaDesc desc = mtlc_tensor_mma_f16_f32_m16n16k16_desc();
  desc.a_scale_mode = MTLC_TENSOR_SCALE_BLOCK_32;
  if (mtlc_tensor_mma_desc_is_valid(&desc)) return 0;

  desc = mtlc_tensor_mma_f16_f32_m16n16k16_desc();
  desc.a_scale_element = MTLC_TENSOR_ELEMENT_SCALE_UE8M0;
  if (mtlc_tensor_mma_desc_is_valid(&desc)) return 0;

  desc = mtlc_tensor_mma_f16_f32_m16n16k16_desc();
  desc.a_packing = MTLC_TENSOR_PACKING_DENSE_SUBBYTE;
  if (mtlc_tensor_mma_desc_is_valid(&desc)) return 0;

  desc = (MtlcTensorMmaDesc){0};
  desc.m = desc.n = 16;
  desc.k = 64;
  desc.a_element = desc.b_element = MTLC_TENSOR_ELEMENT_FLOAT4_E2M1;
  desc.accumulator_element = desc.result_element =
      MTLC_TENSOR_ELEMENT_FLOAT32;
  desc.a_layout = desc.c_layout = desc.d_layout =
      MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  desc.b_layout = MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
  desc.a_leading_dimension = desc.b_leading_dimension = 64;
  desc.c_leading_dimension = desc.d_leading_dimension = 16;
  desc.a_scale_mode = desc.b_scale_mode = MTLC_TENSOR_SCALE_BLOCK_32;
  desc.a_scale_element = desc.b_scale_element =
      MTLC_TENSOR_ELEMENT_SCALE_UE8M0;
  desc.a_packing = desc.b_packing = MTLC_TENSOR_PACKING_DENSE_SUBBYTE;
  desc.a_scale_leading_dimension = 1;
  desc.scope = MTLC_MEMORY_SCOPE_SUBGROUP;
  if (mtlc_tensor_mma_desc_is_valid(&desc)) return 0;
  desc.a_scale_leading_dimension = 2;
  if (!mtlc_tensor_mma_desc_is_valid(&desc)) return 0;

  desc.a_scale_mode = desc.b_scale_mode = MTLC_TENSOR_SCALE_BLOCK_16;
  desc.a_scale_element = desc.b_scale_element =
      MTLC_TENSOR_ELEMENT_SCALE_UE4M3;
  desc.a_scale_leading_dimension = 3;
  if (mtlc_tensor_mma_desc_is_valid(&desc)) return 0;
  desc.a_scale_leading_dimension = 4;
  if (!mtlc_tensor_mma_desc_is_valid(&desc)) return 0;

  desc.a_element = MTLC_TENSOR_ELEMENT_SCALE_UE8M0;
  return !mtlc_tensor_mma_desc_is_valid(&desc);
}

static int file_starts_with(const char *path, const unsigned char *magic,
                            size_t n) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return 0;
  }
  unsigned char buf[8] = {0};
  size_t got = fread(buf, 1, n, f);
  fclose(f);
  return got == n && memcmp(buf, magic, n) == 0;
}

static int file_contains(const char *path, const char *needle) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return 0;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)n + 1);
  size_t got = fread(buf, 1, (size_t)n, f);
  buf[got] = 0;
  fclose(f);
  size_t needle_len = strlen(needle);
  int found = 0;
  for (size_t i = 0; needle_len <= got && i <= got - needle_len; i++) {
    if (memcmp(buf + i, needle, needle_len) == 0) {
      found = 1;
      break;
    }
  }
  free(buf);
  return found;
}

static size_t file_occurrences(const char *path, const char *needle) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)n + 1);
  if (!buf) {
    fclose(f);
    return 0;
  }
  size_t got = fread(buf, 1, (size_t)n, f);
  buf[got] = 0;
  fclose(f);
  const size_t needle_len = strlen(needle);
  size_t count = 0;
  for (size_t i = 0; needle_len && needle_len <= got &&
                     i <= got - needle_len;) {
    if (memcmp(buf + i, needle, needle_len) == 0) {
      count++;
      i += needle_len;
    } else {
      i++;
    }
  }
  free(buf);
  return count;
}

static int public_atomic_orders_are_checked(void) {
  const MtlcType *u32 = mtlc_type_scalar(MTLC_TYPE_UINT32);
  const MtlcType *voidt = mtlc_type_scalar(MTLC_TYPE_VOID);
  const MtlcType *pu32 =
      mtlc_type_pointer_in(u32, MTLC_ADDRESS_SPACE_GLOBAL);
  const char *names[] = {"counter"};
  const MtlcType *types[] = {pu32};

  MtlcBuilder *load_builder = mtlc_builder_create();
  MtlcFn *load_kernel =
      mtlc_builder_kernel(load_builder, "bad_load", names, types, 1);
  MtlcValue load_args[2] = {mtlc_fn_param(load_kernel, 0),
                            mtlc_const_int(load_kernel, u32, 0)};
  mtlc_intrinsic_memory(load_kernel, MTLC_INTRINSIC_GPU_ATOMIC_LOAD_U32,
                        load_args, 2, u32, MTLC_ADDRESS_SPACE_GLOBAL,
                        MTLC_MEMORY_ORDER_RELEASE,
                        MTLC_MEMORY_SCOPE_DEVICE);
  MtlcModule *bad_load = mtlc_builder_finish(load_builder);
  if (bad_load) {
    mtlc_module_destroy(bad_load);
    return 0;
  }

  MtlcBuilder *store_builder = mtlc_builder_create();
  MtlcFn *store_kernel =
      mtlc_builder_kernel(store_builder, "bad_store", names, types, 1);
  MtlcValue store_args[3] = {mtlc_fn_param(store_kernel, 0),
                             mtlc_const_int(store_kernel, u32, 0),
                             mtlc_const_int(store_kernel, u32, 1)};
  mtlc_intrinsic_memory(store_kernel, MTLC_INTRINSIC_GPU_ATOMIC_STORE_U32,
                        store_args, 3, voidt, MTLC_ADDRESS_SPACE_GLOBAL,
                        MTLC_MEMORY_ORDER_ACQUIRE,
                        MTLC_MEMORY_SCOPE_DEVICE);
  MtlcModule *bad_store = mtlc_builder_finish(store_builder);
  if (bad_store) {
    mtlc_module_destroy(bad_store);
    return 0;
  }
  return 1;
}

static int public_async_copy_contract_is_checked(void) {
  const MtlcType *f32 = mtlc_type_scalar(MTLC_TYPE_FLOAT32);
  const MtlcType *pf32 =
      mtlc_type_pointer_in(f32, MTLC_ADDRESS_SPACE_GLOBAL);
  const char *names[] = {"source"};
  const MtlcType *types[] = {pf32};
  MtlcBuilder *builder = mtlc_builder_create();
  MtlcFn *kernel =
      mtlc_builder_kernel(builder, "bad_async_copy", names, types, 1);
  MtlcValue destination = mtlc_address_space_alloc(
      kernel, "destination", f32, 1, MTLC_ADDRESS_SPACE_WORKGROUP);
  /* Four bytes cannot be represented by an eight-byte transaction. */
  mtlc_async_copy_workgroup(kernel, destination, mtlc_fn_param(kernel, 0),
                            f32, 1, 8, MTLC_ASYNC_CACHE_ALL);
  return mtlc_builder_finish(builder) == NULL;
}

/* ------------------------------------------------------- 7. ergonomics ---- */

/* Diagnostics are collected rather than printed, which is the whole point of
 * the handler: an embedder formats them itself. */
typedef struct {
  char last[512];
  int count;
} DiagSink;

static void diag_sink(void *user_data, MtlcDiagSeverity severity,
                      const char *message) {
  DiagSink *sink = (DiagSink *)user_data;
  if (severity == MTLC_DIAG_ERROR) {
    snprintf(sink->last, sizeof(sink->last), "%s", message);
    sink->count++;
  }
}

/* The layout mtlc_type_struct must compute for { int32; float64; int8 } under
 * the standard C rule: 0, 8 (padded to alignment 8), 16, size 24, align 8. */
static int composite_types_are_laid_out(void) {
  const MtlcType *i32 = mtlc_type_scalar(MTLC_TYPE_INT32);
  const MtlcType *f64 = mtlc_type_scalar(MTLC_TYPE_FLOAT64);
  const MtlcType *i8 = mtlc_type_scalar(MTLC_TYPE_INT8);
  const char *names[] = {"tag", "weight", "flag"};
  const MtlcType *types[] = {i32, f64, i8};
  const MtlcType *record = mtlc_type_struct("ErgRecord", names, types, 3);
  if (!record || record->kind != MTLC_TYPE_STRUCT) return 0;
  if (mtlc_type_size(record) != 24 || mtlc_type_alignment(record) != 8) return 0;
  if (mtlc_type_field_count(record) != 3) return 0;
  if (mtlc_type_field_offset(record, 0) != 0 ||
      mtlc_type_field_offset(record, 1) != 8 ||
      mtlc_type_field_offset(record, 2) != 16) {
    return 0;
  }
  if (mtlc_type_field_offset(record, 3) != (size_t)-1) return 0;
  if (mtlc_type_field_index(record, "weight") != 1 ||
      mtlc_type_field_index(record, "absent") != (size_t)-1) {
    return 0;
  }
  /* Interned by name: the same declaration is the same descriptor, and a
   * conflicting one is refused rather than silently shadowing. */
  if (mtlc_type_struct("ErgRecord", names, types, 3) != record) return 0;
  {
    const char *other_names[] = {"tag"};
    const MtlcType *other_types[] = {i32};
    if (mtlc_type_struct("ErgRecord", other_names, other_types, 1) != NULL) {
      return 0;
    }
  }

  const MtlcType *array = mtlc_type_array(i32, 4);
  if (!array || array->kind != MTLC_TYPE_ARRAY) return 0;
  if (mtlc_type_size(array) != 16 || mtlc_type_alignment(array) != 4) return 0;
  if (mtlc_type_array(i32, 4) != array) return 0;
  if (mtlc_type_array(i32, 0) != NULL || mtlc_type_array(NULL, 4) != NULL) {
    return 0;
  }

  const MtlcType *params[] = {i32, i32};
  const MtlcType *fp = mtlc_type_function_pointer(i32, params, 2);
  if (!fp || fp->kind != MTLC_TYPE_FUNCTION_POINTER) return 0;
  if (mtlc_type_size(fp) != 8 || fp->fn_param_count != 2) return 0;
  if (mtlc_type_function_pointer(i32, params, 2) != fp) return 0;
  if (mtlc_type_function_pointer(NULL, params, 2) != NULL) return 0;
  return 1;
}

/* A bad operator string, a bad label handle, and a branch to a label that was
 * never defined must each fail the builder with a message that names the
 * offending thing -- not produce a module that breaks later in codegen. */
static int builder_reports_its_errors(void) {
  const MtlcType *i64 = mtlc_type_scalar(MTLC_TYPE_INT64);

  /* 1. an operator the IR does not have */
  {
    DiagSink sink = {{0}, 0};
    MtlcBuilder *b = mtlc_builder_create();
    mtlc_builder_set_diagnostic_handler(b, diag_sink, &sink);
    MtlcFn *f = mtlc_builder_function(b, "main", i64, NULL, NULL, 0, 0);
    mtlc_binary(f, "<=>", mtlc_const_int(f, i64, 1), mtlc_const_int(f, i64, 2),
                i64);
    if (mtlc_builder_ok(b) || mtlc_fn_ok(f)) return 0;
    if (sink.count == 0 || !strstr(sink.last, "<=>")) return 0;
    if (!mtlc_builder_error(b) || !strstr(mtlc_builder_error(b), "mtlc_binary")) {
      return 0;
    }
    if (mtlc_builder_finish(b) != NULL) return 0;
  }

  /* 2. a label handle that was never allocated */
  {
    DiagSink sink = {{0}, 0};
    MtlcBuilder *b = mtlc_builder_create();
    mtlc_builder_set_diagnostic_handler(b, diag_sink, &sink);
    MtlcFn *f = mtlc_builder_function(b, "main", i64, NULL, NULL, 0, 0);
    mtlc_jump_to(f, 41);
    if (mtlc_builder_ok(b)) return 0;
    if (!strstr(sink.last, "mtlc_jump_to")) return 0;
    mtlc_builder_destroy(b);
  }

  /* 3. a branch to a label string that is never defined */
  {
    DiagSink sink = {{0}, 0};
    MtlcBuilder *b = mtlc_builder_create();
    mtlc_builder_set_diagnostic_handler(b, diag_sink, &sink);
    MtlcFn *f = mtlc_builder_function(b, "main", i64, NULL, NULL, 0, 0);
    mtlc_jump(f, "nowhere");
    mtlc_label(f, "elsewhere");
    mtlc_return(f, mtlc_const_int(f, i64, 0));
    if (!mtlc_builder_ok(b)) return 0; /* only detectable at finish */
    if (mtlc_builder_finish(b) != NULL) return 0;
    if (!strstr(sink.last, "nowhere") || !strstr(sink.last, "main")) return 0;
  }

  /* 4. placing the same allocated label twice */
  {
    DiagSink sink = {{0}, 0};
    MtlcBuilder *b = mtlc_builder_create();
    mtlc_builder_set_diagnostic_handler(b, diag_sink, &sink);
    MtlcFn *f = mtlc_builder_function(b, "main", i64, NULL, NULL, 0, 0);
    MtlcLabel twice = mtlc_label_new(f, "twice");
    mtlc_label_here(f, twice);
    mtlc_label_here(f, twice);
    if (mtlc_builder_ok(b)) return 0;
    if (!strstr(sink.last, "already placed")) return 0;
    mtlc_builder_destroy(b);
  }

  /* 5. indexing through something that is not a pointer */
  {
    DiagSink sink = {{0}, 0};
    MtlcBuilder *b = mtlc_builder_create();
    mtlc_builder_set_diagnostic_handler(b, diag_sink, &sink);
    MtlcFn *f = mtlc_builder_function(b, "main", i64, NULL, NULL, 0, 0);
    if (mtlc_element_address(f, mtlc_const_int(f, i64, 0), i64,
                             mtlc_const_int(f, i64, 1)) != MTLC_NO_VALUE) {
      return 0;
    }
    if (mtlc_builder_ok(b) || !strstr(sink.last, "not a pointer")) return 0;
    mtlc_builder_destroy(b);
  }

  /* 6. a missing index, including at a stride of one where the scaling
   *    multiply that would otherwise catch it is skipped. */
  {
    const MtlcType *pi8 = mtlc_type_pointer(mtlc_type_scalar(MTLC_TYPE_INT8));
    DiagSink sink = {{0}, 0};
    MtlcBuilder *b = mtlc_builder_create();
    mtlc_builder_set_diagnostic_handler(b, diag_sink, &sink);
    MtlcFn *f = mtlc_builder_function(b, "main", i64, NULL, NULL, 0, 0);
    if (mtlc_element_address(f, mtlc_const_int(f, i64, 0), pi8,
                             MTLC_NO_VALUE) != MTLC_NO_VALUE) {
      return 0;
    }
    if (mtlc_builder_ok(b) || !strstr(sink.last, "index")) return 0;
    mtlc_builder_destroy(b);
  }
  return 1;
}

/* A failing pipeline call must leave a retrievable message on the context. */
static int context_reports_pipeline_errors(void) {
  DiagSink sink = {{0}, 0};
  MtlcContext *ctx = mtlc_context_create();
  if (!ctx) return 0;
  if (mtlc_context_last_error(ctx) != NULL) return 0;

  if (mtlc_emit_object(ctx, NULL, "unused.obj")) return 0;
  const char *recorded = mtlc_context_last_error(ctx);
  if (!recorded || !strstr(recorded, "mtlc_emit_object")) return 0;

  mtlc_context_clear_error(ctx);
  if (mtlc_context_last_error(ctx) != NULL) return 0;

  /* With a handler installed the message goes there instead of stderr. */
  mtlc_context_set_diagnostic_handler(ctx, diag_sink, &sink);
  if (mtlc_emit(ctx, NULL, MTLC_ARCH_PTX, "unused.ptx")) return 0;
  if (sink.count != 1 || !strstr(sink.last, "mtlc_emit")) return 0;
  if (strcmp(mtlc_diag_severity_name(MTLC_DIAG_ERROR), "error") != 0 ||
      strcmp(mtlc_diag_severity_name(MTLC_DIAG_WARNING), "warning") != 0 ||
      strcmp(mtlc_diag_severity_name(MTLC_DIAG_NOTE), "note") != 0) {
    return 0;
  }
  mtlc_context_destroy(ctx);
  return 1;
}

/* A whole program built with the convenience surface only: allocated labels
 * for the loop, operator enums for the arithmetic, element addressing over a
 * malloc'd array, and field addressing through a struct pointer.
 *
 *   sum = 0; for (i = 0; i < 5; i++) { a[i] = i * i; sum += a[i]; }  -> 30
 *   r->tag = sum; r->flag = 47;  return r->tag + r->flag;            -> 77
 */
static MtlcModule *build_ergonomics_module(void) {
  const MtlcType *i32 = mtlc_type_scalar(MTLC_TYPE_INT32);
  const MtlcType *i64 = mtlc_type_scalar(MTLC_TYPE_INT64);
  const MtlcType *i8 = mtlc_type_scalar(MTLC_TYPE_INT8);
  const MtlcType *f64 = mtlc_type_scalar(MTLC_TYPE_FLOAT64);
  const MtlcType *pi64 = mtlc_type_pointer(i64);

  const char *field_names[] = {"tag", "weight", "flag"};
  const MtlcType *field_types[] = {i32, f64, i8};
  const MtlcType *record = mtlc_type_struct("ErgRecord", field_names,
                                            field_types, 3);
  const MtlcType *precord = mtlc_type_pointer(record);
  if (!record || !precord) return NULL;

  MtlcBuilder *b = mtlc_builder_create();
  if (!b) return NULL;

  {
    const MtlcType *pt[] = {i64};
    if (!mtlc_builder_declare_function(b, "malloc", pi64, NULL, pt, 1)) {
      mtlc_builder_destroy(b);
      return NULL;
    }
  }

  MtlcFn *m = mtlc_builder_function(b, "main", i64, NULL, NULL, 0, 0);
  if (!m) {
    mtlc_builder_destroy(b);
    return NULL;
  }

  /* int64 a[5], via malloc: element addressing does the stride arithmetic. */
  MtlcValue bytes = mtlc_const_int(m, i64, 5 * 8);
  MtlcValue array = mtlc_call(m, "malloc", &bytes, 1, pi64);

  MtlcValue i = mtlc_local(m, "i", i64);
  MtlcValue sum = mtlc_local(m, "sum", i64);
  mtlc_assign(m, i, mtlc_const_int(m, i64, 0));
  mtlc_assign(m, sum, mtlc_const_int(m, i64, 0));

  MtlcLabel top = mtlc_label_new(m, "loop_top");
  MtlcLabel done = mtlc_label_new(m, "loop_done");
  mtlc_label_here(m, top);
  MtlcValue more = mtlc_binary_op(m, MTLC_BINOP_LT, i,
                                  mtlc_const_int(m, i64, 5), i64);
  mtlc_branch_if_zero_to(m, more, done);

  mtlc_store_element(m, array, pi64, i,
                     mtlc_binary_op(m, MTLC_BINOP_MUL, i, i, i64));
  mtlc_assign(m, sum,
              mtlc_binary_op(m, MTLC_BINOP_ADD, sum,
                             mtlc_load_element(m, array, pi64, i), i64));
  mtlc_assign(m, i,
              mtlc_binary_op(m, MTLC_BINOP_ADD, i, mtlc_const_int(m, i64, 1),
                             i64));
  mtlc_jump_to(m, top);
  mtlc_label_here(m, done);

  /* A struct through a pointer: offsets come from the computed layout. */
  MtlcValue record_bytes = mtlc_const_int(m, i64, (long long)record->size);
  MtlcValue r = mtlc_cast(m, mtlc_call(m, "malloc", &record_bytes, 1, pi64),
                          precord);
  mtlc_store_field(m, r, precord, 0, mtlc_cast(m, sum, i32));   /* tag  = 30 */
  mtlc_store_field(m, r, precord, 2, mtlc_const_int(m, i8, 47)); /* flag = 47 */

  MtlcValue tag = mtlc_cast(m, mtlc_load_field(m, r, precord, 0), i64);
  MtlcValue flag = mtlc_cast(m, mtlc_load_field(m, r, precord, 2), i64);
  mtlc_return(m, mtlc_binary_op(m, MTLC_BINOP_ADD, tag, flag, i64));

  if (!mtlc_builder_ok(b)) {
    fprintf(stderr, "public_api_test: ergonomics build: %s\n",
            mtlc_builder_error(b));
    mtlc_builder_destroy(b);
    return NULL;
  }
  return mtlc_builder_finish(b);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <outdir>\n", argv[0]);
    return 2;
  }
  const char *outdir = argv[1];
  MtlcContext *ctx = mtlc_context_create();
  if (!ctx || strcmp(mtlc_context_ptx_target(ctx), "sm_121a") != 0 ||
      mtlc_context_ptx_isa_major(ctx) != 8 ||
      mtlc_context_ptx_isa_minor(ctx) != 8 ||
      mtlc_context_ptx_tensor_tuple_budget(ctx) != 0) {
    return fail("default GB10 PTX context profile");
  }
  const char *runtime_dir = getenv("MTLC_RUNTIME_DIR");
  if (!runtime_dir || !runtime_dir[0]) {
    runtime_dir = "bin/runtime";
  }
  if (!mtlc_context_set_runtime_directory(ctx, runtime_dir) ||
      strcmp(mtlc_context_runtime_directory(ctx), runtime_dir) != 0) {
    return fail("freestanding runtime context path");
  }
  if (mtlc_context_set_ptx_target(ctx, "sm_bad", 8, 8)) {
    return fail("malformed PTX target accepted");
  }
  if (!mtlc_context_set_ptx_tensor_tuple_budget(ctx, 31) ||
      mtlc_context_ptx_tensor_tuple_budget(ctx) != 31 ||
      mtlc_context_set_ptx_tensor_tuple_budget(ctx, -1) ||
      mtlc_context_ptx_tensor_tuple_budget(ctx) != 31 ||
      !mtlc_context_set_ptx_tensor_tuple_budget(ctx, 0)) {
    return fail("PTX tensor tuple-budget context policy");
  }
  mtlc_context_set_opt_level(ctx, 1);
  mtlc_context_set_whole_program(ctx, 1);
  if (!public_atomic_orders_are_checked()) {
    return fail("public atomic load/store order validation");
  }
  if (!public_async_copy_contract_is_checked()) {
    return fail("public async-copy contract validation");
  }

  /* 1. native executable (optimized) */
  {
    MtlcModule *m = build_native_module();
    if (!m) {
      return fail("native module construction");
    }
    if (!mtlc_optimize(ctx, m)) {
      return fail("mtlc_optimize");
    }
    char *exe = path_join(outdir, "pubapi_native.exe");
    if (!mtlc_build_executable(ctx, m, exe)) {
      return fail("mtlc_build_executable");
    }
    printf("native: %s\n", exe);
    free(exe);
    mtlc_module_destroy(m);
  }

  /* 2. PTX + SPIR-V from the same target-neutral optimized kernel module. */
  {
    MtlcModule *m = build_gpu_module();
    if (!m) {
      return fail("gpu module construction");
    }
    if (!mtlc_optimize_for(ctx, m, MTLC_ARCH_PTX)) {
      return fail("mtlc_optimize_for GPU");
    }
    char *ptx = path_join(outdir, "pubapi_kernel.ptx");
    char *ptx_portable = path_join(outdir, "pubapi_kernel_compute75.ptx");
    char *spv = path_join(outdir, "pubapi_kernel.spv");
    if (!mtlc_emit(ctx, m, MTLC_ARCH_PTX, ptx)) {
      return fail("mtlc_emit PTX");
    }
    if (!file_contains(ptx, ".visible .entry scale2")) {
      return fail("PTX output missing the kernel entry");
    }
    if (!file_contains(ptx, "%tid.x")) {
      return fail("PTX output missing the typed local-id intrinsic");
    }
    if (!file_contains(ptx, "%laneid") ||
        !file_contains(ptx, "activemask.b32") ||
        !file_contains(ptx, "shfl.sync.idx.b32") ||
        !file_contains(ptx, "shfl.sync.down.b32") ||
        !file_contains(ptx, "shfl.sync.up.b32") ||
        !file_contains(ptx, "vote.sync.ballot.b32") ||
        !file_contains(ptx, "vote.sync.any.pred") ||
        !file_contains(ptx, "vote.sync.all.pred") ||
        !file_contains(ptx, "min.f32") ||
        !file_contains(ptx, "min.u32") ||
        !file_contains(ptx, "max.f32") ||
        !file_contains(ptx, "max.u32")) {
      return fail("PTX output missing semantic subgroup collectives");
    }
    if (!file_contains(ptx, ".ptr.global") ||
        !file_contains(ptx, ".shared .align 32 .b8 scale2_tile_storage[128]") ||
        !file_contains(ptx,
                       ".extern .shared .align 32 .b8 "
                       "scale2_dynamic_workgroup_storage[]") ||
        !file_contains(ptx, ".local .align 4 .b8 scale2_scratch_storage[16]") ||
        !file_contains(ptx, "st.shared.f32") ||
        !file_contains(ptx, "ld.shared.f32") ||
        !file_contains(ptx, "st.local.u32") ||
        !file_contains(ptx, "ld.local.u32") ||
        !file_contains(ptx, "atom.relaxed.cta.global.add.u32") ||
        !file_contains(ptx, "atom.acquire.cta.global.add.u32") ||
        !file_contains(ptx, "atom.release.cta.global.add.u32") ||
        !file_contains(ptx, "atom.acq_rel.sys.global.add.u32") ||
        !file_contains(ptx, "fence.sc.gpu") ||
        !file_contains(ptx, "atom.acquire.gpu.global.add.u32") ||
        !file_contains(ptx, "neg.s64") ||
        !file_contains(ptx, ".global.add.u64") ||
        !file_contains(ptx, ".global.max.u64") ||
        !file_contains(ptx, ".global.and.b64") ||
        !file_contains(ptx, ".global.or.b64") ||
        !file_contains(ptx, ".global.xor.b64") ||
        !file_contains(ptx, ".global.exch.b64") ||
        !file_contains(ptx, ".global.cas.b32") ||
        !file_contains(ptx, ".global.cas.b64") ||
        !file_contains(ptx, "ld.acquire.gpu.global.u32") ||
        !file_contains(ptx, "ld.acquire.sys.global.u64") ||
        !file_contains(ptx, "st.release.gpu.global.u32") ||
        !file_contains(ptx, "st.relaxed.sys.global.u64") ||
        !file_contains(ptx, "ld.relaxed.cta.shared.u32") ||
        !file_contains(ptx, "ld.acquire.cta.shared.u64") ||
        !file_contains(ptx, "st.relaxed.cta.shared.u32") ||
        !file_contains(ptx, "st.release.cta.shared.u64")) {
      return fail("PTX output missing explicit address-space/atomic semantics");
    }
    if (!file_contains(ptx, "cp.async.ca.shared.global") ||
        !file_contains(ptx, "cp.async.commit_group") ||
        !file_contains(ptx, "cp.async.wait_group 0")) {
      return fail("PTX output missing native neutral async staging");
    }
    if (!file_contains(ptx, ".visible .entry auto_stage_public") ||
        !file_contains(ptx, "mtlc.async_copy auto-promoted native")) {
      return fail("public ordinary IR did not receive shared auto-staging");
    }
    if (!file_contains(ptx, ".func (.param .f32 scale_value_ret) scale_value") ||
        !file_contains(ptx, ".func (.param .f32 load_scaled_ret) load_scaled") ||
         !file_contains(ptx, ".func store_value") ||
         !file_contains(ptx, ".func (.param .b32 identity_i8_ret) identity_i8") ||
         !file_contains(ptx, ".func (.param .b32 identity_u16_ret) identity_u16") ||
         !file_contains(ptx, ".func conditional_reduce") ||
        !file_contains(ptx, "call.uni") ||
        !file_contains(ptx, "scale_value")) {
      return fail("PTX output missing a reachable device helper call");
    }
    if (file_contains(ptx, "ordinary_not_entry")) {
      return fail("PTX emitted an unreachable ordinary function");
    }
    if (!file_contains(ptx, ".version 8.8") ||
        !file_contains(ptx, ".target sm_121a")) {
      return fail("PTX output missing the default GB10 profile");
    }
    if (!mtlc_context_set_ptx_target(ctx, "compute_75", 6, 4) ||
        !mtlc_emit(ctx, m, MTLC_ARCH_PTX, ptx_portable) ||
        !file_contains(ptx_portable, ".version 6.4") ||
        !file_contains(ptx_portable, ".target compute_75") ||
        !file_contains(ptx_portable,
                       "mtlc.async_copy synchronous-fallback") ||
        !file_contains(ptx_portable,
                       "mtlc.async_copy auto-promoted synchronous-fallback") ||
        file_contains(ptx_portable, "cp.async.")) {
      return fail("configurable PTX context profile");
    }
    if (!mtlc_emit(ctx, m, MTLC_ARCH_SPIRV, spv)) {
      return fail("mtlc_emit SPIR-V");
    }
    const unsigned char spv_magic[4] = {0x03, 0x02, 0x23, 0x07}; /* LE */
    if (!file_starts_with(spv, spv_magic, 4)) {
      return fail("SPIR-V output missing the module magic");
    }
    if (!spirv_has_atomic_contract(spv, 4u, 0x200u) ||
        !spirv_has_atomic_contract(spv, 3u, 0x202u) ||
        !spirv_has_atomic_contract(spv, 2u, 0x204u) ||
        !spirv_has_atomic_contract(spv, 0u, 0x208u) ||
        !spirv_has_atomic_contract(spv, 1u, 0x210u) ||
        !spirv_has_compare_exchange_contract(spv, 1u, 0x208u, 0x202u) ||
        !spirv_has_compare_exchange_contract(spv, 0u, 0x210u, 0x210u)) {
      return fail("SPIR-V output missing explicit atomic order/scope semantics");
    }
    if (spirv_count_opcode(spv, 227u) != 4u ||
        spirv_count_opcode(spv, 228u) != 4u ||
        spirv_count_opcode(spv, 229u) < 2u ||
        spirv_count_opcode(spv, 230u) != 2u ||
        spirv_count_opcode(spv, 235u) < 2u ||
        spirv_count_opcode(spv, 237u) < 2u ||
        spirv_count_opcode(spv, 239u) < 2u ||
        spirv_count_opcode(spv, 240u) < 2u ||
        spirv_count_opcode(spv, 241u) < 2u ||
        spirv_count_opcode(spv, 242u) < 2u) {
      return fail("SPIR-V output missing broad u32/u64 atomic family");
    }
    if (spirv_count_opcode(spv, 28u) < 2u ||
        spirv_count_array_variable_storage(spv, 4u) != 2u ||
        spirv_count_array_variable_storage(spv, 7u) != 1u ||
        spirv_count_pointer_parameters(spv, 4u) != 1u) {
      return fail("SPIR-V output missing exact workgroup/private allocation");
    }
    if (!spirv_has_barrier_contract(spv, 0x308u)) {
      return fail("SPIR-V output missing explicit barrier memory semantics");
    }
    if (spirv_count_opcode(spv, 263u) != 2u ||
        spirv_count_opcode(spv, 264u) != 4u ||
        spirv_count_opcode(spv, 265u) != 3u ||
        spirv_count_group_operation(spv, 264u, 0u) != 2u ||
        spirv_count_group_operation(spv, 264u, 1u) != 1u ||
        spirv_count_group_operation(spv, 264u, 2u) != 1u ||
        spirv_count_group_operation(spv, 265u, 0u) != 1u ||
        spirv_count_group_operation(spv, 265u, 1u) != 1u ||
        spirv_count_group_operation(spv, 265u, 2u) != 1u ||
        spirv_count_opcode(spv, 266u) != 1u ||
        spirv_count_opcode(spv, 267u) != 1u ||
        spirv_count_opcode(spv, 269u) != 1u ||
        spirv_count_opcode(spv, 270u) != 1u ||
        spirv_count_opcode(spv, 4421u) != 1u ||
        spirv_count_opcode(spv, 4428u) != 1u ||
        spirv_count_opcode(spv, 4429u) != 1u ||
        !spirv_has_capability(spv, 18u) ||
        !spirv_has_capability(spv, 4423u) ||
        !spirv_has_capability(spv, 4431u)) {
      return fail("SPIR-V output missing semantic subgroup collectives");
    }
    /* OpFunction=54: six helpers + two kernels; OpFunctionCall=57: one
     * transitive and five scale2 calls. The unreachable function is absent. */
    if (spirv_count_opcode(spv, 54u) != 8u ||
        spirv_count_opcode(spv, 57u) != 6u) {
      return fail("SPIR-V device reachability/function-call contract");
    }
    printf("gpu: %s, %s\n", ptx, spv);
    free(ptx);
    free(ptx_portable);
    free(spv);
    mtlc_module_destroy(m);
  }

  /* 2b. Neutral variable-source shuffle with target capability selection. */
  {
    MtlcModule *m = build_subgroup_shuffle_module();
    char *ptx = path_join(outdir, "pubapi_subgroup_shuffle_sm121a.ptx");
    char *spv = path_join(outdir, "pubapi_subgroup_shuffle_unsupported.spv");
    if (!m || !mtlc_optimize_for(ctx, m, MTLC_ARCH_PTX) ||
        !mtlc_context_set_ptx_target(ctx, "sm_121a", 8, 8) ||
        !mtlc_emit(ctx, m, MTLC_ARCH_PTX, ptx) ||
        file_occurrences(ptx, "shfl.sync.idx.b32") != 2) {
      return fail("public neutral subgroup shuffle PTX construction");
    }
    /* The neutral operation is not weakened into uniform broadcast. The
     * current OpenCL 2.0 profile rejects it until a non-uniform-shuffle
     * capability profile is selected. */
    if (mtlc_emit(ctx, m, MTLC_ARCH_SPIRV, spv)) {
      return fail("SPIR-V OpenCL 2.0 silently accepted non-uniform shuffle");
    }
    free(ptx);
    free(spv);
    mtlc_module_destroy(m);
  }

  /* 3. AArch64 relocatable ELF object after scalar target-neutral passes. */
  {
    MtlcModule *m = build_arm64_module();
    if (!m) {
      return fail("arm64 module construction");
    }
    if (!mtlc_optimize_for(ctx, m, MTLC_ARCH_ARM64)) {
      return fail("mtlc_optimize_for ARM64");
    }
    char *elf = path_join(outdir, "pubapi_arm64.elf");
    if (!mtlc_emit(ctx, m, MTLC_ARCH_ARM64, elf)) {
      return fail("mtlc_emit ARM64");
    }
    if (!is_arm64_relocatable_with_native_relocs(elf, 1)) {
      return fail("ARM64 object identity/relocations");
    }
    printf("arm64: %s\n", elf);
    free(elf);
    mtlc_module_destroy(m);
  }

  /* 4. Typed semantic launch lowered to a provider-neutral host ABI. */
  {
    MtlcModule *m = build_launch_module();
    if (!m) {
      return fail("launch module construction");
    }
    if (!mtlc_optimize_for(ctx, m, MTLC_ARCH_ARM64)) {
      return fail("mtlc_optimize_for ARM64 launch");
    }
    char *obj = path_join(outdir, "pubapi_launch.obj");
    char *arm_obj = path_join(outdir, "pubapi_launch_arm64.o");
    if (!mtlc_emit(ctx, m, MTLC_ARCH_ARM64, arm_obj) ||
        !is_arm64_relocatable_with_native_relocs(arm_obj, 0) ||
        !file_contains(arm_obj, "mtlc_gpu_launch_checked")) {
      return fail("AArch64 semantic launch object emission");
    }
    if (!mtlc_emit_object(ctx, m, obj)) {
      return fail("semantic launch object emission");
    }
    if (!file_contains(obj, "mtlc_gpu_launch_checked")) {
      return fail("launch object missing checked runtime-provider symbol");
    }
    printf("launch: %s, %s\n", obj, arm_obj);
    free(obj);
    free(arm_obj);
    mtlc_module_destroy(m);
  }

  /* 5. The shared tensor contract spans the stable WMMA family; PTX chooses
   * instruction spellings only after checking the requested target profile. */
  {
    if (!tensor_chain_rejects_invalid_connectivity()) {
      return fail("tensor chain accepted disconnected output");
    }
    if (!tensor_descriptor_rejects_invalid_scaling()) {
      return fail("tensor descriptor accepted invalid packing/scaling");
    }
    if (!tensor_epilogue_descriptor_validation()) {
      return fail("tensor epilogue descriptor validation");
    }
    MtlcModule *m = build_tensor_module();
    char *ptx = path_join(outdir, "pubapi_tensor_sm121a.ptx");
    char *portable = path_join(outdir, "pubapi_tensor_unsupported_compute75.ptx");
    char *spv = path_join(outdir, "pubapi_tensor_unsupported.spv");
    char *budgeted = path_join(outdir, "pubapi_tensor_budget31_sm121a.ptx");
    if (!m) return fail("broad tensor module construction");
    if (!mtlc_optimize_for(ctx, m, MTLC_ARCH_PTX)) {
      return fail("mtlc_optimize_for tensor GPU");
    }
    if (!mtlc_context_set_ptx_target(ctx, "compute_75", 6, 4) ||
        mtlc_emit(ctx, m, MTLC_ARCH_PTX, portable)) {
      return fail("unsupported tensor profile was silently accepted on sm_75");
    }
    if (mtlc_emit(ctx, m, MTLC_ARCH_SPIRV, spv)) {
      return fail("SPIR-V OpenCL 2.0 silently accepted tensor MMA");
    }
    if (!mtlc_context_set_ptx_target(ctx, "sm_121a", 8, 8) ||
        !mtlc_emit(ctx, m, MTLC_ARCH_PTX, ptx)) {
      return fail("broad tensor module PTX emission");
    }
    if (!file_contains(ptx, ".entry tensor_f16_f32_strided") ||
        !file_contains(ptx, ".entry tensor_epilogue_public") ||
        !file_contains(ptx, ".entry tensor_epilogue_fused_public") ||
        !file_contains(
            ptx,
            "mtlc.tensor_epilogue resident stable-wmma tiles=1 subtiles=1") ||
        !file_contains(ptx,
                       "mtlc.tensor_epilogue cooperative-memory m=19 n=37") ||
        !file_contains(ptx, "setp.lt.f32") ||
        !file_contains(ptx, "setp.gt.f32") ||
        !file_contains(ptx, "selp.f32") ||
        !file_contains(ptx, ".entry tensor_f16_f32_chain3") ||
        !file_contains(ptx, ".entry tensor_tiled_f16_m32n32") ||
        !file_contains(ptx, ".entry tensor_tiled_f16_chain3_m32n32") ||
        !file_contains(ptx, ".entry tensor_f16_f32_strided_chain3") ||
        !file_contains(ptx, ".entry tensor_f16_f32_runtime_k") ||
         !file_contains(ptx, ".entry tensor_pipeline_public") ||
         !file_contains(ptx, ".entry tensor_sparse_f16_2to4") ||
         !file_contains(ptx,
                        ".entry tensor_matmul_sparse_f16_2to4_public") ||
        !file_contains(ptx, ".entry tensor_fp8_m16n16k32") ||
        !file_contains(ptx, ".entry tensor_matmul_fp8_public") ||
        !file_contains(ptx, ".entry tensor_fp8_chain3_m16n16k32") ||
        !file_contains(ptx, ".entry tensor_fp8_runtime_k_m16n16k32") ||
        !file_contains(ptx, ".entry tensor_fp8_m32n24k16_transposed") ||
        !file_contains(ptx, ".entry tensor_mxfp4_m16n16k64") ||
        !file_contains(ptx, ".entry tensor_mxfp4_chain3_m16n16k64") ||
        !file_contains(ptx, ".entry tensor_mxfp4_runtime_k_m16n16k64") ||
        !file_contains(ptx, ".entry tensor_nvfp4_m16n16k64") ||
         !file_contains(ptx, ".entry tensor_nvfp4_chain3_m16n16k64") ||
         !file_contains(ptx, ".entry tensor_nvfp4_runtime_k_m16n16k64") ||
         !file_contains(ptx, ".entry tensor_mxfp6_m16n16k32") ||
         !file_contains(ptx, ".entry tensor_mxfp6_chain3_m16n16k32") ||
         !file_contains(ptx, ".entry tensor_mxfp6_runtime_k_m16n16k32") ||
        !file_contains(ptx,
                       "mtlc.tensor_chain resident tiles=3 tuple_peak=32") ||
        !file_contains(ptx,
                       "mtlc.tensor_loop resident group=1 tuple_peak=32") ||
        !file_contains(ptx,
                       "mtlc.tensor_pipeline resident group=1 tuple_peak=32") ||
        !file_contains(
            ptx,
            "mtlc.tensor_mma tiled logical=m32n32k16 physical=m16n16k16 subtiles=4 reuse=A") ||
        !file_contains(
            ptx,
            "mtlc.tensor_chain resident tiles=3 subtiles=4 tuple_peak=56 budget=96") ||
        !file_contains(ptx, "cp.async.cg.shared.global") ||
        !file_contains(ptx,
                       ".shared .align 32 .b8 tensor_pipeline_public_a_stage_storage[2048]") ||
        !file_contains(ptx, "cp.async.wait_group 3") ||
        file_occurrences(ptx, "cp.async.cg.shared.global") < 8 ||
        file_occurrences(ptx, "cp.async.commit_group") < 4 ||
        !file_contains(ptx, ".entry tensor_matmul_public") ||
        !file_contains(ptx, ".entry tensor_matmul_transpose_public") ||
        !file_contains(ptx,
                       "mtlc.tensor_matmul native interior runtime-K resident stable-wmma") ||
        !file_contains(ptx,
                       "mtlc.tensor_matmul cooperative-full exact M/N/K edge replay") ||
        !file_contains(ptx,
                       "mtlc.tensor_matmul cooperative-tail exact M/N/K edge replay") ||
        !file_contains(ptx, "wmma.mma.sync.aligned.m16n16k16") ||
        !file_contains(ptx,
                       "wmma.mma.sync.aligned.m16n16k16.col.row.f32.f32") ||
        !file_contains(ptx, "wmma.mma.sync.aligned.m8n32k16") ||
        !file_contains(ptx, ".f32.bf16.bf16.f32") ||
        !file_contains(ptx, "cvt.rn.f16x2.e4m3x2") ||
        !file_contains(ptx, "cvt.rn.f16x2.e5m2x2") ||
        !file_contains(ptx, ".f32.tf32.tf32.f32") ||
        !file_contains(ptx, ".f64.f64.f64.f64") ||
        !file_contains(ptx, ".s32.s8.s8.s32.satfinite") ||
        !file_contains(ptx, ".s32.u4.u4.s32") ||
         !file_contains(ptx, "wmma.mma.xor.popc")) {
      return fail("PTX output missing stable tensor profile coverage");
    }
    if (file_occurrences(
            ptx,
            "mma.sp::ordered_metadata.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32") != 6 ||
        !file_contains(
            ptx,
            "mtlc.tensor_mma native-mma sparse-f16-2to4 whole-tile lowering") ||
        file_occurrences(ptx, "popc.b32") < 8) {
      return fail("public tensor API missing canonical structured-sparse lowering");
    }
    if (file_occurrences(
            ptx,
            "mma.sync.aligned.m16n8k32.row.col.f32.e4m3.e5m2.f32") != 16 ||
        file_occurrences(
            ptx,
            "mma.sync.aligned.m16n8k16.row.col.f32.e5m2.e4m3.f32") != 6 ||
        file_occurrences(
            ptx,
            "mtlc.tensor_mma native-mma fp8 whole-tile lowering") != 2 ||
        !file_contains(
            ptx,
            "mtlc.tensor_chain resident native-mma fp8 tiles=3 subtiles=2") ||
        !file_contains(
            ptx,
            "mtlc.tensor_loop resident native-mma fp8 group=1 subtiles=2") ||
        file_occurrences(
            ptx,
            "mtlc.tensor_matmul native interior runtime-K resident direct-mma") != 27 ||
        file_occurrences(
            ptx,
            "mtlc.tensor_matmul cooperative-full exact M/N/K edge replay") != 29 ||
        file_occurrences(
            ptx,
            "mtlc.tensor_matmul cooperative-tail exact M/N/K edge replay") != 29 ||
        file_occurrences(ptx, "cvt.rn.f16x2.e4m3x2") != 22 ||
        file_occurrences(ptx, "cvt.rn.f16x2.e5m2x2") != 22 ||
        file_contains(ptx, "cvt.rn.f16x2.e2m1x2") ||
        file_contains(ptx, "cvt.rn.f16x2.e2m3x2") ||
        file_contains(ptx, "cvt.rn.f16x2.e3m2x2")) {
      return fail("public tensor API missing native mixed-FP8 lowering");
    }
    if (file_occurrences(
            ptx,
            "mma.sync.aligned.m16n8k64.row.col.kind::mxf4.block_scale.scale_vec::2X.f32.e2m1.e2m1.f32.ue8m0") != 12 ||
        !file_contains(
            ptx,
            "mtlc.tensor_mma native-mma mxfp4 whole-tile lowering") ||
        !file_contains(
            ptx,
            "mtlc.tensor_chain resident native-mma mxfp4 tiles=3 subtiles=2") ||
        !file_contains(
            ptx,
            "mtlc.tensor_loop resident native-mma mxfp4 group=1 subtiles=2")) {
      return fail("public tensor API missing packed MXFP4 block scaling");
    }
    if (file_occurrences(
            ptx,
            "mma.sync.aligned.m16n8k64.row.col.kind::mxf4nvf4.block_scale.scale_vec::4X.f32.e2m1.e2m1.f32.ue4m3") != 12 ||
        !file_contains(
            ptx,
            "mtlc.tensor_mma native-mma nvfp4 whole-tile lowering") ||
        !file_contains(
            ptx,
            "mtlc.tensor_chain resident native-mma nvfp4 tiles=3 subtiles=2") ||
        !file_contains(
            ptx,
            "mtlc.tensor_loop resident native-mma nvfp4 group=1 subtiles=2")) {
      return fail("public tensor API missing packed NVFP4 block scaling");
    }
    if (file_occurrences(
            ptx,
            "mma.sync.aligned.m16n8k32.row.col.kind::mxf8f6f4.block_scale.scale_vec::1X.f32.e3m2.e2m3.f32.ue8m0") != 15 ||
        !file_contains(
            ptx,
            "mtlc.tensor_mma native-mma mxf8f6f4 whole-tile lowering") ||
        !file_contains(
            ptx,
            "mtlc.tensor_chain resident native-mma mxf8f6f4 tiles=3 subtiles=2") ||
        !file_contains(
            ptx,
            "mtlc.tensor_loop resident native-mma mxf8f6f4 group=1 subtiles=2")) {
      return fail("public tensor API missing mixed dense-FP6 block scaling");
    }
    if (file_occurrences(ptx, "kind::mxf8f6f4.block_scale.scale_vec::1X") !=
        87) {
      return fail("public tensor API missing complete mxf8f6f4 type matrix");
    }
    for (size_t a_kind = 0; a_kind < 5; a_kind++) {
      for (size_t b_kind = 0; b_kind < 5; b_kind++) {
        char entry[128], matmul_entry[160], instruction[256];
        snprintf(entry, sizeof(entry),
                 ".entry tensor_mxf8f6f4_%s_%s_m16n8k32",
                 k_mxf8f6f4_element_names[a_kind],
                 k_mxf8f6f4_element_names[b_kind]);
        snprintf(matmul_entry, sizeof(matmul_entry),
                 ".entry tensor_matmul_mxf8f6f4_%s_%s_public",
                 k_mxf8f6f4_element_names[a_kind],
                 k_mxf8f6f4_element_names[b_kind]);
        snprintf(
            instruction, sizeof(instruction),
            "mma.sync.aligned.m16n8k32.row.col.kind::mxf8f6f4.block_scale.scale_vec::1X.f32.%s.%s.f32.ue8m0",
            k_mxf8f6f4_element_names[a_kind],
            k_mxf8f6f4_element_names[b_kind]);
        if (!file_contains(ptx, entry) || !file_contains(ptx, matmul_entry) ||
            !file_contains(ptx, instruction)) {
          return fail("public tensor API omitted an mxf8f6f4 A/B type pair");
        }
      }
    }
    if (!mtlc_context_set_ptx_tensor_tuple_budget(ctx, 31) ||
        !mtlc_emit(ctx, m, MTLC_ARCH_PTX, budgeted) ||
        !file_contains(
            budgeted,
            "mtlc.tensor_chain replay tiles=3 tuple_peak=32 budget=31") ||
        !file_contains(
            budgeted,
            "mtlc.tensor_chain replay tiles=3 subtiles=4 tuple_peak=56 budget=31") ||
        file_occurrences(budgeted, "wmma.load.c.sync") < 3 ||
        file_occurrences(budgeted, "wmma.store.d.sync") < 3 ||
        !mtlc_context_set_ptx_tensor_tuple_budget(ctx, 0)) {
      return fail("public PTX tensor tuple-budget replay policy");
    }
    printf("tensor: %s\n", ptx);
    free(budgeted);
    free(portable);
    free(spv);
    free(ptx);
    mtlc_module_destroy(m);
  }

  /* 6. Rank-aware movement remains replayable on old PTX, while GB10 selects
   * tensor maps, transaction barriers, and both TMA directions. */
  {
    if (!tensor_transfer_descriptor_validation())
      return fail("tensor transfer descriptor validation");
    MtlcModule *m = build_tensor_transfer_module();
    char *portable = path_join(outdir, "pubapi_transfer_compute75.ptx");
    char *gb10 = path_join(outdir, "pubapi_transfer_sm121a.ptx");
    if (!m) return fail("tensor transfer module construction");
    if (!mtlc_context_set_ptx_target(ctx, "compute_75", 6, 4) ||
        !mtlc_emit(ctx, m, MTLC_ARCH_PTX, portable) ||
        file_contains(portable, "cp.async.bulk.tensor") ||
        file_occurrences(portable,
                         "mtlc.tensor_transfer cooperative-fallback") != 2) {
      return fail("portable tensor transfer replay");
    }
    if (!mtlc_context_set_ptx_target(ctx, "sm_121a", 8, 8) ||
        !mtlc_emit(ctx, m, MTLC_ARCH_PTX, gb10) ||
        !file_contains(gb10,
                       "cp.async.bulk.tensor.2d.shared::cta.global.tile.mbarrier::complete_tx::bytes") ||
        !file_contains(gb10,
                       "cp.async.bulk.tensor.5d.global.shared::cta.tile.bulk_group") ||
        !file_contains(gb10,
                       "fence.proxy.tensormap::generic.acquire.sys") ||
        !file_contains(gb10, "fence.proxy.async.shared::cta") ||
        !file_contains(gb10, "mbarrier.arrive.expect_tx") ||
        !file_contains(gb10, "cp.async.bulk.wait_group 0") ||
        file_occurrences(gb10,
                         "mtlc.tensor_transfer cooperative-fallback") != 2) {
      return fail("GB10 native tensor transfer lowering");
    }
    printf("tensor transfer: %s, %s\n", portable, gb10);
    free(portable);
    free(gb10);
    mtlc_module_destroy(m);
  }

  /* 7. the ergonomic convenience surface, end to end. */
  {
    if (!composite_types_are_laid_out()) {
      return fail("composite type constructors");
    }
    if (!builder_reports_its_errors()) {
      return fail("builder diagnostics");
    }
    if (!context_reports_pipeline_errors()) {
      return fail("context diagnostics");
    }
    MtlcModule *m = build_ergonomics_module();
    if (!m) {
      return fail("ergonomics module construction");
    }
    if (!mtlc_optimize(ctx, m)) {
      return fail("mtlc_optimize (ergonomics)");
    }
    char *exe = path_join(outdir, "pubapi_ergonomics.exe");
    if (!mtlc_build_executable(ctx, m, exe)) {
      return fail("mtlc_build_executable (ergonomics)");
    }
    printf("ergonomics: %s\n", exe);
    free(exe);
    mtlc_module_destroy(m);
  }

  mtlc_context_destroy(ctx);
  printf("public_api_test: all surfaces OK\n");
  return 0;
}
