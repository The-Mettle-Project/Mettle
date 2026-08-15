#ifndef COMPTIME_VALUE_H
#define COMPTIME_VALUE_H

#include <stdint.h>

/* Compile-time values. Integers and floats already fold in the type checker;
 * TypeRef and FieldRef are the reflection cases. A TypeRef is an index into
 * the type checker's type table. A FieldRef names one field of a table type.
 *
 * These are values, not a parallel type system. `int32` in value position is
 * a TypeRef; `Point.x` is a FieldRef. Neither has a machine layout.
 *
 * A ComptimeValue is trivially copyable and owns nothing. The two variants
 * that carry a pointer both borrow: a String points at an interned string,
 * which lives for the compile, and a Sequence points into an arena owned by
 * the TypeChecker. Keeping the value POD matters because it is passed and
 * returned by value everywhere, with no copy or free hook to hang ownership
 * on. */

typedef enum {
  COMPTIME_NONE = 0,
  COMPTIME_INT,
  COMPTIME_FLOAT,
  COMPTIME_TYPE_REF,
  COMPTIME_FIELD_REF,
  COMPTIME_STRING,
  COMPTIME_SEQUENCE
} ComptimeValueKind;

typedef struct {
  uint32_t type_index;
} TypeRef;

typedef struct {
  uint32_t type_index;
  uint32_t field_index;
} FieldRef;

/* Interned; never freed by the value. */
typedef struct {
  const char *value;
} ComptimeString;

struct ComptimeValue;

/* Borrowed view of a run of values in the checker's sequence arena. */
typedef struct {
  const struct ComptimeValue *items;
  uint32_t count;
} ComptimeSequence;

typedef struct ComptimeValue {
  ComptimeValueKind kind;
  union {
    long long int_value;
    double float_value;
    TypeRef type_ref;
    FieldRef field_ref;
    ComptimeString string;
    ComptimeSequence sequence;
  } as;
} ComptimeValue;

ComptimeValue comptime_none(void);
ComptimeValue comptime_int(long long value);
ComptimeValue comptime_float(double value);
ComptimeValue comptime_type_ref(uint32_t type_index);
ComptimeValue comptime_field_ref(uint32_t type_index, uint32_t field_index);
/* `value` must be interned or otherwise outlive the compile. */
ComptimeValue comptime_string(const char *value);
/* `items` must point into storage that outlives the value; see the arena on
 * TypeChecker. */
ComptimeValue comptime_sequence(const ComptimeValue *items, uint32_t count);

int comptime_is_none(ComptimeValue value);
int comptime_is_reflection(ComptimeValue value);
const char *comptime_kind_name(ComptimeValueKind kind);

#endif /* COMPTIME_VALUE_H */
