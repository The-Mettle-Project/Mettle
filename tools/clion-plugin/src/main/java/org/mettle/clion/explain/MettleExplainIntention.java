package org.mettle.clion.explain;

import com.intellij.codeInsight.intention.IntentionAction;
import com.intellij.openapi.editor.Editor;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.psi.PsiFile;
import com.intellij.util.IncorrectOperationException;
import org.jetbrains.annotations.Nls;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.psi.MettleFile;

/**
 * Alt-Enter on a loop the optimizer refused: offers the fix it verified.
 *
 * <p>The suggestion only appears once a report exists for the file, because the compiler is the
 * one that decides a fix is real - the plugin never invents optimizer advice of its own.
 */
public class MettleExplainIntention implements IntentionAction {

    private @Nullable MettleExplainFix pending;

    @Override
    public @Nls @NotNull String getText() {
        MettleExplainFix fix = pending;
        return fix == null ? "Apply the verified Mettle fix" : fix.title();
    }

    @Override
    public @Nls @NotNull String getFamilyName() {
        return "Mettle optimization fixes";
    }

    @Override
    public boolean isAvailable(@NotNull Project project, Editor editor, PsiFile file) {
        pending = null;
        if (editor == null || !(file instanceof MettleFile)) return false;
        VirtualFile virtualFile = file.getVirtualFile();
        MettleExplainService.Snapshot snapshot = MettleExplainService.getInstance(project).snapshot(virtualFile);
        if (snapshot == null || snapshot.report == null) return false;

        int line = editor.getDocument().getLineNumber(editor.getCaretModel().getOffset()) + 1;
        for (MettleExplainReport.Remark remark : snapshot.report.at(line)) {
            MettleExplainFix fix = MettleExplainFix.synthesize(file, remark);
            if (fix != null) {
                pending = fix;
                return true;
            }
        }
        return false;
    }

    @Override
    public void invoke(@NotNull Project project, Editor editor, PsiFile file)
            throws IncorrectOperationException {
        MettleExplainFix fix = pending;
        if (fix != null) fix.applyEdits(project);
    }

    @Override
    public boolean startInWriteAction() {
        return true;
    }
}
