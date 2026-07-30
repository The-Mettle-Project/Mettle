package org.mettle.clion.run;

import com.intellij.execution.lineMarker.ExecutorAction;
import com.intellij.execution.lineMarker.RunLineMarkerContributor;
import com.intellij.icons.AllIcons;
import com.intellij.psi.PsiElement;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.lang.MettleTypes;
import org.mettle.clion.psi.MettleDeclaration;
import org.mettle.clion.psi.MettlePsiUtil;

/** A gutter run button on {@code main} and on {@code @test} functions. */
public class MettleRunLineMarkerContributor extends RunLineMarkerContributor {

    @Override
    public @Nullable Info getInfo(@NotNull PsiElement element) {
        if (MettlePsiUtil.elementType(element) != MettleTypes.IDENTIFIER) return null;
        PsiElement parent = element.getParent();
        if (!(parent instanceof MettleDeclaration)) return null;
        MettleDeclaration declaration = (MettleDeclaration) parent;
        if (declaration.getKind() != MettleTypes.FUNCTION_DECL) return null;
        if (declaration.getNameIdentifier() != element) return null;

        boolean isMain = "main".equals(declaration.getName());
        boolean isTest = hasTestDecorator(declaration);
        if (!isMain && !isTest) return null;

        String tooltip = isMain ? "Build and run this file" : "Run the file's @test functions";
        return new Info(AllIcons.RunConfigurations.TestState.Run,
                element1 -> tooltip,
                ExecutorAction.getActions(0));
    }

    private static boolean hasTestDecorator(@NotNull MettleDeclaration declaration) {
        for (PsiElement decorator : MettlePsiUtil.childrenOfType(declaration, MettleTypes.DECORATOR)) {
            if (decorator.getText().startsWith("@test")) return true;
        }
        return false;
    }
}
