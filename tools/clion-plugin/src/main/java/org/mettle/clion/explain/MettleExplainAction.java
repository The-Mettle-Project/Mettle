package org.mettle.clion.explain;

import com.intellij.openapi.actionSystem.ActionUpdateThread;
import com.intellij.openapi.actionSystem.AnAction;
import com.intellij.openapi.actionSystem.AnActionEvent;
import com.intellij.openapi.actionSystem.CommonDataKeys;
import com.intellij.openapi.project.DumbAware;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.openapi.wm.ToolWindow;
import com.intellij.openapi.wm.ToolWindowManager;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.lang.MettleFileType;

/** Opens the optimization report for the current file and compiles it. */
public class MettleExplainAction extends AnAction implements DumbAware {

    @Override
    public void actionPerformed(@NotNull AnActionEvent event) {
        Project project = event.getProject();
        VirtualFile file = mettleFile(event);
        if (project == null || file == null) return;

        MettleExplainService.getInstance(project).setShownFile(file);
        ToolWindow window = ToolWindowManager.getInstance(project)
                .getToolWindow(MettleExplainToolWindowFactory.ID);
        if (window == null) return;
        window.activate(() -> MettleExplainService.getInstance(project).refresh(file), true);
    }

    @Override
    public void update(@NotNull AnActionEvent event) {
        event.getPresentation().setEnabledAndVisible(mettleFile(event) != null);
    }

    @Override
    public @NotNull ActionUpdateThread getActionUpdateThread() {
        return ActionUpdateThread.BGT;
    }

    private static @Nullable VirtualFile mettleFile(@NotNull AnActionEvent event) {
        VirtualFile file = event.getData(CommonDataKeys.VIRTUAL_FILE);
        return file != null && file.getFileType() == MettleFileType.INSTANCE ? file : null;
    }
}
