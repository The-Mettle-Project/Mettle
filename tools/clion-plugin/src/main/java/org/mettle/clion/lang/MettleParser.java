package org.mettle.clion.lang;

import com.intellij.lang.ASTNode;
import com.intellij.lang.PsiBuilder;
import com.intellij.lang.PsiParser;
import com.intellij.psi.tree.IElementType;
import com.intellij.psi.tree.TokenSet;
import org.jetbrains.annotations.NotNull;

/**
 * Recursive-descent parser for Mettle.
 *
 * <p>It is deliberately tolerant: anything it cannot make sense of is reported once and skipped
 * to the next statement or declaration boundary, so a half-typed file still yields a usable tree
 * for the structure view, folding and resolution. Every loop guards on the builder offset, so a
 * construct the grammar does not cover can never stall the parser.
 */
public class MettleParser implements PsiParser {

    private static final TokenSet TYPE_START = TokenSet.orSet(
            MettleTypes.BUILTIN_TYPES, TokenSet.create(MettleTypes.IDENTIFIER, MettleTypes.KW_FN));

    private static final TokenSet EXPR_START = TokenSet.orSet(
            MettleTypes.BUILTIN_TYPES, MettleTypes.LITERALS,
            TokenSet.create(MettleTypes.IDENTIFIER, MettleTypes.LPAREN, MettleTypes.LBRACKET,
                    MettleTypes.LBRACE, MettleTypes.MINUS, MettleTypes.PLUS, MettleTypes.STAR,
                    MettleTypes.AMP, MettleTypes.BANG, MettleTypes.TILDE, MettleTypes.KW_NEW,
                    MettleTypes.KW_THIS, MettleTypes.KW_MATCH, MettleTypes.KW_WORKGROUP,
                    MettleTypes.KW_PRIVATE, MettleTypes.KW_BARRIER, MettleTypes.KW_IMPORT_STR,
                    MettleTypes.KW_FN));

    private static final TokenSet STATEMENT_START = TokenSet.orSet(EXPR_START, TokenSet.create(
            MettleTypes.KW_VAR, MettleTypes.KW_CONST, MettleTypes.KW_IF, MettleTypes.KW_WHILE,
            MettleTypes.KW_FOR, MettleTypes.KW_SWITCH, MettleTypes.KW_RETURN, MettleTypes.KW_BREAK,
            MettleTypes.KW_CONTINUE, MettleTypes.KW_DEFER, MettleTypes.KW_ERRDEFER,
            MettleTypes.KW_DISPATCH, MettleTypes.KW_ASM, MettleTypes.KW_BARRIER, MettleTypes.AT));

    private static final TokenSet DECL_START = TokenSet.create(
            MettleTypes.KW_IMPORT, MettleTypes.KW_EXPORT, MettleTypes.KW_EXTERN, MettleTypes.KW_FN,
            MettleTypes.KW_KERNEL, MettleTypes.KW_STRUCT, MettleTypes.KW_ENUM, MettleTypes.KW_TRAIT,
            MettleTypes.KW_IMPL, MettleTypes.KW_VAR, MettleTypes.KW_CONST, MettleTypes.AT,
            MettleTypes.KW_WORKGROUP, MettleTypes.KW_PRIVATE);

    /** Tokens that can only begin a statement, never a value-producing match arm. */
    private static final TokenSet STATEMENT_KEYWORDS = TokenSet.create(
            MettleTypes.KW_RETURN, MettleTypes.KW_IF, MettleTypes.KW_WHILE, MettleTypes.KW_FOR,
            MettleTypes.KW_SWITCH, MettleTypes.KW_BREAK, MettleTypes.KW_CONTINUE, MettleTypes.KW_VAR,
            MettleTypes.KW_CONST, MettleTypes.KW_DEFER, MettleTypes.KW_ERRDEFER,
            MettleTypes.KW_DISPATCH, MettleTypes.KW_ASM, MettleTypes.KW_WORKGROUP,
            MettleTypes.KW_PRIVATE, MettleTypes.AT);

    private static final TokenSet ASSIGN_OPS = TokenSet.create(
            MettleTypes.ASSIGN, MettleTypes.PLUS_EQ, MettleTypes.MINUS_EQ, MettleTypes.STAR_EQ,
            MettleTypes.SLASH_EQ, MettleTypes.PERCENT_EQ, MettleTypes.AMP_EQ, MettleTypes.PIPE_EQ,
            MettleTypes.CARET_EQ, MettleTypes.SHL_EQ, MettleTypes.SHR_EQ);

    private PsiBuilder b;

    /** Set when a {@code >>} closed two type-argument levels at once. */
    private int pendingAngleClose;

    @Override
    public @NotNull ASTNode parse(@NotNull IElementType root, @NotNull PsiBuilder builder) {
        this.b = builder;
        PsiBuilder.Marker file = builder.mark();
        while (!builder.eof()) {
            int before = builder.getCurrentOffset();
            parseTopLevel();
            if (builder.getCurrentOffset() == before && !builder.eof()) {
                builder.advanceLexer();
            }
        }
        file.done(root);
        return builder.getTreeBuilt();
    }

    // -------------------------------------------------------- declarations

    private void parseTopLevel() {
        if (at(MettleTypes.SEMICOLON)) {
            b.advanceLexer();
            return;
        }
        if (at(MettleTypes.KW_IMPORT)) {
            parseImport();
            return;
        }
        if (!DECL_START.contains(current())) {
            // Top level also carries compile-time statements such as `static_assert(...)`.
            if (STATEMENT_START.contains(current())) {
                parseStatement();
                return;
            }
            PsiBuilder.Marker err = b.mark();
            b.advanceLexer();
            err.error("Expected a declaration");
            skipToDeclarationBoundary();
            return;
        }

        PsiBuilder.Marker m = b.mark();
        parseDecorators();
        if (at(MettleTypes.KW_EXPORT)) b.advanceLexer();

        if (at(MettleTypes.KW_FN) || at(MettleTypes.KW_KERNEL)) {
            b.advanceLexer();
            parseFunctionRest(m, MettleTypes.FUNCTION_DECL);
        } else if (at(MettleTypes.KW_EXTERN)) {
            b.advanceLexer();
            parseExternRest(m);
        } else if (at(MettleTypes.KW_STRUCT)) {
            b.advanceLexer();
            parseStructRest(m);
        } else if (at(MettleTypes.KW_ENUM)) {
            b.advanceLexer();
            parseEnumRest(m);
        } else if (at(MettleTypes.KW_TRAIT)) {
            b.advanceLexer();
            parseTraitRest(m);
        } else if (at(MettleTypes.KW_IMPL)) {
            b.advanceLexer();
            parseImplRest(m);
        } else if (at(MettleTypes.KW_VAR) || at(MettleTypes.KW_CONST)
                || at(MettleTypes.KW_WORKGROUP) || at(MettleTypes.KW_PRIVATE)) {
            parseVarLike(m);
        } else {
            b.error("Expected a declaration");
            skipToDeclarationBoundary();
            m.drop();
        }
    }

    /**
     * {@code import "m";}, {@code import "m" as alias;}, {@code import { a, b } from "m";},
     * each optionally guarded by {@code if windows} / {@code if linux}.
     */
    private void parseImport() {
        PsiBuilder.Marker m = b.mark();
        b.advanceLexer();
        if (at(MettleTypes.LBRACE)) {
            b.advanceLexer();
            while (!b.eof() && !at(MettleTypes.RBRACE)) {
                int before = b.getCurrentOffset();
                if (at(MettleTypes.IDENTIFIER)) b.advanceLexer();
                else b.advanceLexer();
                if (at(MettleTypes.COMMA)) b.advanceLexer();
                if (b.getCurrentOffset() == before) b.advanceLexer();
            }
            expect(MettleTypes.RBRACE, "Expected '}'");
            if (at(MettleTypes.IDENTIFIER) && "from".contentEquals(nullToEmpty(b.getTokenText()))) {
                b.advanceLexer();
            } else {
                b.error("Expected 'from'");
            }
        }
        if (at(MettleTypes.STRING_LITERAL)) {
            b.advanceLexer();
        } else {
            b.error("Expected a quoted module path");
        }
        if (at(MettleTypes.IDENTIFIER) && "as".contentEquals(nullToEmpty(b.getTokenText()))) {
            b.advanceLexer();
            expectName("Expected an alias name");
        }
        if (at(MettleTypes.KW_IF)) {
            b.advanceLexer();
            if (at(MettleTypes.IDENTIFIER)) b.advanceLexer();
            else b.error("Expected 'windows' or 'linux'");
        }
        expectStatementEnd();
        m.done(MettleTypes.IMPORT_DECL);
    }

    private void parseDecorators() {
        while (at(MettleTypes.AT)) {
            PsiBuilder.Marker d = b.mark();
            b.advanceLexer();
            if (at(MettleTypes.IDENTIFIER) || MettleTypes.KEYWORD_TOKENS.contains(current())) {
                b.advanceLexer();
            } else {
                b.error("Expected a decorator name");
            }
            if (at(MettleTypes.BANG)) b.advanceLexer();
            d.done(MettleTypes.DECORATOR);
        }
    }

    /** Everything after the {@code fn} / {@code kernel} / {@code method} keyword. */
    private void parseFunctionRest(PsiBuilder.Marker m, IElementType type) {
        expectName("Expected a function name");
        parseTypeParams();
        parseParamList();
        parseReturnType();
        parseWhereClause();
        if (at(MettleTypes.LBRACE)) {
            parseBlock();
        } else {
            expectStatementEnd();
        }
        m.done(type);
    }

    private void parseExternRest(PsiBuilder.Marker m) {
        if (at(MettleTypes.KW_FN)) {
            b.advanceLexer();
            expectName("Expected a function name");
            parseParamList();
            parseReturnType();
            if (at(MettleTypes.ASSIGN)) {
                b.advanceLexer();
                if (at(MettleTypes.STRING_LITERAL)) b.advanceLexer();
                else b.error("Expected a quoted link name");
            }
            expectStatementEnd();
            m.done(MettleTypes.FUNCTION_DECL);
            return;
        }
        if (at(MettleTypes.KW_VAR)) {
            b.advanceLexer();
            expectName("Expected a variable name");
            if (at(MettleTypes.COLON)) {
                b.advanceLexer();
                parseType();
            }
            if (at(MettleTypes.ASSIGN)) {
                b.advanceLexer();
                if (at(MettleTypes.STRING_LITERAL)) b.advanceLexer();
                else b.error("Expected a quoted link name");
            }
            expectStatementEnd();
            m.done(MettleTypes.VAR_DECL);
            return;
        }
        b.error("Expected 'fn' or 'var' after 'extern'");
        skipToDeclarationBoundary();
        m.drop();
    }

    private void parseStructRest(PsiBuilder.Marker m) {
        expectName("Expected a struct name");
        parseTypeParams();
        parseWhereClause();
        if (expect(MettleTypes.LBRACE, "Expected '{'")) {
            while (!b.eof() && !at(MettleTypes.RBRACE)) {
                int before = b.getCurrentOffset();
                parseStructMember();
                if (b.getCurrentOffset() == before) b.advanceLexer();
            }
            expect(MettleTypes.RBRACE, "Expected '}'");
        }
        m.done(MettleTypes.STRUCT_DECL);
    }

    private void parseStructMember() {
        if (at(MettleTypes.KW_METHOD)) {
            PsiBuilder.Marker m = b.mark();
            b.advanceLexer();
            parseFunctionRest(m, MettleTypes.METHOD_DECL);
            return;
        }
        if (at(MettleTypes.AT) || at(MettleTypes.KW_FN)) {
            PsiBuilder.Marker m = b.mark();
            parseDecorators();
            if (at(MettleTypes.KW_FN)) {
                b.advanceLexer();
                parseFunctionRest(m, MettleTypes.METHOD_DECL);
            } else {
                b.error("Expected 'fn'");
                m.drop();
            }
            return;
        }
        if (at(MettleTypes.IDENTIFIER)) {
            PsiBuilder.Marker m = b.mark();
            b.advanceLexer();
            if (expect(MettleTypes.COLON, "Expected ':'")) {
                parseType();
            }
            expectStatementEnd();
            m.done(MettleTypes.FIELD_DECL);
            return;
        }
        PsiBuilder.Marker err = b.mark();
        b.advanceLexer();
        err.error("Expected a field or method");
    }

    private void parseEnumRest(PsiBuilder.Marker m) {
        expectName("Expected an enum name");
        parseTypeParams();
        parseWhereClause();
        if (expect(MettleTypes.LBRACE, "Expected '{'")) {
            while (!b.eof() && !at(MettleTypes.RBRACE)) {
                int before = b.getCurrentOffset();
                if (at(MettleTypes.IDENTIFIER)) {
                    PsiBuilder.Marker member = b.mark();
                    b.advanceLexer();
                    if (at(MettleTypes.LPAREN)) {
                        b.advanceLexer();
                        while (!b.eof() && !at(MettleTypes.RPAREN)) {
                            int inner = b.getCurrentOffset();
                            parseType();
                            if (at(MettleTypes.COMMA)) b.advanceLexer();
                            if (b.getCurrentOffset() == inner) b.advanceLexer();
                        }
                        expect(MettleTypes.RPAREN, "Expected ')'");
                    } else if (at(MettleTypes.ASSIGN)) {
                        b.advanceLexer();
                        parseExpression();
                    }
                    member.done(MettleTypes.ENUM_MEMBER);
                } else {
                    PsiBuilder.Marker err = b.mark();
                    b.advanceLexer();
                    err.error("Expected an enum member");
                }
                if (at(MettleTypes.COMMA) || at(MettleTypes.SEMICOLON)) b.advanceLexer();
                if (b.getCurrentOffset() == before) b.advanceLexer();
            }
            expect(MettleTypes.RBRACE, "Expected '}'");
        }
        m.done(MettleTypes.ENUM_DECL);
    }

    private void parseTraitRest(PsiBuilder.Marker m) {
        expectName("Expected a trait name");
        parseTypeParams();
        parseWhereClause();
        if (at(MettleTypes.LBRACE)) {
            b.advanceLexer();
            while (!b.eof() && !at(MettleTypes.RBRACE)) {
                int before = b.getCurrentOffset();
                if (at(MettleTypes.KW_FN) || at(MettleTypes.AT)) {
                    PsiBuilder.Marker fn = b.mark();
                    parseDecorators();
                    if (at(MettleTypes.KW_FN)) {
                        b.advanceLexer();
                        parseFunctionRest(fn, MettleTypes.FUNCTION_DECL);
                    } else {
                        fn.drop();
                        b.advanceLexer();
                    }
                } else {
                    PsiBuilder.Marker err = b.mark();
                    b.advanceLexer();
                    err.error("Expected a trait method");
                }
                if (b.getCurrentOffset() == before) b.advanceLexer();
            }
            expect(MettleTypes.RBRACE, "Expected '}'");
        } else {
            expectStatementEnd();
        }
        m.done(MettleTypes.TRAIT_DECL);
    }

    private void parseImplRest(PsiBuilder.Marker m) {
        parseType();
        if (at(MettleTypes.KW_FOR)) {
            b.advanceLexer();
            parseType();
        }
        parseWhereClause();
        if (at(MettleTypes.LBRACE)) {
            b.advanceLexer();
            while (!b.eof() && !at(MettleTypes.RBRACE)) {
                int before = b.getCurrentOffset();
                if (at(MettleTypes.KW_FN) || at(MettleTypes.AT) || at(MettleTypes.KW_METHOD)) {
                    PsiBuilder.Marker fn = b.mark();
                    parseDecorators();
                    if (at(MettleTypes.KW_FN) || at(MettleTypes.KW_METHOD)) {
                        b.advanceLexer();
                        parseFunctionRest(fn, MettleTypes.METHOD_DECL);
                    } else {
                        fn.drop();
                        b.advanceLexer();
                    }
                } else {
                    PsiBuilder.Marker err = b.mark();
                    b.advanceLexer();
                    err.error("Expected an impl method");
                }
                if (b.getCurrentOffset() == before) b.advanceLexer();
            }
            expect(MettleTypes.RBRACE, "Expected '}'");
        } else {
            expectStatementEnd();
        }
        m.done(MettleTypes.IMPL_DECL);
    }

    /** {@code var} / {@code const} / {@code workgroup var} / {@code private var}. */
    private void parseVarLike(PsiBuilder.Marker m) {
        boolean isConst = at(MettleTypes.KW_CONST);
        if (at(MettleTypes.KW_WORKGROUP) || at(MettleTypes.KW_PRIVATE)) {
            b.advanceLexer();
        }
        if (at(MettleTypes.KW_VAR) || at(MettleTypes.KW_CONST)) {
            isConst = at(MettleTypes.KW_CONST);
            b.advanceLexer();
        }
        expectName("Expected a name");
        if (at(MettleTypes.COLON)) {
            b.advanceLexer();
            parseType();
        }
        if (at(MettleTypes.ASSIGN)) {
            b.advanceLexer();
            parseExpression();
        }
        expectStatementEnd();
        m.done(isConst ? MettleTypes.CONST_DECL : MettleTypes.VAR_DECL);
    }

    private void parseTypeParams() {
        if (!at(MettleTypes.LT)) return;
        PsiBuilder.Marker m = b.mark();
        b.advanceLexer();
        while (!b.eof() && !at(MettleTypes.GT)) {
            int before = b.getCurrentOffset();
            if (at(MettleTypes.IDENTIFIER)) {
                PsiBuilder.Marker p = b.mark();
                b.advanceLexer();
                if (at(MettleTypes.COLON)) {
                    b.advanceLexer();
                    parseTypeName();
                    while (at(MettleTypes.PLUS)) {
                        b.advanceLexer();
                        parseTypeName();
                    }
                }
                p.done(MettleTypes.TYPE_PARAM);
            } else if (MettleTypes.BUILTIN_TYPES.contains(current())) {
                b.advanceLexer();
            } else {
                b.advanceLexer();
            }
            if (at(MettleTypes.COMMA)) b.advanceLexer();
            if (b.getCurrentOffset() == before) b.advanceLexer();
        }
        expect(MettleTypes.GT, "Expected '>'");
        m.done(MettleTypes.TYPE_PARAM_LIST);
    }

    private void parseParamList() {
        if (!at(MettleTypes.LPAREN)) {
            b.error("Expected '('");
            return;
        }
        PsiBuilder.Marker m = b.mark();
        b.advanceLexer();
        while (!b.eof() && !at(MettleTypes.RPAREN)) {
            int before = b.getCurrentOffset();
            if (at(MettleTypes.IDENTIFIER)) {
                PsiBuilder.Marker p = b.mark();
                b.advanceLexer();
                if (at(MettleTypes.COLON)) {
                    b.advanceLexer();
                    parseType();
                }
                p.done(MettleTypes.PARAM_DECL);
            } else if (TYPE_START.contains(current())) {
                parseType();
            } else {
                PsiBuilder.Marker err = b.mark();
                b.advanceLexer();
                err.error("Expected a parameter");
            }
            if (at(MettleTypes.COMMA)) b.advanceLexer();
            if (b.getCurrentOffset() == before) b.advanceLexer();
        }
        expect(MettleTypes.RPAREN, "Expected ')'");
        m.done(MettleTypes.PARAM_LIST);
    }

    private void parseReturnType() {
        if (at(MettleTypes.ARROW) || at(MettleTypes.COLON)) {
            b.advanceLexer();
            parseType();
        }
    }

    /** {@code where T: Addable + SignedNumber, U: Copyable} after a signature. */
    private void parseWhereClause() {
        if (!at(MettleTypes.KW_WHERE)) return;
        b.advanceLexer();
        while (!b.eof()) {
            int before = b.getCurrentOffset();
            if (!at(MettleTypes.IDENTIFIER)) break;
            b.advanceLexer();
            if (at(MettleTypes.COLON)) {
                b.advanceLexer();
                parseTypeName();
                while (at(MettleTypes.PLUS)) {
                    b.advanceLexer();
                    parseTypeName();
                }
            }
            if (at(MettleTypes.COMMA)) b.advanceLexer();
            else break;
            if (b.getCurrentOffset() == before) break;
        }
    }

    // ---------------------------------------------------------------- types

    private void parseType() {
        PsiBuilder.Marker m = b.mark();
        if (at(MettleTypes.KW_FN)) {
            // fn(int32, int32) -> int32: a thin function pointer
            b.advanceLexer();
            parseFunctionTypeTail();
        } else if (MettleTypes.BUILTIN_TYPES.contains(current()) || at(MettleTypes.IDENTIFIER)) {
            // Fn(int32) -> int32 is the closure type constructor, an ordinary identifier
            boolean closure = at(MettleTypes.IDENTIFIER)
                    && "Fn".contentEquals(nullToEmpty(b.getTokenText()));
            b.advanceLexer();
            parseQualifier();
            parseTypeArgs();
            if (closure && at(MettleTypes.LPAREN)) parseFunctionTypeTail();
        } else {
            b.error("Expected a type");
            m.drop();
            return;
        }
        parseTypeSuffixes();
        m.done(MettleTypes.TYPE_REF);
    }

    /** The {@code (param types) -> return} tail shared by {@code fn} and {@code Fn} types. */
    private void parseFunctionTypeTail() {
        if (at(MettleTypes.LPAREN)) {
            b.advanceLexer();
            while (!b.eof() && !at(MettleTypes.RPAREN)) {
                int before = b.getCurrentOffset();
                parseType();
                if (at(MettleTypes.COMMA)) b.advanceLexer();
                if (b.getCurrentOffset() == before) b.advanceLexer();
            }
            expect(MettleTypes.RPAREN, "Expected ')'");
        }
        if (at(MettleTypes.ARROW)) {
            b.advanceLexer();
            parseType();
        }
    }

    /** A namespaced import exposes its types as {@code alias.Name}. */
    private void parseQualifier() {
        while (at(MettleTypes.DOT) && b.lookAhead(1) == MettleTypes.IDENTIFIER) {
            b.advanceLexer();
            b.advanceLexer();
        }
    }

    /** A bare type name, used for trait bounds. */
    private void parseTypeName() {
        PsiBuilder.Marker m = b.mark();
        if (at(MettleTypes.IDENTIFIER) || MettleTypes.BUILTIN_TYPES.contains(current())) {
            b.advanceLexer();
            parseQualifier();
            parseTypeArgs();
            m.done(MettleTypes.TYPE_REF);
        } else {
            b.error("Expected a type name");
            m.drop();
        }
    }

    private void parseTypeArgs() {
        if (!at(MettleTypes.LT)) return;
        b.advanceLexer();
        while (!b.eof() && !at(MettleTypes.GT) && !at(MettleTypes.SHR) && pendingAngleClose == 0) {
            int before = b.getCurrentOffset();
            parseType();
            if (at(MettleTypes.COMMA)) b.advanceLexer();
            if (b.getCurrentOffset() == before) b.advanceLexer();
        }
        closeTypeArgs();
    }

    /**
     * Closes one {@code <...>} level. Nested arguments end in {@code >>}, which the lexer sees as a
     * shift; consuming it closes this level and leaves the outer one already closed.
     */
    private void closeTypeArgs() {
        if (pendingAngleClose > 0) {
            pendingAngleClose--;
            return;
        }
        if (at(MettleTypes.SHR)) {
            b.advanceLexer();
            pendingAngleClose = 1;
            return;
        }
        expect(MettleTypes.GT, "Expected '>'");
    }

    private void parseTypeSuffixes() {
        while (!b.eof()) {
            if (at(MettleTypes.STAR)) {
                b.advanceLexer();
            } else if (at(MettleTypes.LBRACKET)) {
                b.advanceLexer();
                if (!at(MettleTypes.RBRACKET)) parseExpression();
                expect(MettleTypes.RBRACKET, "Expected ']'");
            } else {
                return;
            }
        }
    }

    /** Token-level type matcher used for cast / generic-call lookahead. Never reports errors. */
    private boolean matchTypeTokens() {
        if (at(MettleTypes.KW_FN)) {
            b.advanceLexer();
            if (!at(MettleTypes.LPAREN)) return false;
            if (!skipBalancedQuiet(MettleTypes.LPAREN, MettleTypes.RPAREN)) return false;
            if (at(MettleTypes.ARROW)) {
                b.advanceLexer();
                if (!matchTypeTokens()) return false;
            }
            return true;
        }
        if (MettleTypes.BUILTIN_TYPES.contains(current())) {
            b.advanceLexer();
        } else if (at(MettleTypes.IDENTIFIER)) {
            b.advanceLexer();
            while (at(MettleTypes.DOT) && b.lookAhead(1) == MettleTypes.IDENTIFIER) {
                b.advanceLexer();
                b.advanceLexer();
            }
            if (at(MettleTypes.LPAREN)) {
                // Fn(int32) -> int32
                if (!skipBalancedQuiet(MettleTypes.LPAREN, MettleTypes.RPAREN)) return false;
                if (at(MettleTypes.ARROW)) {
                    b.advanceLexer();
                    if (!matchTypeTokens()) return false;
                }
                return true;
            }
            if (at(MettleTypes.LT)) {
                b.advanceLexer();
                while (!b.eof() && !at(MettleTypes.GT) && !at(MettleTypes.SHR)) {
                    if (!matchTypeTokens()) return false;
                    if (at(MettleTypes.COMMA)) b.advanceLexer();
                    else break;
                }
                if (!at(MettleTypes.GT) && !at(MettleTypes.SHR)) return false;
                b.advanceLexer();
            }
        } else {
            return false;
        }
        while (at(MettleTypes.STAR)) b.advanceLexer();
        while (at(MettleTypes.LBRACKET)) {
            b.advanceLexer();
            while (!b.eof() && !at(MettleTypes.RBRACKET)) b.advanceLexer();
            if (!at(MettleTypes.RBRACKET)) return false;
            b.advanceLexer();
        }
        return true;
    }

    // ----------------------------------------------------------- statements

    private void parseBlock() {
        PsiBuilder.Marker m = b.mark();
        expect(MettleTypes.LBRACE, "Expected '{'");
        while (!b.eof() && !at(MettleTypes.RBRACE)) {
            int before = b.getCurrentOffset();
            parseStatement();
            if (b.getCurrentOffset() == before) b.advanceLexer();
        }
        expect(MettleTypes.RBRACE, "Expected '}'");
        m.done(MettleTypes.BLOCK);
    }

    private void parseStatement() {
        IElementType t = current();

        if (t == MettleTypes.SEMICOLON) {
            b.advanceLexer();
            return;
        }
        if (t == MettleTypes.LBRACE) {
            parseBlock();
            return;
        }
        if (t == MettleTypes.AT) {
            PsiBuilder.Marker m = b.mark();
            parseDecorators();
            parseStatement();
            m.drop();
            return;
        }
        if (t == MettleTypes.KW_VAR || t == MettleTypes.KW_CONST
                || t == MettleTypes.KW_WORKGROUP || t == MettleTypes.KW_PRIVATE) {
            parseVarLike(b.mark());
            return;
        }
        if (t == MettleTypes.KW_IF) {
            PsiBuilder.Marker m = b.mark();
            b.advanceLexer();
            parseParenCondition();
            parseStatement();
            if (at(MettleTypes.KW_ELSE)) {
                b.advanceLexer();
                parseStatement();
            }
            m.done(MettleTypes.IF_STMT);
            return;
        }
        if (t == MettleTypes.KW_WHILE) {
            PsiBuilder.Marker m = b.mark();
            b.advanceLexer();
            parseParenCondition();
            parseStatement();
            m.done(MettleTypes.WHILE_STMT);
            return;
        }
        if (t == MettleTypes.KW_FOR) {
            parseFor();
            return;
        }
        if (t == MettleTypes.KW_SWITCH || t == MettleTypes.KW_MATCH) {
            parseSwitchLike(t == MettleTypes.KW_MATCH);
            return;
        }
        if (t == MettleTypes.KW_RETURN) {
            PsiBuilder.Marker m = b.mark();
            b.advanceLexer();
            if (!at(MettleTypes.SEMICOLON)) parseExpression();
            expectStatementEnd();
            m.done(MettleTypes.RETURN_STMT);
            return;
        }
        if (t == MettleTypes.KW_BREAK || t == MettleTypes.KW_CONTINUE) {
            PsiBuilder.Marker m = b.mark();
            b.advanceLexer();
            if (at(MettleTypes.IDENTIFIER)) b.advanceLexer();
            expectStatementEnd();
            m.done(t == MettleTypes.KW_BREAK ? MettleTypes.BREAK_STMT : MettleTypes.CONTINUE_STMT);
            return;
        }
        if (t == MettleTypes.KW_DEFER || t == MettleTypes.KW_ERRDEFER) {
            PsiBuilder.Marker m = b.mark();
            b.advanceLexer();
            parseStatement();
            m.done(MettleTypes.DEFER_STMT);
            return;
        }
        if (t == MettleTypes.KW_DISPATCH) {
            parseDispatch();
            return;
        }
        if (t == MettleTypes.KW_ASM) {
            PsiBuilder.Marker m = b.mark();
            b.advanceLexer();
            skipBalancedBraces();
            m.done(MettleTypes.ASM_BLOCK);
            return;
        }
        // label: while / for
        if (t == MettleTypes.IDENTIFIER && b.lookAhead(1) == MettleTypes.COLON) {
            IElementType after = b.lookAhead(2);
            if (after == MettleTypes.KW_WHILE || after == MettleTypes.KW_FOR) {
                b.advanceLexer();
                b.advanceLexer();
                parseStatement();
                return;
            }
        }
        if (EXPR_START.contains(t)) {
            PsiBuilder.Marker m = b.mark();
            parseExpression();
            if (ASSIGN_OPS.contains(current())) {
                b.advanceLexer();
                parseExpression();
                expectStatementEnd();
                m.done(MettleTypes.ASSIGN_STMT);
            } else {
                expectStatementEnd();
                m.done(MettleTypes.EXPR_STMT);
            }
            return;
        }
        PsiBuilder.Marker err = b.mark();
        b.advanceLexer();
        err.error("Expected a statement");
        skipToStatementBoundary();
    }

    private void parseParenCondition() {
        if (expect(MettleTypes.LPAREN, "Expected '('")) {
            parseExpression();
            expect(MettleTypes.RPAREN, "Expected ')'");
        }
    }

    private void parseFor() {
        PsiBuilder.Marker m = b.mark();
        b.advanceLexer();
        if (at(MettleTypes.LPAREN)) {
            b.advanceLexer();
            if (!at(MettleTypes.SEMICOLON)) parseForClause();
            expectStatementEnd();
            if (!at(MettleTypes.SEMICOLON)) parseExpression();
            expectStatementEnd();
            if (!at(MettleTypes.RPAREN)) parseForClause();
            expect(MettleTypes.RPAREN, "Expected ')'");
        } else {
            // for i in lo..hi, or for i: int64 in lo..hi
            if (at(MettleTypes.IDENTIFIER)) {
                PsiBuilder.Marker v = b.mark();
                b.advanceLexer();
                if (at(MettleTypes.COLON)) {
                    b.advanceLexer();
                    parseType();
                }
                v.done(MettleTypes.VAR_DECL);
            } else {
                b.error("Expected a loop variable");
            }
            if (at(MettleTypes.IDENTIFIER) && "in".contentEquals(nullToEmpty(b.getTokenText()))) {
                b.advanceLexer();
            } else {
                b.error("Expected 'in'");
            }
            parseExpression();
        }
        parseStatement();
        m.done(MettleTypes.FOR_STMT);
    }

    /** A for-loop init or step: a declaration, an assignment, or a bare expression. */
    private void parseForClause() {
        if (at(MettleTypes.KW_VAR) || at(MettleTypes.KW_CONST)) {
            PsiBuilder.Marker m = b.mark();
            b.advanceLexer();
            expectName("Expected a name");
            if (at(MettleTypes.COLON)) {
                b.advanceLexer();
                parseType();
            }
            if (at(MettleTypes.ASSIGN)) {
                b.advanceLexer();
                parseExpression();
            }
            m.done(MettleTypes.VAR_DECL);
            return;
        }
        PsiBuilder.Marker m = b.mark();
        parseExpression();
        if (ASSIGN_OPS.contains(current())) {
            b.advanceLexer();
            parseExpression();
            m.done(MettleTypes.ASSIGN_STMT);
        } else {
            m.done(MettleTypes.EXPR_STMT);
        }
    }

    private void parseSwitchLike(boolean isMatch) {
        PsiBuilder.Marker m = b.mark();
        b.advanceLexer();
        parseParenCondition();
        if (expect(MettleTypes.LBRACE, "Expected '{'")) {
            while (!b.eof() && !at(MettleTypes.RBRACE)) {
                int before = b.getCurrentOffset();
                if (at(MettleTypes.KW_CASE) || at(MettleTypes.KW_DEFAULT)) {
                    PsiBuilder.Marker c = b.mark();
                    boolean isDefault = at(MettleTypes.KW_DEFAULT);
                    b.advanceLexer();
                    if (!isDefault) {
                        if (isMatch && at(MettleTypes.IDENTIFIER)) {
                            b.advanceLexer();
                            if (at(MettleTypes.LPAREN)) {
                                b.advanceLexer();
                                if (at(MettleTypes.IDENTIFIER)) {
                                    PsiBuilder.Marker binding = b.mark();
                                    b.advanceLexer();
                                    binding.done(MettleTypes.VAR_DECL);
                                }
                                expect(MettleTypes.RPAREN, "Expected ')'");
                            }
                        } else {
                            parseExpression();
                        }
                    }
                    expect(MettleTypes.COLON, "Expected ':'");
                    parseCaseBody();
                    c.done(MettleTypes.CASE_CLAUSE);
                } else {
                    PsiBuilder.Marker err = b.mark();
                    b.advanceLexer();
                    err.error("Expected 'case' or 'default'");
                }
                if (b.getCurrentOffset() == before) b.advanceLexer();
            }
            expect(MettleTypes.RBRACE, "Expected '}'");
        }
        m.done(isMatch ? MettleTypes.MATCH_STMT : MettleTypes.SWITCH_STMT);
    }

    /**
     * The body of one {@code case} / {@code default} arm.
     *
     * <p>Statement arms run until the next arm; an expression arm - the {@code match} expression
     * form - is a single value with no semicolon, so a parsed expression that is not followed by
     * {@code ;} or an assignment operator ends the arm.
     */
    private void parseCaseBody() {
        while (!b.eof() && !at(MettleTypes.RBRACE)
                && !at(MettleTypes.KW_CASE) && !at(MettleTypes.KW_DEFAULT)) {
            int before = b.getCurrentOffset();
            if (at(MettleTypes.LBRACE) || STATEMENT_KEYWORDS.contains(current())
                    || !EXPR_START.contains(current())) {
                parseStatement();
            } else {
                PsiBuilder.Marker m = b.mark();
                parseExpression();
                if (ASSIGN_OPS.contains(current())) {
                    b.advanceLexer();
                    parseExpression();
                    expectStatementEnd();
                    m.done(MettleTypes.ASSIGN_STMT);
                } else if (at(MettleTypes.SEMICOLON)) {
                    b.advanceLexer();
                    m.done(MettleTypes.EXPR_STMT);
                } else {
                    m.done(MettleTypes.EXPR_STMT);
                    if (at(MettleTypes.COMMA)) b.advanceLexer();
                    return;
                }
            }
            if (at(MettleTypes.COMMA)) b.advanceLexer();
            if (b.getCurrentOffset() == before) b.advanceLexer();
        }
    }

    /** {@code dispatch name[launch controls](args);} - the controls are skipped verbatim. */
    private void parseDispatch() {
        PsiBuilder.Marker m = b.mark();
        b.advanceLexer();
        if (at(MettleTypes.IDENTIFIER)) {
            PsiBuilder.Marker ref = b.mark();
            b.advanceLexer();
            ref.done(MettleTypes.REF_EXPR);
        }
        if (at(MettleTypes.LBRACKET)) skipBalanced(MettleTypes.LBRACKET, MettleTypes.RBRACKET);
        if (at(MettleTypes.LPAREN)) parseArgList();
        expectStatementEnd();
        m.done(MettleTypes.DISPATCH_STMT);
    }

    // ---------------------------------------------------------- expressions

    private void parseExpression() {
        parseRange();
    }

    private void parseRange() {
        PsiBuilder.Marker m = b.mark();
        parseBinary(0);
        if (at(MettleTypes.DOTDOT) || at(MettleTypes.DOTDOTEQ)) {
            b.advanceLexer();
            parseBinary(0);
            m.done(MettleTypes.RANGE_EXPR);
        } else {
            m.drop();
        }
    }

    private static final TokenSet[] PRECEDENCE = {
            TokenSet.create(MettleTypes.OROR),
            TokenSet.create(MettleTypes.ANDAND),
            TokenSet.create(MettleTypes.PIPE),
            TokenSet.create(MettleTypes.CARET),
            TokenSet.create(MettleTypes.AMP),
            TokenSet.create(MettleTypes.EQ, MettleTypes.NE),
            TokenSet.create(MettleTypes.LT, MettleTypes.GT, MettleTypes.LE, MettleTypes.GE),
            TokenSet.create(MettleTypes.SHL, MettleTypes.SHR),
            TokenSet.create(MettleTypes.PLUS, MettleTypes.MINUS),
            TokenSet.create(MettleTypes.STAR, MettleTypes.SLASH, MettleTypes.PERCENT),
    };

    private void parseBinary(int level) {
        if (level >= PRECEDENCE.length) {
            parseUnary();
            return;
        }
        PsiBuilder.Marker m = b.mark();
        parseBinary(level + 1);
        boolean folded = false;
        while (PRECEDENCE[level].contains(current())) {
            b.advanceLexer();
            parseBinary(level + 1);
            folded = true;
        }
        if (folded) m.done(MettleTypes.BINARY_EXPR);
        else m.drop();
    }

    private void parseUnary() {
        IElementType t = current();
        if (t == MettleTypes.MINUS || t == MettleTypes.PLUS || t == MettleTypes.BANG
                || t == MettleTypes.TILDE || t == MettleTypes.STAR || t == MettleTypes.AMP) {
            PsiBuilder.Marker m = b.mark();
            b.advanceLexer();
            parseUnary();
            m.done(MettleTypes.UNARY_EXPR);
            return;
        }
        if (t == MettleTypes.KW_NEW) {
            PsiBuilder.Marker m = b.mark();
            b.advanceLexer();
            parseType();
            if (at(MettleTypes.LBRACKET)) {
                b.advanceLexer();
                parseExpression();
                expect(MettleTypes.RBRACKET, "Expected ']'");
            }
            m.done(MettleTypes.NEW_EXPR);
            return;
        }
        if (t == MettleTypes.KW_IMPORT_STR) {
            PsiBuilder.Marker m = b.mark();
            b.advanceLexer();
            if (at(MettleTypes.STRING_LITERAL)) b.advanceLexer();
            else b.error("Expected a quoted path");
            m.done(MettleTypes.LITERAL_EXPR);
            return;
        }
        if (t == MettleTypes.LPAREN && looksLikeCast()) {
            PsiBuilder.Marker m = b.mark();
            b.advanceLexer();
            parseType();
            expect(MettleTypes.RPAREN, "Expected ')'");
            parseUnary();
            m.done(MettleTypes.CAST_EXPR);
            return;
        }
        parsePostfix();
    }

    private boolean looksLikeCast() {
        PsiBuilder.Marker probe = b.mark();
        b.advanceLexer();
        boolean cast = matchTypeTokens() && at(MettleTypes.RPAREN);
        if (cast) {
            b.advanceLexer();
            cast = EXPR_START.contains(current());
        }
        probe.rollbackTo();
        return cast;
    }

    private void parsePostfix() {
        PsiBuilder.Marker m = b.mark();
        parsePrimary();
        while (!b.eof()) {
            IElementType t = current();
            if (t == MettleTypes.LPAREN) {
                parseArgList();
                m.done(MettleTypes.CALL_EXPR);
                m = m.precede();
            } else if (t == MettleTypes.LBRACKET) {
                b.advanceLexer();
                if (!at(MettleTypes.RBRACKET)) parseExpression();
                expect(MettleTypes.RBRACKET, "Expected ']'");
                m.done(MettleTypes.INDEX_EXPR);
                m = m.precede();
            } else if (t == MettleTypes.DOT || t == MettleTypes.ARROW) {
                b.advanceLexer();
                if (at(MettleTypes.IDENTIFIER) || MettleTypes.KEYWORD_TOKENS.contains(current())) {
                    b.advanceLexer();
                } else {
                    b.error("Expected a member name");
                }
                m.done(MettleTypes.FIELD_EXPR);
                m = m.precede();
            } else if (t == MettleTypes.LT && looksLikeGenericCall()) {
                b.advanceLexer();
                while (!b.eof() && !at(MettleTypes.GT)) {
                    int before = b.getCurrentOffset();
                    parseType();
                    if (at(MettleTypes.COMMA)) b.advanceLexer();
                    if (b.getCurrentOffset() == before) b.advanceLexer();
                }
                expect(MettleTypes.GT, "Expected '>'");
                parseArgList();
                m.done(MettleTypes.CALL_EXPR);
                m = m.precede();
            } else {
                break;
            }
        }
        m.drop();
    }

    private boolean looksLikeGenericCall() {
        PsiBuilder.Marker probe = b.mark();
        b.advanceLexer();
        boolean ok = true;
        while (ok && !b.eof() && !at(MettleTypes.GT)) {
            ok = matchTypeTokens();
            if (ok && at(MettleTypes.COMMA)) b.advanceLexer();
            else break;
        }
        ok = ok && at(MettleTypes.GT);
        if (ok) {
            b.advanceLexer();
            ok = at(MettleTypes.LPAREN);
        }
        probe.rollbackTo();
        return ok;
    }

    private void parseArgList() {
        PsiBuilder.Marker m = b.mark();
        expect(MettleTypes.LPAREN, "Expected '('");
        while (!b.eof() && !at(MettleTypes.RPAREN)) {
            int before = b.getCurrentOffset();
            // named argument: `order: acq_rel`, `shape: m16n16k16`
            if (at(MettleTypes.IDENTIFIER) && b.lookAhead(1) == MettleTypes.COLON) {
                b.advanceLexer();
                b.advanceLexer();
            }
            if (EXPR_START.contains(current())) {
                parseExpression();
            } else if (!at(MettleTypes.COMMA) && !at(MettleTypes.RPAREN)) {
                PsiBuilder.Marker err = b.mark();
                b.advanceLexer();
                err.error("Expected an argument");
            }
            if (at(MettleTypes.COMMA)) b.advanceLexer();
            if (b.getCurrentOffset() == before) b.advanceLexer();
        }
        expect(MettleTypes.RPAREN, "Expected ')'");
        m.done(MettleTypes.ARG_LIST);
    }

    private void parsePrimary() {
        IElementType t = current();
        if (MettleTypes.LITERALS.contains(t)) {
            PsiBuilder.Marker m = b.mark();
            b.advanceLexer();
            m.done(MettleTypes.LITERAL_EXPR);
            return;
        }
        if (t == MettleTypes.IDENTIFIER || t == MettleTypes.KW_THIS
                || MettleTypes.BUILTIN_TYPES.contains(t)
                || t == MettleTypes.KW_WORKGROUP || t == MettleTypes.KW_PRIVATE
                || t == MettleTypes.KW_BARRIER) {
            PsiBuilder.Marker m = b.mark();
            b.advanceLexer();
            m.done(MettleTypes.REF_EXPR);
            return;
        }
        if (t == MettleTypes.LPAREN) {
            PsiBuilder.Marker m = b.mark();
            b.advanceLexer();
            if (!at(MettleTypes.RPAREN)) parseExpression();
            expect(MettleTypes.RPAREN, "Expected ')'");
            m.done(MettleTypes.PAREN_EXPR);
            return;
        }
        if (t == MettleTypes.KW_FN) {
            // Anonymous function: fn(x: int32) -> int32 { ... }
            PsiBuilder.Marker m = b.mark();
            b.advanceLexer();
            parseParamList();
            parseReturnType();
            if (at(MettleTypes.LBRACE)) parseBlock();
            else b.error("Expected the lambda body");
            m.done(MettleTypes.LAMBDA_EXPR);
            return;
        }
        if (t == MettleTypes.LBRACKET || t == MettleTypes.LBRACE) {
            parseAggregate();
            return;
        }
        if (t == MettleTypes.KW_MATCH) {
            parseSwitchLike(true);
            return;
        }
        PsiBuilder.Marker err = b.mark();
        if (!b.eof()) b.advanceLexer();
        err.error("Expected an expression");
    }

    /** {@code [1, 2, 3]}, {@code [0; 64]} and {@code { x: 1.0, y: 2.0 }}. */
    private void parseAggregate() {
        boolean brace = at(MettleTypes.LBRACE);
        IElementType close = brace ? MettleTypes.RBRACE : MettleTypes.RBRACKET;
        PsiBuilder.Marker m = b.mark();
        b.advanceLexer();
        while (!b.eof() && !at(close)) {
            int before = b.getCurrentOffset();
            if (brace && at(MettleTypes.IDENTIFIER) && b.lookAhead(1) == MettleTypes.COLON) {
                b.advanceLexer();
                b.advanceLexer();
            }
            if (EXPR_START.contains(current())) {
                parseExpression();
            } else if (!at(MettleTypes.COMMA) && !at(MettleTypes.SEMICOLON)) {
                PsiBuilder.Marker err = b.mark();
                b.advanceLexer();
                err.error("Expected a value");
            }
            if (at(MettleTypes.COMMA) || at(MettleTypes.SEMICOLON)) b.advanceLexer();
            if (b.getCurrentOffset() == before) b.advanceLexer();
        }
        expect(close, brace ? "Expected '}'" : "Expected ']'");
        m.done(MettleTypes.AGGREGATE_EXPR);
    }

    // -------------------------------------------------------------- helpers

    private IElementType current() {
        return b.getTokenType();
    }

    private boolean at(IElementType type) {
        return b.getTokenType() == type;
    }

    private boolean expect(IElementType type, String message) {
        if (at(type)) {
            b.advanceLexer();
            return true;
        }
        b.error(message);
        return false;
    }

    /**
     * A statement ends at a semicolon or a newline (see {@code parser_expect_statement_end} in the
     * compiler). Newlines are whitespace to this lexer, so the check looks at the raw text between
     * the previous token and this one.
     */
    private void expectStatementEnd() {
        if (at(MettleTypes.SEMICOLON)) {
            b.advanceLexer();
            return;
        }
        if (b.eof() || at(MettleTypes.RBRACE) || precededByNewline()) return;
        b.error("Expected ';' or a newline at the end of the statement");
    }

    private boolean precededByNewline() {
        CharSequence text = b.getOriginalText();
        for (int i = Math.min(b.getCurrentOffset(), text.length()) - 1; i >= 0; i--) {
            char c = text.charAt(i);
            if (c == '\n') return true;
            if (!Character.isWhitespace(c)) return false;
        }
        return true;
    }

    private void expectName(String message) {
        if (at(MettleTypes.IDENTIFIER)) b.advanceLexer();
        else b.error(message);
    }

    private void skipBalancedBraces() {
        if (at(MettleTypes.LBRACE)) skipBalanced(MettleTypes.LBRACE, MettleTypes.RBRACE);
    }

    /** Balanced skip for lookahead: false when the closer is missing. */
    private boolean skipBalancedQuiet(IElementType open, IElementType close) {
        int depth = 0;
        while (!b.eof()) {
            if (at(open)) depth++;
            else if (at(close)) depth--;
            b.advanceLexer();
            if (depth == 0) return true;
        }
        return false;
    }

    private void skipBalanced(IElementType open, IElementType close) {
        int depth = 0;
        while (!b.eof()) {
            if (at(open)) depth++;
            else if (at(close)) depth--;
            b.advanceLexer();
            if (depth == 0) return;
        }
    }

    private void skipToStatementBoundary() {
        while (!b.eof() && !at(MettleTypes.SEMICOLON) && !at(MettleTypes.RBRACE)
                && !STATEMENT_START.contains(current())) {
            b.advanceLexer();
        }
        if (at(MettleTypes.SEMICOLON)) b.advanceLexer();
    }

    private void skipToDeclarationBoundary() {
        while (!b.eof() && !DECL_START.contains(current())) {
            b.advanceLexer();
        }
    }

    private static CharSequence nullToEmpty(String value) {
        return value == null ? "" : value;
    }
}
