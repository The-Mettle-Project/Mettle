package org.mettle.clion.run;

import com.intellij.execution.actions.ConfigurationContext;
import com.intellij.execution.actions.LazyRunConfigurationProducer;
import com.intellij.execution.configurations.ConfigurationFactory;
import com.intellij.openapi.util.Ref;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiFile;
import org.jetbrains.annotations.NotNull;
import org.mettle.clion.psi.MettleFile;

/** "Run" on a Mettle file creates the configuration for it. */
public class MettleRunConfigurationProducer extends LazyRunConfigurationProducer<MettleRunConfiguration> {

    @Override
    public @NotNull ConfigurationFactory getConfigurationFactory() {
        return MettleRunConfigurationType.getInstance().factory();
    }

    @Override
    protected boolean setupConfigurationFromContext(@NotNull MettleRunConfiguration configuration,
                                                    @NotNull ConfigurationContext context,
                                                    @NotNull Ref<PsiElement> sourceElement) {
        VirtualFile file = sourceFile(context);
        if (file == null) return false;
        configuration.setFilePath(file.getPath());
        configuration.setName(MettleBuild.stem(java.nio.file.Paths.get(file.getPath())));
        return true;
    }

    @Override
    public boolean isConfigurationFromContext(@NotNull MettleRunConfiguration configuration,
                                              @NotNull ConfigurationContext context) {
        VirtualFile file = sourceFile(context);
        return file != null && file.getPath().equals(configuration.getFilePath());
    }

    private static VirtualFile sourceFile(@NotNull ConfigurationContext context) {
        PsiElement location = context.getPsiLocation();
        PsiFile file = location == null ? null : location.getContainingFile();
        if (!(file instanceof MettleFile)) return null;
        return file.getVirtualFile();
    }
}
