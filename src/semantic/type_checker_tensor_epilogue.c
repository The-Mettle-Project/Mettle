#include "type_checker_internal.h"

/* Kept in a separate translation unit so the normal MinGW CodeView build
 * retains complete debug information for the already-large expression
 * checker. The implementation remains frontend-only and lowers to the
 * target-neutral tensor epilogue descriptor. */
Type *type_checker_tensor_epilogue_builtin(TypeChecker *checker,
                                                  ASTNode *expression,
                                                  CallExpression *call,
                                                  int *handled) {
  MtlcTensorEpilogueDesc desc = {0};
  int have_m = 0, have_n = 0, have_shape = 0, have_element = 0;
  int have_stride = 0, have_bias_stride = 0, have_bias_layout = 0;
  *handled = 0;
  if (!call || !call->function_name || call->object ||
      strcmp(call->function_name, "tensor_epilogue") != 0) {
    return NULL;
  }
  *handled = 1;
  FunctionDeclaration *owner =
      checker->current_function_decl &&
              checker->current_function_decl->type == AST_FUNCTION_DECLARATION
          ? (FunctionDeclaration *)checker->current_function_decl->data
          : NULL;
  if (!owner || !owner->is_kernel) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "tensor_epilogue is only legal directly inside a GPU kernel");
    return NULL;
  }
  if (call->argument_count < 3) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "tensor_epilogue expects a destination pointer followed by named m/n and element_type options");
    return NULL;
  }
  if (call->argument_names && call->argument_names[0]) {
    type_checker_set_error_at_location(
        checker, call->arguments[0]->location,
        "The tensor_epilogue destination operand is positional");
    return NULL;
  }

  desc.layout = MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  desc.bias_mode = MTLC_TENSOR_BIAS_NONE;
  desc.bias_layout = MTLC_TENSOR_LAYOUT_INVALID;
  desc.activation = MTLC_TENSOR_ACTIVATION_IDENTITY;
  desc.scope = MTLC_MEMORY_SCOPE_SUBGROUP;
  call->tensor_epilogue_bias_argument = SIZE_MAX;
  call->tensor_epilogue_alpha_argument = SIZE_MAX;
  call->tensor_epilogue_beta_argument = SIZE_MAX;
  call->tensor_epilogue_clamp_min_argument = SIZE_MAX;
  call->tensor_epilogue_clamp_max_argument = SIZE_MAX;
  call->tensor_epilogue_stride_argument = SIZE_MAX;
  call->tensor_epilogue_bias_stride_argument = SIZE_MAX;

  for (size_t i = 1; i < call->argument_count; i++) {
    const char *option = call->argument_names ? call->argument_names[i] : NULL;
    ASTNode *value = call->arguments[i];
    const char *identifier = type_checker_tensor_option_identifier(value);
    if (!option) {
      type_checker_set_error_at_location(
          checker, value->location,
          "Tensor epilogue arguments after the destination must be named");
      return NULL;
    }
    for (size_t prior = 1; prior < i; prior++) {
      if (call->argument_names[prior] &&
          strcmp(call->argument_names[prior], option) == 0) {
        type_checker_set_error_at_location(checker, value->location,
                                           "Duplicate tensor epilogue option '%s'",
                                           option);
        return NULL;
      }
    }

    if (!strcmp(option, "shape")) {
      unsigned m = 0, n = 0;
      char tail = 0;
      if (!identifier || sscanf(identifier, "m%un%u%c", &m, &n, &tail) != 2 ||
          m == 0 || n == 0 || m > UINT16_MAX || n > UINT16_MAX || have_m ||
          have_n) {
        type_checker_set_error_at_location(
            checker, value->location,
            "Tensor epilogue shape must be an identifier like m16n16 and cannot be mixed with m/n");
        return NULL;
      }
      desc.m = (uint16_t)m;
      desc.n = (uint16_t)n;
      have_m = have_n = have_shape = 1;
    } else if (!strcmp(option, "m") || !strcmp(option, "n")) {
      uint32_t dimension = 0;
      if (have_shape ||
          !type_checker_tensor_option_u32(checker, value, option, UINT16_MAX,
                                          &dimension)) {
        if (have_shape)
          type_checker_set_error_at_location(
              checker, value->location,
              "Tensor epilogue shape cannot be mixed with explicit m/n options");
        return NULL;
      }
      if (!strcmp(option, "m")) {
        desc.m = (uint16_t)dimension;
        have_m = 1;
      } else {
        desc.n = (uint16_t)dimension;
        have_n = 1;
      }
    } else if (!strcmp(option, "element") ||
               !strcmp(option, "element_type")) {
      if (have_element) {
        type_checker_set_error_at_location(
            checker, value->location,
            "Tensor epilogue element type was specified more than once");
        return NULL;
      }
      desc.element = type_checker_tensor_element_name(identifier);
      if (desc.element != MTLC_TENSOR_ELEMENT_FLOAT16 &&
          desc.element != MTLC_TENSOR_ELEMENT_BFLOAT16 &&
          desc.element != MTLC_TENSOR_ELEMENT_FLOAT32 &&
          desc.element != MTLC_TENSOR_ELEMENT_FLOAT64) {
        type_checker_set_error_at_location(
            checker, value->location,
            "Tensor epilogue element type must be f16, bf16, f32, or f64");
        return NULL;
      }
      have_element = 1;
    } else if (!strcmp(option, "layout")) {
      desc.layout = type_checker_tensor_layout_name(identifier);
      if (desc.layout == MTLC_TENSOR_LAYOUT_INVALID) {
        type_checker_set_error_at_location(
            checker, value->location,
            "Tensor epilogue layout must be row or col");
        return NULL;
      }
    } else if (!strcmp(option, "ldd") ||
               !strcmp(option, "leading_dimension")) {
      long long constant = 0;
      if (have_stride) {
        type_checker_set_error_at_location(
            checker, value->location,
            "Tensor epilogue destination leading dimension was specified more than once");
        return NULL;
      }
      have_stride = 1;
      if (type_checker_eval_integer_constant(value, &constant)) {
        if (constant <= 0 || (unsigned long long)constant > UINT32_MAX) {
          type_checker_set_error_at_location(
              checker, value->location,
              "Tensor epilogue leading dimension must be in [1, %u] or a runtime integer expression",
              UINT32_MAX);
          return NULL;
        }
        desc.leading_dimension = (uint32_t)constant;
      } else {
        Type *stride_type = type_checker_infer_type(checker, value);
        if (!stride_type || !type_checker_is_integer_type(stride_type)) {
          type_checker_set_error_at_location(
              checker, value->location,
              "Runtime tensor epilogue leading dimension must have integer type");
          return NULL;
        }
        desc.leading_dimension = 0;
        call->tensor_epilogue_stride_argument = i;
      }
    } else if (!strcmp(option, "bias_mode")) {
      if (identifier && !strcmp(identifier, "none"))
        desc.bias_mode = MTLC_TENSOR_BIAS_NONE;
      else if (identifier && (!strcmp(identifier, "row") ||
                              !strcmp(identifier, "per_row")))
        desc.bias_mode = MTLC_TENSOR_BIAS_PER_ROW;
      else if (identifier && (!strcmp(identifier, "column") ||
                              !strcmp(identifier, "col") ||
                              !strcmp(identifier, "per_column")))
        desc.bias_mode = MTLC_TENSOR_BIAS_PER_COLUMN;
      else if (identifier && !strcmp(identifier, "matrix"))
        desc.bias_mode = MTLC_TENSOR_BIAS_MATRIX;
      else {
        type_checker_set_error_at_location(
            checker, value->location,
            "Tensor epilogue bias mode must be none, row, column, or matrix");
        return NULL;
      }
    } else if (!strcmp(option, "bias_layout")) {
      desc.bias_layout = type_checker_tensor_layout_name(identifier);
      have_bias_layout = 1;
      if (desc.bias_layout == MTLC_TENSOR_LAYOUT_INVALID) {
        type_checker_set_error_at_location(
            checker, value->location,
            "Tensor epilogue matrix-bias layout must be row or col");
        return NULL;
      }
    } else if (!strcmp(option, "ldbias") ||
               !strcmp(option, "bias_leading_dimension")) {
      long long constant = 0;
      if (have_bias_stride) {
        type_checker_set_error_at_location(
            checker, value->location,
            "Tensor epilogue bias leading dimension was specified more than once");
        return NULL;
      }
      have_bias_stride = 1;
      if (type_checker_eval_integer_constant(value, &constant)) {
        if (constant <= 0 || (unsigned long long)constant > UINT32_MAX) {
          type_checker_set_error_at_location(
              checker, value->location,
              "Tensor epilogue bias leading dimension must be in [1, %u] or a runtime integer expression",
              UINT32_MAX);
          return NULL;
        }
        desc.bias_leading_dimension = (uint32_t)constant;
      } else {
        Type *stride_type = type_checker_infer_type(checker, value);
        if (!stride_type || !type_checker_is_integer_type(stride_type)) {
          type_checker_set_error_at_location(
              checker, value->location,
              "Runtime tensor epilogue bias leading dimension must have integer type");
          return NULL;
        }
        desc.bias_leading_dimension = 0;
        call->tensor_epilogue_bias_stride_argument = i;
      }
    } else if (!strcmp(option, "activation")) {
      if (identifier && !strcmp(identifier, "identity"))
        desc.activation = MTLC_TENSOR_ACTIVATION_IDENTITY;
      else if (identifier && !strcmp(identifier, "relu"))
        desc.activation = MTLC_TENSOR_ACTIVATION_RELU;
      else if (identifier && !strcmp(identifier, "clamp"))
        desc.activation = MTLC_TENSOR_ACTIVATION_CLAMP;
      else {
        type_checker_set_error_at_location(
            checker, value->location,
            "Tensor epilogue activation must be identity, relu, or clamp");
        return NULL;
      }
    } else if (!strcmp(option, "scope")) {
      if (identifier && !strcmp(identifier, "subgroup"))
        desc.scope = MTLC_MEMORY_SCOPE_SUBGROUP;
      else if (identifier && !strcmp(identifier, "workgroup"))
        desc.scope = MTLC_MEMORY_SCOPE_WORKGROUP;
      else {
        type_checker_set_error_at_location(
            checker, value->location,
            "Tensor epilogue scope must be subgroup or workgroup");
        return NULL;
      }
    } else if (!strcmp(option, "bias")) {
      call->tensor_epilogue_bias_argument = i;
    } else if (!strcmp(option, "alpha")) {
      call->tensor_epilogue_alpha_argument = i;
      desc.scale_output = 1;
    } else if (!strcmp(option, "beta")) {
      call->tensor_epilogue_beta_argument = i;
      desc.scale_bias = 1;
    } else if (!strcmp(option, "clamp_min")) {
      call->tensor_epilogue_clamp_min_argument = i;
    } else if (!strcmp(option, "clamp_max")) {
      call->tensor_epilogue_clamp_max_argument = i;
    } else {
      type_checker_set_error_at_location(
          checker, value->location,
          "Unknown tensor epilogue option '%s'", option);
      return NULL;
    }
  }

  if (!have_m || !have_n || !have_element) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "tensor_epilogue requires shape (or m/n) and element_type");
    return NULL;
  }
  if (!have_stride)
    desc.leading_dimension =
        desc.layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? desc.n : desc.m;

  int has_bias = call->tensor_epilogue_bias_argument != SIZE_MAX;
  int needs_bias = desc.bias_mode != MTLC_TENSOR_BIAS_NONE;
  if (has_bias != needs_bias || (desc.scale_bias && !needs_bias)) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "Tensor epilogue bias operand must be present exactly when bias_mode is row, column, or matrix");
    return NULL;
  }
  if (desc.bias_mode == MTLC_TENSOR_BIAS_MATRIX) {
    if (!have_bias_layout) desc.bias_layout = desc.layout;
    if (!have_bias_stride)
      desc.bias_leading_dimension =
          desc.bias_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? desc.n : desc.m;
  } else if (have_bias_layout || have_bias_stride) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "bias_layout and ldbias are only valid for matrix bias");
    return NULL;
  }

  int has_clamp_min = call->tensor_epilogue_clamp_min_argument != SIZE_MAX;
  int has_clamp_max = call->tensor_epilogue_clamp_max_argument != SIZE_MAX;
  int needs_clamp = desc.activation == MTLC_TENSOR_ACTIVATION_CLAMP;
  if (has_clamp_min != needs_clamp || has_clamp_max != needs_clamp) {
    type_checker_set_error_at_location(
        checker, expression->location,
        "clamp activation requires exactly one clamp_min and one clamp_max operand");
    return NULL;
  }
  if (!mtlc_tensor_epilogue_desc_is_valid(&desc)) {
    type_checker_set_error_at_location(checker, expression->location,
                                       "Invalid tensor epilogue descriptor");
    return NULL;
  }

  Type *destination =
      type_checker_infer_type(checker, call->arguments[0]);
  if (!type_checker_tensor_pointer_matches(destination, desc.element)) {
    type_checker_set_error_at_location(
        checker, call->arguments[0]->location,
        "Tensor epilogue destination storage is incompatible with element_type");
    return NULL;
  }
  if (has_bias) {
    Type *bias = type_checker_infer_type(
        checker, call->arguments[call->tensor_epilogue_bias_argument]);
    if (!type_checker_tensor_pointer_matches(bias, desc.element)) {
      type_checker_set_error_at_location(
          checker,
          call->arguments[call->tensor_epilogue_bias_argument]->location,
          "Tensor epilogue bias storage is incompatible with element_type");
      return NULL;
    }
  }

  Type *compute_type = desc.element == MTLC_TENSOR_ELEMENT_FLOAT64
                           ? checker->builtin_float64
                           : checker->builtin_float32;
  size_t scalar_indices[4] = {
      call->tensor_epilogue_alpha_argument,
      call->tensor_epilogue_beta_argument,
      call->tensor_epilogue_clamp_min_argument,
      call->tensor_epilogue_clamp_max_argument};
  const char *scalar_names[4] = {"alpha", "beta", "clamp_min", "clamp_max"};
  for (size_t i = 0; i < 4; i++) {
    if (scalar_indices[i] == SIZE_MAX) continue;
    Type *actual = type_checker_infer_type(
        checker, call->arguments[scalar_indices[i]]);
    if (!actual || actual->kind != compute_type->kind) {
      type_checker_set_error_at_location(
          checker, call->arguments[scalar_indices[i]]->location,
          "Tensor epilogue %s must have type %s",
          scalar_names[i], compute_type->name);
      return NULL;
    }
  }

  call->is_tensor_epilogue = 1;
  call->tensor_epilogue_desc = desc;
  return checker->builtin_void;
}

