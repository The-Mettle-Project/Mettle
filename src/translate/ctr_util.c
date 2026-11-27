#include "translate/ctr.h"
#include "runtime/owned.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct CtrChunk {
  struct CtrChunk *next;
  size_t used;
  size_t size;
  char data[];
} CtrChunk;

static CtrChunk *ctr_chunks;

void *ctr_alloc(size_t size) {
  size = (size + 15) & ~(size_t)15;
  if (!ctr_chunks || ctr_chunks->used + size > ctr_chunks->size) {
    size_t want = size > (1u << 20) ? size : (1u << 20);
    CtrChunk *chunk = malloc(sizeof(CtrChunk) + want);
    if (!chunk) {
      fprintf(stderr, "mettle translate: out of memory\n");
      exit(1);
    }
    chunk->next = ctr_chunks;
    chunk->used = 0;
    chunk->size = want;
    ctr_chunks = chunk;
  }
  void *p = ctr_chunks->data + ctr_chunks->used;
  ctr_chunks->used += size;
  memset(p, 0, size);
  return p;
}

char *ctr_strndup(const char *s, size_t n) {
  char *p = ctr_alloc(n + 1);
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

char *ctr_strdup(const char *s) { return ctr_strndup(s, strlen(s)); }

static char *ctr_vformat(const char *fmt, va_list ap) {
  va_list copy;
  va_copy(copy, ap);
  int n = vsnprintf(NULL, 0, fmt, copy);
  va_end(copy);
  char *p = ctr_alloc((size_t)n + 1);
  vsnprintf(p, (size_t)n + 1, fmt, ap);
  return p;
}

char *ctr_format(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *p = ctr_vformat(fmt, ap);
  va_end(ap);
  return p;
}

static void ctr_buf_reserve(CtrBuf *buf, size_t extra) {
  if (buf->len + extra + 1 <= buf->cap) {
    return;
  }
  size_t cap = buf->cap ? buf->cap : 256;
  while (cap < buf->len + extra + 1) {
    cap *= 2;
  }
  char *data = realloc(buf->data, cap);
  if (!data) {
    fprintf(stderr, "mettle translate: out of memory\n");
    exit(1);
  }
  buf->data = data;
  buf->cap = cap;
}

void ctr_buf_putn(CtrBuf *buf, const char *s, size_t n) {
  ctr_buf_reserve(buf, n);
  memcpy(buf->data + buf->len, s, n);
  buf->len += n;
  buf->data[buf->len] = '\0';
}

void ctr_buf_put(CtrBuf *buf, const char *s) { ctr_buf_putn(buf, s, strlen(s)); }

void ctr_buf_putc(CtrBuf *buf, char c) { ctr_buf_putn(buf, &c, 1); }

void ctr_buf_printf(CtrBuf *buf, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *p = ctr_vformat(fmt, ap);
  va_end(ap);
  ctr_buf_put(buf, p);
}

char *ctr_buf_take(CtrBuf *buf) {
  char *out = ctr_strndup(buf->data ? buf->data : "", buf->len);
  free(buf->data);
  buf->data = NULL;
  buf->len = 0;
  buf->cap = 0;
  return out;
}

void ctr_vec_push(CtrVec *vec, void *item) {
  if (vec->len == vec->cap) {
    int cap = vec->cap ? vec->cap * 2 : 16;
    void **items = realloc(vec->items, sizeof(void *) * (size_t)cap);
    if (!items) {
      fprintf(stderr, "mettle translate: out of memory\n");
      exit(1);
    }
    vec->items = items;
    vec->cap = cap;
  }
  vec->items[vec->len++] = item;
}

static uint64_t ctr_hash(const char *s, int len) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (int i = 0; i < len; i++) {
    hash *= 0x100000001b3ull;
    hash ^= (unsigned char)s[i];
  }
  return hash;
}

#define CTR_TOMBSTONE ((void *)-1)

static void ctr_map_rehash(CtrMap *map) {
  int live = 0;
  for (int i = 0; i < map->capacity; i++) {
    if (map->entries[i].key && map->entries[i].key != CTR_TOMBSTONE) {
      live++;
    }
  }
  int cap = map->capacity;
  while ((live * 100) / cap >= 50) {
    cap *= 2;
  }
  CtrMap next = {0};
  next.entries = calloc((size_t)cap, sizeof(CtrMapEntry));
  next.capacity = cap;
  for (int i = 0; i < map->capacity; i++) {
    CtrMapEntry *ent = &map->entries[i];
    if (ent->key && ent->key != CTR_TOMBSTONE) {
      ctr_map_put2(&next, ent->key, ent->key_len, ent->value);
    }
  }
  free(map->entries);
  *map = next;
}

static int ctr_match(CtrMapEntry *ent, const char *key, int len) {
  return ent->key && ent->key != CTR_TOMBSTONE && ent->key_len == len &&
         memcmp(ent->key, key, (size_t)len) == 0;
}

static CtrMapEntry *ctr_map_find(CtrMap *map, const char *key, int len) {
  if (!map->entries) {
    return NULL;
  }
  uint64_t hash = ctr_hash(key, len);
  for (int i = 0; i < map->capacity; i++) {
    CtrMapEntry *ent = &map->entries[(hash + (uint64_t)i) % (uint64_t)map->capacity];
    if (ctr_match(ent, key, len)) {
      return ent;
    }
    if (ent->key == NULL) {
      return NULL;
    }
  }
  return NULL;
}

static CtrMapEntry *ctr_map_slot(CtrMap *map, const char *key, int len) {
  if (!map->entries) {
    map->entries = calloc(16, sizeof(CtrMapEntry));
    map->capacity = 16;
  } else if ((map->used + 1) * 100 / map->capacity >= 70) {
    ctr_map_rehash(map);
  }
  uint64_t hash = ctr_hash(key, len);
  for (int i = 0; i < map->capacity; i++) {
    CtrMapEntry *ent = &map->entries[(hash + (uint64_t)i) % (uint64_t)map->capacity];
    if (ctr_match(ent, key, len)) {
      return ent;
    }
    if (ent->key == CTR_TOMBSTONE) {
      ent->key = key;
      ent->key_len = len;
      return ent;
    }
    if (ent->key == NULL) {
      ent->key = key;
      ent->key_len = len;
      map->used++;
      return ent;
    }
  }
  return NULL;
}

void *ctr_map_get2(CtrMap *map, const char *key, int key_len) {
  CtrMapEntry *ent = ctr_map_find(map, key, key_len);
  return ent ? ent->value : NULL;
}

void *ctr_map_get(CtrMap *map, const char *key) {
  return ctr_map_get2(map, key, (int)strlen(key));
}

void ctr_map_put2(CtrMap *map, const char *key, int key_len, void *value) {
  CtrMapEntry *ent = ctr_map_slot(map, key, key_len);
  ent->value = value;
}

void ctr_map_put(CtrMap *map, const char *key, void *value) {
  ctr_map_put2(map, key, (int)strlen(key), value);
}

void ctr_map_delete2(CtrMap *map, const char *key, int key_len) {
  CtrMapEntry *ent = ctr_map_find(map, key, key_len);
  if (ent) {
    ent->key = CTR_TOMBSTONE;
  }
}

void ctr_map_delete(CtrMap *map, const char *key) {
  ctr_map_delete2(map, key, (int)strlen(key));
}

char *ctr_read_file(const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    return NULL;
  }
  CtrBuf buf = {0};
  char chunk[65536];
  for (;;) {
    size_t n = fread(chunk, 1, sizeof(chunk), fp);
    if (n == 0) {
      break;
    }
    ctr_buf_putn(&buf, chunk, n);
  }
  fclose(fp);
  if (buf.len == 0 || buf.data[buf.len - 1] != '\n') {
    ctr_buf_putc(&buf, '\n');
  }
  return ctr_buf_take(&buf);
}

int ctr_file_exists(const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    return 0;
  }
  fclose(fp);
  return 1;
}

int ctr_write_file(const char *path, const char *data, size_t len) {
  FILE *fp = fopen(path, "wb");
  if (!fp) {
    return 0;
  }
  size_t wrote = fwrite(data, 1, len, fp);
  fclose(fp);
  return wrote == len;
}

static int ctr_is_sep(char c) { return c == '/' || c == '\\'; }

char *ctr_path_dir(const char *path) {
  const char *end = path + strlen(path);
  while (end > path && !ctr_is_sep(end[-1])) {
    end--;
  }
  if (end == path) {
    return ctr_strdup(".");
  }
  return ctr_strndup(path, (size_t)(end - path - 1 > 0 ? end - path - 1 : 1));
}

char *ctr_path_join(const char *dir, const char *name) {
  if (!dir || !*dir || strcmp(dir, ".") == 0) {
    return ctr_strdup(name);
  }
  size_t n = strlen(dir);
  if (ctr_is_sep(dir[n - 1])) {
    return ctr_format("%s%s", dir, name);
  }
  return ctr_format("%s/%s", dir, name);
}

char *ctr_path_stem(const char *path) {
  const char *base = path + strlen(path);
  while (base > path && !ctr_is_sep(base[-1])) {
    base--;
  }
  const char *dot = strrchr(base, '.');
  return dot ? ctr_strndup(base, (size_t)(dot - base)) : ctr_strdup(base);
}

static void pop_segment(CtrBuf *out, size_t root) {
  while (out->len > root && out->data[out->len - 1] != '/') {
    out->len--;
  }
  if (out->len > root) {
    out->len--;
  }
  out->data[out->len] = '\0';
}

static int last_segment_is_parent(CtrBuf *out, size_t root) {
  size_t n = out->len;
  if (n < root + 2 || out->data[n - 1] != '.' || out->data[n - 2] != '.') {
    return 0;
  }
  return n == root + 2 || out->data[n - 3] == '/';
}

static void put_segment(CtrBuf *out, size_t root, const char *seg, size_t n) {
  if (n == 0 || (n == 1 && seg[0] == '.')) {
    return;
  }
  if (n == 2 && seg[0] == '.' && seg[1] == '.' && out->len > root && !last_segment_is_parent(out, root)) {
    pop_segment(out, root);
    return;
  }
  if (out->len > root) {
    ctr_buf_putc(out, '/');
  }
  for (size_t i = 0; i < n; i++) {
    char c = seg[i];
#if defined(_WIN32)
    c = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
#endif
    ctr_buf_putc(out, c);
  }
}

int ctr_make_dirs(const char *path) {
  char *walk = ctr_strdup(path);
  for (char *at = walk; *at; at++) {
    if (ctr_is_sep(*at) && at > walk && at[-1] != ':' && !ctr_is_sep(at[-1])) {
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

char *ctr_path_key(const char *path) {
  CtrBuf out = {0};
  const char *p = path;
  if (ctr_is_sep(*p)) {
    ctr_buf_putc(&out, '/');
    p++;
  } else if (p[0] && p[1] == ':') {
    ctr_buf_putc(&out, (char)(p[0] >= 'A' && p[0] <= 'Z' ? p[0] + 32 : p[0]));
    ctr_buf_put(&out, ":/");
    p += ctr_is_sep(p[2]) ? 3 : 2;
  }
  size_t root = out.len;
  while (*p) {
    const char *end = p;
    while (*end && !ctr_is_sep(*end)) {
      end++;
    }
    put_segment(&out, root, p, (size_t)(end - p));
    p = *end ? end + 1 : end;
  }
  if (!out.data) {
    return ctr_strdup(".");
  }
  return ctr_buf_take(&out);
}

static void ctr_report(CtrToken *tok, const char *kind, const char *fmt, va_list ap) {
  if (tok && tok->origin) {
    CtrToken *use = tok;
    while (use->origin) {
      use = use->origin;
    }
    fprintf(stderr, "%s:%d:%d: %s: ", use->file ? use->file->display : "?",
            use->line, use->col, kind);
  } else if (tok && tok->file) {
    fprintf(stderr, "%s:%d:%d: %s: ", tok->file->display, tok->line, tok->col, kind);
  } else {
    fprintf(stderr, "mettle translate: %s: ", kind);
  }
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  if (tok && tok->origin && tok->file) {
    fprintf(stderr, "%s:%d:%d: note: inside this macro expansion\n",
            tok->file->display, tok->line, tok->col);
  }
}

void ctr_error_tok(CtrToken *tok, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  ctr_report(tok, "error", fmt, ap);
  va_end(ap);
  if (ctr_prog) {
    ctr_prog->errors++;
  }
}

void ctr_warn_tok(CtrToken *tok, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  ctr_report(tok, "warning", fmt, ap);
  va_end(ap);
  if (ctr_prog) {
    ctr_prog->warnings++;
  }
}

void ctr_fatal_tok(CtrToken *tok, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  ctr_report(tok, "error", fmt, ap);
  va_end(ap);
  exit(1);
}

void ctr_fatal(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "mettle translate: error: ");
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  exit(1);
}

void ctr_note(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}

int ctr_equal(CtrToken *tok, const char *op) {
  size_t n = strlen(op);
  return (size_t)tok->len == n && memcmp(tok->loc, op, n) == 0 &&
         tok->kind != TK_STR && tok->kind != TK_MARK_BEGIN &&
         tok->kind != TK_MARK_END;
}

CtrToken *ctr_skip(CtrToken *tok, const char *op) {
  if (!ctr_equal(tok, op)) {
    ctr_fatal_tok(tok, "expected '%s'", op);
  }
  return tok->next;
}

int ctr_consume(CtrToken **rest, CtrToken *tok, const char *str) {
  if (ctr_equal(tok, str)) {
    *rest = tok->next;
    return 1;
  }
  *rest = tok;
  return 0;
}

char *ctr_token_text(CtrToken *tok) { return ctr_strndup(tok->loc, (size_t)tok->len); }
