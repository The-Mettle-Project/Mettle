#ifndef OWNERSHIP_INTERNAL_H
#define OWNERSHIP_INTERNAL_H

#include "semantic/ownership.h"
#include "semantic/symbol_table.h"

enum { OWN_LIVE = 1, OWN_MOVED = 2 };

typedef enum { OWN_MODE_DISCOVER, OWN_MODE_CHECK } OwnMode;

typedef enum { OWN_BIND_LOCAL, OWN_BIND_PARAM, OWN_BIND_ARM } OwnBindKind;

typedef struct {
  const char *name;
  struct Type *type;
  OwnBindKind kind;
  ASTNode *declaration;
  ASTNode *block;
  size_t scope;
  SourceLocation location;
  int root;
  int needs_drop;
  int reported;
  int declared;
} OwnBinding;

typedef struct {
  int binding;
  int parent;
  int field;
  struct Type *type;
  int bit;
  int wants_bit;
  char *path;
  ASTNode *last_move;
  ASTNode *last_drop;
} OwnPlace;

typedef struct {
  const void **keys;
  int *values;
  size_t slots;
  size_t used;
} OwnNodeMap;

typedef struct {
  unsigned char *flags;
  int dead;
} OwnState;

typedef enum {
  OWN_DEFER_DROP,
  OWN_DEFER_FORGET,
  OWN_DEFER_DROP_UNTRACKED,
  OWN_DEFER_STATEMENT
} OwnDeferKind;

typedef struct {
  OwnDeferKind kind;
  ASTNode *defer;
  ASTNode *call;
  int place;
} OwnDeferEntry;

typedef struct {
  ASTNode *block;
  OwnDeferEntry *defers;
  size_t defer_count;
  size_t defer_capacity;
  int *bindings;
  size_t binding_count;
  size_t binding_capacity;
} OwnScope;

typedef enum { OWN_FRAME_LOOP, OWN_FRAME_SWITCH, OWN_FRAME_VALUE } OwnFrameKind;

typedef struct {
  OwnFrameKind kind;
  const char *label;
  int scope_id;
  size_t depth;
  OwnState breaks;
  OwnState continues;
  ASTNode *node;
} OwnFrame;

typedef enum {
  OWN_OB_SCOPE_DROP,
  OWN_OB_ASSIGN_DROP,
  OWN_OB_DISCARD,
  OWN_OB_TEMPORARY,
  OWN_OB_SCRUTINEE
} OwnObligationKind;

typedef struct {
  OwnObligationKind kind;
  int binding;
  ASTNode *node;
  ASTNode *root;
  ASTNode *statement;
  ASTNode *block;
  int root_is_statement;
  int untracked;
  SourceLocation exit_at;
} OwnObligation;

typedef struct {
  const void *unit;
  int place;
  int value;
} OwnEvent;

typedef struct {
  ASTNode *call;
  ASTNode *exit;
  OwnDropStep *steps;
  size_t count;
} OwnPendingPlan;

typedef struct OwnWalker {
  TypeChecker *checker;
  ErrorReporter *reporter;
  ASTNode *function_node;
  FunctionDeclaration *function;
  OwnMode mode;
  int is_explicit;
  int silent;
  OwnBinding *bindings;
  size_t binding_count;
  size_t binding_capacity;
  OwnPlace *places;
  size_t place_count;
  size_t place_capacity;
  OwnNodeMap idents;
  OwnNodeMap declarations;
  OwnNodeMap assigns;
  OwnScope *scopes;
  size_t scope_count;
  size_t scope_capacity;
  OwnFrame *frames;
  size_t frame_count;
  size_t frame_capacity;
  OwnState state;
  OwnObligation *obligations;
  size_t obligation_count;
  size_t obligation_capacity;
  OwnEvent *events;
  size_t event_count;
  size_t event_capacity;
  OwnPendingPlan *pending;
  size_t pending_count;
  size_t pending_capacity;
  const void *unit;
  ASTNode *statement;
  ASTNode *note_statement;
  ASTNode *statement_block;
  ASTNode *root;
  int conditional;
  int in_errdefer;
  int in_deferred;
  int errors;
  int internal_errors;
} OwnWalker;

typedef struct {
  ASTNode **items;
  size_t count;
  size_t capacity;
} OwnNodeList;

typedef enum {
  OWN_FIX_INSERT_AFTER,
  OWN_FIX_INSERT_BEFORE,
  OWN_FIX_WRAP_DROP,
  OWN_FIX_REPLACE,
  OWN_FIX_ASSIGN_HOLD,
  OWN_FIX_TEMPORARY
} OwnFixKind;

typedef struct {
  OwnFixKind kind;
  char *file;
  size_t line;
  size_t column;
  size_t statement_line;
  size_t statement_column;
  int statement_returns;
  char *text;
  ASTNode *node;
} OwnFix;

void own_add_fix(OwnFixKind kind, SourceLocation at, const char *text,
                 ASTNode *node);
void own_add_statement_fix(OwnFixKind kind, SourceLocation at,
                           SourceLocation statement, int returns,
                           const char *text);
size_t own_fix_count(void);
OwnFix *own_fix_at(size_t index);
void own_note_block_end(ASTNode *block, const char *text);
void own_note_statement(ASTNode *statement, const char *text);
int own_elaborate_round(OwnWalker *walker, int *changed);

void own_map_init(OwnNodeMap *map);
void own_map_free(OwnNodeMap *map);
int own_map_put(OwnNodeMap *map, const void *key, int value);
int own_map_get(const OwnNodeMap *map, const void *key, int *value);

int own_type_owns(TypeChecker *checker, struct Type *type);
const char *own_destructor_name(TypeChecker *checker, struct Type *type);
int own_type_has_destructor(TypeChecker *checker, struct Type *type);
struct Type *own_field_type(struct Type *type, const char *field, int *index);
struct Type *own_declared_type(TypeChecker *checker, VarDeclaration *decl);
struct Type *own_param_type(TypeChecker *checker, FunctionDeclaration *fd,
                            size_t index);
char *own_type_text(struct Type *type);
void own_require_glue(TypeChecker *checker, struct Type *type);

int own_walk_function(OwnWalker *walker);
void own_walker_init(OwnWalker *walker, TypeChecker *checker,
                     ErrorReporter *reporter, ASTNode *function_node,
                     OwnMode mode);
void own_walker_free(OwnWalker *walker);

int own_apply_obligations(OwnWalker *walker);
int own_report_obligations(OwnWalker *walker);
void own_commit_outputs(OwnWalker *walker);

ASTNode *own_make_identifier(const char *name, struct Type *type,
                             SourceLocation at);
ASTNode *own_make_call(const char *name, ASTNode **args, size_t count,
                       struct Type *result, SourceLocation at);
ASTNode *own_make_defer(ASTNode *statement, SourceLocation at);
ASTNode *own_make_block(SourceLocation at);
void own_block_insert(ASTNode *block, size_t index, ASTNode *statement);
long own_block_index(ASTNode *block, const ASTNode *statement);
ASTNode *own_node_move_out(ASTNode *node);
ASTNode *own_clone_place(ASTNode *place);
int own_places_equal(const ASTNode *a, const ASTNode *b);
const char *own_place_text(const ASTNode *place, char *out, size_t size);
struct Type *own_void_type(TypeChecker *checker);

void own_attach_note(ASTNode *node, OwnRecordKind kind, const char *subject,
                     struct Type *type, SourceLocation at,
                     SourceLocation origin, const char *reason,
                     const char *function);
void own_add_record(OwnRecordKind kind, SourceLocation at,
                    SourceLocation origin, const char *function,
                    const char *subject, struct Type *type, const char *text,
                    const char *reason, int inserted, int written);
void own_store_plan(const ASTNode *drop_call, const ASTNode *exit,
                    OwnDropStep *steps, size_t count);
void own_store_unit_write(const void *unit, int bit, int value);
int own_new_bit(void);
void own_store_function_bit(const ASTNode *function, int bit);
void own_link_compiled(const ASTNode *drop_call);
int own_is_glue_function(const FunctionDeclaration *fd);
const char *own_function_name(const OwnWalker *walker);

#endif
