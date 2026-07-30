package org.mettle.clion.psi;

import com.intellij.extapi.psi.ASTWrapperPsiElement;
import com.intellij.lang.ASTNode;
import com.intellij.openapi.util.TextRange;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiReference;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.lang.MettleTypes;

/** The PSI elements that carry a reference. */
public final class MettleExpressions {

    private MettleExpressions() {
    }

    /** A bare identifier used as a value: a local, parameter, function or enum variant. */
    public static class RefExpr extends ASTWrapperPsiElement {
        public RefExpr(@NotNull ASTNode node) {
            super(node);
        }

        @Override
        public @Nullable PsiReference getReference() {
            PsiElement token = getFirstChild();
            if (token == null) return null;
            if (MettlePsiUtil.elementType(token) != MettleTypes.IDENTIFIER) return null;
            return new MettleReference(this, new TextRange(0, getTextLength()), MettleReference.Kind.SYMBOL);
        }

        @Override
        public PsiReference @NotNull [] getReferences() {
            PsiReference reference = getReference();
            return reference == null ? PsiReference.EMPTY_ARRAY : new PsiReference[]{reference};
        }
    }

    /** {@code receiver.member} / {@code receiver->member}. */
    public static class FieldExpr extends ASTWrapperPsiElement {
        public FieldExpr(@NotNull ASTNode node) {
            super(node);
        }

        @Override
        public @Nullable PsiReference getReference() {
            PsiElement name = getLastChild();
            if (name == null || MettlePsiUtil.elementType(name) != MettleTypes.IDENTIFIER) return null;
            int start = name.getTextRange().getStartOffset() - getTextRange().getStartOffset();
            return new MettleReference(this, new TextRange(start, start + name.getTextLength()),
                    MettleReference.Kind.MEMBER);
        }

        @Override
        public PsiReference @NotNull [] getReferences() {
            PsiReference reference = getReference();
            return reference == null ? PsiReference.EMPTY_ARRAY : new PsiReference[]{reference};
        }
    }

    /** A type as written: {@code Point*}, {@code Pair<int32, float64>}, {@code uint8[64]}. */
    public static class TypeRef extends ASTWrapperPsiElement {
        public TypeRef(@NotNull ASTNode node) {
            super(node);
        }

        @Override
        public @Nullable PsiReference getReference() {
            ASTNode name = getNode().findChildByType(MettleTypes.IDENTIFIER);
            if (name == null) return null;
            int start = name.getStartOffset() - getTextRange().getStartOffset();
            return new MettleReference(this, new TextRange(start, start + name.getTextLength()),
                    MettleReference.Kind.TYPE);
        }

        @Override
        public PsiReference @NotNull [] getReferences() {
            PsiReference reference = getReference();
            return reference == null ? PsiReference.EMPTY_ARRAY : new PsiReference[]{reference};
        }
    }

    /** {@code import "std/io";} - the quoted path navigates to the module. */
    public static class ImportDecl extends ASTWrapperPsiElement {
        public ImportDecl(@NotNull ASTNode node) {
            super(node);
        }

        public @Nullable String getImportPath() {
            return MettleResolver.importPath(this);
        }

        @Override
        public @Nullable PsiReference getReference() {
            PsiElement literal = MettlePsiUtil.childOfType(this, MettleTypes.STRING_LITERAL);
            if (literal == null || literal.getTextLength() < 2) return null;
            int start = literal.getTextRange().getStartOffset() - getTextRange().getStartOffset();
            String text = literal.getText();
            int contentStart = start + (text.startsWith("\"") ? 1 : 0);
            int contentEnd = start + text.length() - (text.length() > 1 && text.endsWith("\"") ? 1 : 0);
            if (contentEnd <= contentStart) return null;
            return new MettleReference(this, new TextRange(contentStart, contentEnd), MettleReference.Kind.FILE);
        }

        @Override
        public PsiReference @NotNull [] getReferences() {
            PsiReference reference = getReference();
            return reference == null ? PsiReference.EMPTY_ARRAY : new PsiReference[]{reference};
        }
    }
}
