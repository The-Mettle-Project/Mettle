package org.mettle.clion.explain;

import com.intellij.openapi.project.Project;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.openapi.vfs.newvfs.BulkFileListener;
import com.intellij.openapi.vfs.newvfs.events.VFileContentChangeEvent;
import com.intellij.openapi.vfs.newvfs.events.VFileEvent;
import org.jetbrains.annotations.NotNull;
import org.mettle.clion.lang.MettleFileType;

import java.util.List;

/**
 * Recompiles the report when its file is saved.
 *
 * <p>Fires on the content change rather than before the save, so the compiler reads what the
 * editor just wrote. Only files that already have a report are refreshed: opening the tool window
 * is what opts a file in.
 */
public class MettleExplainSaveListener implements BulkFileListener {

    private final Project project;

    public MettleExplainSaveListener(@NotNull Project project) {
        this.project = project;
    }

    @Override
    public void after(@NotNull List<? extends VFileEvent> events) {
        MettleExplainService service = MettleExplainService.getInstance(project);
        if (!service.isAutoRefresh()) return;
        for (VFileEvent event : events) {
            if (!(event instanceof VFileContentChangeEvent)) continue;
            VirtualFile file = event.getFile();
            if (file == null || file.getFileType() != MettleFileType.INSTANCE) continue;
            if (!service.hasReportFor(file)) continue;
            service.refresh(file);
        }
    }
}
