package org.mettle.clion.editor;

import com.intellij.formatting.FormattingContext;
import com.intellij.formatting.FormattingModel;
import com.intellij.formatting.FormattingModelBuilder;
import com.intellij.formatting.FormattingModelProvider;
import com.intellij.formatting.SpacingBuilder;
import com.intellij.psi.codeStyle.CodeStyleSettings;
import org.jetbrains.annotations.NotNull;
import org.mettle.clion.lang.MettleLanguage;
import org.mettle.clion.lang.MettleTypes;

public class MettleFormattingModelBuilder implements FormattingModelBuilder {

    @Override
    public @NotNull FormattingModel createModel(@NotNull FormattingContext context) {
        CodeStyleSettings settings = context.getCodeStyleSettings();
        MettleBlock root = new MettleBlock(context.getNode(), null, spacingBuilder(settings));
        return FormattingModelProvider.createFormattingModelForPsiFile(
                context.getContainingFile(), root, settings);
    }

    static SpacingBuilder spacingBuilder(@NotNull CodeStyleSettings settings) {
        return new SpacingBuilder(settings, MettleLanguage.INSTANCE)
                // control flow: `if (`, `while (`, `return x`
                .after(MettleTypes.KW_IF).spaces(1)
                .after(MettleTypes.KW_WHILE).spaces(1)
                .after(MettleTypes.KW_FOR).spaces(1)
                .after(MettleTypes.KW_SWITCH).spaces(1)
                .after(MettleTypes.KW_MATCH).spaces(1)
                .after(MettleTypes.KW_RETURN).spaces(1)
                .after(MettleTypes.KW_ELSE).spaces(1)
                .after(MettleTypes.KW_VAR).spaces(1)
                .after(MettleTypes.KW_CONST).spaces(1)
                .after(MettleTypes.KW_FN).spaces(1)
                .after(MettleTypes.KW_KERNEL).spaces(1)
                .after(MettleTypes.KW_STRUCT).spaces(1)
                .after(MettleTypes.KW_ENUM).spaces(1)
                .after(MettleTypes.KW_TRAIT).spaces(1)
                .after(MettleTypes.KW_IMPL).spaces(1)
                .after(MettleTypes.KW_METHOD).spaces(1)
                .after(MettleTypes.KW_IMPORT).spaces(1)
                .after(MettleTypes.KW_NEW).spaces(1)
                .before(MettleTypes.LBRACE).spaces(1)

                // punctuation
                .before(MettleTypes.COMMA).none()
                .after(MettleTypes.COMMA).spaces(1)
                .before(MettleTypes.SEMICOLON).none()
                .before(MettleTypes.COLON).none()
                .after(MettleTypes.COLON).spaces(1)
                .after(MettleTypes.LPAREN).none()
                .before(MettleTypes.RPAREN).none()
                .after(MettleTypes.LBRACKET).none()
                .before(MettleTypes.RBRACKET).none()
                .around(MettleTypes.DOT).none()
                .before(MettleTypes.LPAREN).none()

                // `->` is a return type in a signature and a field access in an expression
                .aroundInside(MettleTypes.ARROW, MettleTypes.FUNCTION_DECL).spaces(1)
                .aroundInside(MettleTypes.ARROW, MettleTypes.METHOD_DECL).spaces(1)
                .aroundInside(MettleTypes.ARROW, MettleTypes.FIELD_EXPR).none()

                // binary operators only: `*` and `&` are also unary, and `int32*` is a type
                .aroundInside(MettleTypes.PLUS, MettleTypes.BINARY_EXPR).spaces(1)
                .aroundInside(MettleTypes.MINUS, MettleTypes.BINARY_EXPR).spaces(1)
                .aroundInside(MettleTypes.STAR, MettleTypes.BINARY_EXPR).spaces(1)
                .aroundInside(MettleTypes.SLASH, MettleTypes.BINARY_EXPR).spaces(1)
                .aroundInside(MettleTypes.PERCENT, MettleTypes.BINARY_EXPR).spaces(1)
                .aroundInside(MettleTypes.AMP, MettleTypes.BINARY_EXPR).spaces(1)
                .aroundInside(MettleTypes.PIPE, MettleTypes.BINARY_EXPR).spaces(1)
                .aroundInside(MettleTypes.CARET, MettleTypes.BINARY_EXPR).spaces(1)
                .aroundInside(MettleTypes.SHL, MettleTypes.BINARY_EXPR).spaces(1)
                .aroundInside(MettleTypes.SHR, MettleTypes.BINARY_EXPR).spaces(1)
                .aroundInside(MettleTypes.EQ, MettleTypes.BINARY_EXPR).spaces(1)
                .aroundInside(MettleTypes.NE, MettleTypes.BINARY_EXPR).spaces(1)
                .aroundInside(MettleTypes.LT, MettleTypes.BINARY_EXPR).spaces(1)
                .aroundInside(MettleTypes.GT, MettleTypes.BINARY_EXPR).spaces(1)
                .aroundInside(MettleTypes.LE, MettleTypes.BINARY_EXPR).spaces(1)
                .aroundInside(MettleTypes.GE, MettleTypes.BINARY_EXPR).spaces(1)
                .aroundInside(MettleTypes.ANDAND, MettleTypes.BINARY_EXPR).spaces(1)
                .aroundInside(MettleTypes.OROR, MettleTypes.BINARY_EXPR).spaces(1)

                // assignment, in statements and initializers alike
                .around(MettleTypes.ASSIGN).spaces(1)
                .around(MettleTypes.PLUS_EQ).spaces(1)
                .around(MettleTypes.MINUS_EQ).spaces(1)
                .around(MettleTypes.STAR_EQ).spaces(1)
                .around(MettleTypes.SLASH_EQ).spaces(1)
                .around(MettleTypes.PERCENT_EQ).spaces(1)
                .around(MettleTypes.AMP_EQ).spaces(1)
                .around(MettleTypes.PIPE_EQ).spaces(1)
                .around(MettleTypes.CARET_EQ).spaces(1)
                .around(MettleTypes.SHL_EQ).spaces(1)
                .around(MettleTypes.SHR_EQ).spaces(1);
    }
}
