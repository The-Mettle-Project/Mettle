package org.mettle.clion.lang;

import com.intellij.psi.tree.IElementType;
import com.intellij.psi.tree.TokenSet;

import java.util.Collections;
import java.util.HashMap;
import java.util.Map;

/**
 * Every token and composite element type of the Mettle grammar.
 *
 * <p>The keyword list mirrors {@code docs/lexical-structure.md}: names that are actually
 * reserved live in {@link #KEYWORDS}, while names that merely have built-in meaning
 * ({@code bool}, {@code true}, {@code sizeof}, the GPU built-ins, ...) stay identifiers and
 * are highlighted by the annotator instead.
 */
public interface MettleTypes {
    // ---------------------------------------------------------------- trivia
    IElementType LINE_COMMENT = new MettleTokenType("LINE_COMMENT");
    IElementType BLOCK_COMMENT = new MettleTokenType("BLOCK_COMMENT");

    // -------------------------------------------------------------- literals
    IElementType IDENTIFIER = new MettleTokenType("IDENTIFIER");
    IElementType INT_LITERAL = new MettleTokenType("INT_LITERAL");
    IElementType FLOAT_LITERAL = new MettleTokenType("FLOAT_LITERAL");
    IElementType CHAR_LITERAL = new MettleTokenType("CHAR_LITERAL");
    IElementType STRING_LITERAL = new MettleTokenType("STRING_LITERAL");

    // -------------------------------------------------------------- keywords
    IElementType KW_IMPORT = new MettleTokenType("import");
    IElementType KW_IMPORT_STR = new MettleTokenType("import_str");
    IElementType KW_EXTERN = new MettleTokenType("extern");
    IElementType KW_EXPORT = new MettleTokenType("export");
    IElementType KW_VAR = new MettleTokenType("var");
    IElementType KW_CONST = new MettleTokenType("const");
    IElementType KW_FN = new MettleTokenType("fn");
    IElementType KW_KERNEL = new MettleTokenType("kernel");
    IElementType KW_STRUCT = new MettleTokenType("struct");
    IElementType KW_ENUM = new MettleTokenType("enum");
    IElementType KW_TRAIT = new MettleTokenType("trait");
    IElementType KW_IMPL = new MettleTokenType("impl");
    IElementType KW_WHERE = new MettleTokenType("where");
    IElementType KW_METHOD = new MettleTokenType("method");
    IElementType KW_WORKGROUP = new MettleTokenType("workgroup");
    IElementType KW_PRIVATE = new MettleTokenType("private");
    IElementType KW_IF = new MettleTokenType("if");
    IElementType KW_ELSE = new MettleTokenType("else");
    IElementType KW_WHILE = new MettleTokenType("while");
    IElementType KW_FOR = new MettleTokenType("for");
    IElementType KW_SWITCH = new MettleTokenType("switch");
    IElementType KW_CASE = new MettleTokenType("case");
    IElementType KW_DEFAULT = new MettleTokenType("default");
    IElementType KW_MATCH = new MettleTokenType("match");
    IElementType KW_BREAK = new MettleTokenType("break");
    IElementType KW_CONTINUE = new MettleTokenType("continue");
    IElementType KW_RETURN = new MettleTokenType("return");
    IElementType KW_DEFER = new MettleTokenType("defer");
    IElementType KW_ERRDEFER = new MettleTokenType("errdefer");
    IElementType KW_DISPATCH = new MettleTokenType("dispatch");
    IElementType KW_BARRIER = new MettleTokenType("barrier");
    IElementType KW_ASM = new MettleTokenType("asm");
    IElementType KW_THIS = new MettleTokenType("this");
    IElementType KW_NEW = new MettleTokenType("new");

    // ---------------------------------------------------------- builtin types
    IElementType TY_INT8 = new MettleTokenType("int8");
    IElementType TY_INT16 = new MettleTokenType("int16");
    IElementType TY_INT32 = new MettleTokenType("int32");
    IElementType TY_INT64 = new MettleTokenType("int64");
    IElementType TY_UINT8 = new MettleTokenType("uint8");
    IElementType TY_UINT16 = new MettleTokenType("uint16");
    IElementType TY_UINT32 = new MettleTokenType("uint32");
    IElementType TY_UINT64 = new MettleTokenType("uint64");
    IElementType TY_FLOAT32 = new MettleTokenType("float32");
    IElementType TY_FLOAT64 = new MettleTokenType("float64");
    IElementType TY_STRING = new MettleTokenType("string");

    // ------------------------------------------------------------- operators
    IElementType PLUS = new MettleTokenType("+");
    IElementType MINUS = new MettleTokenType("-");
    IElementType STAR = new MettleTokenType("*");
    IElementType SLASH = new MettleTokenType("/");
    IElementType PERCENT = new MettleTokenType("%");
    IElementType AMP = new MettleTokenType("&");
    IElementType PIPE = new MettleTokenType("|");
    IElementType CARET = new MettleTokenType("^");
    IElementType TILDE = new MettleTokenType("~");
    IElementType BANG = new MettleTokenType("!");
    IElementType LT = new MettleTokenType("<");
    IElementType GT = new MettleTokenType(">");
    IElementType ASSIGN = new MettleTokenType("=");
    IElementType EQ = new MettleTokenType("==");
    IElementType NE = new MettleTokenType("!=");
    IElementType LE = new MettleTokenType("<=");
    IElementType GE = new MettleTokenType(">=");
    IElementType ANDAND = new MettleTokenType("&&");
    IElementType OROR = new MettleTokenType("||");
    IElementType SHL = new MettleTokenType("<<");
    IElementType SHR = new MettleTokenType(">>");
    IElementType PLUS_EQ = new MettleTokenType("+=");
    IElementType MINUS_EQ = new MettleTokenType("-=");
    IElementType STAR_EQ = new MettleTokenType("*=");
    IElementType SLASH_EQ = new MettleTokenType("/=");
    IElementType PERCENT_EQ = new MettleTokenType("%=");
    IElementType AMP_EQ = new MettleTokenType("&=");
    IElementType PIPE_EQ = new MettleTokenType("|=");
    IElementType CARET_EQ = new MettleTokenType("^=");
    IElementType SHL_EQ = new MettleTokenType("<<=");
    IElementType SHR_EQ = new MettleTokenType(">>=");
    IElementType ARROW = new MettleTokenType("->");
    IElementType DOT = new MettleTokenType(".");
    IElementType DOTDOT = new MettleTokenType("..");
    IElementType DOTDOTEQ = new MettleTokenType("..=");
    IElementType COLON = new MettleTokenType(":");
    IElementType SEMICOLON = new MettleTokenType(";");
    IElementType COMMA = new MettleTokenType(",");
    IElementType LPAREN = new MettleTokenType("(");
    IElementType RPAREN = new MettleTokenType(")");
    IElementType LBRACE = new MettleTokenType("{");
    IElementType RBRACE = new MettleTokenType("}");
    IElementType LBRACKET = new MettleTokenType("[");
    IElementType RBRACKET = new MettleTokenType("]");
    IElementType AT = new MettleTokenType("@");

    // ------------------------------------------------------ composite elements
    IElementType IMPORT_DECL = new MettleElementType("IMPORT_DECL");
    IElementType FUNCTION_DECL = new MettleElementType("FUNCTION_DECL");
    IElementType METHOD_DECL = new MettleElementType("METHOD_DECL");
    IElementType STRUCT_DECL = new MettleElementType("STRUCT_DECL");
    IElementType ENUM_DECL = new MettleElementType("ENUM_DECL");
    IElementType TRAIT_DECL = new MettleElementType("TRAIT_DECL");
    IElementType IMPL_DECL = new MettleElementType("IMPL_DECL");
    IElementType FIELD_DECL = new MettleElementType("FIELD_DECL");
    IElementType ENUM_MEMBER = new MettleElementType("ENUM_MEMBER");
    IElementType PARAM_DECL = new MettleElementType("PARAM_DECL");
    IElementType PARAM_LIST = new MettleElementType("PARAM_LIST");
    IElementType TYPE_PARAM_LIST = new MettleElementType("TYPE_PARAM_LIST");
    IElementType TYPE_PARAM = new MettleElementType("TYPE_PARAM");
    IElementType VAR_DECL = new MettleElementType("VAR_DECL");
    IElementType CONST_DECL = new MettleElementType("CONST_DECL");
    IElementType DECORATOR = new MettleElementType("DECORATOR");
    IElementType TYPE_REF = new MettleElementType("TYPE_REF");
    IElementType BLOCK = new MettleElementType("BLOCK");
    IElementType ASM_BLOCK = new MettleElementType("ASM_BLOCK");

    IElementType EXPR_STMT = new MettleElementType("EXPR_STMT");
    IElementType ASSIGN_STMT = new MettleElementType("ASSIGN_STMT");
    IElementType RETURN_STMT = new MettleElementType("RETURN_STMT");
    IElementType IF_STMT = new MettleElementType("IF_STMT");
    IElementType WHILE_STMT = new MettleElementType("WHILE_STMT");
    IElementType FOR_STMT = new MettleElementType("FOR_STMT");
    IElementType SWITCH_STMT = new MettleElementType("SWITCH_STMT");
    IElementType MATCH_STMT = new MettleElementType("MATCH_STMT");
    IElementType CASE_CLAUSE = new MettleElementType("CASE_CLAUSE");
    IElementType BREAK_STMT = new MettleElementType("BREAK_STMT");
    IElementType CONTINUE_STMT = new MettleElementType("CONTINUE_STMT");
    IElementType DEFER_STMT = new MettleElementType("DEFER_STMT");
    IElementType DISPATCH_STMT = new MettleElementType("DISPATCH_STMT");

    IElementType BINARY_EXPR = new MettleElementType("BINARY_EXPR");
    IElementType UNARY_EXPR = new MettleElementType("UNARY_EXPR");
    IElementType CAST_EXPR = new MettleElementType("CAST_EXPR");
    IElementType CALL_EXPR = new MettleElementType("CALL_EXPR");
    IElementType INDEX_EXPR = new MettleElementType("INDEX_EXPR");
    IElementType FIELD_EXPR = new MettleElementType("FIELD_EXPR");
    IElementType PAREN_EXPR = new MettleElementType("PAREN_EXPR");
    IElementType REF_EXPR = new MettleElementType("REF_EXPR");
    IElementType LITERAL_EXPR = new MettleElementType("LITERAL_EXPR");
    IElementType NEW_EXPR = new MettleElementType("NEW_EXPR");
    IElementType AGGREGATE_EXPR = new MettleElementType("AGGREGATE_EXPR");
    IElementType LAMBDA_EXPR = new MettleElementType("LAMBDA_EXPR");
    IElementType ARG_LIST = new MettleElementType("ARG_LIST");
    IElementType RANGE_EXPR = new MettleElementType("RANGE_EXPR");

    // ------------------------------------------------------------ token sets
    TokenSet COMMENTS = TokenSet.create(LINE_COMMENT, BLOCK_COMMENT);
    TokenSet STRINGS = TokenSet.create(STRING_LITERAL);
    TokenSet LITERALS = TokenSet.create(INT_LITERAL, FLOAT_LITERAL, CHAR_LITERAL, STRING_LITERAL);

    TokenSet BUILTIN_TYPES = TokenSet.create(
            TY_INT8, TY_INT16, TY_INT32, TY_INT64,
            TY_UINT8, TY_UINT16, TY_UINT32, TY_UINT64,
            TY_FLOAT32, TY_FLOAT64, TY_STRING);

    TokenSet KEYWORD_TOKENS = TokenSet.create(
            KW_IMPORT, KW_IMPORT_STR, KW_EXTERN, KW_EXPORT, KW_VAR, KW_CONST, KW_FN, KW_KERNEL,
            KW_STRUCT, KW_ENUM, KW_TRAIT, KW_IMPL, KW_WHERE, KW_METHOD, KW_WORKGROUP, KW_PRIVATE,
            KW_IF, KW_ELSE, KW_WHILE, KW_FOR, KW_SWITCH, KW_CASE, KW_DEFAULT, KW_MATCH, KW_BREAK,
            KW_CONTINUE, KW_RETURN, KW_DEFER, KW_ERRDEFER, KW_DISPATCH, KW_BARRIER, KW_ASM,
            KW_THIS, KW_NEW);

    TokenSet OPERATORS = TokenSet.create(
            PLUS, MINUS, STAR, SLASH, PERCENT, AMP, PIPE, CARET, TILDE, BANG, LT, GT, ASSIGN,
            EQ, NE, LE, GE, ANDAND, OROR, SHL, SHR, PLUS_EQ, MINUS_EQ, STAR_EQ, SLASH_EQ,
            PERCENT_EQ, AMP_EQ, PIPE_EQ, CARET_EQ, SHL_EQ, SHR_EQ, ARROW, DOT, DOTDOT, DOTDOTEQ);

    TokenSet DECLARATIONS = TokenSet.create(
            FUNCTION_DECL, METHOD_DECL, STRUCT_DECL, ENUM_DECL, TRAIT_DECL, IMPL_DECL,
            FIELD_DECL, ENUM_MEMBER, PARAM_DECL, VAR_DECL, CONST_DECL, TYPE_PARAM);

    /** Reserved words, mapped to their token type. */
    Map<String, IElementType> KEYWORDS = Collections.unmodifiableMap(new HashMap<>() {{
        put("import", KW_IMPORT);
        put("import_str", KW_IMPORT_STR);
        put("extern", KW_EXTERN);
        put("export", KW_EXPORT);
        put("var", KW_VAR);
        put("const", KW_CONST);
        put("fn", KW_FN);
        put("kernel", KW_KERNEL);
        put("struct", KW_STRUCT);
        put("enum", KW_ENUM);
        put("trait", KW_TRAIT);
        put("impl", KW_IMPL);
        put("where", KW_WHERE);
        put("method", KW_METHOD);
        put("workgroup", KW_WORKGROUP);
        put("private", KW_PRIVATE);
        put("if", KW_IF);
        put("else", KW_ELSE);
        put("while", KW_WHILE);
        put("for", KW_FOR);
        put("switch", KW_SWITCH);
        put("case", KW_CASE);
        put("default", KW_DEFAULT);
        put("match", KW_MATCH);
        put("break", KW_BREAK);
        put("continue", KW_CONTINUE);
        put("return", KW_RETURN);
        put("defer", KW_DEFER);
        put("errdefer", KW_ERRDEFER);
        put("dispatch", KW_DISPATCH);
        put("barrier", KW_BARRIER);
        put("asm", KW_ASM);
        put("this", KW_THIS);
        put("new", KW_NEW);
        put("int8", TY_INT8);
        put("int16", TY_INT16);
        put("int32", TY_INT32);
        put("int64", TY_INT64);
        put("uint8", TY_UINT8);
        put("uint16", TY_UINT16);
        put("uint32", TY_UINT32);
        put("uint64", TY_UINT64);
        put("float32", TY_FLOAT32);
        put("float64", TY_FLOAT64);
        put("string", TY_STRING);
    }});
}
