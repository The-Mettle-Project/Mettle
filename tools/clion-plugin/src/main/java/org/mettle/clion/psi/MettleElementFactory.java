package org.mettle.clion.psi;

import com.intellij.openapi.project.Project;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiFileFactory;
import com.intellij.psi.util.PsiTreeUtil;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.lang.MettleFileType;
import org.mettle.clion.lang.MettleTypes;

/** Builds throwaway PSI for rename and other edits. */
public final class MettleElementFactory {

    private MettleElementFactory() {
    }

    public static @NotNull MettleFile createFile(@NotNull Project project, @NotNull String text) {
        return (MettleFile) PsiFileFactory.getInstance(project)
                .createFileFromText("_dummy_.mettle", MettleFileType.INSTANCE, text);
    }

    /** An identifier leaf carrying {@code name}, ready to replace an existing one. */
    public static @Nullable PsiElement createIdentifier(@NotNull Project project, @NotNull String name) {
        MettleFile file = createFile(project, "fn " + name + "() {}\n");
        MettleDeclaration function = PsiTreeUtil.findChildOfType(file, MettleDeclaration.class);
        if (function == null || function.getKind() != MettleTypes.FUNCTION_DECL) return null;
        return function.getNameIdentifier();
    }
}
