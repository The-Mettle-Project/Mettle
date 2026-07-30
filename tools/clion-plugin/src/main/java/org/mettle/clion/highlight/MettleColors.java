package org.mettle.clion.highlight;

import com.intellij.openapi.editor.DefaultLanguageHighlighterColors;
import com.intellij.openapi.editor.colors.TextAttributesKey;

/** The colour keys the Mettle highlighter paints with. */
public final class MettleColors {

    public static final TextAttributesKey KEYWORD =
            key("METTLE_KEYWORD", DefaultLanguageHighlighterColors.KEYWORD);
    public static final TextAttributesKey BUILTIN_TYPE =
            key("METTLE_BUILTIN_TYPE", DefaultLanguageHighlighterColors.KEYWORD);
    public static final TextAttributesKey DECORATOR =
            key("METTLE_DECORATOR", DefaultLanguageHighlighterColors.METADATA);
    public static final TextAttributesKey NUMBER =
            key("METTLE_NUMBER", DefaultLanguageHighlighterColors.NUMBER);
    public static final TextAttributesKey STRING =
            key("METTLE_STRING", DefaultLanguageHighlighterColors.STRING);
    public static final TextAttributesKey CHARACTER =
            key("METTLE_CHARACTER", DefaultLanguageHighlighterColors.STRING);
    public static final TextAttributesKey LINE_COMMENT =
            key("METTLE_LINE_COMMENT", DefaultLanguageHighlighterColors.LINE_COMMENT);
    public static final TextAttributesKey BLOCK_COMMENT =
            key("METTLE_BLOCK_COMMENT", DefaultLanguageHighlighterColors.BLOCK_COMMENT);
    public static final TextAttributesKey OPERATOR =
            key("METTLE_OPERATOR", DefaultLanguageHighlighterColors.OPERATION_SIGN);
    public static final TextAttributesKey PARENTHESES =
            key("METTLE_PARENTHESES", DefaultLanguageHighlighterColors.PARENTHESES);
    public static final TextAttributesKey BRACES =
            key("METTLE_BRACES", DefaultLanguageHighlighterColors.BRACES);
    public static final TextAttributesKey BRACKETS =
            key("METTLE_BRACKETS", DefaultLanguageHighlighterColors.BRACKETS);
    public static final TextAttributesKey SEMICOLON =
            key("METTLE_SEMICOLON", DefaultLanguageHighlighterColors.SEMICOLON);
    public static final TextAttributesKey COMMA =
            key("METTLE_COMMA", DefaultLanguageHighlighterColors.COMMA);
    public static final TextAttributesKey DOT =
            key("METTLE_DOT", DefaultLanguageHighlighterColors.DOT);
    public static final TextAttributesKey IDENTIFIER =
            key("METTLE_IDENTIFIER", DefaultLanguageHighlighterColors.IDENTIFIER);
    public static final TextAttributesKey BAD_CHARACTER =
            key("METTLE_BAD_CHARACTER", DefaultLanguageHighlighterColors.INVALID_STRING_ESCAPE);

    // semantic, applied by the annotator
    public static final TextAttributesKey FUNCTION_DECLARATION =
            key("METTLE_FUNCTION_DECLARATION", DefaultLanguageHighlighterColors.FUNCTION_DECLARATION);
    public static final TextAttributesKey FUNCTION_CALL =
            key("METTLE_FUNCTION_CALL", DefaultLanguageHighlighterColors.FUNCTION_CALL);
    public static final TextAttributesKey TYPE_NAME =
            key("METTLE_TYPE_NAME", DefaultLanguageHighlighterColors.CLASS_NAME);
    public static final TextAttributesKey TYPE_PARAMETER =
            key("METTLE_TYPE_PARAMETER", DefaultLanguageHighlighterColors.PARAMETER);
    public static final TextAttributesKey FIELD =
            key("METTLE_FIELD", DefaultLanguageHighlighterColors.INSTANCE_FIELD);
    public static final TextAttributesKey PARAMETER =
            key("METTLE_PARAMETER", DefaultLanguageHighlighterColors.PARAMETER);
    public static final TextAttributesKey LOCAL_VARIABLE =
            key("METTLE_LOCAL_VARIABLE", DefaultLanguageHighlighterColors.LOCAL_VARIABLE);
    public static final TextAttributesKey GLOBAL_VARIABLE =
            key("METTLE_GLOBAL_VARIABLE", DefaultLanguageHighlighterColors.GLOBAL_VARIABLE);
    public static final TextAttributesKey CONSTANT =
            key("METTLE_CONSTANT", DefaultLanguageHighlighterColors.CONSTANT);
    public static final TextAttributesKey ENUM_VARIANT =
            key("METTLE_ENUM_VARIANT", DefaultLanguageHighlighterColors.STATIC_FIELD);
    public static final TextAttributesKey BUILTIN_NAME =
            key("METTLE_BUILTIN_NAME", DefaultLanguageHighlighterColors.PREDEFINED_SYMBOL);
    public static final TextAttributesKey GPU_BUILTIN =
            key("METTLE_GPU_BUILTIN", DefaultLanguageHighlighterColors.STATIC_METHOD);

    private MettleColors() {
    }

    private static TextAttributesKey key(String externalName, TextAttributesKey fallback) {
        return TextAttributesKey.createTextAttributesKey(externalName, fallback);
    }
}
