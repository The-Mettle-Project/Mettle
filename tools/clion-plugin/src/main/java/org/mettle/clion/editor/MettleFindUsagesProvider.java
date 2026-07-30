package org.mettle.clion.editor;

import com.intellij.lang.HelpID;
import com.intellij.lang.cacheBuilder.DefaultWordsScanner;
import com.intellij.lang.cacheBuilder.WordsScanner;
import com.intellij.lang.findUsages.FindUsagesProvider;
import com.intellij.psi.PsiElement;
import com.intellij.psi.tree.IElementType;
import com.intellij.psi.tree.TokenSet;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.lang.MettleLexer;
import org.mettle.clion.lang.MettleTypes;
import org.mettle.clion.psi.MettleDeclaration;

public class MettleFindUsagesProvider implements FindUsagesProvider {

    @Override
    public @Nullable WordsScanner getWordsScanner() {
        return new DefaultWordsScanner(new MettleLexer(),
                TokenSet.create(MettleTypes.IDENTIFIER),
                MettleTypes.COMMENTS,
                MettleTypes.LITERALS);
    }

    @Override
    public boolean canFindUsagesFor(@NotNull PsiElement element) {
        return element instanceof MettleDeclaration && ((MettleDeclaration) element).getName() != null;
    }

    @Override
    public @Nullable String getHelpId(@NotNull PsiElement element) {
        return HelpID.FIND_OTHER_USAGES;
    }

    @Override
    public @NotNull String getType(@NotNull PsiElement element) {
        if (!(element instanceof MettleDeclaration)) return "element";
        IElementType kind = ((MettleDeclaration) element).getKind();
        if (kind == MettleTypes.FUNCTION_DECL) return "function";
        if (kind == MettleTypes.METHOD_DECL) return "method";
        if (kind == MettleTypes.STRUCT_DECL) return "struct";
        if (kind == MettleTypes.ENUM_DECL) return "enum";
        if (kind == MettleTypes.TRAIT_DECL) return "trait";
        if (kind == MettleTypes.IMPL_DECL) return "impl";
        if (kind == MettleTypes.FIELD_DECL) return "field";
        if (kind == MettleTypes.ENUM_MEMBER) return "enum variant";
        if (kind == MettleTypes.PARAM_DECL) return "parameter";
        if (kind == MettleTypes.CONST_DECL) return "constant";
        if (kind == MettleTypes.TYPE_PARAM) return "type parameter";
        return "variable";
    }

    @Override
    public @NotNull String getDescriptiveName(@NotNull PsiElement element) {
        if (element instanceof MettleDeclaration) {
            String name = ((MettleDeclaration) element).getName();
            if (name != null) return name;
        }
        return element.getText();
    }

    @Override
    public @NotNull String getNodeText(@NotNull PsiElement element, boolean useFullName) {
        return getDescriptiveName(element);
    }
}
