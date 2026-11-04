#include "pkg.h"
#include "runtime/owned.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PKG_DEFAULT_REGISTRY "https://the-mettle-project.github.io/Mettle/registry/v1"
#define PKG_INDEX_HEADER "mettle-registry-1"
#define PKG_LOCK_HEADER "mettle-lock-1"
#define PKG_MANIFEST "mettle.package"
#define PKG_DEPS "mettle.deps"
#define PKG_LOCK "mettle.lock"
#define PKG_STATE ".mettle"
#define PKG_PACKAGES ".mettle/packages"
#define PKG_TMP ".mettle/tmp"
#define PKG_MAX_ARCHIVE ((size_t)(16u << 20))
#define PKG_MAX_INDEX ((size_t)(4u << 20))
#define PKG_MAX_FILES 4096
#define PKG_LIST_BYTES ((size_t)(256u << 10))
#define PKG_BLOCK_BYTES ((size_t)(256u << 10))
#define PKG_TAR_BLOCK 512
#define PKG_MAX_DEPTH 64

typedef struct {
  unsigned major;
  unsigned minor;
  unsigned patch;
} PkgVersion;

typedef struct {
  const char *name;
  PkgVersion version;
} PkgNeed;

typedef struct {
  PkgNeed *items;
  size_t count;
  size_t capacity;
} PkgNeeds;

typedef struct {
  const char *name;
  const char *description;
  const char *license;
  const char *source;
  const char *compiler;
  PkgVersion version;
  int has_version;
  PkgNeeds needs;
} PkgManifest;

typedef struct {
  PkgVersion version;
  const char *sha256;
  PkgVersion compiler;
  int has_compiler;
  int yanked;
  PkgNeeds needs;
} PkgRelease;

typedef struct {
  const char *name;
  const char *description;
  PkgRelease *releases;
  size_t count;
  size_t capacity;
} PkgIndex;

typedef struct {
  const char *name;
  PkgVersion version;
  const char *sha256;
  const char *required_by;
} PkgPicked;

typedef struct {
  PkgPicked *items;
  size_t count;
  size_t capacity;
} PkgPicks;

typedef struct {
  int present;
  PkgNeeds roots;
  PkgPicks picks;
} PkgLockFile;

typedef struct {
  const char *path;
} PkgFile;

typedef struct {
  PkgFile *items;
  size_t count;
  size_t capacity;
} PkgFiles;

typedef struct {
  unsigned char *bytes;
  size_t size;
} PkgBytes;

typedef struct PkgBlock {
  struct PkgBlock *next;
  size_t used;
  size_t size;
} PkgBlock;

static PkgBlock *g_pkg_blocks = NULL;
static const char *g_pkg_root = "";
static const char *g_pkg_compiler = NULL;
static const char *g_pkg_scratch = NULL;
static PkgIndex **g_pkg_indexes = NULL;
static size_t g_pkg_index_count = 0;
static size_t g_pkg_index_capacity = 0;
static unsigned g_pkg_tmp_serial = 0;

static void *pkg_alloc(size_t size) {
  size = (size + 7u) & ~(size_t)7u;
  if (!g_pkg_blocks || g_pkg_blocks->used + size > g_pkg_blocks->size) {
    size_t block_size = size > PKG_BLOCK_BYTES ? size : PKG_BLOCK_BYTES;
    PkgBlock *block = (PkgBlock *)malloc(sizeof(PkgBlock) + block_size);
    if (!block) {
      fprintf(stderr, "Error: out of memory\n");
      abort();
    }
    block->next = g_pkg_blocks;
    block->used = 0;
    block->size = block_size;
    g_pkg_blocks = block;
  }
  {
    void *result = (char *)(g_pkg_blocks + 1) + g_pkg_blocks->used;
    g_pkg_blocks->used += size;
    memset(result, 0, size);
    return result;
  }
}

static void pkg_release_memory(void) {
  while (g_pkg_blocks) {
    PkgBlock *next = g_pkg_blocks->next;
    free(g_pkg_blocks);
    g_pkg_blocks = next;
  }
}

static char *pkg_format(const char *format, ...) {
  va_list first;
  va_list second;
  int length;
  char *text;
  va_start(first, format);
  va_copy(second, first);
  length = vsnprintf(NULL, 0, format, first);
  va_end(first);
  text = (char *)pkg_alloc((size_t)(length > 0 ? length : 0) + 1u);
  vsnprintf(text, (size_t)(length > 0 ? length : 0) + 1u, format, second);
  va_end(second);
  return text;
}

static char *pkg_copy(const char *text) { return pkg_format("%s", text); }

static int pkg_fail(const char *format, ...) {
  va_list arguments;
  fputs("Error: ", stderr);
  va_start(arguments, format);
  vfprintf(stderr, format, arguments);
  va_end(arguments);
  fputc('\n', stderr);
  return 0;
}

static void *pkg_grow(void *items, size_t count, size_t *capacity,
                      size_t item_size) {
  void *grown;
  if (count < *capacity) {
    return items;
  }
  *capacity = *capacity ? *capacity * 2u : 8u;
  grown = pkg_alloc(*capacity * item_size);
  if (count) {
    memcpy(grown, items, count * item_size);
  }
  return grown;
}

static const char *pkg_path(const char *relative) {
  return pkg_format("%s%s", g_pkg_root, relative);
}

static char *pkg_next_line(char **cursor) {
  char *line = *cursor;
  char *end;
  if (!line || !*line) {
    return NULL;
  }
  end = strchr(line, '\n');
  if (end) {
    *end = '\0';
    *cursor = end + 1;
  } else {
    *cursor = line + strlen(line);
  }
  end = line + strlen(line);
  if (end > line && end[-1] == '\r') {
    end[-1] = '\0';
  }
  return line;
}

static char *pkg_trim(char *text) {
  char *end;
  while (*text && isspace((unsigned char)*text)) {
    text++;
  }
  end = text + strlen(text);
  while (end > text && isspace((unsigned char)end[-1])) {
    *--end = '\0';
  }
  return text;
}

static size_t pkg_split_tabs(char *line, char **fields, size_t limit) {
  size_t count = 0;
  while (count < limit) {
    char *tab = strchr(line, '\t');
    fields[count++] = line;
    if (!tab) {
      break;
    }
    *tab = '\0';
    line = tab + 1;
  }
  return count;
}

static int pkg_read_file(const char *path, size_t limit, PkgBytes *out) {
  FILE *file = fopen(path, "rb");
  long size;
  size_t got;
  if (!file) {
    return 0;
  }
  fseek(file, 0, SEEK_END);
  size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (size < 0 || (size_t)size > limit) {
    fclose(file);
    return 0;
  }
  out->bytes = (unsigned char *)pkg_alloc((size_t)size + 1u);
  got = size ? fread(out->bytes, 1, (size_t)size, file) : 0;
  fclose(file);
  out->size = (size_t)size;
  out->bytes[out->size] = 0;
  return got == (size_t)size;
}

static int pkg_write_file(const char *path, const void *bytes, size_t size) {
  FILE *file = fopen(path, "wb");
  size_t put;
  if (!file) {
    return 0;
  }
  put = size ? fwrite(bytes, 1, size, file) : 0;
  return fclose(file) == 0 && put == size;
}

static int pkg_write_bytes_atomic(const char *path, const void *bytes,
                                  size_t size) {
  const char *temporary = pkg_format("%s.%u.new", path, g_pkg_tmp_serial++);
  if (!pkg_write_file(temporary, bytes, size) ||
      mettle_move_path(temporary, path) != 0) {
    remove(temporary);
    return 0;
  }
  return 1;
}

static int pkg_write_atomic(const char *path, const char *text) {
  return pkg_write_bytes_atomic(path, text, strlen(text))
             ? 1
             : pkg_fail("could not write %s", path);
}

static int pkg_folded_prefix(const char *text, const char *prefix,
                             size_t length) {
  for (size_t i = 0; i < length; i++) {
    if (tolower((unsigned char)text[i]) != tolower((unsigned char)prefix[i])) {
      return 0;
    }
  }
  return 1;
}

static int pkg_make_dirs(const char *path) {
  char *walk = pkg_copy(path);
  for (char *at = walk; *at; at++) {
    if ((*at == '/' || *at == '\\') && at > walk && at[-1] != ':' &&
        at[-1] != '/' && at[-1] != '\\' && at[-1] != '.') {
      char saved = *at;
      *at = '\0';
      if (!mettle_path_is_directory(walk)) {
        mettle_make_directory(walk);
      }
      *at = saved;
    }
  }
  if (!mettle_path_is_directory(walk)) {
    mettle_make_directory(walk);
  }
  return mettle_path_is_directory(walk);
}

static int pkg_list_dir(const char *path, char **names, int *count) {
  *names = (char *)pkg_alloc(PKG_LIST_BYTES);
  *count = mettle_dir_list(path, *names, (int)PKG_LIST_BYTES);
  return *count >= 0;
}

static int pkg_remove_tree(const char *path) {
  char *names;
  int count;
  const char *entry;
  int ok = 1;
  if (!strstr(path, PKG_STATE "/") || !pkg_list_dir(path, &names, &count)) {
    return 0;
  }
  entry = names;
  for (int i = 0; i < count; i++) {
    const char *child = pkg_format("%s/%s", path, entry + 1);
    if (entry[0] == 'd') {
      ok = pkg_remove_tree(child) && ok;
    } else if (remove(child) != 0 && mettle_remove_directory(child) != 0) {
      ok = 0;
    }
    entry += strlen(entry) + 1u;
  }
  return mettle_remove_directory(path) == 0 && ok;
}

static const uint32_t PKG_SHA_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

static uint32_t pkg_rotr(uint32_t value, unsigned bits) {
  return (value >> bits) | (value << (32u - bits));
}

static void pkg_sha_block(uint32_t state[8], const unsigned char *block) {
  uint32_t w[64];
  uint32_t v[8];
  for (unsigned i = 0; i < 16u; i++) {
    w[i] = ((uint32_t)block[i * 4u] << 24) |
           ((uint32_t)block[i * 4u + 1u] << 16) |
           ((uint32_t)block[i * 4u + 2u] << 8) | (uint32_t)block[i * 4u + 3u];
  }
  for (unsigned i = 16; i < 64u; i++) {
    uint32_t s0 = pkg_rotr(w[i - 15u], 7) ^ pkg_rotr(w[i - 15u], 18) ^
                  (w[i - 15u] >> 3);
    uint32_t s1 = pkg_rotr(w[i - 2u], 17) ^ pkg_rotr(w[i - 2u], 19) ^
                  (w[i - 2u] >> 10);
    w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
  }
  memcpy(v, state, sizeof(v));
  for (unsigned i = 0; i < 64u; i++) {
    uint32_t s1 = pkg_rotr(v[4], 6) ^ pkg_rotr(v[4], 11) ^ pkg_rotr(v[4], 25);
    uint32_t choose = (v[4] & v[5]) ^ (~v[4] & v[6]);
    uint32_t t1 = v[7] + s1 + choose + PKG_SHA_K[i] + w[i];
    uint32_t s0 = pkg_rotr(v[0], 2) ^ pkg_rotr(v[0], 13) ^ pkg_rotr(v[0], 22);
    uint32_t majority = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
    uint32_t t2 = s0 + majority;
    v[7] = v[6];
    v[6] = v[5];
    v[5] = v[4];
    v[4] = v[3] + t1;
    v[3] = v[2];
    v[2] = v[1];
    v[1] = v[0];
    v[0] = t1 + t2;
  }
  for (unsigned i = 0; i < 8u; i++) {
    state[i] += v[i];
  }
}

static const char *pkg_sha256(const unsigned char *bytes, size_t size) {
  uint32_t state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                       0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  unsigned char tail[128];
  size_t whole = size / 64u;
  size_t rest = size - whole * 64u;
  size_t tail_size = rest < 56u ? 64u : 128u;
  uint64_t bits = (uint64_t)size * 8u;
  char *hex = (char *)pkg_alloc(65);
  for (size_t i = 0; i < whole; i++) {
    pkg_sha_block(state, bytes + i * 64u);
  }
  memset(tail, 0, sizeof(tail));
  if (rest) {
    memcpy(tail, bytes + whole * 64u, rest);
  }
  tail[rest] = 0x80u;
  for (unsigned i = 0; i < 8u; i++) {
    tail[tail_size - 1u - i] = (unsigned char)(bits >> (i * 8u));
  }
  pkg_sha_block(state, tail);
  if (tail_size == 128u) {
    pkg_sha_block(state, tail + 64);
  }
  for (unsigned i = 0; i < 8u; i++) {
    snprintf(hex + i * 8u, 9, "%08x", (unsigned)state[i]);
  }
  return hex;
}

static int pkg_parse_version(const char *text, PkgVersion *out) {
  unsigned parts[3] = {0, 0, 0};
  const char *at = text;
  for (int part = 0; part < 3; part++) {
    const char *start = at;
    while (isdigit((unsigned char)*at)) {
      if (at - start >= 9) {
        return 0;
      }
      parts[part] = parts[part] * 10u + (unsigned)(*at - '0');
      at++;
    }
    if (at == start || (*start == '0' && at - start > 1)) {
      return 0;
    }
    if (part < 2 && *at++ != '.') {
      return 0;
    }
  }
  if (*at) {
    return 0;
  }
  out->major = parts[0];
  out->minor = parts[1];
  out->patch = parts[2];
  return 1;
}

static int pkg_version_compare(PkgVersion left, PkgVersion right) {
  if (left.major != right.major) {
    return left.major < right.major ? -1 : 1;
  }
  if (left.minor != right.minor) {
    return left.minor < right.minor ? -1 : 1;
  }
  if (left.patch != right.patch) {
    return left.patch < right.patch ? -1 : 1;
  }
  return 0;
}

static const char *pkg_version_text(PkgVersion version) {
  return pkg_format("%u.%u.%u", version.major, version.minor, version.patch);
}

static const char *pkg_tag(const char *name, PkgVersion version) {
  return pkg_format("%s-%s", name, pkg_version_text(version));
}

static int pkg_name_valid(const char *name) {
  static const char *const reserved[] = {"std", "mettle", "con", "prn",
                                         "aux", "nul",    "v1"};
  size_t length = strlen(name);
  if (length == 0 || length > 64u || !islower((unsigned char)name[0])) {
    return 0;
  }
  for (size_t i = 0; i < length; i++) {
    unsigned char ch = (unsigned char)name[i];
    if (!(islower(ch) || isdigit(ch) || ch == '_')) {
      return 0;
    }
  }
  for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
    if (strcmp(name, reserved[i]) == 0) {
      return 0;
    }
  }
  return !(length == 4u &&
           (strncmp(name, "com", 3) == 0 || strncmp(name, "lpt", 3) == 0) &&
           isdigit((unsigned char)name[3]));
}

static int pkg_extension_is_binary(const char *path) {
  static const char *const banned[] = {".exe", ".dll", ".so",  ".dylib",
                                       ".o",   ".obj", ".a",   ".lib",
                                       ".pdb", ".tar", ".zip", ".gz"};
  const char *dot = strrchr(path, '.');
  if (!dot) {
    return 0;
  }
  for (size_t i = 0; i < sizeof(banned) / sizeof(banned[0]); i++) {
    if (strcasecmp(dot, banned[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

static int pkg_component_valid(const char *start, size_t length) {
  static const char *const reserved[] = {"con", "prn", "aux", "nul"};
  size_t stem = 0;
  if (length == 0 || start[0] == '.' || start[length - 1u] == '.') {
    return 0;
  }
  for (size_t i = 0; i < length; i++) {
    unsigned char ch = (unsigned char)start[i];
    if (!(isalnum(ch) || ch == '_' || ch == '-' || ch == '.' || ch == '+')) {
      return 0;
    }
  }
  while (stem < length && start[stem] != '.') {
    stem++;
  }
  for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
    if (stem == 3u && pkg_folded_prefix(start, reserved[i], 3)) {
      return 0;
    }
  }
  return !(stem == 4u &&
           (pkg_folded_prefix(start, "com", 3) ||
            pkg_folded_prefix(start, "lpt", 3)) &&
           isdigit((unsigned char)start[3]));
}

static int pkg_path_valid(const char *path) {
  const char *start = path;
  if (strlen(path) > 240u) {
    return 0;
  }
  for (;;) {
    const char *slash = strchr(start, '/');
    size_t length = slash ? (size_t)(slash - start) : strlen(start);
    if (!pkg_component_valid(start, length)) {
      return 0;
    }
    if (!slash) {
      return 1;
    }
    start = slash + 1;
  }
}

static void pkg_needs_set(PkgNeeds *needs, const char *name,
                          PkgVersion version) {
  for (size_t i = 0; i < needs->count; i++) {
    if (strcmp(needs->items[i].name, name) == 0) {
      needs->items[i].version = version;
      return;
    }
  }
  needs->items = (PkgNeed *)pkg_grow(needs->items, needs->count,
                                     &needs->capacity, sizeof(PkgNeed));
  needs->items[needs->count].name = pkg_copy(name);
  needs->items[needs->count].version = version;
  needs->count++;
}

static const PkgNeed *pkg_needs_find(const PkgNeeds *needs, const char *name) {
  for (size_t i = 0; i < needs->count; i++) {
    if (strcmp(needs->items[i].name, name) == 0) {
      return &needs->items[i];
    }
  }
  return NULL;
}

static int pkg_need_compare(const void *left, const void *right) {
  return strcmp(((const PkgNeed *)left)->name, ((const PkgNeed *)right)->name);
}

static void pkg_needs_sort(PkgNeeds *needs) {
  if (needs->count > 1u) {
    qsort(needs->items, needs->count, sizeof(PkgNeed), pkg_need_compare);
  }
}

static int pkg_needs_equal(const PkgNeeds *left, const PkgNeeds *right) {
  if (left->count != right->count) {
    return 0;
  }
  for (size_t i = 0; i < left->count; i++) {
    const PkgNeed *other = pkg_needs_find(right, left->items[i].name);
    if (!other ||
        pkg_version_compare(other->version, left->items[i].version) != 0) {
      return 0;
    }
  }
  return 1;
}

static int pkg_parse_need(const char *where, char *value, PkgNeeds *needs) {
  char *space = strchr(value, ' ');
  PkgVersion version;
  if (!space) {
    return pkg_fail("%s: a require line is `require = <name> <version>`",
                    where);
  }
  *space = '\0';
  if (!pkg_name_valid(value) || !pkg_parse_version(pkg_trim(space + 1), &version)) {
    return pkg_fail("%s: a require line is `require = <name> <version>`",
                    where);
  }
  pkg_needs_set(needs, value, version);
  return 1;
}

static int pkg_manifest_field(PkgManifest *manifest, const char *where,
                              const char *key, char *value) {
  if (strcmp(key, "name") == 0) {
    if (!pkg_name_valid(value)) {
      return pkg_fail("%s: `%s` is not a package name; use a lower case letter "
                      "followed by lower case letters, digits or underscores",
                      where, value);
    }
    manifest->name = pkg_copy(value);
  } else if (strcmp(key, "version") == 0) {
    if (!pkg_parse_version(value, &manifest->version)) {
      return pkg_fail("%s: `%s` is not a version; use MAJOR.MINOR.PATCH", where,
                      value);
    }
    manifest->has_version = 1;
  } else if (strcmp(key, "description") == 0) {
    manifest->description = pkg_copy(value);
  } else if (strcmp(key, "license") == 0) {
    manifest->license = pkg_copy(value);
  } else if (strcmp(key, "source") == 0) {
    manifest->source = pkg_copy(value);
  } else if (strcmp(key, "compiler") == 0) {
    PkgVersion check;
    if (!pkg_parse_version(value, &check)) {
      return pkg_fail("%s: `%s` is not a version; use MAJOR.MINOR.PATCH", where,
                      value);
    }
    manifest->compiler = pkg_copy(value);
  } else if (strcmp(key, "require") == 0) {
    return pkg_parse_need(where, value, &manifest->needs);
  } else {
    return pkg_fail("%s: unknown key `%s`", where, key);
  }
  return 1;
}

static int pkg_manifest_parse(const char *path, char *text,
                              PkgManifest *manifest) {
  char *cursor = text;
  char *line;
  int number = 0;
  while ((line = pkg_next_line(&cursor))) {
    char *equals;
    number++;
    line = pkg_trim(line);
    if (!*line || *line == '#') {
      continue;
    }
    equals = strchr(line, '=');
    if (!equals) {
      return pkg_fail("%s:%d: expected `key = value`", path, number);
    }
    *equals = '\0';
    if (!pkg_manifest_field(manifest, pkg_format("%s:%d", path, number),
                            pkg_trim(line), pkg_trim(equals + 1))) {
      return 0;
    }
  }
  return 1;
}

static int pkg_manifest_load(const char *path, PkgManifest *manifest) {
  PkgBytes file;
  memset(manifest, 0, sizeof(*manifest));
  if (!pkg_read_file(path, PKG_MAX_INDEX, &file)) {
    return 0;
  }
  return pkg_manifest_parse(path, (char *)file.bytes, manifest) ? 1 : -1;
}

static int pkg_manifest_save(const PkgNeeds *roots) {
  const char *path = pkg_path(PKG_MANIFEST);
  PkgBytes file;
  char *cursor;
  char *line;
  char *out = pkg_copy("");
  PkgNeeds sorted = *roots;
  pkg_needs_sort(&sorted);
  if (pkg_read_file(path, PKG_MAX_INDEX, &file)) {
    cursor = (char *)file.bytes;
    while ((line = pkg_next_line(&cursor))) {
      char *copy = pkg_copy(line);
      char *key = pkg_trim(copy);
      char *equals = strchr(key, '=');
      if (equals) {
        *equals = '\0';
        if (strcmp(pkg_trim(key), "require") == 0) {
          continue;
        }
      }
      out = pkg_format("%s%s\n", out, line);
    }
  }
  for (size_t i = 0; i < sorted.count; i++) {
    out = pkg_format("%srequire = %s %s\n", out, sorted.items[i].name,
                     pkg_version_text(sorted.items[i].version));
  }
  return pkg_write_atomic(path, out);
}

static int pkg_lock_parse_line(const char *path, char *line, PkgLockFile *lock) {
  char *fields[5];
  size_t count = pkg_split_tabs(line, fields, 5);
  PkgVersion version;
  if (count >= 3u && strcmp(fields[0], "require") == 0 &&
      pkg_parse_version(fields[2], &version)) {
    pkg_needs_set(&lock->roots, fields[1], version);
    return 1;
  }
  if (count == 4u && strcmp(fields[0], "package") == 0 &&
      pkg_parse_version(fields[2], &version) && strlen(fields[3]) == 64u) {
    PkgPicks *picks = &lock->picks;
    picks->items = (PkgPicked *)pkg_grow(picks->items, picks->count,
                                         &picks->capacity, sizeof(PkgPicked));
    picks->items[picks->count].name = pkg_copy(fields[1]);
    picks->items[picks->count].version = version;
    picks->items[picks->count].sha256 = pkg_copy(fields[3]);
    picks->count++;
    return 1;
  }
  return pkg_fail("%s: unreadable line; delete the file and run `mettle pkg "
                  "install` to rebuild it",
                  path);
}

static int pkg_lock_load(PkgLockFile *lock) {
  const char *path = pkg_path(PKG_LOCK);
  PkgBytes file;
  char *cursor;
  char *line;
  memset(lock, 0, sizeof(*lock));
  if (!pkg_read_file(path, PKG_MAX_INDEX, &file)) {
    return 1;
  }
  cursor = (char *)file.bytes;
  line = pkg_next_line(&cursor);
  if (!line || strcmp(line, PKG_LOCK_HEADER) != 0) {
    return pkg_fail("%s was written by a newer mettle; update the compiler",
                    path);
  }
  while ((line = pkg_next_line(&cursor))) {
    if (*line && !pkg_lock_parse_line(path, line, lock)) {
      return 0;
    }
  }
  lock->present = 1;
  return 1;
}

static const PkgPicked *pkg_picks_find(const PkgPicks *picks, const char *name) {
  for (size_t i = 0; i < picks->count; i++) {
    if (strcmp(picks->items[i].name, name) == 0) {
      return &picks->items[i];
    }
  }
  return NULL;
}

static int pkg_pick_compare(const void *left, const void *right) {
  return strcmp(((const PkgPicked *)left)->name,
                ((const PkgPicked *)right)->name);
}

static int pkg_lock_save(const PkgNeeds *roots, const PkgPicks *picks) {
  char *out = pkg_format("%s\n", PKG_LOCK_HEADER);
  PkgNeeds sorted = *roots;
  pkg_needs_sort(&sorted);
  for (size_t i = 0; i < sorted.count; i++) {
    out = pkg_format("%srequire\t%s\t%s\n", out, sorted.items[i].name,
                     pkg_version_text(sorted.items[i].version));
  }
  for (size_t i = 0; i < picks->count; i++) {
    out = pkg_format("%spackage\t%s\t%s\t%s\n", out, picks->items[i].name,
                     pkg_version_text(picks->items[i].version),
                     picks->items[i].sha256);
  }
  return pkg_write_atomic(pkg_path(PKG_LOCK), out);
}

static const char *pkg_registry_base(void) {
  const char *value = getenv("METTLE_REGISTRY");
  char *base = pkg_copy(value && *value ? value : PKG_DEFAULT_REGISTRY);
  size_t length = strlen(base);
  while (length > 1u && (base[length - 1u] == '/' || base[length - 1u] == '\\')) {
    base[--length] = '\0';
  }
  return base;
}

static int pkg_copy_local(const char *from, const char *to, size_t limit) {
  PkgBytes file;
  if (!pkg_read_file(from, limit, &file)) {
    return 22;
  }
  return pkg_write_file(to, file.bytes, file.size) ? 0 : 23;
}

static const char *pkg_curl_meaning(int code) {
  switch (code) {
  case 22:
    return "the registry has no such file";
  case 6:
    return "could not resolve the host";
  case 7:
    return "could not connect";
  case 28:
    return "timed out";
  case 35:
  case 60:
    return "TLS failed";
  case 63:
    return "the file is larger than the limit";
  case 127:
    return "downloading packages needs curl on PATH";
  default:
    return pkg_format("curl exited with %d", code);
  }
}

static int pkg_curl(const char *url, const char *out, size_t limit) {
  const char *scheme_end = strstr(url, "://");
  const char *scheme =
      pkg_format("=%.*s", (int)(scheme_end - url), url);
  const char *const arguments[] = {
      "curl",        "--fail",
      "--silent",    "--location",
      "--retry",     "2",
      "--max-time",  "300",
      "--max-redirs", "4",
      "--proto",     scheme,
      "--proto-redir", scheme,
      "--max-filesize", pkg_format("%zu", limit),
      "--output",    out,
      url,           NULL};
  return mettle_find_executable("curl") ? mettle_run_process("curl", arguments)
                                        : 127;
}

static int pkg_fetch(const char *relative, const char *out, size_t limit) {
  const char *base = pkg_registry_base();
  if (!strstr(base, "://")) {
    return pkg_copy_local(pkg_format("%s/%s", base, relative), out, limit);
  }
  return pkg_curl(pkg_format("%s/%s", base, relative), out, limit);
}

static const char *pkg_tmp_file(const char *stem) {
  if (g_pkg_scratch) {
    return pkg_format("%s/%s.%u", g_pkg_scratch, stem, g_pkg_tmp_serial++);
  }
  return pkg_path(pkg_format(PKG_TMP "/%s.%u", stem, g_pkg_tmp_serial++));
}

static int pkg_ensure_state(void) {
  const char *state = pkg_path(PKG_STATE);
  const char *ignore = pkg_format("%s/.gitignore", state);
  if (!pkg_make_dirs(pkg_path(PKG_PACKAGES)) ||
      !pkg_make_dirs(pkg_path(PKG_TMP))) {
    return pkg_fail("could not create %s", state);
  }
  if (!mettle_path_exists(ignore)) {
    pkg_write_file(ignore, "*\n", 2);
  }
  return 1;
}

static int pkg_parse_release_needs(char *text, PkgNeeds *needs) {
  while (text && *text && strcmp(text, "-") != 0) {
    char *comma = strchr(text, ',');
    char *at = strchr(text, '@');
    PkgVersion version;
    if (comma) {
      *comma = '\0';
    }
    if (!at) {
      return 0;
    }
    *at = '\0';
    if (!pkg_name_valid(text) || !pkg_parse_version(at + 1, &version)) {
      return 0;
    }
    pkg_needs_set(needs, text, version);
    text = comma ? comma + 1 : NULL;
  }
  return 1;
}

static int pkg_parse_release(char *line, PkgIndex *index) {
  char *fields[8];
  PkgRelease release;
  size_t count = pkg_split_tabs(line, fields, 8);
  memset(&release, 0, sizeof(release));
  if (count != 7u || !pkg_parse_version(fields[1], &release.version) ||
      strlen(fields[2]) != 64u) {
    return 0;
  }
  release.sha256 = pkg_copy(fields[2]);
  if (strcmp(fields[4], "-") != 0) {
    if (!pkg_parse_version(fields[4], &release.compiler)) {
      return 0;
    }
    release.has_compiler = 1;
  }
  release.yanked = strcmp(fields[5], "yanked") == 0;
  if (!pkg_parse_release_needs(fields[6], &release.needs)) {
    return 0;
  }
  index->releases = (PkgRelease *)pkg_grow(index->releases, index->count,
                                           &index->capacity, sizeof(PkgRelease));
  index->releases[index->count++] = release;
  return 1;
}

static int pkg_parse_index(const char *name, char *text, PkgIndex *index) {
  char *cursor = text;
  char *line = pkg_next_line(&cursor);
  if (!line || strcmp(line, PKG_INDEX_HEADER) != 0) {
    return pkg_fail("the registry's index for %s has a newer format; update "
                    "the compiler",
                    name);
  }
  index->name = pkg_copy(name);
  while ((line = pkg_next_line(&cursor))) {
    if (strncmp(line, "description\t", 12) == 0) {
      index->description = pkg_copy(line + 12);
    } else if (strncmp(line, "release\t", 8) == 0 &&
               !pkg_parse_release(line, index)) {
      return pkg_fail("the registry's index for %s is malformed", name);
    }
  }
  return 1;
}

static int pkg_download_text(const char *relative, const char *subject,
                             PkgBytes *file) {
  const char *temporary = pkg_tmp_file("index");
  int code = pkg_fetch(relative, temporary, PKG_MAX_INDEX);
  if (code != 0) {
    remove(temporary);
    if (code == 22) {
      return pkg_fail("the registry %s has no %s", pkg_registry_base(), subject);
    }
    return pkg_fail("could not read %s from %s: %s", subject,
                    pkg_registry_base(), pkg_curl_meaning(code));
  }
  if (!pkg_read_file(temporary, PKG_MAX_INDEX, file)) {
    remove(temporary);
    return pkg_fail("could not read %s", temporary);
  }
  remove(temporary);
  return 1;
}

static PkgIndex *pkg_load_index(const char *name) {
  PkgIndex *index;
  PkgBytes file;
  for (size_t i = 0; i < g_pkg_index_count; i++) {
    if (strcmp(g_pkg_indexes[i]->name, name) == 0) {
      return g_pkg_indexes[i];
    }
  }
  if (!pkg_download_text(pkg_format("packages/%s.txt", name),
                         pkg_format("package named %s", name), &file)) {
    return NULL;
  }
  index = (PkgIndex *)pkg_alloc(sizeof(PkgIndex));
  if (!pkg_parse_index(name, (char *)file.bytes, index)) {
    return NULL;
  }
  g_pkg_indexes =
      (PkgIndex **)pkg_grow(g_pkg_indexes, g_pkg_index_count,
                            &g_pkg_index_capacity, sizeof(PkgIndex *));
  g_pkg_indexes[g_pkg_index_count++] = index;
  return index;
}

static const PkgRelease *pkg_find_release(const PkgIndex *index,
                                          PkgVersion version) {
  for (size_t i = 0; i < index->count; i++) {
    if (pkg_version_compare(index->releases[i].version, version) == 0) {
      return &index->releases[i];
    }
  }
  return NULL;
}

static int pkg_compiler_version(PkgVersion *out) {
  const char *text = g_pkg_compiler;
  if (!text) {
    return 0;
  }
  if (*text == 'v') {
    text++;
  }
  return pkg_parse_version(text, out);
}

static int pkg_release_fits_compiler(const PkgRelease *release) {
  PkgVersion ours;
  return !release->has_compiler || !pkg_compiler_version(&ours) ||
         pkg_version_compare(release->compiler, ours) <= 0;
}

static const PkgRelease *pkg_latest_release(const PkgIndex *index,
                                            const unsigned *major) {
  const PkgRelease *best = NULL;
  for (size_t i = 0; i < index->count; i++) {
    const PkgRelease *release = &index->releases[i];
    if (release->yanked || !pkg_release_fits_compiler(release)) {
      continue;
    }
    if (major && release->version.major != *major) {
      continue;
    }
    if (!best || pkg_version_compare(release->version, best->version) > 0) {
      best = release;
    }
  }
  return best;
}

typedef struct {
  const char *name;
  PkgVersion version;
  const char *required_by;
} PkgWork;

typedef struct {
  PkgWork *items;
  size_t count;
  size_t capacity;
} PkgWorkList;

static void pkg_work_push(PkgWorkList *list, const char *name,
                          PkgVersion version, const char *required_by) {
  list->items = (PkgWork *)pkg_grow(list->items, list->count, &list->capacity,
                                    sizeof(PkgWork));
  list->items[list->count].name = name;
  list->items[list->count].version = version;
  list->items[list->count].required_by = required_by;
  list->count++;
}

static int pkg_work_seen(const PkgWorkList *seen, const PkgWork *work) {
  for (size_t i = 0; i < seen->count; i++) {
    if (strcmp(seen->items[i].name, work->name) == 0 &&
        pkg_version_compare(seen->items[i].version, work->version) == 0) {
      return 1;
    }
  }
  return 0;
}

static int pkg_pick_raise(PkgPicks *picks, const PkgWork *work) {
  for (size_t i = 0; i < picks->count; i++) {
    PkgPicked *pick = &picks->items[i];
    if (strcmp(pick->name, work->name) != 0) {
      continue;
    }
    if (pick->version.major != work->version.major) {
      return pkg_fail("%s needs %s %s, and %s needs %s %s. Two major versions "
                      "of one package cannot share a program",
                      pick->required_by, work->name,
                      pkg_version_text(pick->version), work->required_by,
                      work->name, pkg_version_text(work->version));
    }
    if (pkg_version_compare(work->version, pick->version) > 0) {
      pick->version = work->version;
      pick->required_by = work->required_by;
    }
    return 1;
  }
  picks->items = (PkgPicked *)pkg_grow(picks->items, picks->count,
                                       &picks->capacity, sizeof(PkgPicked));
  picks->items[picks->count].name = work->name;
  picks->items[picks->count].version = work->version;
  picks->items[picks->count].required_by = work->required_by;
  picks->count++;
  return 1;
}

static int pkg_resolve_graph(const PkgNeeds *roots, PkgPicks *picks) {
  PkgWorkList queue;
  PkgWorkList seen;
  memset(&queue, 0, sizeof(queue));
  memset(&seen, 0, sizeof(seen));
  for (size_t i = 0; i < roots->count; i++) {
    pkg_work_push(&queue, roots->items[i].name, roots->items[i].version,
                  "this project");
  }
  for (size_t next = 0; next < queue.count; next++) {
    PkgWork work = queue.items[next];
    const PkgIndex *index;
    const PkgRelease *release;
    if (pkg_work_seen(&seen, &work)) {
      continue;
    }
    pkg_work_push(&seen, work.name, work.version, work.required_by);
    if (!pkg_pick_raise(picks, &work)) {
      return 0;
    }
    index = pkg_load_index(work.name);
    if (!index) {
      return 0;
    }
    release = pkg_find_release(index, work.version);
    if (!release) {
      return pkg_fail("%s needs %s %s, which the registry does not have",
                      work.required_by, work.name,
                      pkg_version_text(work.version));
    }
    for (size_t i = 0; i < release->needs.count; i++) {
      pkg_work_push(&queue, release->needs.items[i].name,
                    release->needs.items[i].version,
                    pkg_format("%s %s", work.name,
                               pkg_version_text(work.version)));
    }
  }
  return 1;
}

static int pkg_seal_picks(PkgPicks *picks, const PkgLockFile *lock) {
  for (size_t i = 0; i < picks->count; i++) {
    PkgPicked *pick = &picks->items[i];
    const PkgIndex *index = pkg_load_index(pick->name);
    const PkgRelease *release =
        index ? pkg_find_release(index, pick->version) : NULL;
    const PkgPicked *locked = pkg_picks_find(&lock->picks, pick->name);
    if (!release) {
      return pkg_fail("the registry does not have %s %s", pick->name,
                      pkg_version_text(pick->version));
    }
    if (!pkg_release_fits_compiler(release)) {
      return pkg_fail("%s %s needs mettle %s or newer; this is %s", pick->name,
                      pkg_version_text(pick->version),
                      pkg_version_text(release->compiler), g_pkg_compiler);
    }
    if (locked && pkg_version_compare(locked->version, pick->version) == 0 &&
        strcmp(locked->sha256, release->sha256) != 0) {
      return pkg_fail("the registry's hash for %s %s is %s, and mettle.lock "
                      "says %s. A published version never changes, so treat "
                      "this as tampering",
                      pick->name, pkg_version_text(pick->version),
                      release->sha256, locked->sha256);
    }
    if (release->yanked) {
      fprintf(stderr, "warning: %s %s is yanked\n", pick->name,
              pkg_version_text(pick->version));
    }
    pick->sha256 = release->sha256;
  }
  if (picks->count > 1u) {
    qsort(picks->items, picks->count, sizeof(PkgPicked), pkg_pick_compare);
  }
  return 1;
}

static int pkg_collect_walk(const char *dir, const char *prefix,
                            PkgFiles *files) {
  char *names;
  int count;
  const char *entry;
  if (!pkg_list_dir(dir, &names, &count)) {
    return pkg_fail("could not read directory %s", dir);
  }
  entry = names;
  for (int i = 0; i < count; i++, entry += strlen(entry) + 1u) {
    const char *name = entry + 1;
    const char *relative =
        *prefix ? pkg_format("%s/%s", prefix, name) : pkg_copy(name);
    const char *full = pkg_format("%s/%s", dir, name);
    if (name[0] == '.' ||
        (!*prefix && (strcmp(name, PKG_DEPS) == 0 || strcmp(name, PKG_LOCK) == 0))) {
      continue;
    }
    if (entry[0] == 'l') {
      return pkg_fail("%s is a link; a package holds regular files", relative);
    }
    if (entry[0] == 'd') {
      if (!pkg_collect_walk(full, relative, files)) {
        return 0;
      }
      continue;
    }
    if (!pkg_path_valid(relative)) {
      return pkg_fail("%s: a package path uses letters, digits, `_`, `-`, `.` "
                      "and `+`, and is at most 240 bytes",
                      relative);
    }
    if (pkg_extension_is_binary(relative)) {
      return pkg_fail("%s: a package holds source, not binaries or archives",
                      relative);
    }
    files->items = (PkgFile *)pkg_grow(files->items, files->count,
                                       &files->capacity, sizeof(PkgFile));
    files->items[files->count].path = relative;
    files->count++;
    if (files->count > PKG_MAX_FILES) {
      return pkg_fail("a package holds at most %d files", PKG_MAX_FILES);
    }
  }
  return 1;
}

static int pkg_file_compare(const void *left, const void *right) {
  return strcmp(((const PkgFile *)left)->path, ((const PkgFile *)right)->path);
}

static int pkg_file_compare_folded(const void *left, const void *right) {
  return strcasecmp(((const PkgFile *)left)->path,
                    ((const PkgFile *)right)->path);
}

static int pkg_collect(const char *dir, PkgFiles *files) {
  memset(files, 0, sizeof(*files));
  if (!pkg_collect_walk(dir, "", files)) {
    return 0;
  }
  if (files->count > 1u) {
    qsort(files->items, files->count, sizeof(PkgFile), pkg_file_compare_folded);
    for (size_t i = 1; i < files->count; i++) {
      if (strcasecmp(files->items[i - 1u].path, files->items[i].path) == 0) {
        return pkg_fail("%s and %s differ only by case", files->items[i - 1u].path,
                        files->items[i].path);
      }
    }
    qsort(files->items, files->count, sizeof(PkgFile), pkg_file_compare);
  }
  return 1;
}

static void pkg_tar_octal(char *field, size_t width, size_t value) {
  field[width - 1u] = '\0';
  for (size_t i = width - 1u; i > 0; i--) {
    field[i - 1u] = (char)('0' + (value & 7u));
    value >>= 3;
  }
}

static int pkg_tar_split_name(const char *path, char *header) {
  size_t length = strlen(path);
  if (length <= 100u) {
    memcpy(header, path, length);
    return 1;
  }
  for (const char *slash = strchr(path, '/'); slash;
       slash = strchr(slash + 1, '/')) {
    size_t head = (size_t)(slash - path);
    size_t tail = length - head - 1u;
    if (head <= 155u && tail <= 100u) {
      memcpy(header, slash + 1, tail);
      memcpy(header + 345, path, head);
      return 1;
    }
  }
  return 0;
}

static int pkg_tar_header(const char *path, size_t size, unsigned char *block) {
  char *header = (char *)block;
  unsigned sum = 0;
  memset(block, 0, PKG_TAR_BLOCK);
  if (!pkg_tar_split_name(path, header)) {
    return pkg_fail("%s: path does not fit a tar header", path);
  }
  memcpy(header + 100, "0000644", 8);
  memcpy(header + 108, "0000000", 8);
  memcpy(header + 116, "0000000", 8);
  pkg_tar_octal(header + 124, 12, size);
  memcpy(header + 136, "00000000000", 12);
  memset(header + 148, ' ', 8);
  header[156] = '0';
  memcpy(header + 257, "ustar", 6);
  memcpy(header + 263, "00", 2);
  memcpy(header + 329, "0000000", 8);
  memcpy(header + 337, "0000000", 8);
  for (size_t i = 0; i < PKG_TAR_BLOCK; i++) {
    sum += block[i];
  }
  pkg_tar_octal(header + 148, 7, sum);
  header[155] = ' ';
  return 1;
}

static int pkg_pack_dir(const char *dir, const PkgFiles *files, PkgBytes *out) {
  size_t total = PKG_TAR_BLOCK * 2u;
  PkgBytes *contents = (PkgBytes *)pkg_alloc(sizeof(PkgBytes) * (files->count + 1u));
  size_t at = 0;
  for (size_t i = 0; i < files->count; i++) {
    const char *full = pkg_format("%s/%s", dir, files->items[i].path);
    if (!pkg_read_file(full, PKG_MAX_ARCHIVE, &contents[i])) {
      return pkg_fail("could not read %s, or it is over 16 MiB", full);
    }
    total += PKG_TAR_BLOCK +
             ((contents[i].size + PKG_TAR_BLOCK - 1u) / PKG_TAR_BLOCK) *
                 PKG_TAR_BLOCK;
  }
  if (total > PKG_MAX_ARCHIVE) {
    return pkg_fail("a package is at most 16 MiB");
  }
  out->bytes = (unsigned char *)pkg_alloc(total);
  out->size = total;
  for (size_t i = 0; i < files->count; i++) {
    if (!pkg_tar_header(files->items[i].path, contents[i].size, out->bytes + at)) {
      return 0;
    }
    at += PKG_TAR_BLOCK;
    if (contents[i].size) {
      memcpy(out->bytes + at, contents[i].bytes, contents[i].size);
    }
    at += ((contents[i].size + PKG_TAR_BLOCK - 1u) / PKG_TAR_BLOCK) * PKG_TAR_BLOCK;
  }
  return 1;
}

static const char *pkg_dir_hash(const char *dir) {
  PkgFiles files;
  PkgBytes tar;
  if (!pkg_collect(dir, &files) || !pkg_pack_dir(dir, &files, &tar)) {
    return NULL;
  }
  return pkg_sha256(tar.bytes, tar.size);
}

static int pkg_tar_field_octal(const char *field, size_t width, size_t *value) {
  size_t result = 0;
  for (size_t i = 0; i < width && field[i]; i++) {
    if (field[i] < '0' || field[i] > '7') {
      return 0;
    }
    result = (result << 3) | (size_t)(field[i] - '0');
  }
  *value = result;
  return 1;
}

static int pkg_tar_checksum_valid(const unsigned char *block) {
  size_t stored;
  unsigned sum = 0;
  if (!pkg_tar_field_octal((const char *)block + 148, 6, &stored)) {
    return 0;
  }
  for (size_t i = 0; i < PKG_TAR_BLOCK; i++) {
    sum += (i >= 148u && i < 156u) ? (unsigned)' ' : block[i];
  }
  return sum == stored;
}

static const char *pkg_tar_entry_path(const unsigned char *block) {
  char name[101];
  char prefix[156];
  memcpy(name, block, 100);
  name[100] = '\0';
  memcpy(prefix, block + 345, 155);
  prefix[155] = '\0';
  return *prefix ? pkg_format("%s/%s", prefix, name) : pkg_copy(name);
}

static int pkg_write_entry(const char *dir, const char *path,
                           const unsigned char *bytes, size_t size) {
  char *full = pkg_format("%s/%s", dir, path);
  char *slash = strrchr(full, '/');
  *slash = '\0';
  if (!pkg_make_dirs(full)) {
    return pkg_fail("could not create %s", full);
  }
  *slash = '/';
  if (!pkg_write_file(full, bytes, size)) {
    return pkg_fail("could not write %s", full);
  }
  return 1;
}

static int pkg_block_is_zero(const unsigned char *block) {
  for (size_t i = 0; i < PKG_TAR_BLOCK; i++) {
    if (block[i]) {
      return 0;
    }
  }
  return 1;
}

static int pkg_unpack(const PkgBytes *tar, const char *dir, const char *label) {
  size_t at = 0;
  while (at + PKG_TAR_BLOCK <= tar->size) {
    const unsigned char *block = tar->bytes + at;
    const char *path;
    size_t size;
    if (pkg_block_is_zero(block)) {
      return 1;
    }
    if (memcmp(block + 257, "ustar", 6) != 0 || !pkg_tar_checksum_valid(block) ||
        !pkg_tar_field_octal((const char *)block + 124, 11, &size) ||
        block[156] != '0') {
      return pkg_fail("%s: the archive is not in the registry's tar form", label);
    }
    path = pkg_tar_entry_path(block);
    at += PKG_TAR_BLOCK;
    if (!pkg_path_valid(path)) {
      return pkg_fail("%s: the archive holds an unsafe path: %s", label, path);
    }
    if (size > tar->size - at) {
      return pkg_fail("%s: the archive is truncated", label);
    }
    if (!pkg_write_entry(dir, path, tar->bytes + at, size)) {
      return 0;
    }
    at += ((size + PKG_TAR_BLOCK - 1u) / PKG_TAR_BLOCK) * PKG_TAR_BLOCK;
  }
  return pkg_fail("%s: the archive has no end marker", label);
}

static const char *pkg_cache_dir(void) {
  const char *value = getenv("METTLE_CACHE");
  if (value && *value) {
    return pkg_copy(value);
  }
  value = getenv("LOCALAPPDATA");
  if (value && *value) {
    return pkg_format("%s/Mettle/cache", value);
  }
  value = getenv("XDG_CACHE_HOME");
  if (value && *value) {
    return pkg_format("%s/mettle", value);
  }
  value = getenv("HOME");
  if (value && *value) {
    return pkg_format("%s/.cache/mettle", value);
  }
  return NULL;
}

static int pkg_cached_archive(const PkgPicked *pick, PkgBytes *tar) {
  const char *cache = pkg_cache_dir();
  const char *path;
  if (!cache) {
    return 0;
  }
  path = pkg_format("%s/%s.tar", cache, pick->sha256);
  if (!pkg_read_file(path, PKG_MAX_ARCHIVE, tar)) {
    return 0;
  }
  if (strcmp(pkg_sha256(tar->bytes, tar->size), pick->sha256) != 0) {
    remove(path);
    return 0;
  }
  return 1;
}

static void pkg_cache_store(const PkgPicked *pick, const PkgBytes *tar) {
  const char *cache = pkg_cache_dir();
  if (cache && pkg_make_dirs(cache)) {
    pkg_write_bytes_atomic(pkg_format("%s/%s.tar", cache, pick->sha256),
                           tar->bytes, tar->size);
  }
}

static int pkg_download_archive(const PkgPicked *pick, PkgBytes *tar) {
  const char *tag = pkg_tag(pick->name, pick->version);
  const char *temporary = pkg_tmp_file("archive");
  const char *got;
  int code = pkg_fetch(pkg_format("archives/%s.tar", tag), temporary, PKG_MAX_ARCHIVE);
  if (code != 0) {
    remove(temporary);
    return pkg_fail("could not download %s from %s: %s", tag, pkg_registry_base(),
                    pkg_curl_meaning(code));
  }
  if (!pkg_read_file(temporary, PKG_MAX_ARCHIVE, tar)) {
    remove(temporary);
    return pkg_fail("could not read the download of %s", tag);
  }
  remove(temporary);
  got = pkg_sha256(tar->bytes, tar->size);
  if (strcmp(got, pick->sha256) != 0) {
    return pkg_fail("%s: downloaded sha256 %s, expected %s", tag, got, pick->sha256);
  }
  pkg_cache_store(pick, tar);
  return 1;
}

static int pkg_install_one(const PkgPicked *pick) {
  const char *tag = pkg_tag(pick->name, pick->version);
  const char *final_dir = pkg_path(pkg_format(PKG_PACKAGES "/%s", tag));
  const char *work_dir = pkg_path(pkg_format(PKG_TMP "/unpack-%s", tag));
  const char *hash;
  PkgBytes tar;
  if (mettle_path_is_directory(final_dir)) {
    hash = pkg_dir_hash(final_dir);
    if (hash && strcmp(hash, pick->sha256) == 0) {
      return 1;
    }
    printf("%s was changed on disk; installing it again\n", tag);
    if (!pkg_remove_tree(final_dir)) {
      return pkg_fail("could not remove %s", final_dir);
    }
  }
  if (!pkg_cached_archive(pick, &tar) && !pkg_download_archive(pick, &tar)) {
    return 0;
  }
  if (mettle_path_is_directory(work_dir) && !pkg_remove_tree(work_dir)) {
    return pkg_fail("could not remove %s", work_dir);
  }
  if (!pkg_make_dirs(work_dir) || !pkg_unpack(&tar, work_dir, tag)) {
    return 0;
  }
  hash = pkg_dir_hash(work_dir);
  if (!hash || strcmp(hash, pick->sha256) != 0) {
    pkg_remove_tree(work_dir);
    return pkg_fail("%s: the unpacked files do not hash to the archive", tag);
  }
  if (mettle_move_path(work_dir, final_dir) != 0) {
    return pkg_fail("could not move %s into place", tag);
  }
  printf("installed %s %s\n", pick->name, pkg_version_text(pick->version));
  return 1;
}

static int pkg_prune_stale(const PkgPicks *picks) {
  const char *dir = pkg_path(PKG_PACKAGES);
  char *names;
  int count;
  const char *entry;
  if (!pkg_list_dir(dir, &names, &count)) {
    return 1;
  }
  entry = names;
  for (int i = 0; i < count; i++, entry += strlen(entry) + 1u) {
    int keep = entry[0] != 'd';
    for (size_t k = 0; !keep && k < picks->count; k++) {
      keep = strcmp(entry + 1, pkg_tag(picks->items[k].name, picks->items[k].version)) == 0;
    }
    if (!keep) {
      printf("removed %s\n", entry + 1);
      pkg_remove_tree(pkg_format("%s/%s", dir, entry + 1));
    }
  }
  return 1;
}

static int pkg_deps_value_is_managed(const char *value) {
  if (value[0] == '.' && (value[1] == '/' || value[1] == '\\')) {
    value += 2;
  }
  return strncmp(value, PKG_PACKAGES "/", strlen(PKG_PACKAGES) + 1u) == 0;
}

static int pkg_deps_check(const PkgNeeds *roots) {
  PkgBytes file;
  char *cursor;
  char *line;
  if (!pkg_read_file(pkg_path(PKG_DEPS), PKG_MAX_INDEX, &file)) {
    return 1;
  }
  cursor = (char *)file.bytes;
  while ((line = pkg_next_line(&cursor))) {
    char *equals = strchr(line, '=');
    char *name = pkg_trim(line);
    if (*name == '#' || !equals) {
      continue;
    }
    *equals = '\0';
    name = pkg_trim(line);
    if (pkg_needs_find(roots, name) &&
        !pkg_deps_value_is_managed(pkg_trim(equals + 1))) {
      return pkg_fail("mettle.deps already maps %s to %s; remove that line "
                      "first",
                      name, pkg_trim(equals + 1));
    }
  }
  return 1;
}

static int pkg_deps_save(const PkgNeeds *roots, const PkgPicks *picks) {
  const char *path = pkg_path(PKG_DEPS);
  PkgBytes file;
  char *out = pkg_copy("");
  char *inner = pkg_copy("");
  if (pkg_read_file(path, PKG_MAX_INDEX, &file)) {
    char *cursor = (char *)file.bytes;
    char *line;
    while ((line = pkg_next_line(&cursor))) {
      char *copy = pkg_copy(line);
      char *equals = strchr(copy, '=');
      if (equals && *pkg_trim(copy) != '#' &&
          pkg_deps_value_is_managed(pkg_trim(equals + 1))) {
        continue;
      }
      out = pkg_format("%s%s\n", out, line);
    }
  }
  for (size_t i = 0; i < picks->count; i++) {
    const PkgPicked *pick = &picks->items[i];
    const char *tag = pkg_tag(pick->name, pick->version);
    inner = pkg_format("%s%s=./%s\n", inner, pick->name, tag);
    if (pkg_needs_find(roots, pick->name)) {
      out = pkg_format("%s%s=" PKG_PACKAGES "/%s\n", out, pick->name, tag);
    }
  }
  if (!pkg_write_atomic(pkg_path(PKG_PACKAGES "/" PKG_DEPS), inner)) {
    return 0;
  }
  if (!*out) {
    remove(path);
    return 1;
  }
  return pkg_write_atomic(path, out);
}

static int pkg_finish_install(const PkgNeeds *roots, const PkgPicks *picks,
                              int write_lock) {
  if (!pkg_ensure_state() || !pkg_deps_check(roots)) {
    return 0;
  }
  for (size_t i = 0; i < picks->count; i++) {
    if (!pkg_install_one(&picks->items[i])) {
      return 0;
    }
  }
  pkg_prune_stale(picks);
  return pkg_deps_save(roots, picks) &&
         (!write_lock || pkg_lock_save(roots, picks));
}

static int pkg_apply_roots(const PkgNeeds *roots, const PkgLockFile *lock) {
  PkgPicks picks;
  memset(&picks, 0, sizeof(picks));
  if (!pkg_resolve_graph(roots, &picks) || !pkg_seal_picks(&picks, lock) ||
      !pkg_finish_install(roots, &picks, 1)) {
    return 0;
  }
  return pkg_manifest_save(roots);
}

static void pkg_use_shared_scratch(void) {
  const char *cache = pkg_cache_dir();
  g_pkg_scratch = cache && pkg_make_dirs(cache) ? cache : ".";
}

static void pkg_find_root(void) {
  char *probe = pkg_copy("");
  char *here = mettle_realpath(".", NULL);
  for (int depth = 0; depth < PKG_MAX_DEPTH; depth++) {
    char *up;
    char *resolved;
    if (mettle_path_exists(pkg_format("%s%s", probe, PKG_MANIFEST))) {
      g_pkg_root = probe;
      break;
    }
    up = pkg_format("%s../", probe);
    resolved = mettle_realpath(up, NULL);
    if (!resolved || !here || strcmp(resolved, here) == 0) {
      free(resolved);
      break;
    }
    free(here);
    here = resolved;
    probe = up;
  }
  free(here);
}

static int pkg_load_project(PkgManifest *manifest, PkgLockFile *lock) {
  pkg_find_root();
  if (pkg_manifest_load(pkg_path(PKG_MANIFEST), manifest) < 0 ||
      !pkg_ensure_state()) {
    return 0;
  }
  return pkg_lock_load(lock);
}

static int pkg_add_one(const char *spec, PkgNeeds *roots) {
  char *name = pkg_copy(spec);
  char *at = strchr(name, '@');
  PkgVersion version;
  const PkgIndex *index;
  if (at) {
    *at = '\0';
    if (!pkg_parse_version(at + 1, &version)) {
      return pkg_fail("`%s` is not a version; use MAJOR.MINOR.PATCH", at + 1);
    }
  }
  if (!pkg_name_valid(name)) {
    return pkg_fail("`%s` is not a package name", name);
  }
  index = pkg_load_index(name);
  if (!index) {
    return 0;
  }
  if (!at) {
    const PkgRelease *latest = pkg_latest_release(index, NULL);
    if (!latest) {
      return pkg_fail("%s has no release that this compiler (%s) can use", name,
                      g_pkg_compiler);
    }
    version = latest->version;
  }
  pkg_needs_set(roots, name, version);
  printf("added %s %s\n", name, pkg_version_text(version));
  return 1;
}

static int pkg_cmd_add(int argc, char **argv) {
  PkgManifest manifest;
  PkgLockFile lock;
  if (argc < 1) {
    return pkg_fail("usage: mettle pkg add <name>[@version]...");
  }
  if (!pkg_load_project(&manifest, &lock)) {
    return 0;
  }
  for (int i = 0; i < argc; i++) {
    if (!pkg_add_one(argv[i], &manifest.needs)) {
      return 0;
    }
  }
  return pkg_apply_roots(&manifest.needs, &lock);
}

static int pkg_cmd_remove(int argc, char **argv) {
  PkgManifest manifest;
  PkgLockFile lock;
  PkgNeeds kept;
  if (argc < 1) {
    return pkg_fail("usage: mettle pkg remove <name>...");
  }
  if (!pkg_load_project(&manifest, &lock)) {
    return 0;
  }
  for (int i = 0; i < argc; i++) {
    if (!pkg_needs_find(&manifest.needs, argv[i])) {
      return pkg_fail("this project does not require %s", argv[i]);
    }
  }
  memset(&kept, 0, sizeof(kept));
  for (size_t i = 0; i < manifest.needs.count; i++) {
    int drop = 0;
    for (int k = 0; k < argc; k++) {
      drop = drop || strcmp(argv[k], manifest.needs.items[i].name) == 0;
    }
    if (!drop) {
      pkg_needs_set(&kept, manifest.needs.items[i].name,
                    manifest.needs.items[i].version);
    }
  }
  return pkg_apply_roots(&kept, &lock);
}

static int pkg_cmd_install(int argc, char **argv) {
  PkgManifest manifest;
  PkgLockFile lock;
  int frozen = argc >= 1 && strcmp(argv[0], "--frozen") == 0;
  if (!pkg_load_project(&manifest, &lock)) {
    return 0;
  }
  if (!mettle_path_exists(pkg_path(PKG_MANIFEST))) {
    return pkg_fail("no " PKG_MANIFEST " here or above");
  }
  if (lock.present && pkg_needs_equal(&lock.roots, &manifest.needs)) {
    return pkg_finish_install(&manifest.needs, &lock.picks, 0);
  }
  if (frozen) {
    return pkg_fail(PKG_LOCK " does not match " PKG_MANIFEST
                             "; run `mettle pkg install` and commit the result");
  }
  return pkg_apply_roots(&manifest.needs, &lock);
}

static int pkg_update_root(PkgNeed *need) {
  const PkgIndex *index = pkg_load_index(need->name);
  const PkgRelease *latest;
  if (!index) {
    return 0;
  }
  latest = pkg_latest_release(index, &need->version.major);
  if (latest && pkg_version_compare(latest->version, need->version) > 0) {
    printf("updated %s %s to %s\n", need->name, pkg_version_text(need->version),
           pkg_version_text(latest->version));
    need->version = latest->version;
  }
  return 1;
}

static int pkg_cmd_update(int argc, char **argv) {
  PkgManifest manifest;
  PkgLockFile lock;
  if (!pkg_load_project(&manifest, &lock)) {
    return 0;
  }
  for (int i = 0; i < argc; i++) {
    if (!pkg_needs_find(&manifest.needs, argv[i])) {
      return pkg_fail("this project does not require %s", argv[i]);
    }
  }
  for (size_t i = 0; i < manifest.needs.count; i++) {
    int wanted = argc == 0;
    for (int k = 0; k < argc; k++) {
      wanted = wanted || strcmp(argv[k], manifest.needs.items[i].name) == 0;
    }
    if (wanted && !pkg_update_root(&manifest.needs.items[i])) {
      return 0;
    }
  }
  return pkg_apply_roots(&manifest.needs, &lock);
}

static int pkg_cmd_list(void) {
  PkgManifest manifest;
  PkgLockFile lock;
  pkg_find_root();
  if (pkg_manifest_load(pkg_path(PKG_MANIFEST), &manifest) < 0 ||
      !pkg_lock_load(&lock)) {
    return 0;
  }
  if (!lock.present) {
    return pkg_fail("no " PKG_LOCK "; run `mettle pkg install`");
  }
  for (size_t i = 0; i < lock.picks.count; i++) {
    const PkgPicked *pick = &lock.picks.items[i];
    printf("%-24s %-10s %s\n", pick->name, pkg_version_text(pick->version),
           pkg_needs_find(&manifest.needs, pick->name) ? "direct" : "indirect");
  }
  return 1;
}

static int pkg_contains_folded(const char *haystack, const char *needle) {
  size_t length = strlen(needle);
  size_t available = strlen(haystack);
  for (size_t i = 0; i + length <= available; i++) {
    if (pkg_folded_prefix(haystack + i, needle, length)) {
      return 1;
    }
  }
  return length == 0;
}

static int pkg_cmd_search(int argc, char **argv) {
  const char *query = argc >= 1 ? argv[0] : "";
  PkgBytes file;
  char *cursor;
  char *line;
  int shown = 0;
  pkg_use_shared_scratch();
  if (!pkg_download_text("index.txt", "package index", &file)) {
    return 0;
  }
  cursor = (char *)file.bytes;
  line = pkg_next_line(&cursor);
  if (!line || strcmp(line, PKG_INDEX_HEADER) != 0) {
    return pkg_fail("the registry's index has a newer format; update the compiler");
  }
  while ((line = pkg_next_line(&cursor))) {
    char *fields[4];
    if (pkg_split_tabs(line, fields, 4) != 4u ||
        strcmp(fields[0], "package") != 0) {
      continue;
    }
    if (pkg_contains_folded(fields[1], query) ||
        pkg_contains_folded(fields[3], query)) {
      printf("%-24s %-10s %s\n", fields[1], fields[2], fields[3]);
      shown++;
    }
  }
  if (!shown) {
    printf("no package matches `%s`\n", query);
  }
  return 1;
}

static int pkg_release_compare(const void *left, const void *right) {
  return pkg_version_compare(((const PkgRelease *)left)->version,
                             ((const PkgRelease *)right)->version);
}

static int pkg_cmd_info(int argc, char **argv) {
  PkgIndex *index;
  if (argc < 1) {
    return pkg_fail("usage: mettle pkg info <name>");
  }
  if (!pkg_name_valid(argv[0])) {
    return pkg_fail("`%s` is not a package name", argv[0]);
  }
  pkg_use_shared_scratch();
  index = pkg_load_index(argv[0]);
  if (!index) {
    return 0;
  }
  printf("%s\n", index->name);
  if (index->description) {
    printf("%s\n", index->description);
  }
  printf("\n");
  if (index->count > 1u) {
    qsort(index->releases, index->count, sizeof(PkgRelease), pkg_release_compare);
  }
  for (size_t i = index->count; i > 0; i--) {
    const PkgRelease *release = &index->releases[i - 1u];
    printf("%-10s %s%s\n", pkg_version_text(release->version), release->sha256,
           release->yanked ? " yanked" : "");
    for (size_t k = 0; k < release->needs.count; k++) {
      printf("           requires %s %s\n", release->needs.items[k].name,
             pkg_version_text(release->needs.items[k].version));
    }
  }
  return 1;
}

static int pkg_pack_check(const char *dir, const PkgManifest *manifest) {
  if (!manifest->name || !manifest->has_version || !manifest->description ||
      !manifest->license) {
    return pkg_fail("%s/" PKG_MANIFEST
                    " needs name, version, description and license",
                    dir);
  }
  if (!mettle_path_exists(pkg_format("%s/%s.mettle", dir, manifest->name))) {
    return pkg_fail("%s/%s.mettle is missing; `import \"%s\"` loads it", dir,
                    manifest->name, manifest->name);
  }
  if (!mettle_path_exists(pkg_format("%s/LICENSE", dir))) {
    return pkg_fail("%s/LICENSE is missing", dir);
  }
  return 1;
}

static int pkg_cmd_pack(int argc, char **argv) {
  const char *dir = ".";
  const char *out = NULL;
  PkgManifest manifest;
  PkgFiles files;
  PkgBytes tar;
  int loaded;
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      out = argv[++i];
    } else {
      dir = argv[i];
    }
  }
  loaded = pkg_manifest_load(pkg_format("%s/" PKG_MANIFEST, dir), &manifest);
  if (loaded == 0) {
    return pkg_fail("%s/" PKG_MANIFEST " is missing", dir);
  }
  if (loaded < 0 || !pkg_pack_check(dir, &manifest) ||
      !pkg_collect(dir, &files) || !pkg_pack_dir(dir, &files, &tar)) {
    return 0;
  }
  printf("%s %s\n", manifest.name, pkg_version_text(manifest.version));
  for (size_t i = 0; i < files.count; i++) {
    printf("  %s\n", files.items[i].path);
  }
  printf("size %zu\nsha256 %s\n", tar.size, pkg_sha256(tar.bytes, tar.size));
  if (out && !pkg_write_file(out, tar.bytes, tar.size)) {
    return pkg_fail("could not write %s", out);
  }
  return 1;
}

static int pkg_usage(void) {
  printf("mettle pkg - packages from the Mettle registry\n\n"
         "  mettle pkg add <name>[@version]...   require packages and install them\n"
         "  mettle pkg remove <name>...          stop requiring packages\n"
         "  mettle pkg install [--frozen]        install what mettle.lock names\n"
         "  mettle pkg update [name...]          raise requirements within their major version\n"
         "  mettle pkg list                      show what this project resolved to\n"
         "  mettle pkg search [text]             search the registry\n"
         "  mettle pkg info <name>               show a package's versions\n"
         "  mettle pkg pack [dir] [-o file.tar]  check a package and print its hash\n\n"
         "  METTLE_REGISTRY  registry address or directory\n"
         "                   default " PKG_DEFAULT_REGISTRY "\n"
         "  METTLE_CACHE     archive cache directory\n");
  return 1;
}

int mettle_pkg_main(int argc, char **argv, const char *compiler_version) {
  const char *command = argc >= 3 ? argv[2] : "";
  int rest = argc >= 3 ? argc - 3 : 0;
  char **args = argv + 3;
  int ok;
  g_pkg_compiler = compiler_version;
  if (strcmp(command, "add") == 0) {
    ok = pkg_cmd_add(rest, args);
  } else if (strcmp(command, "remove") == 0) {
    ok = pkg_cmd_remove(rest, args);
  } else if (strcmp(command, "install") == 0) {
    ok = pkg_cmd_install(rest, args);
  } else if (strcmp(command, "update") == 0) {
    ok = pkg_cmd_update(rest, args);
  } else if (strcmp(command, "list") == 0) {
    ok = pkg_cmd_list();
  } else if (strcmp(command, "search") == 0) {
    ok = pkg_cmd_search(rest, args);
  } else if (strcmp(command, "info") == 0) {
    ok = pkg_cmd_info(rest, args);
  } else if (strcmp(command, "pack") == 0) {
    ok = pkg_cmd_pack(rest, args);
  } else {
    ok = pkg_usage() && (!*command || strcmp(command, "help") == 0 ||
                         strcmp(command, "--help") == 0);
  }
  if (mettle_path_is_directory(pkg_path(PKG_TMP))) {
    pkg_remove_tree(pkg_path(PKG_TMP));
  }
  pkg_release_memory();
  return ok ? 0 : 1;
}
