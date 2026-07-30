package org.mettle.clion.psi;

import com.intellij.extapi.psi.PsiFileBase;
import com.intellij.openapi.fileTypes.FileType;
import com.intellij.psi.FileViewProvider;
import org.jetbrains.annotations.NotNull;
import org.mettle.clion.lang.MettleFileType;
import org.mettle.clion.lang.MettleLanguage;

public class MettleFile extends PsiFileBase {
    public MettleFile(@NotNull FileViewProvider viewProvider) {
        super(viewProvider, MettleLanguage.INSTANCE);
    }

    @Override
    public @NotNull FileType getFileType() {
        return MettleFileType.INSTANCE;
    }

    @Override
    public String toString() {
        return "Mettle file";
    }
}
