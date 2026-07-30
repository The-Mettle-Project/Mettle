package org.mettle.clion.highlight;

import com.intellij.lexer.Lexer;
import com.intellij.openapi.editor.colors.TextAttributesKey;
import com.intellij.openapi.fileTypes.SyntaxHighlighterBase;
import com.intellij.psi.TokenType;
import com.intellij.psi.tree.IElementType;
import org.jetbrains.annotations.NotNull;
import org.mettle.clion.lang.MettleLexer;
import org.mettle.clion.lang.MettleTypes;

public class MettleSyntaxHighlighter extends SyntaxHighlighterBase {

    @Override
    public @NotNull Lexer getHighlightingLexer() {
        return new MettleLexer();
    }

    @Override
    public TextAttributesKey @NotNull [] getTokenHighlights(IElementType tokenType) {
        if (MettleTypes.KEYWORD_TOKENS.contains(tokenType)) return pack(MettleColors.KEYWORD);
        if (MettleTypes.BUILTIN_TYPES.contains(tokenType)) return pack(MettleColors.BUILTIN_TYPE);
        if (tokenType == MettleTypes.LINE_COMMENT) return pack(MettleColors.LINE_COMMENT);
        if (tokenType == MettleTypes.BLOCK_COMMENT) return pack(MettleColors.BLOCK_COMMENT);
        if (tokenType == MettleTypes.STRING_LITERAL) return pack(MettleColors.STRING);
        if (tokenType == MettleTypes.CHAR_LITERAL) return pack(MettleColors.CHARACTER);
        if (tokenType == MettleTypes.INT_LITERAL || tokenType == MettleTypes.FLOAT_LITERAL) {
            return pack(MettleColors.NUMBER);
        }
        if (tokenType == MettleTypes.IDENTIFIER) return pack(MettleColors.IDENTIFIER);
        if (tokenType == MettleTypes.AT) return pack(MettleColors.DECORATOR);
        if (tokenType == MettleTypes.LPAREN || tokenType == MettleTypes.RPAREN) {
            return pack(MettleColors.PARENTHESES);
        }
        if (tokenType == MettleTypes.LBRACE || tokenType == MettleTypes.RBRACE) {
            return pack(MettleColors.BRACES);
        }
        if (tokenType == MettleTypes.LBRACKET || tokenType == MettleTypes.RBRACKET) {
            return pack(MettleColors.BRACKETS);
        }
        if (tokenType == MettleTypes.SEMICOLON) return pack(MettleColors.SEMICOLON);
        if (tokenType == MettleTypes.COMMA) return pack(MettleColors.COMMA);
        if (tokenType == MettleTypes.DOT) return pack(MettleColors.DOT);
        if (MettleTypes.OPERATORS.contains(tokenType)) return pack(MettleColors.OPERATOR);
        if (tokenType == TokenType.BAD_CHARACTER) return pack(MettleColors.BAD_CHARACTER);
        return TextAttributesKey.EMPTY_ARRAY;
    }
}
