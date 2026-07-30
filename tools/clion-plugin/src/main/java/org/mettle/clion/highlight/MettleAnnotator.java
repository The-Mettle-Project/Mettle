package org.mettle.clion.highlight;

import com.intellij.lang.annotation.AnnotationHolder;
import com.intellij.lang.annotation.Annotator;
import com.intellij.lang.annotation.HighlightSeverity;
import com.intellij.openapi.editor.colors.TextAttributesKey;
import com.intellij.openapi.util.TextRange;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiFile;
import com.intellij.psi.tree.IElementType;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.lang.MettleBuiltins;
import org.mettle.clion.lang.MettleTypes;
import org.mettle.clion.psi.MettleDeclaration;
import org.mettle.clion.psi.MettleExpressions;
import org.mettle.clion.psi.MettlePsiUtil;
import org.mettle.clion.psi.MettleResolver;

/**
 * Semantic colouring: what the lexer cannot know - that a name is a parameter, a field, a call or
 * a built-in - is painted here, from the resolved declaration.
 */
public class MettleAnnotator implements Annotator {

    @Override
    public void annotate(@NotNull PsiElement element, @NotNull AnnotationHolder holder) {
        if (element instanceof MettleDeclaration) {
            annotateDeclaration((MettleDeclaration) element, holder);
            return;
        }
        if (element instanceof MettleExpressions.RefExpr) {
            annotateReference(element, holder);
            return;
        }
        if (element instanceof MettleExpressions.FieldExpr) {
            annotateMember(element, holder);
            return;
        }
        if (element instanceof MettleExpressions.TypeRef) {
            annotateType(element, holder);
            return;
        }
        if (MettlePsiUtil.elementType(element) == MettleTypes.DECORATOR) {
            paint(holder, element.getTextRange(), MettleColors.DECORATOR);
        }
    }

    private void annotateDeclaration(@NotNull MettleDeclaration declaration, @NotNull AnnotationHolder holder) {
        PsiElement identifier = declaration.getNameIdentifier();
        if (identifier == null) return;
        IElementType kind = declaration.getKind();
        TextAttributesKey key;
        if (kind == MettleTypes.FUNCTION_DECL || kind == MettleTypes.METHOD_DECL) {
            key = MettleColors.FUNCTION_DECLARATION;
        } else if (kind == MettleTypes.STRUCT_DECL || kind == MettleTypes.ENUM_DECL
                || kind == MettleTypes.TRAIT_DECL) {
            key = MettleColors.TYPE_NAME;
        } else if (kind == MettleTypes.TYPE_PARAM) {
            key = MettleColors.TYPE_PARAMETER;
        } else if (kind == MettleTypes.FIELD_DECL) {
            key = MettleColors.FIELD;
        } else if (kind == MettleTypes.PARAM_DECL) {
            key = MettleColors.PARAMETER;
        } else if (kind == MettleTypes.ENUM_MEMBER) {
            key = MettleColors.ENUM_VARIANT;
        } else if (kind == MettleTypes.CONST_DECL) {
            key = MettleColors.CONSTANT;
        } else if (kind == MettleTypes.VAR_DECL) {
            key = isTopLevel(declaration) ? MettleColors.GLOBAL_VARIABLE : MettleColors.LOCAL_VARIABLE;
        } else {
            return;
        }
        paint(holder, identifier.getTextRange(), key);
    }

    private void annotateReference(@NotNull PsiElement reference, @NotNull AnnotationHolder holder) {
        String name = reference.getText();
        if (MettleBuiltins.isGpuBuiltin(name)) {
            paint(holder, reference.getTextRange(), MettleColors.GPU_BUILTIN);
            return;
        }
        if (MettleBuiltins.isBuiltin(name)) {
            paint(holder, reference.getTextRange(), MettleColors.BUILTIN_NAME);
            return;
        }
        PsiElement target = MettleResolver.resolveSymbol(reference, name);
        if (target == null) {
            if (isCallee(reference)) paint(holder, reference.getTextRange(), MettleColors.FUNCTION_CALL);
            return;
        }
        paint(holder, reference.getTextRange(), keyForTarget(target));
    }

    private void annotateMember(@NotNull PsiElement fieldExpression, @NotNull AnnotationHolder holder) {
        PsiElement name = fieldExpression.getLastChild();
        if (name == null || MettlePsiUtil.elementType(name) != MettleTypes.IDENTIFIER) return;
        PsiElement target = MettleResolver.resolveFieldExpression(fieldExpression);
        TextAttributesKey key = MettleColors.FIELD;
        if (target != null && MettlePsiUtil.elementType(target) == MettleTypes.METHOD_DECL) {
            key = MettleColors.FUNCTION_CALL;
        } else if (isCallee(fieldExpression)) {
            key = MettleColors.FUNCTION_CALL;
        }
        paint(holder, name.getTextRange(), key);
    }

    private void annotateType(@NotNull PsiElement typeRef, @NotNull AnnotationHolder holder) {
        PsiElement name = MettlePsiUtil.childOfType(typeRef, MettleTypes.IDENTIFIER);
        if (name == null) return;
        String text = name.getText();
        if (MettleBuiltins.TYPES.contains(text)) {
            paint(holder, name.getTextRange(), MettleColors.BUILTIN_NAME);
            return;
        }
        PsiElement target = MettleResolver.resolveTypeName(typeRef, text);
        TextAttributesKey key = target != null
                && MettlePsiUtil.elementType(target) == MettleTypes.TYPE_PARAM
                ? MettleColors.TYPE_PARAMETER : MettleColors.TYPE_NAME;
        paint(holder, name.getTextRange(), key);
    }

    private static TextAttributesKey keyForTarget(@NotNull PsiElement target) {
        IElementType kind = MettlePsiUtil.elementType(target);
        if (kind == MettleTypes.FUNCTION_DECL || kind == MettleTypes.METHOD_DECL) {
            return MettleColors.FUNCTION_CALL;
        }
        if (kind == MettleTypes.PARAM_DECL) return MettleColors.PARAMETER;
        if (kind == MettleTypes.CONST_DECL) return MettleColors.CONSTANT;
        if (kind == MettleTypes.ENUM_MEMBER) return MettleColors.ENUM_VARIANT;
        if (kind == MettleTypes.STRUCT_DECL || kind == MettleTypes.ENUM_DECL
                || kind == MettleTypes.TRAIT_DECL) {
            return MettleColors.TYPE_NAME;
        }
        if (kind == MettleTypes.TYPE_PARAM) return MettleColors.TYPE_PARAMETER;
        if (kind == MettleTypes.VAR_DECL) {
            return isTopLevel(target) ? MettleColors.GLOBAL_VARIABLE : MettleColors.LOCAL_VARIABLE;
        }
        return MettleColors.IDENTIFIER;
    }

    private static boolean isTopLevel(@Nullable PsiElement element) {
        return element != null && element.getParent() instanceof PsiFile;
    }

    private static boolean isCallee(@NotNull PsiElement element) {
        PsiElement parent = element.getParent();
        return MettlePsiUtil.elementType(parent) == MettleTypes.CALL_EXPR
                && MettleResolver.firstExpression(parent) == element;
    }

    private static void paint(@NotNull AnnotationHolder holder, @NotNull TextRange range,
                              @NotNull TextAttributesKey key) {
        holder.newSilentAnnotation(HighlightSeverity.INFORMATION)
                .range(range)
                .textAttributes(key)
                .create();
    }
}
