package org.mettle.clion.lang;

import com.intellij.lexer.LexerBase;
import com.intellij.psi.TokenType;
import com.intellij.psi.tree.IElementType;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

/**
 * Hand-written lexer for Mettle.
 *
 * <p>Every token is produced from a single scan starting at the token offset, so the lexer is
 * stateless and fully restartable: {@link #getState()} is always 0. Nested block comments and
 * multi-line strings are consumed inside one token, which is what keeps that true.
 */
public class MettleLexer extends LexerBase {
    private CharSequence buffer = "";
    private int endOffset;
    private int tokenStart;
    private int tokenEnd;
    private IElementType tokenType;

    @Override
    public void start(@NotNull CharSequence buffer, int startOffset, int endOffset, int initialState) {
        this.buffer = buffer;
        this.endOffset = endOffset;
        this.tokenStart = startOffset;
        this.tokenEnd = startOffset;
        advance();
    }

    @Override
    public int getState() {
        return 0;
    }

    @Override
    public @Nullable IElementType getTokenType() {
        return tokenType;
    }

    @Override
    public int getTokenStart() {
        return tokenStart;
    }

    @Override
    public int getTokenEnd() {
        return tokenEnd;
    }

    @Override
    public @NotNull CharSequence getBufferSequence() {
        return buffer;
    }

    @Override
    public int getBufferEnd() {
        return endOffset;
    }

    @Override
    public void advance() {
        tokenStart = tokenEnd;
        if (tokenStart >= endOffset) {
            tokenType = null;
            return;
        }
        char c = buffer.charAt(tokenStart);

        if (isWhitespace(c)) {
            int i = tokenStart;
            while (i < endOffset && isWhitespace(buffer.charAt(i))) i++;
            finish(TokenType.WHITE_SPACE, i);
            return;
        }
        if (c == '/' && peek(tokenStart + 1) == '/') {
            int i = tokenStart + 2;
            while (i < endOffset && buffer.charAt(i) != '\n') i++;
            finish(MettleTypes.LINE_COMMENT, i);
            return;
        }
        if (c == '/' && peek(tokenStart + 1) == '*') {
            finish(MettleTypes.BLOCK_COMMENT, scanBlockComment(tokenStart));
            return;
        }
        if (c == '"') {
            finish(MettleTypes.STRING_LITERAL, scanString(tokenStart));
            return;
        }
        if (c == '\'') {
            int end = scanChar(tokenStart);
            if (end < 0) {
                finish(TokenType.BAD_CHARACTER, tokenStart + 1);
            } else {
                finish(MettleTypes.CHAR_LITERAL, end);
            }
            return;
        }
        if (isIdentifierStart(c)) {
            int i = tokenStart + 1;
            while (i < endOffset && isIdentifierPart(buffer.charAt(i))) i++;
            String text = buffer.subSequence(tokenStart, i).toString();
            IElementType keyword = MettleTypes.KEYWORDS.get(text);
            finish(keyword != null ? keyword : MettleTypes.IDENTIFIER, i);
            return;
        }
        if (c >= '0' && c <= '9') {
            scanNumber(tokenStart);
            return;
        }
        scanOperator(tokenStart, c);
    }

    // ------------------------------------------------------------- scanners

    /** Block comments nest; an unterminated one runs to the end of the file. */
    private int scanBlockComment(int from) {
        int i = from + 2;
        int depth = 1;
        while (i < endOffset && depth > 0) {
            char c = buffer.charAt(i);
            if (c == '/' && peek(i + 1) == '*') {
                depth++;
                i += 2;
            } else if (c == '*' && peek(i + 1) == '/') {
                depth--;
                i += 2;
            } else {
                i++;
            }
        }
        return i;
    }

    /** Strings may span lines; the escape consumes whatever follows the backslash. */
    private int scanString(int from) {
        int i = from + 1;
        while (i < endOffset) {
            char c = buffer.charAt(i);
            if (c == '\\') {
                i += 2;
                continue;
            }
            i++;
            if (c == '"') return i;
        }
        return endOffset;
    }

    /** Returns the end offset of a well-formed character literal, or -1. */
    private int scanChar(int from) {
        int i = from + 1;
        if (i >= endOffset) return -1;
        char c = buffer.charAt(i);
        if (c == '\n' || c == '\'') return -1;
        i += (c == '\\') ? 2 : 1;
        if (i < endOffset && buffer.charAt(i) == '\'') return i + 1;
        return -1;
    }

    private void scanNumber(int from) {
        int i = from;
        char second = peek(from + 1);
        if (buffer.charAt(from) == '0' && (second == 'x' || second == 'X')) {
            i = from + 2;
            while (i < endOffset && isHexDigit(buffer.charAt(i))) i++;
            finish(MettleTypes.INT_LITERAL, i);
            return;
        }
        if (buffer.charAt(from) == '0' && (second == 'b' || second == 'B')) {
            i = from + 2;
            while (i < endOffset && (buffer.charAt(i) == '0' || buffer.charAt(i) == '1')) i++;
            finish(MettleTypes.INT_LITERAL, i);
            return;
        }

        boolean isFloat = false;
        while (i < endOffset && isDigit(buffer.charAt(i))) i++;
        // A '.' that starts a `..` range belongs to the range, not to the number.
        if (i < endOffset && buffer.charAt(i) == '.' && peek(i + 1) != '.') {
            isFloat = true;
            i++;
            while (i < endOffset && isDigit(buffer.charAt(i))) i++;
        }
        if (i < endOffset && (buffer.charAt(i) == 'e' || buffer.charAt(i) == 'E')) {
            int j = i + 1;
            if (j < endOffset && (buffer.charAt(j) == '+' || buffer.charAt(j) == '-')) j++;
            if (j < endOffset && isDigit(buffer.charAt(j))) {
                isFloat = true;
                i = j;
                while (i < endOffset && isDigit(buffer.charAt(i))) i++;
            }
        }
        finish(isFloat ? MettleTypes.FLOAT_LITERAL : MettleTypes.INT_LITERAL, i);
    }

    private void scanOperator(int from, char c) {
        char c1 = peek(from + 1);
        char c2 = peek(from + 2);
        switch (c) {
            case '.':
                if (c1 == '.' && c2 == '=') { finish(MettleTypes.DOTDOTEQ, from + 3); return; }
                if (c1 == '.') { finish(MettleTypes.DOTDOT, from + 2); return; }
                finish(MettleTypes.DOT, from + 1);
                return;
            case '<':
                if (c1 == '<' && c2 == '=') { finish(MettleTypes.SHL_EQ, from + 3); return; }
                if (c1 == '<') { finish(MettleTypes.SHL, from + 2); return; }
                if (c1 == '=') { finish(MettleTypes.LE, from + 2); return; }
                finish(MettleTypes.LT, from + 1);
                return;
            case '>':
                if (c1 == '>' && c2 == '=') { finish(MettleTypes.SHR_EQ, from + 3); return; }
                if (c1 == '>') { finish(MettleTypes.SHR, from + 2); return; }
                if (c1 == '=') { finish(MettleTypes.GE, from + 2); return; }
                finish(MettleTypes.GT, from + 1);
                return;
            case '-':
                if (c1 == '>') { finish(MettleTypes.ARROW, from + 2); return; }
                if (c1 == '=') { finish(MettleTypes.MINUS_EQ, from + 2); return; }
                finish(MettleTypes.MINUS, from + 1);
                return;
            case '+':
                finish(c1 == '=' ? MettleTypes.PLUS_EQ : MettleTypes.PLUS, from + (c1 == '=' ? 2 : 1));
                return;
            case '*':
                finish(c1 == '=' ? MettleTypes.STAR_EQ : MettleTypes.STAR, from + (c1 == '=' ? 2 : 1));
                return;
            case '/':
                finish(c1 == '=' ? MettleTypes.SLASH_EQ : MettleTypes.SLASH, from + (c1 == '=' ? 2 : 1));
                return;
            case '%':
                finish(c1 == '=' ? MettleTypes.PERCENT_EQ : MettleTypes.PERCENT, from + (c1 == '=' ? 2 : 1));
                return;
            case '^':
                finish(c1 == '=' ? MettleTypes.CARET_EQ : MettleTypes.CARET, from + (c1 == '=' ? 2 : 1));
                return;
            case '&':
                if (c1 == '&') { finish(MettleTypes.ANDAND, from + 2); return; }
                finish(c1 == '=' ? MettleTypes.AMP_EQ : MettleTypes.AMP, from + (c1 == '=' ? 2 : 1));
                return;
            case '|':
                if (c1 == '|') { finish(MettleTypes.OROR, from + 2); return; }
                finish(c1 == '=' ? MettleTypes.PIPE_EQ : MettleTypes.PIPE, from + (c1 == '=' ? 2 : 1));
                return;
            case '=':
                finish(c1 == '=' ? MettleTypes.EQ : MettleTypes.ASSIGN, from + (c1 == '=' ? 2 : 1));
                return;
            case '!':
                finish(c1 == '=' ? MettleTypes.NE : MettleTypes.BANG, from + (c1 == '=' ? 2 : 1));
                return;
            case '~': finish(MettleTypes.TILDE, from + 1); return;
            case ':': finish(MettleTypes.COLON, from + 1); return;
            case ';': finish(MettleTypes.SEMICOLON, from + 1); return;
            case ',': finish(MettleTypes.COMMA, from + 1); return;
            case '(': finish(MettleTypes.LPAREN, from + 1); return;
            case ')': finish(MettleTypes.RPAREN, from + 1); return;
            case '{': finish(MettleTypes.LBRACE, from + 1); return;
            case '}': finish(MettleTypes.RBRACE, from + 1); return;
            case '[': finish(MettleTypes.LBRACKET, from + 1); return;
            case ']': finish(MettleTypes.RBRACKET, from + 1); return;
            case '@': finish(MettleTypes.AT, from + 1); return;
            default: finish(TokenType.BAD_CHARACTER, from + 1);
        }
    }

    // -------------------------------------------------------------- helpers

    private void finish(IElementType type, int end) {
        tokenType = type;
        tokenEnd = Math.min(Math.max(end, tokenStart + 1), endOffset);
    }

    private char peek(int offset) {
        return offset < endOffset ? buffer.charAt(offset) : '\0';
    }

    private static boolean isWhitespace(char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == 11;
    }

    private static boolean isDigit(char c) {
        return c >= '0' && c <= '9';
    }

    private static boolean isHexDigit(char c) {
        return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    /** Identifiers are strictly ASCII, like the compiler's isalpha/isalnum. */
    private static boolean isIdentifierStart(char c) {
        return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }

    private static boolean isIdentifierPart(char c) {
        return isIdentifierStart(c) || isDigit(c);
    }
}
