package org.mettle.clion.editor;

import com.intellij.codeInsight.template.TemplateActionContext;
import com.intellij.codeInsight.template.TemplateContextType;
import org.jetbrains.annotations.NotNull;
import org.mettle.clion.psi.MettleFile;

public class MettleTemplateContextType extends TemplateContextType {

    protected MettleTemplateContextType() {
        super("Mettle");
    }

    @Override
    public boolean isInContext(@NotNull TemplateActionContext context) {
        return context.getFile() instanceof MettleFile;
    }
}
