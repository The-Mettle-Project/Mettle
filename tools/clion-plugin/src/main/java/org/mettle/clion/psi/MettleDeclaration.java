package org.mettle.clion.psi;

import com.intellij.extapi.psi.ASTWrapperPsiElement;
import com.intellij.icons.AllIcons;
import com.intellij.lang.ASTNode;
import com.intellij.navigation.ItemPresentation;
import com.intellij.psi.PsiElement;
import com.intellij.psi.tree.IElementType;
import com.intellij.util.IncorrectOperationException;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.lang.MettleTypes;

import javax.swing.Icon;

/**
 * One implementation for every named declaration. The kind - function, struct, field, binding -
 * comes from the node's element type, which keeps the PSI layer small enough to read in one sitting.
 */
public class MettleDeclaration extends ASTWrapperPsiElement implements MettleNamedElement {

    public MettleDeclaration(@NotNull ASTNode node) {
        super(node);
    }

    public IElementType getKind() {
        return getNode().getElementType();
    }

    @Override
    public @Nullable PsiElement getNameIdentifier() {
        return MettlePsiUtil.nameIdentifier(this);
    }

    @Override
    public @Nullable String getName() {
        IElementType kind = getKind();
        if (kind == MettleTypes.IMPL_DECL) {
            PsiElement type = MettlePsiUtil.childOfType(this, MettleTypes.TYPE_REF);
            return type == null ? null : type.getText();
        }
        PsiElement identifier = getNameIdentifier();
        return identifier == null ? null : identifier.getText();
    }

    @Override
    public PsiElement setName(@NotNull String name) throws IncorrectOperationException {
        PsiElement identifier = getNameIdentifier();
        if (identifier != null) {
            PsiElement replacement = MettleElementFactory.createIdentifier(getProject(), name);
            if (replacement != null) identifier.replace(replacement);
        }
        return this;
    }

    @Override
    public int getTextOffset() {
        PsiElement identifier = getNameIdentifier();
        return identifier == null ? super.getTextOffset() : identifier.getTextOffset();
    }

    /** The type as written, for presentations: {@code int32*} for {@code var p: int32*}. */
    public @Nullable String getDeclaredTypeText() {
        PsiElement type = MettlePsiUtil.declaredType(this);
        return type == null ? null : type.getText();
    }

    @Override
    public @Nullable ItemPresentation getPresentation() {
        String name = getName();
        if (name == null) return null;
        String detail = presentationDetail();
        Icon icon = getIcon(0);
        return new ItemPresentation() {
            @Override
            public @Nullable String getPresentableText() {
                return detail == null ? name : name + detail;
            }

            @Override
            public @Nullable String getLocationString() {
                return getContainingFile() == null ? null : getContainingFile().getName();
            }

            @Override
            public @Nullable Icon getIcon(boolean unused) {
                return icon;
            }
        };
    }

    private @Nullable String presentationDetail() {
        IElementType kind = getKind();
        if (kind == MettleTypes.FUNCTION_DECL || kind == MettleTypes.METHOD_DECL) {
            PsiElement params = MettlePsiUtil.childOfType(this, MettleTypes.PARAM_LIST);
            PsiElement returnType = MettlePsiUtil.childOfType(this, MettleTypes.TYPE_REF);
            String signature = params == null ? "()" : params.getText().replaceAll("\\s+", " ");
            return returnType == null ? signature : signature + ": " + returnType.getText();
        }
        String type = getDeclaredTypeText();
        return type == null ? null : ": " + type;
    }

    @Override
    public Icon getIcon(int flags) {
        IElementType kind = getKind();
        if (kind == MettleTypes.FUNCTION_DECL) return AllIcons.Nodes.Function;
        if (kind == MettleTypes.METHOD_DECL) return AllIcons.Nodes.Method;
        if (kind == MettleTypes.STRUCT_DECL) return AllIcons.Nodes.Class;
        if (kind == MettleTypes.ENUM_DECL) return AllIcons.Nodes.Enum;
        if (kind == MettleTypes.TRAIT_DECL) return AllIcons.Nodes.Interface;
        if (kind == MettleTypes.IMPL_DECL) return AllIcons.Nodes.Class;
        if (kind == MettleTypes.FIELD_DECL) return AllIcons.Nodes.Field;
        if (kind == MettleTypes.ENUM_MEMBER) return AllIcons.Nodes.Constant;
        if (kind == MettleTypes.PARAM_DECL) return AllIcons.Nodes.Parameter;
        if (kind == MettleTypes.CONST_DECL) return AllIcons.Nodes.Constant;
        if (kind == MettleTypes.TYPE_PARAM) return AllIcons.Nodes.Class;
        return AllIcons.Nodes.Variable;
    }

    @Override
    public String toString() {
        return getKind() + "(" + getName() + ")";
    }
}
