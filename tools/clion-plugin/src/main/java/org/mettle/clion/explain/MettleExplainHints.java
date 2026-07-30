package org.mettle.clion.explain;

import com.intellij.icons.AllIcons;
import com.intellij.openapi.Disposable;
import com.intellij.openapi.actionSystem.AnAction;
import com.intellij.openapi.actionSystem.AnActionEvent;
import com.intellij.openapi.application.ApplicationManager;
import com.intellij.openapi.components.Service;
import com.intellij.openapi.editor.Editor;
import com.intellij.openapi.editor.EditorFactory;
import com.intellij.openapi.editor.Inlay;
import com.intellij.openapi.editor.colors.EditorFontType;
import com.intellij.openapi.editor.event.EditorFactoryEvent;
import com.intellij.openapi.editor.event.EditorFactoryListener;
import com.intellij.openapi.editor.markup.GutterIconRenderer;
import com.intellij.openapi.editor.markup.HighlighterLayer;
import com.intellij.openapi.editor.markup.RangeHighlighter;
import com.intellij.openapi.editor.markup.TextAttributes;
import com.intellij.openapi.fileEditor.FileDocumentManager;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.util.Disposer;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.psi.PsiDocumentManager;
import com.intellij.psi.PsiFile;
import com.intellij.ui.JBColor;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import javax.swing.Icon;
import java.awt.Color;
import java.awt.Font;
import java.awt.Graphics;
import java.awt.Rectangle;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * The report, in the editor: an end-of-line hint saying what the optimizer did with the loop or
 * call on that line, and a gutter icon whose click applies the fix when the compiler verified one.
 *
 * <p>Managed directly rather than through a hint provider, so the hints appear the moment a report
 * lands and disappear the moment it is turned off, without waiting for a highlighting pass.
 */
@Service(Service.Level.PROJECT)
public final class MettleExplainHints implements Disposable {

    private static final int MAX_TEXT = 90;

    private final Project project;
    private final Map<Editor, List<Inlay<?>>> inlays = new ConcurrentHashMap<>();
    private final Map<Editor, List<RangeHighlighter>> markers = new ConcurrentHashMap<>();
    private volatile boolean enabled = true;

    public MettleExplainHints(@NotNull Project project) {
        this.project = project;
        EditorFactory.getInstance().addEditorFactoryListener(new EditorFactoryListener() {
            @Override
            public void editorCreated(@NotNull EditorFactoryEvent event) {
                Editor editor = event.getEditor();
                if (editor.getProject() != project) return;
                VirtualFile file = FileDocumentManager.getInstance().getFile(editor.getDocument());
                if (file != null) ApplicationManager.getApplication().invokeLater(() -> refresh(file));
            }

            @Override
            public void editorReleased(@NotNull EditorFactoryEvent event) {
                clear(event.getEditor());
            }
        }, this);
    }

    public static @NotNull MettleExplainHints getInstance(@NotNull Project project) {
        return project.getService(MettleExplainHints.class);
    }

    public boolean isEnabled() {
        return enabled;
    }

    public void setEnabled(boolean enabled) {
        this.enabled = enabled;
        if (!enabled) {
            for (Editor editor : new ArrayList<>(inlays.keySet())) clear(editor);
        } else {
            VirtualFile shown = MettleExplainService.getInstance(project).shownFile();
            if (shown != null) refresh(shown);
        }
    }

    /** Repaints the hints for every editor showing {@code file}. */
    public void refresh(@NotNull VirtualFile file) {
        ApplicationManager.getApplication().assertIsDispatchThread();
        MettleExplainService.Snapshot snapshot = MettleExplainService.getInstance(project).snapshot(file);
        MettleExplainReport report = snapshot == null ? null : snapshot.report;
        for (Editor editor : EditorFactory.getInstance().getEditorList()) {
            if (editor.getProject() != project) continue;
            VirtualFile editorFile = FileDocumentManager.getInstance().getFile(editor.getDocument());
            if (editorFile == null || !editorFile.equals(file)) continue;
            clear(editor);
            if (!enabled || report == null) continue;
            add(editor, report);
        }
    }

    private void add(@NotNull Editor editor, @NotNull MettleExplainReport report) {
        List<Inlay<?>> createdInlays = new ArrayList<>();
        List<RangeHighlighter> createdMarkers = new ArrayList<>();
        int lineCount = editor.getDocument().getLineCount();
        PsiFile psiFile = psiFile(editor);

        for (MettleExplainReport.Remark remark : report.remarks) {
            int line = remark.line - 1;
            if (line < 0 || line >= lineCount) continue;
            Inlay<?> inlay = editor.getInlayModel().addAfterLineEndElement(
                    editor.getDocument().getLineEndOffset(line), false, new Renderer(remark));
            if (inlay != null) createdInlays.add(inlay);

            if (remark.positive) continue;
            MettleExplainFix fix = psiFile == null ? null : MettleExplainFix.synthesize(psiFile, remark);
            RangeHighlighter marker = editor.getMarkupModel().addLineHighlighter(
                    null, line, HighlighterLayer.ADDITIONAL_SYNTAX);
            marker.setGutterIconRenderer(new Marker(project, remark, fix));
            createdMarkers.add(marker);
        }
        if (!createdInlays.isEmpty()) inlays.put(editor, createdInlays);
        if (!createdMarkers.isEmpty()) markers.put(editor, createdMarkers);
    }

    private void clear(@NotNull Editor editor) {
        List<Inlay<?>> existingInlays = inlays.remove(editor);
        if (existingInlays != null) {
            for (Inlay<?> inlay : existingInlays) {
                if (inlay.isValid()) Disposer.dispose(inlay);
            }
        }
        List<RangeHighlighter> existingMarkers = markers.remove(editor);
        if (existingMarkers != null) {
            for (RangeHighlighter marker : existingMarkers) {
                if (marker.isValid()) editor.getMarkupModel().removeHighlighter(marker);
            }
        }
    }

    private @Nullable PsiFile psiFile(@NotNull Editor editor) {
        return PsiDocumentManager.getInstance(project).getPsiFile(editor.getDocument());
    }

    @Override
    public void dispose() {
        for (Editor editor : new ArrayList<>(inlays.keySet())) clear(editor);
        for (Editor editor : new ArrayList<>(markers.keySet())) clear(editor);
    }

    /** The gutter icon for a refusal: hover for the full reason, click to apply a verified fix. */
    private static class Marker extends GutterIconRenderer {
        private final Project project;
        private final MettleExplainReport.Remark remark;
        private final @Nullable MettleExplainFix fix;

        Marker(@NotNull Project project, MettleExplainReport.@NotNull Remark remark,
               @Nullable MettleExplainFix fix) {
            this.project = project;
            this.remark = remark;
            this.fix = fix;
        }

        @Override
        public @NotNull Icon getIcon() {
            return fix != null ? AllIcons.Actions.IntentionBulb : AllIcons.General.BalloonInformation;
        }

        @Override
        public @Nullable String getTooltipText() {
            StringBuilder html = new StringBuilder("<html><body><b>")
                    .append(escape(remark.headline)).append("</b>");
            if (remark.reason != null) html.append("<br/><br/>").append(escape(remark.reason));
            if (remark.fix != null) html.append("<br/><br/><i>fix:</i> ").append(escape(remark.fix));
            if (remark.verified != null) {
                html.append("<br/><br/><i>verified:</i> ").append(escape(remark.verified));
            }
            if (fix != null) html.append("<br/><br/>Click to ").append(escape(fix.title().toLowerCase()));
            return html.append("</body></html>").toString();
        }

        @Override
        public @Nullable AnAction getClickAction() {
            if (fix == null) return null;
            return new AnAction(fix.title()) {
                @Override
                public void actionPerformed(@NotNull AnActionEvent event) {
                    fix.apply(project);
                }
            };
        }

        @Override
        public boolean isNavigateAction() {
            return fix != null;
        }

        @Override
        public @NotNull Alignment getAlignment() {
            return Alignment.RIGHT;
        }

        @Override
        public boolean equals(Object other) {
            if (!(other instanceof Marker)) return false;
            Marker marker = (Marker) other;
            return remark.line == marker.remark.line
                    && remark.headline.equals(marker.remark.headline);
        }

        @Override
        public int hashCode() {
            return remark.line * 31 + remark.headline.hashCode();
        }

        private static String escape(@Nullable String text) {
            return text == null ? "" : text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;");
        }
    }

    /** The hint text for one remark, kept to one readable line. */
    static @NotNull String hintText(MettleExplainReport.@NotNull Remark remark) {
        String text;
        if (remark.positive) {
            text = remark.headline.replace("vectorized -> ", "→ ").replace("vectorized → ", "→ ");
        } else {
            String why = remark.reason == null ? remark.headline : remark.reason;
            int stop = why.indexOf(", but ");
            if (stop < 0) stop = why.indexOf(" -- ");
            String head = stop > 0 ? why.substring(0, stop) : why;
            text = (remark.isLoop() ? "not vectorized: " : "not inlined: ") + head;
        }
        text = text.replaceAll("\\s+", " ").trim();
        return text.length() > MAX_TEXT ? text.substring(0, MAX_TEXT - 1) + "…" : text;
    }

    /** Draws the hint in the editor's italic font, green for a win and muted for a refusal. */
    private static class Renderer implements com.intellij.openapi.editor.EditorCustomElementRenderer {
        private final String text;
        private final boolean positive;

        Renderer(MettleExplainReport.@NotNull Remark remark) {
            this.text = "  " + hintText(remark);
            this.positive = remark.positive;
        }

        @Override
        public int calcWidthInPixels(@NotNull Inlay inlay) {
            Editor editor = inlay.getEditor();
            return editor.getContentComponent().getFontMetrics(font(editor)).stringWidth(text) + 4;
        }

        @Override
        public void paint(@NotNull Inlay inlay, @NotNull Graphics g, @NotNull Rectangle region,
                          @NotNull TextAttributes attributes) {
            Editor editor = inlay.getEditor();
            Font font = font(editor);
            g.setFont(font);
            g.setColor(color());
            int baseline = region.y + editor.getAscent();
            g.drawString(text, region.x, baseline);
        }

        private static Font font(@NotNull Editor editor) {
            return editor.getColorsScheme().getFont(EditorFontType.ITALIC);
        }

        private @Nullable Color color() {
            return positive
                    ? new JBColor(new Color(0x2E7D32), new Color(0x6AAB73))
                    : new JBColor(new Color(0x8A6D3B), new Color(0xBB9A5A));
        }
    }
}
