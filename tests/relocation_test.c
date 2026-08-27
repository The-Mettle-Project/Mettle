#include "binary_emitter.h"
#include "linker/relocation.h"
#include "test_helpers.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_object(BinaryEmitter *emitter, const char *path) {
  if (!binary_emitter_write_object_file(emitter, path)) {
    fprintf(stderr, "Emitter write failed for %s: %s\n", path,
            binary_emitter_get_error(emitter)
                ? binary_emitter_get_error(emitter)
                : "unknown error");
    return 0;
  }
  return 1;
}

static int create_rel32_caller(const char *path) {
  BinaryEmitter *emitter = binary_emitter_create(BINARY_TARGET_FORMAT_COFF_WIN64);
  size_t text = 0;
  unsigned char placeholder[4] = {0, 0, 0, 0};
  int ok = 0;

  if (!emitter) {
    return 0;
  }

  text = binary_emitter_get_or_create_section(emitter, ".text", BINARY_SECTION_TEXT,
                                              0, 16u);
  if (text == (size_t)-1 ||
      !binary_emitter_append_bytes(emitter, text, placeholder, sizeof(placeholder),
                                   NULL) ||
      !binary_emitter_define_symbol(emitter, "caller", BINARY_SYMBOL_GLOBAL, text,
                                    0u, sizeof(placeholder)) ||
      !binary_emitter_declare_external(emitter, "callee") ||
      !binary_emitter_add_relocation(emitter, text, 0u, BINARY_RELOCATION_REL32,
                                     "callee", 0)) {
    goto cleanup;
  }

  ok = write_object(emitter, path);

cleanup:
  binary_emitter_destroy(emitter);
  return ok;
}

static int create_text_provider(const char *path) {
  BinaryEmitter *emitter = binary_emitter_create(BINARY_TARGET_FORMAT_COFF_WIN64);
  size_t text = 0;
  unsigned char ret_instruction = 0xC3u;
  int ok = 0;

  if (!emitter) {
    return 0;
  }

  text = binary_emitter_get_or_create_section(emitter, ".text", BINARY_SECTION_TEXT,
                                              0, 16u);
  if (text == (size_t)-1 ||
      !binary_emitter_append_bytes(emitter, text, &ret_instruction, 1u, NULL) ||
      !binary_emitter_define_symbol(emitter, "callee", BINARY_SYMBOL_GLOBAL, text,
                                    0u, 1u)) {
    goto cleanup;
  }

  ok = write_object(emitter, path);

cleanup:
  binary_emitter_destroy(emitter);
  return ok;
}

static int create_addr64_holder(const char *path) {
  BinaryEmitter *emitter = binary_emitter_create(BINARY_TARGET_FORMAT_COFF_WIN64);
  size_t rdata = 0;
  unsigned char placeholder[8] = {5, 0, 0, 0, 0, 0, 0, 0};
  int ok = 0;

  if (!emitter) {
    return 0;
  }

  rdata = binary_emitter_get_or_create_section(emitter, ".rdata", BINARY_SECTION_RDATA,
                                               0, 8u);
  if (rdata == (size_t)-1 ||
      !binary_emitter_append_bytes(emitter, rdata, placeholder, sizeof(placeholder),
                                   NULL) ||
      !binary_emitter_define_symbol(emitter, "holder64", BINARY_SYMBOL_GLOBAL,
                                    rdata, 0u, sizeof(placeholder)) ||
      !binary_emitter_declare_external(emitter, TEST_TARGET_DATA_SYMBOL) ||
      !binary_emitter_add_relocation(emitter, rdata, 0u, BINARY_RELOCATION_ADDR64,
                                     TEST_TARGET_DATA_SYMBOL, 0)) {
    goto cleanup;
  }

  ok = write_object(emitter, path);

cleanup:
  binary_emitter_destroy(emitter);
  return ok;
}

static int create_addr32nb_holder(const char *path) {
  BinaryEmitter *emitter = binary_emitter_create(BINARY_TARGET_FORMAT_COFF_WIN64);
  size_t data = 0;
  unsigned char placeholder[4] = {7, 0, 0, 0};
  int ok = 0;

  if (!emitter) {
    return 0;
  }

  data = binary_emitter_get_or_create_section(emitter, ".data", BINARY_SECTION_DATA,
                                              0, 4u);
  if (data == (size_t)-1 ||
      !binary_emitter_append_bytes(emitter, data, placeholder, sizeof(placeholder),
                                   NULL) ||
      !binary_emitter_define_symbol(emitter, "holder32nb", BINARY_SYMBOL_GLOBAL,
                                    data, 0u, sizeof(placeholder)) ||
      !binary_emitter_declare_external(emitter, TEST_TARGET_DATA_SYMBOL) ||
      !binary_emitter_add_relocation(emitter, data, 0u,
                                     BINARY_RELOCATION_ADDR32NB,
                                     TEST_TARGET_DATA_SYMBOL, 0)) {
    goto cleanup;
  }

  ok = write_object(emitter, path);

cleanup:
  binary_emitter_destroy(emitter);
  return ok;
}

static int create_secrel_holder(const char *path) {
  BinaryEmitter *emitter = binary_emitter_create(BINARY_TARGET_FORMAT_COFF_WIN64);
  size_t data = 0;
  unsigned char placeholder[4] = {3, 0, 0, 0};
  int ok = 0;

  if (!emitter) {
    return 0;
  }

  data = binary_emitter_get_or_create_section(emitter, ".data", BINARY_SECTION_DATA,
                                              0, 4u);
  if (data == (size_t)-1 ||
      !binary_emitter_append_bytes(emitter, data, placeholder, sizeof(placeholder),
                                   NULL) ||
      !binary_emitter_define_symbol(emitter, "holder_secrel", BINARY_SYMBOL_GLOBAL,
                                    data, 0u, sizeof(placeholder)) ||
      !binary_emitter_declare_external(emitter, TEST_TARGET_DATA_SYMBOL) ||
      !binary_emitter_add_relocation(emitter, data, 0u,
                                     BINARY_RELOCATION_SECTION_REL32,
                                     TEST_TARGET_DATA_SYMBOL, 0)) {
    goto cleanup;
  }

  ok = write_object(emitter, path);

cleanup:
  binary_emitter_destroy(emitter);
  return ok;
}

static int create_data_provider(const char *path) {
  BinaryEmitter *emitter = binary_emitter_create(BINARY_TARGET_FORMAT_COFF_WIN64);
  size_t data = 0;
  unsigned char prefix[4] = {0, 0, 0, 0};
  unsigned char payload[8] = {0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
  int ok = 0;

  if (!emitter) {
    return 0;
  }

  data = binary_emitter_get_or_create_section(emitter, ".data", BINARY_SECTION_DATA,
                                              0, 8u);
  if (data == (size_t)-1 ||
      !binary_emitter_append_bytes(emitter, data, prefix, sizeof(prefix), NULL) ||
      !binary_emitter_define_symbol(emitter, TEST_TARGET_DATA_SYMBOL, BINARY_SYMBOL_GLOBAL,
                                    data, 4u, sizeof(payload)) ||
      !binary_emitter_append_bytes(emitter, data, payload, sizeof(payload), NULL)) {
    goto cleanup;
  }

  ok = write_object(emitter, path);

cleanup:
  binary_emitter_destroy(emitter);
  return ok;
}


static int expect_rel32(const char *caller_path, const char *provider_path) {
  const char *paths[2] = {caller_path, provider_path};
  LinkResolution *resolution = NULL;
  char *error_message = NULL;
  const LinkedSection *text = NULL;
  const LinkedSymbol *callee = NULL;
  int result = 1;
  int32_t patched = 0;
  int32_t expected = 0;

  if (!link_resolution_build(paths, 2u, NULL, &resolution, &error_message) ||
      !link_apply_relocations(resolution, NULL, &error_message)) {
    result = report_failure("REL32 relocation apply failed", error_message);
    goto cleanup;
  }

  text = link_resolution_find_section(resolution, LINK_SECTION_KIND_TEXT);
  callee = link_resolution_find_symbol(resolution, "callee");
  if (!text || !callee || text->size < 4u) {
    result = report_failure("REL32 resolution produced invalid merged text",
                            caller_path);
    goto cleanup;
  }

  patched = (int32_t)test_read_u32(text->data);
  expected = (int32_t)(callee->virtual_address - 4u);
  if (patched != expected) {
    result = report_failure("REL32 relocation value mismatch", "callee");
    goto cleanup;
  }

  result = 0;

cleanup:
  free(error_message);
  link_resolution_destroy(resolution);
  return result;
}

static int expect_addr64(const char *holder_path, const char *provider_path) {
  const char *paths[2] = {holder_path, provider_path};
  LinkResolution *resolution = NULL;
  char *error_message = NULL;
  const LinkedSection *rdata = NULL;
  const LinkedSymbol *target = NULL;
  uint64_t patched = 0;
  uint64_t expected = 0;
  int result = 1;

  if (!link_resolution_build(paths, 2u, NULL, &resolution, &error_message) ||
      !link_apply_relocations(resolution, NULL, &error_message)) {
    result = report_failure("ADDR64 relocation apply failed", error_message);
    goto cleanup;
  }

  rdata = link_resolution_find_section(resolution, LINK_SECTION_KIND_RDATA);
  target = link_resolution_find_symbol(resolution, TEST_TARGET_DATA_SYMBOL);
  if (!rdata || !target || rdata->size < 8u) {
    result = report_failure("ADDR64 resolution produced invalid merged rdata",
                            holder_path);
    goto cleanup;
  }

  patched = test_read_u64(rdata->data);
  expected = target->virtual_address + 5u;
  if (patched != expected) {
    result = report_failure("ADDR64 relocation value mismatch", TEST_TARGET_DATA_SYMBOL);
    goto cleanup;
  }

  result = 0;

cleanup:
  free(error_message);
  link_resolution_destroy(resolution);
  return result;
}

static int expect_addr32nb(const char *holder_path, const char *provider_path) {
  const char *paths[2] = {holder_path, provider_path};
  LinkResolution *resolution = NULL;
  LinkRelocationOptions options = {0x140000000ull};
  char *error_message = NULL;
  const LinkedSection *data = NULL;
  const LinkedSymbol *target = NULL;
  uint32_t patched = 0;
  uint32_t expected = 0;
  int result = 1;

  if (!link_resolution_build(paths, 2u, NULL, &resolution, &error_message) ||
      !link_apply_relocations(resolution, &options, &error_message)) {
    result = report_failure("ADDR32NB relocation apply failed", error_message);
    goto cleanup;
  }

  data = link_resolution_find_section(resolution, LINK_SECTION_KIND_DATA);
  target = link_resolution_find_symbol(resolution, TEST_TARGET_DATA_SYMBOL);
  if (!data || !target || data->size < 4u) {
    result = report_failure("ADDR32NB resolution produced invalid merged data",
                            holder_path);
    goto cleanup;
  }

  patched = test_read_u32(data->data);
  expected = (uint32_t)(target->virtual_address + 7u);
  if (patched != expected) {
    result = report_failure("ADDR32NB relocation value mismatch", TEST_TARGET_DATA_SYMBOL);
    goto cleanup;
  }

  result = 0;

cleanup:
  free(error_message);
  link_resolution_destroy(resolution);
  return result;
}

static int expect_secrel(const char *holder_path, const char *provider_path) {
  const char *paths[2] = {holder_path, provider_path};
  LinkResolution *resolution = NULL;
  char *error_message = NULL;
  const LinkedSection *data = NULL;
  const LinkedSymbol *target = NULL;
  uint32_t patched = 0;
  uint32_t expected = 0;
  int result = 1;

  if (!link_resolution_build(paths, 2u, NULL, &resolution, &error_message) ||
      !link_apply_relocations(resolution, NULL, &error_message)) {
    result = report_failure("SECREL relocation apply failed", error_message);
    goto cleanup;
  }

  data = link_resolution_find_section(resolution, LINK_SECTION_KIND_DATA);
  target = link_resolution_find_symbol(resolution, TEST_TARGET_DATA_SYMBOL);
  if (!data || !target || data->size < 4u) {
    result = report_failure("SECREL resolution produced invalid merged data",
                            holder_path);
    goto cleanup;
  }

  patched = test_read_u32(data->data);
  expected = (uint32_t)(target->merged_offset + 3u);
  if (patched != expected) {
    result = report_failure("SECREL relocation value mismatch", TEST_TARGET_DATA_SYMBOL);
    goto cleanup;
  }

  result = 0;

cleanup:
  free(error_message);
  link_resolution_destroy(resolution);
  return result;
}

int main(int argc, char **argv) {
  char *rel32_caller_path = NULL;
  char *rel32_provider_path = NULL;
  char *addr64_holder_path = NULL;
  char *addr32nb_holder_path = NULL;
  char *secrel_holder_path = NULL;
  char *data_provider_path = NULL;
  int result = 1;

  if (argc != 2) {
    fprintf(stderr, "Usage: %s <temp-dir>\n", argv[0]);
    return 1;
  }

  rel32_caller_path = join_path(argv[1], "rel32_caller.obj");
  rel32_provider_path = join_path(argv[1], "rel32_provider.obj");
  addr64_holder_path = join_path(argv[1], "addr64_holder.obj");
  addr32nb_holder_path = join_path(argv[1], "addr32nb_holder.obj");
  secrel_holder_path = join_path(argv[1], "secrel_holder.obj");
  data_provider_path = join_path(argv[1], "data_provider.obj");
  if (!rel32_caller_path || !rel32_provider_path || !addr64_holder_path ||
      !addr32nb_holder_path || !secrel_holder_path || !data_provider_path) {
    result = report_failure("Failed to allocate relocation test paths", NULL);
    goto cleanup;
  }

  if (!create_rel32_caller(rel32_caller_path) ||
      !create_text_provider(rel32_provider_path) ||
      !create_addr64_holder(addr64_holder_path) ||
      !create_addr32nb_holder(addr32nb_holder_path) ||
      !create_secrel_holder(secrel_holder_path) ||
      !create_data_provider(data_provider_path)) {
    result = report_failure("Failed to create relocation test objects", NULL);
    goto cleanup;
  }

  if (expect_rel32(rel32_caller_path, rel32_provider_path) != 0 ||
      expect_addr64(addr64_holder_path, data_provider_path) != 0 ||
      expect_addr32nb(addr32nb_holder_path, data_provider_path) != 0 ||
      expect_secrel(secrel_holder_path, data_provider_path) != 0) {
    goto cleanup;
  }

  result = 0;

cleanup:
  free(rel32_caller_path);
  free(rel32_provider_path);
  free(addr64_holder_path);
  free(addr32nb_holder_path);
  free(secrel_holder_path);
  free(data_provider_path);
  return result;
}
