package org.mettle.clion.run;

import com.intellij.execution.configurations.ConfigurationFactory;
import com.intellij.execution.configurations.ConfigurationTypeBase;
import com.intellij.execution.configurations.RunConfiguration;
import com.intellij.execution.configurations.RunConfigurationSingletonPolicy;
import com.intellij.openapi.project.Project;
import org.jetbrains.annotations.NotNull;
import org.mettle.clion.lang.MettleIcons;

public class MettleRunConfigurationType extends ConfigurationTypeBase {

    static final String ID = "MettleRunConfiguration";

    public MettleRunConfigurationType() {
        super(ID, "Mettle", "Build and run a Mettle source file", MettleIcons.FILE);
        addFactory(new Factory(this));
    }

    public static MettleRunConfigurationType getInstance() {
        return com.intellij.execution.configurations.ConfigurationTypeUtil
                .findConfigurationType(MettleRunConfigurationType.class);
    }

    public ConfigurationFactory factory() {
        return getConfigurationFactories()[0];
    }

    public static class Factory extends ConfigurationFactory {
        Factory(@NotNull MettleRunConfigurationType type) {
            super(type);
        }

        @Override
        public @NotNull String getId() {
            return "Mettle";
        }

        @Override
        public @NotNull RunConfiguration createTemplateConfiguration(@NotNull Project project) {
            return new MettleRunConfiguration(project, this, "Mettle");
        }

        @Override
        public @NotNull RunConfigurationSingletonPolicy getSingletonPolicy() {
            return RunConfigurationSingletonPolicy.MULTIPLE_INSTANCE;
        }
    }
}
