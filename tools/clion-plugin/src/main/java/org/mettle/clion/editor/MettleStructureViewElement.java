package org.mettle.clion.editor;

import com.intellij.ide.projectView.PresentationData;
import com.intellij.ide.structureView.StructureViewTreeElement;
import com.intellij.ide.util.treeView.smartTree.TreeElement;
import com.intellij.navigation.ItemPresentation;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiFile;
import com.intellij.psi.tree.IElementType;
import com.intellij.psi.tree.TokenSet;
import org.jetbrains.annotations.NotNull;
import org.mettle.clion.lang.MettleTypes;
import org.mettle.clion.psi.MettleDeclaration;
import org.mettle.clion.psi.MettlePsiUtil;

import java.util.ArrayList;
import java.util.List;

/** One node of the Structure view: the file, then its declarations, then struct members. */
public class MettleStructureViewElement implements StructureViewTreeElement {

    private static final TokenSet TOP_LEVEL = TokenSet.create(
            MettleTypes.FUNCTION_DECL, MettleTypes.STRUCT_DECL, MettleTypes.ENUM_DECL,
            MettleTypes.TRAIT_DECL, MettleTypes.IMPL_DECL, MettleTypes.VAR_DECL,
            MettleTypes.CONST_DECL);

    private static final TokenSet MEMBERS = TokenSet.create(
            MettleTypes.FIELD_DECL, MettleTypes.METHOD_DECL, MettleTypes.ENUM_MEMBER,
            MettleTypes.FUNCTION_DECL);

    private final PsiElement element;

    public MettleStructureViewElement(@NotNull PsiElement element) {
        this.element = element;
    }

    @Override
    public Object getValue() {
        return element;
    }

    @Override
    public void navigate(boolean requestFocus) {
        if (element instanceof com.intellij.pom.Navigatable) {
            ((com.intellij.pom.Navigatable) element).navigate(requestFocus);
        }
    }

    @Override
    public boolean canNavigate() {
        return element instanceof com.intellij.pom.Navigatable
                && ((com.intellij.pom.Navigatable) element).canNavigate();
    }

    @Override
    public boolean canNavigateToSource() {
        return canNavigate();
    }

    @Override
    public @NotNull ItemPresentation getPresentation() {
        if (element instanceof MettleDeclaration) {
            ItemPresentation presentation = ((MettleDeclaration) element).getPresentation();
            if (presentation != null) return presentation;
        }
        if (element instanceof PsiFile) {
            return new PresentationData(((PsiFile) element).getName(), null,
                    ((PsiFile) element).getIcon(0), null);
        }
        return new PresentationData(element.getText(), null, null, null);
    }

    @Override
    public TreeElement @NotNull [] getChildren() {
        List<TreeElement> children = new ArrayList<>();
        TokenSet wanted = element instanceof PsiFile ? TOP_LEVEL : MEMBERS;
        if (!(element instanceof PsiFile) && isLeafKind(element)) return EMPTY_ARRAY;
        for (PsiElement child : MettlePsiUtil.childrenOfTypes(element, wanted)) {
            children.add(new MettleStructureViewElement(child));
        }
        return children.toArray(TreeElement.EMPTY_ARRAY);
    }

    static boolean isLeafKind(@NotNull PsiElement element) {
        IElementType type = MettlePsiUtil.elementType(element);
        return type == MettleTypes.FUNCTION_DECL || type == MettleTypes.METHOD_DECL
                || type == MettleTypes.FIELD_DECL || type == MettleTypes.VAR_DECL
                || type == MettleTypes.CONST_DECL || type == MettleTypes.ENUM_MEMBER;
    }
}
