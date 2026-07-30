package org.mettle.clion.debug;

import com.intellij.openapi.editor.Document;
import com.intellij.openapi.fileTypes.FileType;
import com.intellij.openapi.project.Project;
import com.intellij.psi.PsiDocumentManager;
import com.intellij.psi.PsiFile;
import com.intellij.xdebugger.XExpression;
import com.intellij.xdebugger.XSourcePosition;
import com.intellij.xdebugger.evaluation.EvaluationMode;
import com.intellij.xdebugger.evaluation.XDebuggerEditorsProvider;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.lang.MettleFileType;
import org.mettle.clion.psi.MettleElementFactory;

/** Lets breakpoint conditions and watch expressions be edited as Mettle. */
public class MettleDebuggerEditorsProvider extends XDebuggerEditorsProvider {

    @Override
    public @NotNull FileType getFileType() {
        return MettleFileType.INSTANCE;
    }

    @Override
    public @NotNull Document createDocument(@NotNull Project project, @NotNull XExpression expression,
                                            @Nullable XSourcePosition sourcePosition,
                                            @NotNull EvaluationMode mode) {
        PsiFile fragment = MettleElementFactory.createFile(project, expression.getExpression());
        Document document = PsiDocumentManager.getInstance(project).getDocument(fragment);
        if (document != null) return document;
        return com.intellij.openapi.editor.EditorFactory.getInstance()
                .createDocument(expression.getExpression());
    }
}
