package org.mettle.clion.psi;

import com.intellij.openapi.util.TextRange;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiFile;
import com.intellij.psi.PsiReferenceBase;
import com.intellij.util.IncorrectOperationException;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

/** One reference implementation, specialised by what the referencing element is. */
public class MettleReference extends PsiReferenceBase<PsiElement> {

    public enum Kind {
        /** A bare name: a local, parameter, function, global or enum variant. */
        SYMBOL,
        /** The member name of {@code a.b} or {@code a->b}. */
        MEMBER,
        /** A type name in a declaration or cast. */
        TYPE,
        /** The quoted path of an {@code import}. */
        FILE
    }

    private final Kind kind;

    public MettleReference(@NotNull PsiElement element, @NotNull TextRange rangeInElement, @NotNull Kind kind) {
        super(element, rangeInElement);
        this.kind = kind;
    }

    @Override
    public @Nullable PsiElement resolve() {
        PsiElement element = getElement();
        String text = getValue();
        switch (kind) {
            case SYMBOL:
                return MettleResolver.resolveSymbol(element, text);
            case MEMBER:
                return MettleResolver.resolveFieldExpression(element);
            case TYPE:
                return MettleResolver.resolveTypeName(element, text);
            case FILE:
                PsiFile file = element.getContainingFile();
                return file == null ? null : MettleImportResolver.resolve(file, text);
            default:
                return null;
        }
    }

    @Override
    public PsiElement handleElementRename(@NotNull String newElementName) throws IncorrectOperationException {
        if (kind == Kind.FILE) return getElement();
        PsiElement leaf = getElement().findElementAt(getRangeInElement().getStartOffset());
        if (leaf != null) {
            PsiElement replacement = MettleElementFactory.createIdentifier(getElement().getProject(), newElementName);
            if (replacement != null) leaf.replace(replacement);
        }
        return getElement();
    }

    @Override
    public boolean isSoft() {
        // Built-in names (sizeof, printf via extern, GPU intrinsics) have no declaration in
        // source, so an unresolved reference is not by itself an error.
        return true;
    }

    @Override
    public Object @NotNull [] getVariants() {
        return EMPTY_ARRAY;
    }
}
