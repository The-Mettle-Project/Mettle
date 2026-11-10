#include "type_checker_internal.h"

static int type_checker_push_loop_label(TypeChecker *checker,
                                        const char *label) {
  if (checker->loop_label_count == checker->loop_label_capacity) {
    size_t grown =
        checker->loop_label_capacity ? checker->loop_label_capacity * 2 : 8;
    const char **table =
        realloc((void *)checker->loop_labels, grown * sizeof(const char *));
    if (!table) {
      return 0;
    }
    checker->loop_labels = table;
    checker->loop_label_capacity = grown;
  }
  checker->loop_labels[checker->loop_label_count++] = label;
  return 1;
}

static void type_checker_pop_loop_label(TypeChecker *checker) {
  if (checker->loop_label_count > 0) {
    checker->loop_label_count--;
  }
}

static int type_checker_loop_label_in_scope(const TypeChecker *checker,
                                            const char *label) {
  for (size_t i = 0; i < checker->loop_label_count; i++) {
    if (checker->loop_labels[i] && strcmp(checker->loop_labels[i], label) == 0) {
      return 1;
    }
  }
  return 0;
}

static int g_report_launches = 0;
static int g_report_launches_header = 0;

void type_checker_set_launch_report(int enabled) {
  g_report_launches = enabled;
  g_report_launches_header = 0;
}

static int gpu_launch_abi_type(const Type *type) {
  return type_checker_gpu_abi_type(type);
}

static int type_checker_check_launch_controls(TypeChecker *checker,
                                              GpuLaunchStatement *launch) {
  Type *shared_type =
      type_checker_infer_type(checker, launch->dynamic_shared_bytes);
  Type *stream_type = type_checker_infer_type(checker, launch->stream);
  if (!shared_type || !stream_type) {
    return 0;
  }
  if (!type_checker_is_integer_type(shared_type)) {
    type_checker_report_type_mismatch(
        checker, launch->dynamic_shared_bytes->location,
        "integer dynamic shared-memory byte count", shared_type->name);
    return 0;
  }
  if (!type_checker_is_integer_type(stream_type) &&
      stream_type->kind != TYPE_POINTER) {
    type_checker_report_type_mismatch(checker, launch->stream->location,
                                      "integer or pointer stream handle",
                                      stream_type->name);
    return 0;
  }
  if (launch->grid_indirect) {
    Type *grid_type = type_checker_infer_type(checker, launch->grid_indirect);
    if (!grid_type) {
      return 0;
    }
    if (!type_checker_is_integer_type(grid_type) &&
        grid_type->kind != TYPE_POINTER) {
      type_checker_report_type_mismatch(
          checker, launch->grid_indirect->location,
          "device address of three uint32 threadgroup counts",
          grid_type->name);
      return 0;
    }
  }
  return 1;
}

static void type_checker_report_launch(ASTNode *statement,
                                       GpuLaunchStatement *launch,
                                       Symbol *kernel_symbol) {
  long long grid[3] = {0, 0, 0};
  long long block[3] = {0, 0, 0};
  int grid_known = 1;
  int block_known = 1;
  if (!g_report_launches_header) {
    printf("Launch report (grid and block where the compiler can fold "
           "them):\n");
    g_report_launches_header = 1;
  }
  for (size_t d = 0; d < 3; d++) {
    if (!type_checker_eval_integer_constant(launch->grid[d], &grid[d]))
      grid_known = 0;
    if (!type_checker_eval_integer_constant(launch->block[d], &block[d]))
      block_known = 0;
  }
  printf("  %s:%llu: %s",
         statement->location.filename ? statement->location.filename : "?",
         (unsigned long long)statement->location.line,
         kernel_symbol && kernel_symbol->name ? kernel_symbol->name
                                              : "<runtime handle>");
  if (launch->work) {
    long long work = 0;
    long long threads = launch->kernel_block[0] *
                        (launch->kernel_block[1] > 0 ? launch->kernel_block[1]
                                                     : 1) *
                        (launch->kernel_block[2] > 0 ? launch->kernel_block[2]
                                                     : 1);
    long long per_item = launch->kernel_threads_per_item > 0
                             ? launch->kernel_threads_per_item
                             : 1;
    long long items = threads / per_item;
    if (type_checker_eval_integer_constant(launch->work, &work) &&
        items > 0) {
      printf(" work %lld -> grid %lld", work, (work + items - 1) / items);
    } else {
      printf(" work <runtime> -> grid <runtime>");
    }
    printf(", block %lld (declared", threads);
    if (per_item > 1) {
      printf(", %lld items/block", items);
    }
    printf(")");
  } else {
    if (launch->grid_indirect) {
      printf(" grid <device>");
    } else if (grid_known) {
      printf(" grid (%lld, %lld, %lld)", grid[0], grid[1], grid[2]);
    } else {
      printf(" grid <runtime>");
    }
    if (block_known) {
      printf(", block (%lld, %lld, %lld)", block[0], block[1], block[2]);
    } else {
      printf(", block <runtime>");
    }
  }
  if (kernel_symbol && launch->kernel_block[0] <= 0) {
    printf("  [no declared block]");
  }
  if (!kernel_symbol) {
    printf("  [untyped handle: declare it with 'extern kernel' to check "
           "arguments]");
  }
  printf("\n");
}

static int type_checker_check_gpu_launch(TypeChecker *checker,
                                         ASTNode *statement) {
  GpuLaunchStatement *launch = (GpuLaunchStatement *)statement->data;
  if (!launch || !launch->kernel) {
    type_checker_set_error_at_location(checker, statement->location,
                                       "Invalid GPU launch statement");
    return 0;
  }
  if (!checker->current_function) {
    type_checker_set_error_at_location(
        checker, statement->location,
        "GPU launch statements are only valid inside a host function");
    return 0;
  }
  if (checker->current_function_decl &&
      checker->current_function_decl->data &&
      ((FunctionDeclaration *)checker->current_function_decl->data)
          ->is_kernel) {
    type_checker_set_error_at_location(
        checker, statement->location,
        "A GPU kernel cannot launch another kernel; launch from host code");
    return 0;
  }

  Symbol *kernel_symbol = NULL;
  if (launch->kernel->type == AST_IDENTIFIER && launch->kernel->data) {
    Identifier *identifier = (Identifier *)launch->kernel->data;
    Symbol *symbol = type_checker_resolve_identifier(checker, identifier);
    if (symbol && symbol->kind == SYMBOL_FUNCTION && symbol->is_kernel) {
      kernel_symbol = symbol;
    }
  }

  if (kernel_symbol) {
    launch->typed_kernel = 1;
    launch->kernel_block[0] = kernel_symbol->kernel_block[0];
    launch->kernel_block[1] = kernel_symbol->kernel_block[1];
    launch->kernel_block[2] = kernel_symbol->kernel_block[2];
    launch->kernel_threads_per_item = kernel_symbol->kernel_threads_per_item;
    size_t expected = kernel_symbol->data.function.parameter_count;
    if (launch->argument_count != expected) {
      type_checker_set_error_at_location(
          checker, statement->location,
          "GPU kernel '%s' takes %zu argument%s, but this dispatch passes %zu",
          kernel_symbol->name, expected, expected == 1 ? "" : "s",
          launch->argument_count);
      return 0;
    }
    for (size_t i = 0; i < launch->argument_count; i++) {
      Type *arg_type = type_checker_infer_type(checker, launch->arguments[i]);
      if (!arg_type) {
        return 0;
      }
      Type *param_type = kernel_symbol->data.function.parameter_types
                             ? kernel_symbol->data.function.parameter_types[i]
                             : NULL;
      if (!param_type) {
        continue;
      }
      int device_handle_for_pointer = param_type->kind == TYPE_POINTER &&
                                      type_checker_is_integer_type(arg_type);
      if (!device_handle_for_pointer &&
          !type_checker_is_assignable_from(checker, param_type, arg_type,
                                           launch->arguments[i])) {
        type_checker_report_assign_mismatch(checker, launch->arguments[i],
                                            launch->arguments[i]->location,
                                            param_type, arg_type);
        if (kernel_symbol->data.function.parameter_names &&
            kernel_symbol->data.function.parameter_names[i] &&
            checker->error_reporter) {
          char label[224];
          snprintf(label, sizeof(label),
                   "kernel '%s' parameter '%s' expects '%s', this argument is "
                   "'%s'",
                   kernel_symbol->name,
                   kernel_symbol->data.function.parameter_names[i],
                   param_type->name ? param_type->name : "?",
                   arg_type->name ? arg_type->name : "?");
          error_reporter_set_last_label(checker->error_reporter, label);
        }
        type_checker_note_declared_here(checker, kernel_symbol, "kernel");
        return 0;
      }
    }
  } else {
    if (launch->work) {
      type_checker_set_error_at_location(
          checker, statement->location,
          "Dispatch 'work:' needs the kernel's declared block shape; declare "
          "it host-side with 'extern kernel' and dispatch it by name");
      return 0;
    }
    Type *handle_type = type_checker_infer_type(checker, launch->kernel);
    if (!handle_type) {
      return 0;
    }
    if (!type_checker_is_integer_type(handle_type) &&
        handle_type->kind != TYPE_POINTER &&
        handle_type->kind != TYPE_FUNCTION_POINTER) {
      type_checker_report_type_mismatch(checker, launch->kernel->location,
                                        "integer or pointer GPU kernel handle",
                                        handle_type->name);
      return 0;
    }
  }

  if (launch->work) {
    if (launch->kernel_block[0] <= 0) {
      type_checker_set_error_at_location(
          checker, statement->location,
          "Dispatch 'work:' needs a 'kernel(block = ...)' declaration on '%s' "
          "to size the grid",
          kernel_symbol && kernel_symbol->name ? kernel_symbol->name
                                               : "the kernel");
      return 0;
    }
    Type *work_type = type_checker_infer_type(checker, launch->work);
    if (!work_type) {
      return 0;
    }
    if (!type_checker_is_integer_type(work_type)) {
      type_checker_report_type_mismatch(checker, launch->work->location,
                                        "integer GPU work count",
                                        work_type->name);
      return 0;
    }
    long long constant = 0;
    if (type_checker_eval_integer_constant(launch->work, &constant) &&
        constant <= 0) {
      type_checker_set_error_at_location(
          checker, launch->work->location,
          "GPU work count must be greater than zero");
      return 0;
    }
  }

  for (size_t d = 0; d < 3; d++) {
    ASTNode *dims[2] = {launch->grid[d], launch->block[d]};
    const char *labels[2] = {"grid", "block"};
    for (size_t which = 0; which < 2; which++) {
      Type *dim_type = type_checker_infer_type(checker, dims[which]);
      if (!dim_type) {
        return 0;
      }
      if (!type_checker_is_integer_type(dim_type)) {
        type_checker_report_type_mismatch(checker, dims[which]->location,
                                          "integer GPU launch dimension",
                                          dim_type->name);
        return 0;
      }
      long long constant = 0;
      if (type_checker_eval_integer_constant(dims[which], &constant) &&
          constant <= 0) {
        type_checker_set_error_at_location(
            checker, dims[which]->location,
            "GPU %s dimension %zu must be greater than zero", labels[which],
            d);
        return 0;
      }
    }
  }

  if (!type_checker_check_launch_controls(checker, launch)) {
    return 0;
  }

  for (size_t i = 0; i < launch->argument_count; i++) {
    Type *arg_type = type_checker_infer_type(checker, launch->arguments[i]);
    if (!arg_type) {
      return 0;
    }
    if (!gpu_launch_abi_type(arg_type)) {
      type_checker_set_error_at_location(
          checker, launch->arguments[i]->location,
          "GPU launch argument %zu has unsupported ABI type '%s'; use a "
          "scalar, a pointer, or a record built from those",
          i, arg_type->name ? arg_type->name : "unknown");
      return 0;
    }
  }

  if (kernel_symbol && !launch->work && launch->kernel_block[0] > 0) {
    long long actual[3] = {0, 0, 0};
    int all_constant = 1;
    for (size_t d = 0; d < 3; d++) {
      if (!type_checker_eval_integer_constant(launch->block[d], &actual[d])) {
        all_constant = 0;
        break;
      }
    }
    if (all_constant) {
      long long declared[3] = {launch->kernel_block[0],
                               launch->kernel_block[1] > 0
                                   ? launch->kernel_block[1]
                                   : 1,
                               launch->kernel_block[2] > 0
                                   ? launch->kernel_block[2]
                                   : 1};
      if (actual[0] != declared[0] || actual[1] != declared[1] ||
          actual[2] != declared[2]) {
        type_checker_set_error_at_location(
            checker, statement->location,
            "GPU kernel '%s' declares block (%lld, %lld, %lld) but this "
            "dispatch launches (%lld, %lld, %lld); the driver would reject it",
            kernel_symbol->name, declared[0], declared[1], declared[2],
            actual[0], actual[1], actual[2]);
        return 0;
      }
    }
  }

  if (g_report_launches) {
    type_checker_report_launch(statement, launch, kernel_symbol);
  }
  return 1;
}

static void type_checker_if_join_arm(TypeChecker *checker,
                                     unsigned char *joined, size_t joined_count,
                                     ASTNode *arm) {
  unsigned char *arm_state = NULL;
  size_t arm_count = 0;

  if (!joined || !arm || type_checker_statement_guarantees_termination(arm)) {
    return;
  }
  arm_state = type_checker_init_tracker_capture(checker, &arm_count);
  if (!arm_state) {
    return;
  }
  type_checker_init_tracker_join(
      joined, arm_state, arm_count < joined_count ? arm_count : joined_count);
  free(arm_state);
}

static int type_checker_if_check_uniform_condition(TypeChecker *checker,
                                                   IfStatement *if_stmt,
                                                   Type *condition_type) {
  if (if_stmt->uniform_mode || (condition_type && condition_type->refine_uniform)) {
    const char *why = NULL;
    if (!type_checker_expression_is_uniform(checker, if_stmt->condition,
                                            &why) &&
        !(condition_type && condition_type->refine_uniform)) {
      if (if_stmt->uniform_mode == 2) {
        type_checker_set_error_at_location(
            checker, if_stmt->condition->location,
            "'@uniform!' says every work item of the group takes the same arm, "
            "and %s varies by work item",
            why ? why : "this condition");
        return 0;
      }
      if (if_stmt->uniform_mode == 1 && checker->error_reporter) {
        char message[256];
        snprintf(message, sizeof(message),
                 "'@uniform' was asked for and %s varies by work item, so this "
                 "branch stays divergent",
                 why ? why : "this condition");
        error_reporter_add_warning(checker->error_reporter, ERROR_SEMANTIC,
                                   if_stmt->condition->location, message);
      }
    } else {
      if_stmt->uniform_mode = 3;
    }
  } else if (type_checker_module_has_kernel(checker) ||
             checker->device_module) {
    const char *why = NULL;
    if_stmt->condition_uniform =
        type_checker_expression_is_uniform(checker, if_stmt->condition, &why);
  }
  if (if_stmt->uniform_mode != 3 && type_checker_module_has_kernel(checker)) {
    if_stmt->warp_uniform = type_checker_expression_is_warp_uniform(
        checker, if_stmt->condition);
    for (size_t i = 0; if_stmt->warp_uniform && i < if_stmt->else_if_count;
         i++) {
      if_stmt->warp_uniform = type_checker_expression_is_warp_uniform(
          checker, if_stmt->else_ifs[i].condition);
    }
  }
  return 1;
}

int type_checker_check_if_statement(TypeChecker *checker,
                                           ASTNode *statement) {
  IfStatement *if_stmt = (IfStatement *)statement->data;
  if (!if_stmt || !if_stmt->condition) {
    type_checker_set_error_at_location(checker, statement->location,
                                       "Invalid if statement");
    return 0;
  }

  Type *condition_type = type_checker_infer_type(checker, if_stmt->condition);
  if (!condition_type) {
    return 0;
  }
  if (type_checker_reject_comptime_escape(checker, if_stmt->condition->location,
                                          condition_type)) {
    return 0;
  }

  if (!type_checker_is_numeric_type(condition_type)) {
    type_checker_report_type_mismatch(checker, if_stmt->condition->location,
                                      "numeric type", condition_type->name);
    return 0;
  }

  if (!type_checker_if_check_uniform_condition(checker, if_stmt,
                                               condition_type)) {
    return 0;
  }

  size_t init_snapshot_count = 0;
  unsigned char *init_snapshot =
      type_checker_init_tracker_capture(checker, &init_snapshot_count);
  unsigned char *joined = NULL;
  if (checker->tracked_var_count > 0 && !init_snapshot) {
    type_checker_set_error_at_location(
        checker, statement->location,
        "Out of memory while analyzing variable initialization flow");
    return 0;
  }
  if (init_snapshot_count > 0) {
    joined = malloc(init_snapshot_count * sizeof(unsigned char));
    if (!joined) {
      free(init_snapshot);
      type_checker_set_error_at_location(
          checker, statement->location,
          "Out of memory while analyzing variable initialization flow");
      return 0;
    }
    if (if_stmt->else_branch) {
      memset(joined, 1, init_snapshot_count * sizeof(unsigned char));
    } else {
      memcpy(joined, init_snapshot,
             init_snapshot_count * sizeof(unsigned char));
    }
  }

  {
    size_t guard_depth = type_checker_guard_depth(checker);
    int then_ok = 1;
    type_checker_push_guard(checker, if_stmt->condition, 0);
    if (if_stmt->then_branch &&
        !type_checker_check_statement(checker, if_stmt->then_branch)) {
      then_ok = 0;
    }
    type_checker_pop_guards(checker, guard_depth);
    if (!then_ok) {
      free(joined);
      free(init_snapshot);
      return 0;
    }
  }
  type_checker_if_join_arm(checker, joined, init_snapshot_count,
                           if_stmt->then_branch);
  type_checker_init_tracker_restore(checker, init_snapshot,
                                    init_snapshot_count);

  size_t prefix_base = 0;
  size_t prefix_end = 0;
  size_t prefix_arms = 0;
  size_t prefix_pushes = 0;
  int prefix_valid = 0;
  for (size_t i = 0; i < if_stmt->else_if_count; i++) {
    Type *elif_cond_type =
        type_checker_infer_type(checker, if_stmt->else_ifs[i].condition);
    int elif_ok = elif_cond_type != NULL;
    if (elif_ok && type_checker_reject_comptime_escape(
                       checker, if_stmt->else_ifs[i].condition->location,
                       elif_cond_type)) {
      elif_ok = 0;
    }
    if (elif_ok && !type_checker_is_numeric_type(elif_cond_type)) {
      type_checker_report_type_mismatch(
          checker, if_stmt->else_ifs[i].condition->location, "numeric type",
          elif_cond_type->name);
      elif_ok = 0;
    }
    if (elif_ok && if_stmt->else_ifs[i].body) {
      size_t guard_depth = type_checker_guard_depth(checker);
      if (prefix_valid && prefix_arms == i &&
          checker->guard_pushes == prefix_pushes &&
          prefix_base == guard_depth) {
        checker->guard_count = prefix_end;
      } else {
        type_checker_push_guard(checker, if_stmt->condition, 1);
        for (size_t earlier = 0; earlier < i; earlier++) {
          type_checker_push_guard(checker,
                                  if_stmt->else_ifs[earlier].condition, 1);
        }
      }
      size_t arm_depth = type_checker_guard_depth(checker);
      type_checker_push_guard(checker, if_stmt->else_ifs[i].condition, 0);
      if (!type_checker_check_statement(checker, if_stmt->else_ifs[i].body)) {
        elif_ok = 0;
      }
      type_checker_pop_guards(checker, arm_depth);
      type_checker_push_guard(checker, if_stmt->else_ifs[i].condition, 1);
      prefix_base = guard_depth;
      prefix_end = type_checker_guard_depth(checker);
      prefix_arms = i + 1;
      prefix_pushes = checker->guard_pushes;
      prefix_valid = prefix_end == arm_depth + 1;
      type_checker_pop_guards(checker, guard_depth);
    }
    if (!elif_ok) {
      free(joined);
      free(init_snapshot);
      return 0;
    }
    type_checker_if_join_arm(checker, joined, init_snapshot_count,
                             if_stmt->else_ifs[i].body);
    type_checker_init_tracker_restore(checker, init_snapshot,
                                      init_snapshot_count);
  }

  if (if_stmt->else_branch) {
    size_t guard_depth = type_checker_guard_depth(checker);
    int else_ok;
    type_checker_push_guard(checker, if_stmt->condition, 1);
    for (size_t earlier = 0; earlier < if_stmt->else_if_count; earlier++) {
      type_checker_push_guard(checker, if_stmt->else_ifs[earlier].condition,
                              1);
    }
    else_ok = type_checker_check_statement(checker, if_stmt->else_branch);
    type_checker_pop_guards(checker, guard_depth);
    if (!else_ok) {
      free(joined);
      free(init_snapshot);
      return 0;
    }
  }
  type_checker_if_join_arm(checker, joined, init_snapshot_count,
                           if_stmt->else_branch);
  type_checker_init_tracker_restore(checker, init_snapshot,
                                    init_snapshot_count);
  if (joined) {
    type_checker_init_tracker_restore(checker, joined, init_snapshot_count);
  }

  free(joined);
  free(init_snapshot);

  return 1;
}

int type_checker_body_assigns(const ASTNode *node, const char *name) {
  if (!node || !name) {
    return 0;
  }
  if (node->type == AST_ASSIGNMENT && node->data) {
    const Assignment *assignment = (const Assignment *)node->data;
    const ASTNode *target = assignment->target;
    if (assignment->variable_name &&
        strcmp(assignment->variable_name, name) == 0) {
      return 1;
    }
    if (target && target->type == AST_IDENTIFIER && target->data) {
      const Identifier *identifier = (const Identifier *)target->data;
      if (identifier->name && strcmp(identifier->name, name) == 0) {
        return 1;
      }
    }
  }
  if (node->type == AST_UNARY_EXPRESSION && node->data) {
    const UnaryExpression *unary = (const UnaryExpression *)node->data;
    if (unary->operator && strcmp(unary->operator, "&") == 0 &&
        unary->operand && unary->operand->type == AST_IDENTIFIER &&
        unary->operand->data) {
      const Identifier *identifier = (const Identifier *)unary->operand->data;
      if (identifier->name && strcmp(identifier->name, name) == 0) {
        return 1;
      }
    }
  }
  for (size_t i = 0; i < node->child_count; i++) {
    if (type_checker_body_assigns(node->children[i], name)) {
      return 1;
    }
  }
  return 0;
}

static int type_checker_check_loop_uniformity(TypeChecker *checker,
                                              ASTNode *condition,
                                              int *uniform_mode,
                                              int *warp_uniform) {
  const char *why = NULL;
  Type *condition_type = condition ? condition->resolved_type : NULL;
  if (!checker || !condition || !uniform_mode) {
    return 1;
  }
  if ((condition_type && condition_type->refine_uniform) ||
      ((*uniform_mode == 2 ||
        !type_checker_expression_surely_varies(checker, condition, 0)) &&
       type_checker_expression_is_uniform(checker, condition, &why))) {
    *uniform_mode = 3;
    return 1;
  }
  if (*uniform_mode == 2) {
    type_checker_set_error_at_location(
        checker, condition->location,
        "'@uniform!' says every work item of the group goes round the same "
        "number of times, and %s varies by work item",
        why ? why : "this condition");
    return 0;
  }
  if (type_checker_module_has_kernel(checker)) {
    *warp_uniform = type_checker_expression_is_warp_uniform(checker, condition);
  }
  return 1;
}

static int type_checker_range_end_fits(TypeChecker *checker,
                                       ForStatement *for_stmt) {
  ASTNode *condition = for_stmt->condition;
  BinaryExpression *range = condition &&
                                    condition->type == AST_BINARY_EXPRESSION
                                ? (BinaryExpression *)condition->data
                                : NULL;
  long long value = 0;
  long long low = 0;
  unsigned long long high = 0;
  if (!range || !range->operator || !range->left || !range->right ||
      (strcmp(range->operator, "<") != 0 &&
       strcmp(range->operator, "<=") != 0) ||
      range->left->type != AST_IDENTIFIER || !for_stmt->initializer ||
      for_stmt->initializer->type != AST_VAR_DECLARATION ||
      strcmp(((Identifier *)range->left->data)->name,
             ((VarDeclaration *)for_stmt->initializer->data)->name) != 0) {
    return 1;
  }
  Type *counter = type_checker_infer_type(checker, range->left);
  if (!counter || !type_checker_is_integer_type(counter) ||
      counter->kind == TYPE_ENUM ||
      !type_checker_integer_bounds(counter, &low, &high) ||
      !type_checker_eval_integer_constant_with_checker(checker, range->right,
                                                       &value) ||
      value < 0 || (unsigned long long)value <= high) {
    return 1;
  }
  type_checker_report_assign_mismatch(checker, range->right,
                                      range->right->location, counter,
                                      type_checker_infer_type(checker,
                                                              range->right));
  return 0;
}

static int type_checker_narrow_loop_step(TypeChecker *checker,
                                         ForStatement *for_stmt) {
  VarDeclaration *counter_decl;
  Assignment *step;
  Symbol *counter;
  Type *step_type;
  ASTNode *narrowed;

  if (!for_stmt->increment || for_stmt->increment->type != AST_ASSIGNMENT ||
      !for_stmt->initializer ||
      for_stmt->initializer->type != AST_VAR_DECLARATION ||
      !((VarDeclaration *)for_stmt->initializer->data)->structural_type) {
    return 1;
  }
  counter_decl = (VarDeclaration *)for_stmt->initializer->data;
  step = (Assignment *)for_stmt->increment->data;
  counter = symbol_table_lookup(checker->symbol_table, counter_decl->name);
  if (!counter || !counter->type || !step->value ||
      !type_checker_is_integer_type(counter->type)) {
    return 1;
  }
  step_type = type_checker_infer_type(checker, step->value);
  if (!step_type || !type_checker_is_integer_type(step_type) ||
      step_type->size <= counter->type->size) {
    return 1;
  }
  narrowed = ast_create_cast_expression(counter->type->name, step->value,
                                        for_stmt->increment->location);
  if (!narrowed) {
    return 0;
  }
  step->value = narrowed;
  return 1;
}

int type_checker_check_for_statement(TypeChecker *checker,
                                            ASTNode *statement) {
  ForStatement *for_stmt = (ForStatement *)statement->data;
  if (!for_stmt) {
    type_checker_set_error_at_location(checker, statement->location,
                                       "Invalid for statement");
    return 0;
  }

  if (!symbol_table_enter_scope(checker->symbol_table, SCOPE_BLOCK)) {
    type_checker_set_error_at_location(
        checker, statement->location,
        "Out of memory while entering for-loop scope");
    return 0;
  }
  if (!type_checker_init_tracker_enter_scope(checker)) {
    type_checker_set_error_at_location(
        checker, statement->location,
        "Out of memory while entering initialization analysis scope");
    symbol_table_exit_scope(checker->symbol_table);
    return 0;
  }

  if (for_stmt->initializer) {
    int init_ok = 0;
    if (for_stmt->initializer->type == AST_VAR_DECLARATION ||
        for_stmt->initializer->type == AST_ASSIGNMENT ||
        for_stmt->initializer->type == AST_FUNCTION_CALL) {
      init_ok = type_checker_check_statement(checker, for_stmt->initializer);
    } else {
      init_ok = type_checker_check_expression(checker, for_stmt->initializer);
    }
    if (!init_ok) {
      type_checker_init_tracker_exit_scope(checker);
      symbol_table_exit_scope(checker->symbol_table);
      return 0;
    }
  }

  size_t post_init_snapshot_count = 0;
  unsigned char *post_init_snapshot =
      type_checker_init_tracker_capture(checker, &post_init_snapshot_count);
  if (checker->tracked_var_count > 0 && !post_init_snapshot) {
    type_checker_set_error_at_location(
        checker, statement->location,
        "Out of memory while analyzing variable initialization flow");
    type_checker_init_tracker_exit_scope(checker);
    symbol_table_exit_scope(checker->symbol_table);
    return 0;
  }

  if (for_stmt->condition) {
    int range_header = for_stmt->initializer &&
                       for_stmt->initializer->type == AST_VAR_DECLARATION &&
                       for_stmt->initializer->data &&
                       ((VarDeclaration *)for_stmt->initializer->data)
                           ->structural_type;
    type_checker_sign_compare_quiet(range_header ? 1 : 0);
    Type *cond_type = type_checker_infer_type(checker, for_stmt->condition);
    type_checker_sign_compare_quiet(range_header ? -1 : 0);
    if (!cond_type) {
      free(post_init_snapshot);
      type_checker_init_tracker_exit_scope(checker);
      symbol_table_exit_scope(checker->symbol_table);
      return 0;
    }
    if (type_checker_reject_comptime_escape(checker, for_stmt->condition->location,
                                            cond_type)) {
      free(post_init_snapshot);
      type_checker_init_tracker_exit_scope(checker);
      symbol_table_exit_scope(checker->symbol_table);
      return 0;
    }
    if (!type_checker_is_numeric_type(cond_type)) {
      type_checker_report_type_mismatch(checker,
                                        for_stmt->condition->location,
                                        "numeric type", cond_type->name);
      free(post_init_snapshot);
      type_checker_init_tracker_exit_scope(checker);
      symbol_table_exit_scope(checker->symbol_table);
      return 0;
    }
    if (!type_checker_check_loop_uniformity(checker, for_stmt->condition,
                                            &for_stmt->uniform_mode,
                                            &for_stmt->warp_uniform)) {
      free(post_init_snapshot);
      type_checker_init_tracker_exit_scope(checker);
      symbol_table_exit_scope(checker->symbol_table);
      return 0;
    }
  }

  if (!type_checker_narrow_loop_step(checker, for_stmt) ||
      !type_checker_range_end_fits(checker, for_stmt)) {
    free(post_init_snapshot);
    type_checker_init_tracker_exit_scope(checker);
    symbol_table_exit_scope(checker->symbol_table);
    return 0;
  }

  if (for_stmt->increment) {
    int step_ok = (for_stmt->increment->type == AST_ASSIGNMENT ||
                   for_stmt->increment->type == AST_FUNCTION_CALL)
                      ? type_checker_check_statement(checker,
                                                     for_stmt->increment)
                      : type_checker_check_expression(checker,
                                                      for_stmt->increment);
    if (!step_ok) {
      free(post_init_snapshot);
      type_checker_init_tracker_exit_scope(checker);
      symbol_table_exit_scope(checker->symbol_table);
      return 0;
    }
  }

  checker->loop_depth++;
  type_checker_push_loop_label(checker, for_stmt->label);
  {
    size_t guard_depth = type_checker_guard_depth(checker);
    int body_ok = 1;
    if (for_stmt->condition) {
      type_checker_push_guard(checker, for_stmt->condition, 0);
    }
    if (for_stmt->initializer &&
        for_stmt->initializer->type == AST_VAR_DECLARATION &&
        for_stmt->initializer->data &&
        ((VarDeclaration *)for_stmt->initializer->data)->structural_type) {
      VarDeclaration *counter_decl =
          (VarDeclaration *)for_stmt->initializer->data;
      int has_min = 0;
      int has_max = 0;
      long long min = 0;
      long long max = 0;
      if (counter_decl->name && counter_decl->initializer &&
          !type_checker_body_assigns(for_stmt->body, counter_decl->name) &&
          type_checker_expression_range(checker, counter_decl->initializer,
                                        &has_min, &min, &has_max, &max) &&
          has_min) {
        type_checker_push_range_guard(checker, counter_decl->name, 1, min, 0,
                                      0);
      }
    }
    if (for_stmt->body &&
        !type_checker_check_statement(checker, for_stmt->body)) {
      body_ok = 0;
    }
    type_checker_pop_guards(checker, guard_depth);
    if (!body_ok) {
      type_checker_pop_loop_label(checker);
      checker->loop_depth--;
      free(post_init_snapshot);
      type_checker_init_tracker_exit_scope(checker);
      symbol_table_exit_scope(checker->symbol_table);
      return 0;
    }
  }
  type_checker_pop_loop_label(checker);
  checker->loop_depth--;

  type_checker_init_tracker_restore(checker, post_init_snapshot,
                                    post_init_snapshot_count);
  free(post_init_snapshot);
  type_checker_init_tracker_exit_scope(checker);
  symbol_table_exit_scope(checker->symbol_table);
  return 1;
}

static const ASTNode *type_checker_find_fallthrough(const ASTNode *node) {
  size_t i;
  if (!node || node->type == AST_SWITCH_STATEMENT) {
    return NULL;
  }
  if (node->type == AST_FALLTHROUGH_STATEMENT) {
    return node;
  }
  for (i = 0; i < node->child_count; i++) {
    const ASTNode *found = type_checker_find_fallthrough(node->children[i]);
    if (found) {
      return found;
    }
  }
  return NULL;
}

static int type_checker_check_switch_exhaustive(
    TypeChecker *checker, ASTNode *statement, Type *switch_type,
    int seen_default, const long long *case_values,
    size_t case_value_count) {
  if (switch_type->kind == TYPE_ENUM && !seen_default) {
    Scope *global = checker->symbol_table->global_scope;
    for (size_t i = 0; i < global->symbol_count; i++) {
      Symbol *sym = global->symbols[i];
      if (!sym || sym->kind != SYMBOL_CONSTANT || sym->type != switch_type) {
        continue;
      }
      int covered = 0;
      for (size_t j = 0; j < case_value_count; j++) {
        if (case_values[j] == sym->data.constant.value) {
          covered = 1;
          break;
        }
      }
      if (!covered) {
        type_checker_set_error_at_location(
            checker, statement->location,
            "Non-exhaustive switch on '%s': variant '%s' not covered; "
            "add a 'case %s:' arm or a 'default:' arm",
            switch_type->name, sym->name, sym->name);
        return 0;
      }
    }
  }

  if (switch_type->kind == TYPE_BOOL && !seen_default) {
    int has_true = 0, has_false = 0;
    for (size_t i = 0; i < case_value_count; i++) {
      if (case_values[i] == 1) has_true = 1;
      if (case_values[i] == 0) has_false = 1;
    }
    if (!has_true || !has_false) {
      type_checker_set_error_at_location(
          checker, statement->location,
          "Non-exhaustive switch over 'bool': must cover both 'true' and "
          "'false', or add a 'default:' arm");
      return 0;
    }
  }
  return 1;
}

int type_checker_check_switch_statement(TypeChecker *checker,
                                               ASTNode *statement) {
  SwitchStatement *switch_stmt = (SwitchStatement *)statement->data;
  if (!switch_stmt || !switch_stmt->expression) {
    type_checker_set_error_at_location(checker, statement->location,
                                       "Invalid switch statement");
    return 0;
  }

  Type *switch_type =
      type_checker_infer_type(checker, switch_stmt->expression);
  if (!switch_type) {
    return 0;
  }
  if (type_checker_reject_comptime_escape(
          checker, switch_stmt->expression->location, switch_type)) {
    return 0;
  }
  if (!type_checker_is_discrete_type(switch_type)) {
    type_checker_report_type_mismatch(checker,
                                      switch_stmt->expression->location,
                                      "integer type", switch_type->name);
    return 0;
  }

  size_t init_snapshot_count = 0;
  unsigned char *init_snapshot =
      type_checker_init_tracker_capture(checker, &init_snapshot_count);
  if (checker->tracked_var_count > 0 && !init_snapshot) {
    type_checker_set_error_at_location(
        checker, statement->location,
        "Out of memory while analyzing variable initialization flow");
    return 0;
  }

  long long *case_values = NULL;
  size_t case_value_count = 0;
  int seen_default = 0;

  if (switch_stmt->case_count > 0) {
    case_values = malloc(switch_stmt->case_count * sizeof(long long));
    if (!case_values) {
      type_checker_set_error_at_location(
          checker, statement->location,
          "Memory allocation failed in switch validation");
      return 0;
    }
  }

  checker->switch_depth++;
  for (size_t i = 0; i < switch_stmt->case_count; i++) {
    ASTNode *case_node = switch_stmt->cases ? switch_stmt->cases[i] : NULL;
    if (!case_node || case_node->type != AST_CASE_CLAUSE) {
      type_checker_set_error_at_location(checker, statement->location,
                                         "Invalid case clause in switch");
      checker->switch_depth--;
      free(init_snapshot);
      free(case_values);
      return 0;
    }

    CaseClause *case_clause = (CaseClause *)case_node->data;
    if (!case_clause) {
      type_checker_set_error_at_location(checker, case_node->location,
                                         "Invalid case clause");
      checker->switch_depth--;
      free(init_snapshot);
      free(case_values);
      return 0;
    }

    if (case_clause->is_default) {
      if (seen_default) {
        type_checker_set_error_at_location(
            checker, case_node->location,
            "Switch may only contain one default clause");
        checker->switch_depth--;
        free(init_snapshot);
        free(case_values);
        return 0;
      }
      seen_default = 1;
    } else {
      if (!case_clause->value) {
        type_checker_set_error_at_location(
            checker, case_node->location,
            "Case clause is missing a value expression");
        checker->switch_depth--;
        free(init_snapshot);
        free(case_values);
        return 0;
      }

      Type *case_type = type_checker_infer_type(checker, case_clause->value);
      if (!case_type) {
        checker->switch_depth--;
        free(init_snapshot);
        free(case_values);
        return 0;
      }
      if (!type_checker_is_discrete_type(case_type)) {
        type_checker_report_type_mismatch(checker,
                                          case_clause->value->location,
                                          "integer type", case_type->name);
        checker->switch_depth--;
        free(init_snapshot);
        free(case_values);
        return 0;
      }
      if (!type_checker_is_assignable_from(checker, switch_type, case_type,
                                           case_clause->value)) {
        type_checker_report_assign_mismatch(checker, case_clause->value,
                                            case_clause->value->location,
                                            switch_type, case_type);
        checker->switch_depth--;
        free(init_snapshot);
        free(case_values);
        return 0;
      }

      long long case_value = 0;
      int case_eval_ok = type_checker_eval_integer_constant_with_checker(
          checker, case_clause->value, &case_value);
      if (!case_eval_ok &&
          case_clause->value->type == AST_IDENTIFIER) {
        Identifier *cid = (Identifier *)case_clause->value->data;
        Symbol *csym = type_checker_resolve_identifier(checker, cid);
        if (csym && csym->kind == SYMBOL_CONSTANT) {
          case_value = csym->data.constant.value;
          case_eval_ok = 1;
        }
      }
      if (!case_eval_ok &&
          case_clause->value->type == AST_MEMBER_ACCESS) {
        MemberAccess *cma = (MemberAccess *)case_clause->value->data;
        if (cma && cma->object && cma->object->type == AST_IDENTIFIER &&
            cma->member) {
          Identifier *cma_obj = (Identifier *)cma->object->data;
          if (cma_obj && cma_obj->name) {
            Symbol *enum_sym =
                type_checker_resolve_identifier(checker, cma_obj);
            if (enum_sym && enum_sym->kind == SYMBOL_ENUM) {
              Symbol *vsym =
                  symbol_table_lookup(checker->symbol_table, cma->member);
              if (vsym && vsym->kind == SYMBOL_CONSTANT) {
                case_value = vsym->data.constant.value;
                case_eval_ok = 1;
              }
            }
          }
        }
      }
      if (!case_eval_ok) {
        type_checker_set_error_at_location(
            checker, case_clause->value->location,
            "Case value must be a compile-time integer constant expression");
        checker->switch_depth--;
        free(init_snapshot);
        free(case_values);
        return 0;
      }

      if (case_clause->value_high) {
        Type *high_type =
            type_checker_infer_type(checker, case_clause->value_high);
        if (!high_type) {
          checker->switch_depth--;
          free(init_snapshot);
          free(case_values);
          return 0;
        }
        if (!type_checker_is_discrete_type(high_type) ||
            !type_checker_is_assignable_from(checker, switch_type, high_type,
                                             case_clause->value_high)) {
          type_checker_report_assign_mismatch(checker, case_clause->value_high,
                                              case_clause->value_high->location,
                                              switch_type, high_type);
          checker->switch_depth--;
          free(init_snapshot);
          free(case_values);
          return 0;
        }

        long long case_high_value = 0;
        int high_eval_ok = type_checker_eval_integer_constant_with_checker(
            checker, case_clause->value_high, &case_high_value);
        if (!high_eval_ok &&
            case_clause->value_high->type == AST_IDENTIFIER) {
          Identifier *hid = (Identifier *)case_clause->value_high->data;
          Symbol *hsym = type_checker_resolve_identifier(checker, hid);
          if (hsym && hsym->kind == SYMBOL_CONSTANT) {
            case_high_value = hsym->data.constant.value;
            high_eval_ok = 1;
          }
        }
        if (!high_eval_ok) {
          type_checker_set_error_at_location(
              checker, case_clause->value_high->location,
              "Range upper bound must be a compile-time integer constant "
              "expression");
          checker->switch_depth--;
          free(init_snapshot);
          free(case_values);
          return 0;
        }
        if (case_value > case_high_value) {
          type_checker_set_error_at_location(
              checker, case_clause->value->location,
              "Range lower bound '%lld' exceeds upper bound '%lld'",
              case_value, case_high_value);
          checker->switch_depth--;
          free(init_snapshot);
          free(case_values);
          return 0;
        }
      } else {
        for (size_t j = 0; j < case_value_count; j++) {
          if (case_values[j] == case_value) {
            type_checker_set_error_at_location(
                checker, case_clause->value->location,
                "Duplicate case value '%lld' in switch", case_value);
            checker->switch_depth--;
            free(init_snapshot);
            free(case_values);
            return 0;
          }
        }
        case_values[case_value_count++] = case_value;
      }
    }

    if (!case_clause->body) {
      type_checker_set_error_at_location(checker, case_node->location,
                                         "Case clause must have a body");
      checker->switch_depth--;
      free(init_snapshot);
      free(case_values);
      return 0;
    }

    if (!type_checker_check_statement(checker, case_clause->body)) {
      checker->switch_depth--;
      free(init_snapshot);
      free(case_values);
      return 0;
    }
    if (i + 1 == switch_stmt->case_count) {
      const ASTNode *stray = type_checker_find_fallthrough(case_clause->body);
      if (stray) {
        type_checker_set_error_at_location(
            checker, stray->location,
            "'fallthrough' in the last case has no case to fall into");
        checker->switch_depth--;
        free(init_snapshot);
        free(case_values);
        return 0;
      }
    }
    type_checker_init_tracker_restore(checker, init_snapshot,
                                      init_snapshot_count);
  }
  checker->switch_depth--;
  type_checker_init_tracker_restore(checker, init_snapshot,
                                    init_snapshot_count);
  free(init_snapshot);

  if (!type_checker_check_switch_exhaustive(checker, statement, switch_type,
                                            seen_default, case_values,
                                            case_value_count)) {
    free(case_values);
    return 0;
  }

  free(case_values);
  return 1;
}

static int type_checker_check_defer_statement(TypeChecker *checker,
                                              ASTNode *statement);

static int type_checker_check_statement_body(TypeChecker *checker,
                                             ASTNode *statement);

static int type_checker_never_falls_through(TypeChecker *checker,
                                            ASTNode *statement) {
  if (!statement) {
    return 0;
  }
  if (type_checker_statement_guarantees_termination(statement) ||
      type_checker_call_never_returns(checker, statement)) {
    return 1;
  }
  if (statement->type == AST_PROGRAM) {
    for (size_t i = 0; i < statement->child_count; i++) {
      if (type_checker_never_falls_through(checker, statement->children[i])) {
        return 1;
      }
    }
    return 0;
  }
  if (statement->type == AST_IF_STATEMENT && statement->data) {
    IfStatement *branch = (IfStatement *)statement->data;
    if (!branch->else_branch ||
        !type_checker_never_falls_through(checker, branch->then_branch)) {
      return 0;
    }
    for (size_t i = 0; i < branch->else_if_count; i++) {
      if (!type_checker_never_falls_through(checker,
                                            branch->else_ifs[i].body)) {
        return 0;
      }
    }
    return type_checker_never_falls_through(checker, branch->else_branch);
  }
  return 0;
}

static int type_checker_statement_repeats(const ASTNode *statement) {
  return statement->type == AST_WHILE_STATEMENT ||
         statement->type == AST_FOR_STATEMENT ||
         statement->type == AST_SWITCH_STATEMENT ||
         statement->type == AST_MATCH_STATEMENT;
}

static int type_checker_statement_holds_statements(const ASTNode *statement) {
  return type_checker_statement_repeats(statement) ||
         statement->type == AST_IF_STATEMENT ||
         statement->type == AST_PROGRAM ||
         statement->type == AST_CASE_CLAUSE ||
         statement->type == AST_COMPTIME_FOR ||
         statement->type == AST_VALUE_SCOPE ||
         statement->type == AST_FUNCTION_DECLARATION;
}

int type_checker_check_statement(TypeChecker *checker, ASTNode *statement) {
  if (!checker || !statement)
    return 0;
  if (type_checker_statement_repeats(statement)) {
    type_checker_note_effects(checker, statement);
  }
  if (!type_checker_check_statement_body(checker, statement)) {
    return 0;
  }
  if (statement->type == AST_RETURN_STATEMENT && statement->data) {
    const ReturnStatement *ret = (const ReturnStatement *)statement->data;
    type_checker_note_return_facts(checker,
                                   ret->value_count > 1 ? NULL : ret->value);
  }
  if (!type_checker_statement_holds_statements(statement)) {
    type_checker_note_effects(checker, statement);
  }
  return type_checker_check_conflict_free(checker, statement);
}

static int type_checker_check_asm_bindings(TypeChecker *checker,
                                           ASTNode *statement) {
  const InlineAsm *inline_asm = (const InlineAsm *)statement->data;
  const char *at;

  if (!inline_asm || !inline_asm->assembly_code ||
      type_checker_in_kernel_body(checker)) {
    return 1;
  }
  for (at = strchr(inline_asm->assembly_code, '{'); at;
       at = strchr(at + 1, '{')) {
    const char *start = at + 1;
    const char *end;
    char name[128];
    size_t length;
    while (*start == ' ' || *start == '\t') {
      start++;
    }
    if (*start == '{') {
      continue;
    }
    end = start;
    while (*end && *end != '}' && *end != '{' && *end != ' ' && *end != '\t') {
      end++;
    }
    length = (size_t)(end - start);
    while (*end == ' ' || *end == '\t') {
      end++;
    }
    if (*end != '}') {
      continue;
    }
    if (length == 0 || length >= sizeof(name)) {
      type_checker_set_error_at_location(
          checker, statement->location,
          "'{}' in an asm block names a variable, and this one names nothing");
      return 0;
    }
    memcpy(name, start, length);
    name[length] = '\0';
    if (!symbol_table_lookup(checker->symbol_table, name)) {
      type_checker_set_error_at_location(
          checker, statement->location,
          "asm names '{%s}', and no variable '%s' is in scope", name, name);
      return 0;
    }
  }
  return 1;
}

static int type_checker_check_block(TypeChecker *checker, ASTNode *statement) {
    Program *block = (Program *)statement->data;
    if (block) {
      SourceSpan expansion_origin;
      const char *expansion_note =
          type_checker_expansion_note(checker, statement, &expansion_origin);
      int note_frame_pushed =
          expansion_note && checker->error_reporter &&
          error_reporter_push_note_frame(checker->error_reporter,
                                         expansion_origin, expansion_note);

      if (!symbol_table_enter_scope(checker->symbol_table, SCOPE_BLOCK)) {
        type_checker_set_error_at_location(
            checker, statement->location,
            "Out of memory while entering block scope");
        if (note_frame_pushed)
          error_reporter_pop_note_frame(checker->error_reporter);
        return 0;
      }
      if (!type_checker_init_tracker_enter_scope(checker)) {
        type_checker_set_error_at_location(
            checker, statement->location,
            "Out of memory while entering initialization analysis scope");
        symbol_table_exit_scope(checker->symbol_table);
        if (note_frame_pushed)
          error_reporter_pop_note_frame(checker->error_reporter);
        return 0;
      }

      int block_ok =
          type_checker_declare_expansion_binding(checker, statement);
      int reached_terminator = 0;
      int ends_here = 0;
      size_t block_guard_depth = type_checker_guard_depth(checker);
      for (size_t i = 0; i < statement->child_count; i++) {
        ASTNode *child = statement->children[i];
        while (child && child->type == AST_COMPTIME_FOR) {
          if (!type_checker_expand_next_directive(checker, statement)) {
            block_ok = 0;
            break;
          }
          child = i < statement->child_count ? statement->children[i] : NULL;
        }
        if (!child || child->type == AST_COMPTIME_FOR) {
          continue;
        }
        if (reached_terminator && checker->error_reporter && child) {
          error_reporter_add_warning(
              checker->error_reporter, ERROR_SEMANTIC, child->location,
              "Unreachable code: statement will never execute");
        }
        if (!type_checker_check_statement(checker, statement->children[i])) {
          block_ok = 0;
        }
        if (type_checker_statement_guarantees_termination(child)) {
          reached_terminator = 1;
        }
        if (type_checker_never_falls_through(checker, child)) {
          ends_here = 1;
        }
        if (child && child->type == AST_IF_STATEMENT && child->data) {
          IfStatement *early = (IfStatement *)child->data;
          if (early->condition && !early->else_branch &&
              early->else_if_count == 0 && early->then_branch &&
              type_checker_never_falls_through(checker, early->then_branch)) {
            type_checker_push_guard(checker, early->condition, 1);
          }
        }
        type_checker_push_call_facts(checker, child);
      }
      if (!ends_here && checker->current_function_decl &&
          checker->current_function_decl->type == AST_FUNCTION_DECLARATION &&
          checker->current_function_decl->data &&
          ((const FunctionDeclaration *)checker->current_function_decl->data)
                  ->body == statement) {
        type_checker_note_return_facts(checker, NULL);
      }
      type_checker_pop_guards(checker, block_guard_depth);

      if (block_ok)
        type_checker_warn_unused_locals(checker);
      type_checker_init_tracker_exit_scope(checker);
      symbol_table_exit_scope(checker->symbol_table);
      if (note_frame_pushed)
        error_reporter_pop_note_frame(checker->error_reporter);
      if (!block_ok)
        return 0;
    }
    return 1;
}

static int type_checker_check_while(TypeChecker *checker, ASTNode *statement) {
    WhileStatement *while_stmt = (WhileStatement *)statement->data;
    if (!while_stmt || !while_stmt->condition) {
      type_checker_set_error_at_location(checker, statement->location,
                                         "Invalid while statement");
      return 0;
    }

    Type *condition_type =
        type_checker_infer_type(checker, while_stmt->condition);
    if (!condition_type) {
      return 0;
    }
    if (type_checker_reject_comptime_escape(
            checker, while_stmt->condition->location, condition_type)) {
      return 0;
    }

    if (!type_checker_is_numeric_type(condition_type)) {
      type_checker_report_type_mismatch(checker,
                                        while_stmt->condition->location,
                                        "numeric type", condition_type->name);
      return 0;
    }
    if (!type_checker_check_loop_uniformity(checker, while_stmt->condition,
                                            &while_stmt->uniform_mode,
                                            &while_stmt->warp_uniform)) {
      return 0;
    }

    size_t init_snapshot_count = 0;
    unsigned char *init_snapshot =
        type_checker_init_tracker_capture(checker, &init_snapshot_count);
    if (checker->tracked_var_count > 0 && !init_snapshot) {
      type_checker_set_error_at_location(
          checker, statement->location,
          "Out of memory while analyzing variable initialization flow");
      return 0;
    }

    checker->loop_depth++;
    type_checker_push_loop_label(checker, while_stmt->label);
    {
      size_t guard_depth = type_checker_guard_depth(checker);
      size_t trip_depth = type_checker_loop_trip_depth(checker);
      int body_ok = 1;
      type_checker_push_guard(checker, while_stmt->condition, 0);
      type_checker_push_loop_trip(checker, while_stmt->condition,
                                  while_stmt->body);
      if (while_stmt->body &&
          !type_checker_check_statement(checker, while_stmt->body)) {
        body_ok = 0;
      }
      type_checker_pop_loop_trip(checker, trip_depth);
      type_checker_pop_guards(checker, guard_depth);
      if (!body_ok) {
        type_checker_pop_loop_label(checker);
        checker->loop_depth--;
        free(init_snapshot);
        return 0;
      }
    }
    type_checker_pop_loop_label(checker);
    checker->loop_depth--;
    type_checker_init_tracker_restore(checker, init_snapshot,
                                      init_snapshot_count);
    free(init_snapshot);

    return 1;
}

static int type_checker_check_implicit_call(TypeChecker *checker,
                                            ASTNode *statement,
                                            ReturnStatement *ret,
                                            Type *return_type);

static int type_checker_check_return(TypeChecker *checker, ASTNode *statement) {
    ReturnStatement *ret_stmt = (ReturnStatement *)statement->data;
    size_t return_count = ret_stmt
                              ? (ret_stmt->value_count
                                     ? ret_stmt->value_count
                                     : (ret_stmt->value ? 1 : 0))
                              : 0;
    if (ret_stmt && return_count > 0) {
      Type *func_return_type = checker->current_function
                                   ? checker->current_function->data.function
                                         .return_type
                                   : NULL;
      if (ret_stmt->implicit_tail == RETURN_TAIL_CALL ||
          ret_stmt->implicit_tail == RETURN_TAIL_MAIN_CALL) {
        return type_checker_check_implicit_call(checker, statement, ret_stmt,
                                                func_return_type);
      }
      FunctionDeclaration *function_decl =
          checker->current_function_decl &&
                  checker->current_function_decl->type == AST_FUNCTION_DECLARATION
              ? (FunctionDeclaration *)checker->current_function_decl->data
              : NULL;

      if (function_decl && function_decl->return_type_count > 0) {
        if (return_count != function_decl->return_type_count) {
          type_checker_set_error_at_location(
              checker, statement->location,
              "Function '%s' returns %zu values but this return has %zu",
              function_decl->name, function_decl->return_type_count,
              return_count);
          return 0;
        }
        for (size_t i = 0; i < return_count; i++) {
          ASTNode *value = ret_stmt->values ? ret_stmt->values[i]
                                            : (i == 0 ? ret_stmt->value : NULL);
          Type *value_type = type_checker_infer_type(checker, value);
          Type *expected_type = func_return_type->field_types[i];
          if (!value_type) {
            return 0;
          }
          if (type_checker_reject_comptime_escape(checker, value->location,
                                                  value_type)) {
            return 0;
          }
          if (!type_checker_is_assignable_from(checker, expected_type,
                                               value_type, value)) {
            type_checker_report_assign_mismatch(checker, value, value->location,
                                                expected_type, value_type);
            return 0;
          }
        }
        return 1;
      }

      if (return_count > 1) {
        type_checker_set_error_at_location(
            checker, statement->location,
            "This function has one return value, but this return has %zu",
            return_count);
        return 0;
      }

      ASTNode *value = ret_stmt->values ? ret_stmt->values[0] : ret_stmt->value;
      checker->aggregate_target_type = func_return_type;
      Type *value_type = type_checker_infer_type(checker, value);
      checker->aggregate_target_type = NULL;
      if (!value_type) {
        if (!checker->has_error) {
          type_checker_set_error_at_location(
              checker, ret_stmt->value->location,
              "Cannot infer type of return value");
        }
        return 0;
      }
      if (type_checker_reject_comptime_escape(checker, value->location,
                                              value_type)) {
        return 0;
      }

      type_checker_note_return_range(checker, value);
      if (checker->current_function) {
        if (!type_checker_takes_null(func_return_type, value) &&
            !type_checker_is_assignable_from(checker, func_return_type,
                                             value_type, value)) {
          type_checker_report_assign_mismatch(checker, value, value->location,
                                              func_return_type, value_type);
          return 0;
        }

        if (checker->current_function_decl &&
            checker->errdefer_scan_decl != checker->current_function_decl) {
          checker->errdefer_scan_decl = checker->current_function_decl;
          checker->errdefer_scan_found = type_checker_ast_contains_node_type(
              checker->current_function_decl, AST_ERRDEFER_STATEMENT);
        }
        if (checker->current_function_decl && checker->errdefer_scan_found) {
          long long constant_value = 0;
          if (type_checker_eval_integer_constant(value,
                                                 &constant_value) &&
              constant_value != 0) {
            error_reporter_add_warning(
                checker->error_reporter, ERROR_SEMANTIC,
                value->location,
                "Non-zero constant return in function with errdefer will "
                "trigger errdefer by convention");
          }
        }
      } else {
        type_checker_set_error_at_location(
            checker, statement->location,
            "Return statement outside of a function");
        return 0;
      }
    }
    if (checker->current_function_decl &&
        checker->current_function_decl->type == AST_FUNCTION_DECLARATION) {
      FunctionDeclaration *function_decl =
          (FunctionDeclaration *)checker->current_function_decl->data;
      if (function_decl && function_decl->return_type_count > 0 &&
          return_count == 0) {
        type_checker_set_error_at_location(
            checker, statement->location,
            "Function '%s' must return %zu values", function_decl->name,
            function_decl->return_type_count);
        return 0;
      }
    }
    return 1;
}

static int type_checker_check_barrier(TypeChecker *checker, ASTNode *statement) {
    BarrierStatement *barrier = (BarrierStatement *)statement->data;
    FunctionDeclaration *owner =
        checker->current_function_decl &&
                checker->current_function_decl->type == AST_FUNCTION_DECLARATION
            ? (FunctionDeclaration *)checker->current_function_decl->data
            : NULL;
    const unsigned supported = AST_MEMORY_REGION_WORKGROUP |
                               AST_MEMORY_REGION_GLOBAL;
    if (!owner || !owner->is_kernel) {
      type_checker_set_error_at_location(
          checker, statement->location,
          "Barrier statements are only legal inside a GPU kernel");
      return 0;
    }
    if (!barrier || barrier->memory_regions == 0 ||
        (barrier->memory_regions & ~supported) != 0 ||
        barrier->memory_order < AST_MEMORY_ORDER_ACQUIRE ||
        barrier->memory_order > AST_MEMORY_ORDER_SEQ_CST) {
      type_checker_set_error_at_location(checker, statement->location,
                                         "Invalid barrier memory contract");
      return 0;
    }
    return 1;
}

static int type_checker_check_errdefer(TypeChecker *checker, ASTNode *statement) {
    if (!checker->current_function) {
      type_checker_set_error_at_location(
          checker, statement->location,
          "Errdefer statement outside of a function");
      return 0;
    }

    DeferStatement *defer_stmt = (DeferStatement *)statement->data;
    if (!defer_stmt || !defer_stmt->statement) {
      type_checker_set_error_at_location(checker, statement->location,
                                         "Invalid errdefer statement");
      return 0;
    }

    switch (defer_stmt->statement->type) {
    case AST_FUNCTION_CALL:
    case AST_ASSIGNMENT:
    case AST_PROGRAM:
      break;
    default:
      type_checker_set_error_at_location(checker,
                                         defer_stmt->statement->location,
                                         "Errdeferred statement must be a "
                                         "function call, assignment, or block");
      return 0;
    }

    return type_checker_check_statement(checker, defer_stmt->statement);
}

static int type_checker_check_break(TypeChecker *checker, ASTNode *statement) {
    LoopControlStatement *brk = (LoopControlStatement *)statement->data;
    if (checker->loop_depth <= 0 && checker->switch_depth <= 0) {
      type_checker_set_error_at_location(
          checker, statement->location,
          "'break' can only be used inside a loop or switch");
      return 0;
    }
    if (brk && brk->target_label &&
        !type_checker_loop_label_in_scope(checker, brk->target_label)) {
      type_checker_set_error_at_location(
          checker, statement->location,
          "'break %s' has no matching labeled loop", brk->target_label);
      return 0;
    }
    if (brk && brk->value) {
      if (brk->value->type == AST_IDENTIFIER && brk->value->data &&
          !brk->target_label) {
        const Identifier *id = (const Identifier *)brk->value->data;
        if (!symbol_table_lookup(checker->symbol_table, id->name)) {
          type_checker_set_error_at_location(
              checker, statement->location,
              "'break %s' has no matching labeled loop", id->name);
          return 0;
        }
      }
      type_checker_set_error_at_location(
          checker, statement->location,
          "This 'break' carries a value, but the loop's value goes nowhere: "
          "use the loop as an expression, or write the value last in a "
          "function that returns it");
      return 0;
    }
    return 1;
}

static int type_checker_check_implicit_call(TypeChecker *checker,
                                            ASTNode *statement,
                                            ReturnStatement *ret,
                                            Type *return_type) {
  ASTNode *value = ret->value;
  Type *found = type_checker_infer_type(checker, value);
  if (!found) {
    return 0;
  }
  if (type_checker_reject_comptime_escape(checker, value->location, found)) {
    return 0;
  }
  int fits = found->kind != TYPE_VOID && return_type &&
             (type_checker_takes_null(return_type, value) ||
              type_checker_is_assignable_from(checker, return_type, found,
                                              value));
  if (fits && ret->implicit_tail == RETURN_TAIL_CALL) {
    type_checker_note_return_range(checker, value);
    return 1;
  }
  if (fits) {
    error_reporter_add_warning(
        checker->error_reporter, ERROR_SEMANTIC, value->location,
        "main drops the value of its last call; write 'return' to make it the "
        "exit status, or add ';' to say it is dropped");
  }
  ast_unwrap_implicit_return(statement);
  return 1;
}

static int type_checker_check_continue(TypeChecker *checker, ASTNode *statement) {
    LoopControlStatement *cont = (LoopControlStatement *)statement->data;
    if (checker->loop_depth <= 0) {
      type_checker_set_error_at_location(
          checker, statement->location,
          "'continue' can only be used inside a loop");
      return 0;
    }
    if (cont && cont->target_label &&
        !type_checker_loop_label_in_scope(checker, cont->target_label)) {
      type_checker_set_error_at_location(
          checker, statement->location,
          "'continue %s' has no matching labeled loop", cont->target_label);
      return 0;
    }
    return 1;
}

static int type_checker_check_statement_body(TypeChecker *checker,
                                             ASTNode *statement) {
  switch (statement->type) {
  case AST_DEFER_STATEMENT:
    return type_checker_check_defer_statement(checker, statement);

  case AST_ERRDEFER_STATEMENT:
    return type_checker_check_errdefer(checker, statement);

  case AST_VAR_DECLARATION:
  case AST_FUNCTION_DECLARATION:
  case AST_STRUCT_DECLARATION:
  case AST_ASSIGNMENT:
    return type_checker_process_declaration(checker, statement);

  case AST_FUNCTION_CALL: {
    Type *return_type = type_checker_infer_type(checker, statement);
    if (!return_type) {
      return 0;
    }
    if (type_checker_reject_comptime_escape(checker, statement->location,
                                            return_type)) {
      return 0;
    }
    return 1;
  }

  case AST_GPU_LAUNCH:
    return type_checker_check_gpu_launch(checker, statement);

  case AST_BARRIER_STATEMENT:
    return type_checker_check_barrier(checker, statement);

  case AST_RETURN_STATEMENT:
    return type_checker_check_return(checker, statement);

  case AST_IF_STATEMENT:
    return type_checker_check_if_statement(checker, statement);

  case AST_WHILE_STATEMENT:
    return type_checker_check_while(checker, statement);

  case AST_FOR_STATEMENT:
    return type_checker_check_for_statement(checker, statement);

  case AST_SWITCH_STATEMENT:
    return type_checker_check_switch_statement(checker, statement);

  case AST_MATCH_STATEMENT:
    return type_checker_check_match_statement(checker, statement);

  case AST_VALUE_SCOPE:
    return type_checker_check_value_scope_statement(checker, statement);

  case AST_YIELD_STATEMENT:
    return type_checker_check_yield(checker, statement);

  case AST_QUIESCE_STATEMENT:
    return 1;

  case AST_FALLTHROUGH_STATEMENT:
    if (checker->switch_depth <= 0) {
      type_checker_set_error_at_location(
          checker, statement->location,
          "'fallthrough' can only be used inside a switch case");
      return 0;
    }
    return 1;

  case AST_BREAK_STATEMENT:
    return type_checker_check_break(checker, statement);

  case AST_CONTINUE_STATEMENT:
    return type_checker_check_continue(checker, statement);

  case AST_INLINE_ASM:
    return type_checker_check_asm_bindings(checker, statement);

  case AST_COMPTIME_FOR:
    type_checker_set_error_at_location(
        checker, statement->location,
        "'comptime for' must be a statement in a block, not a bare branch body");
    return 0;

  case AST_PROGRAM:
    return type_checker_check_block(checker, statement);

  default: {
    const char *shape = "expression";
    const char *why = "its value goes nowhere";
    switch (statement->type) {
    case AST_BINARY_EXPRESSION:
      shape = "arithmetic or comparison";
      why = "it computes a value and drops it";
      break;
    case AST_UNARY_EXPRESSION:
      shape = "unary expression";
      why = "it computes a value and drops it";
      break;
    case AST_IDENTIFIER:
      shape = "name on its own";
      why = "reading a name does nothing";
      break;
    case AST_NUMBER_LITERAL:
    case AST_STRING_LITERAL:
    case AST_AGGREGATE_LITERAL:
      shape = "literal on its own";
      why = "it computes a value and drops it";
      break;
    case AST_MEMBER_ACCESS:
      shape = "field read";
      why = "it computes a value and drops it";
      break;
    case AST_INDEX_EXPRESSION:
      shape = "index read";
      why = "it computes a value and drops it";
      break;
    case AST_CAST_EXPRESSION:
      shape = "cast";
      why = "it computes a value and drops it";
      break;
    case AST_NEW_EXPRESSION:
      shape = "allocation";
      why = "the memory it returns is lost at once";
      break;
    case AST_LAMBDA_EXPRESSION:
      shape = "function literal";
      why = "nothing holds it and nothing calls it";
      break;
    default:
      break;
    }
    char message[256];
    snprintf(message, sizeof(message), "This %s is not a statement: %s%s", shape,
             why,
             checker->value_frame_count > 0
                 ? "; to make it the value here, write it last with no ';'"
                 : "");
    type_checker_set_error_at_location(checker, statement->location, message);
    if (checker->error_reporter)
      error_reporter_set_last_label(
          checker->error_reporter, "this line has no effect");
    return 0;
  }
  }
}

static int type_checker_check_defer_statement(TypeChecker *checker,
                                              ASTNode *statement) {
  if (!checker->current_function) {
    type_checker_set_error_at_location(
        checker, statement->location,
        "Defer statement outside of a function");
    return 0;
  }

  DeferStatement *defer_stmt = (DeferStatement *)statement->data;
  if (!defer_stmt || !defer_stmt->statement) {
    type_checker_set_error_at_location(checker, statement->location,
                                       "Invalid defer statement");
    return 0;
  }

  switch (defer_stmt->statement->type) {
  case AST_FUNCTION_CALL:
  case AST_ASSIGNMENT:
  case AST_PROGRAM:
    break;
  default:
    type_checker_set_error_at_location(
        checker, defer_stmt->statement->location,
        "Deferred statement must be a function call, assignment, or block");
    return 0;
  }

  return type_checker_check_statement(checker, defer_stmt->statement);
}
