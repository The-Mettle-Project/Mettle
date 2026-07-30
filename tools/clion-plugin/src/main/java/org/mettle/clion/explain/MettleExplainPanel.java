package org.mettle.clion.explain;

import com.intellij.icons.AllIcons;
import com.intellij.openapi.actionSystem.ActionManager;
import com.intellij.openapi.actionSystem.ActionToolbar;
import com.intellij.openapi.actionSystem.ActionUpdateThread;
import com.intellij.openapi.actionSystem.AnAction;
import com.intellij.openapi.actionSystem.AnActionEvent;
import com.intellij.openapi.actionSystem.DefaultActionGroup;
import com.intellij.openapi.actionSystem.ToggleAction;
import com.intellij.openapi.command.WriteCommandAction;
import com.intellij.openapi.editor.Document;
import com.intellij.openapi.fileEditor.FileDocumentManager;
import com.intellij.openapi.fileEditor.FileEditorManager;
import com.intellij.openapi.fileEditor.FileEditorManagerEvent;
import com.intellij.openapi.fileEditor.FileEditorManagerListener;
import com.intellij.openapi.fileEditor.OpenFileDescriptor;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.ui.SimpleToolWindowPanel;
import com.intellij.openapi.util.TextRange;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.psi.PsiFile;
import com.intellij.psi.PsiManager;
import com.intellij.ui.ColorUtil;
import com.intellij.ui.JBColor;
import com.intellij.ui.OnePixelSplitter;
import com.intellij.ui.components.JBLabel;
import com.intellij.ui.components.JBScrollPane;
import com.intellij.ui.treeStructure.Tree;
import com.intellij.util.messages.MessageBusConnection;
import com.intellij.util.ui.JBUI;
import com.intellij.util.ui.UIUtil;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.lang.MettleFileType;

import javax.swing.BorderFactory;
import javax.swing.JButton;
import javax.swing.JComponent;
import javax.swing.JEditorPane;
import javax.swing.JPanel;
import javax.swing.JTree;
import javax.swing.SwingConstants;
import javax.swing.tree.DefaultMutableTreeNode;
import javax.swing.tree.DefaultTreeModel;
import javax.swing.tree.TreePath;
import javax.swing.tree.TreeSelectionModel;
import java.awt.BorderLayout;
import java.awt.event.MouseAdapter;
import java.awt.event.MouseEvent;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * The optimization report, as a tool window.
 *
 * <p>Three things the prose report cannot do: it groups the decisions so a function's loops sit
 * together, it puts what changed since the last build at the top, and where the compiler verified
 * a fix it offers to make the edit.
 */
public class MettleExplainPanel extends SimpleToolWindowPanel implements com.intellij.openapi.Disposable {

    private final Project project;
    private final DefaultMutableTreeNode root = new DefaultMutableTreeNode();
    private final DefaultTreeModel treeModel = new DefaultTreeModel(root);
    private final Tree tree = new Tree(treeModel);
    private final JEditorPane detail = new JEditorPane("text/html", "");
    private final JBLabel summary = new JBLabel();
    private final JButton applyFix = new JButton("Apply fix");

    private @Nullable VirtualFile file;
    private MettleExplainService.@Nullable Snapshot snapshot;
    private boolean refusalsOnly;

    public MettleExplainPanel(@NotNull Project project) {
        super(true, true);
        this.project = project;

        tree.setRootVisible(false);
        tree.setShowsRootHandles(true);
        tree.getSelectionModel().setSelectionMode(TreeSelectionModel.SINGLE_TREE_SELECTION);
        tree.setCellRenderer(new MettleExplainTreeRenderer());
        tree.addTreeSelectionListener(event -> showDetail(selectedNode()));
        tree.addMouseListener(new MouseAdapter() {
            @Override
            public void mouseClicked(MouseEvent event) {
                if (event.getClickCount() == 2) navigateTo(selectedNode());
            }
        });

        detail.setEditable(false);
        detail.setBackground(UIUtil.getPanelBackground());
        detail.setBorder(JBUI.Borders.empty(8));

        applyFix.setEnabled(false);
        applyFix.addActionListener(event -> applySelectedFix());

        JPanel detailPanel = new JPanel(new BorderLayout());
        detailPanel.add(new JBScrollPane(detail), BorderLayout.CENTER);
        JPanel buttons = new JPanel(new BorderLayout());
        buttons.setBorder(JBUI.Borders.empty(4, 8));
        buttons.add(applyFix, BorderLayout.WEST);
        detailPanel.add(buttons, BorderLayout.SOUTH);

        OnePixelSplitter splitter = new OnePixelSplitter(false, 0.5f);
        splitter.setFirstComponent(new JBScrollPane(tree));
        splitter.setSecondComponent(detailPanel);

        summary.setBorder(JBUI.Borders.empty(6, 10));
        summary.setVerticalAlignment(SwingConstants.TOP);

        JPanel content = new JPanel(new BorderLayout());
        content.add(summary, BorderLayout.NORTH);
        content.add(splitter, BorderLayout.CENTER);
        setContent(content);
        setToolbar(buildToolbar());

        MessageBusConnection connection = project.getMessageBus().connect(this);
        connection.subscribe(MettleExplainService.TOPIC, (target, result) -> {
            if (target.equals(file)) show(target, result);
        });
        connection.subscribe(FileEditorManagerListener.FILE_EDITOR_MANAGER, new FileEditorManagerListener() {
            @Override
            public void selectionChanged(@NotNull FileEditorManagerEvent event) {
                VirtualFile selected = event.getNewFile();
                if (selected != null && selected.getFileType() == MettleFileType.INSTANCE) {
                    setFile(selected);
                }
            }
        });

        VirtualFile[] open = FileEditorManager.getInstance(project).getSelectedFiles();
        if (open.length > 0 && open[0].getFileType() == MettleFileType.INSTANCE) {
            setFile(open[0]);
        } else {
            renderEmpty("Open a Mettle file to see what the optimizer did with it.");
        }
    }

    // ------------------------------------------------------------ toolbar

    private JComponent buildToolbar() {
        DefaultActionGroup group = new DefaultActionGroup();
        group.add(new AnAction("Recompile and Refresh", "Recompile with --release --explain",
                AllIcons.Actions.Refresh) {
            @Override
            public void actionPerformed(@NotNull AnActionEvent event) {
                refresh();
            }

            @Override
            public void update(@NotNull AnActionEvent event) {
                event.getPresentation().setEnabled(file != null);
            }

            @Override
            public @NotNull ActionUpdateThread getActionUpdateThread() {
                return ActionUpdateThread.EDT;
            }
        });
        group.add(new ToggleAction("Refresh on Save", "Recompile the report when the file is saved",
                AllIcons.General.AutoscrollFromSource) {
            @Override
            public boolean isSelected(@NotNull AnActionEvent event) {
                return MettleExplainService.getInstance(project).isAutoRefresh();
            }

            @Override
            public void setSelected(@NotNull AnActionEvent event, boolean selected) {
                MettleExplainService.getInstance(project).setAutoRefresh(selected);
            }

            @Override
            public @NotNull ActionUpdateThread getActionUpdateThread() {
                return ActionUpdateThread.EDT;
            }
        });
        group.addSeparator();
        group.add(new ToggleAction("Refusals Only", "Hide what already vectorized or inlined",
                AllIcons.General.Filter) {
            @Override
            public boolean isSelected(@NotNull AnActionEvent event) {
                return refusalsOnly;
            }

            @Override
            public void setSelected(@NotNull AnActionEvent event, boolean selected) {
                refusalsOnly = selected;
                rebuildTree();
            }

            @Override
            public @NotNull ActionUpdateThread getActionUpdateThread() {
                return ActionUpdateThread.EDT;
            }
        });
        group.add(new ToggleAction("Show Hints in Editor",
                "Annotate loop and call lines with what the optimizer decided",
                AllIcons.Actions.PreviewDetails) {
            @Override
            public boolean isSelected(@NotNull AnActionEvent event) {
                return MettleExplainHints.getInstance(project).isEnabled();
            }

            @Override
            public void setSelected(@NotNull AnActionEvent event, boolean selected) {
                MettleExplainHints.getInstance(project).setEnabled(selected);
            }

            @Override
            public @NotNull ActionUpdateThread getActionUpdateThread() {
                return ActionUpdateThread.EDT;
            }
        });
        group.addSeparator();
        group.add(new AnAction("Apply All Verified Fixes",
                "Apply every fix the compiler verified by re-running its own optimizer",
                AllIcons.Actions.IntentionBulb) {
            @Override
            public void actionPerformed(@NotNull AnActionEvent event) {
                applyAllFixes();
            }

            @Override
            public void update(@NotNull AnActionEvent event) {
                event.getPresentation().setEnabled(!fixableRemarks().isEmpty());
            }

            @Override
            public @NotNull ActionUpdateThread getActionUpdateThread() {
                return ActionUpdateThread.EDT;
            }
        });
        group.addSeparator();
        group.add(new AnAction("Expand All", null, AllIcons.Actions.Expandall) {
            @Override
            public void actionPerformed(@NotNull AnActionEvent event) {
                for (int i = 0; i < tree.getRowCount(); i++) tree.expandRow(i);
            }

            @Override
            public @NotNull ActionUpdateThread getActionUpdateThread() {
                return ActionUpdateThread.EDT;
            }
        });
        group.add(new AnAction("Collapse All", null, AllIcons.Actions.Collapseall) {
            @Override
            public void actionPerformed(@NotNull AnActionEvent event) {
                for (int i = tree.getRowCount() - 1; i >= 0; i--) tree.collapseRow(i);
            }

            @Override
            public @NotNull ActionUpdateThread getActionUpdateThread() {
                return ActionUpdateThread.EDT;
            }
        });

        ActionToolbar toolbar = ActionManager.getInstance()
                .createActionToolbar("MettleExplain", group, true);
        toolbar.setTargetComponent(this);
        return toolbar.getComponent();
    }

    // -------------------------------------------------------------- state

    public void setFile(@NotNull VirtualFile file) {
        this.file = file;
        MettleExplainService service = MettleExplainService.getInstance(project);
        service.setShownFile(file);
        MettleExplainService.Snapshot cached = service.snapshot(file);
        if (cached != null) {
            show(file, cached);
        } else {
            this.snapshot = null;
            renderEmpty("No report for " + file.getName()
                    + " yet. Press Refresh to compile it with --release --explain.");
            rebuildTree();
        }
    }

    /** Compiles the shown file and refreshes when it lands. */
    public void refresh() {
        if (file != null) MettleExplainService.getInstance(project).refresh(file);
    }

    private void show(@NotNull VirtualFile target, MettleExplainService.@NotNull Snapshot result) {
        this.file = target;
        this.snapshot = result;
        rebuildTree();
        renderSummary();
        MettleExplainHints.getInstance(project).refresh(target);
    }

    // --------------------------------------------------------------- tree

    private void rebuildTree() {
        root.removeAllChildren();
        MettleExplainReport report = snapshot == null ? null : snapshot.report;
        if (report != null) {
            addChanges(report);
            addRemarks(report);
            addBackend(report);
            addFunctions(report);
            addPasses(report);
            addCallGraph(report);
            addMemory(report);
        }
        treeModel.reload();
        for (int i = 0; i < tree.getRowCount() && i < 40; i++) tree.expandRow(i);
        showDetail(null);
    }

    private void addChanges(@NotNull MettleExplainReport report) {
        if (report.changes.isEmpty()) return;
        int regressed = (int) report.changes.stream().filter(change -> !change.improved).count();
        DefaultMutableTreeNode section = new DefaultMutableTreeNode(MettleExplainNode.section(
                "Changes since the last build",
                regressed + " regressed, " + (report.changes.size() - regressed) + " improved",
                regressed > 0 ? AllIcons.General.Warning : AllIcons.General.InspectionsOK));
        for (MettleExplainReport.Change change : report.changes) {
            section.add(new DefaultMutableTreeNode(MettleExplainNode.change(change)));
        }
        root.add(section);
    }

    private void addRemarks(@NotNull MettleExplainReport report) {
        DefaultMutableTreeNode section = new DefaultMutableTreeNode(MettleExplainNode.section(
                "Loops and calls", null, AllIcons.Actions.ShowCode));
        for (Map.Entry<String, List<MettleExplainReport.Remark>> entry : report.byFunction().entrySet()) {
            List<MettleExplainReport.Remark> visible = new ArrayList<>();
            List<MettleExplainReport.Remark> routine = new ArrayList<>();
            for (MettleExplainReport.Remark remark : entry.getValue()) {
                if (refusalsOnly && remark.positive) continue;
                // A one-line stdlib wrapper going away is not news; the compiler says
                // so itself now, so those collapse into one row instead of burying
                // the decisions that matter.
                if (remark.trivial) routine.add(remark);
                else visible.add(remark);
            }
            if (visible.isEmpty() && routine.isEmpty()) continue;

            // Heaviest first: line order puts a cold one-liner above the loop that
            // costs the program its afternoon.
            visible.sort((a, b) -> Long.compare(report.costOf(b), report.costOf(a)));

            int refused = (int) visible.stream().filter(remark -> !remark.positive).count();
            MettleExplainReport.FunctionRow row = report.function(entry.getKey());
            DefaultMutableTreeNode function = new DefaultMutableTreeNode(MettleExplainNode.section(
                    entry.getKey(), functionSummary(row, refused, visible.size()),
                    AllIcons.Nodes.Function));
            for (MettleExplainReport.Remark remark : visible) {
                function.add(new DefaultMutableTreeNode(
                        MettleExplainNode.remark(remark, report.costAt(remark.function, remark.line))));
            }
            if (!routine.isEmpty()) {
                DefaultMutableTreeNode collapsed = new DefaultMutableTreeNode(
                        MettleExplainNode.section("routine inlines",
                                routine.size() + " one-line callees", AllIcons.General.InspectionsOK));
                for (MettleExplainReport.Remark remark : routine) {
                    collapsed.add(new DefaultMutableTreeNode(MettleExplainNode.remark(remark, null)));
                }
                function.add(collapsed);
            }
            section.add(function);
        }
        if (section.getChildCount() > 0) root.add(section);
    }

    private void addBackend(@NotNull MettleExplainReport report) {
        MettleExplainReport.Backend backend = report.backend;
        if (backend.total == 0) return;
        DefaultMutableTreeNode section = new DefaultMutableTreeNode(MettleExplainNode.section(
                "Backend coverage",
                backend.ok + "/" + backend.total + " functions register-allocated, "
                        + backend.coveragePercent() + "% of instructions",
                AllIcons.General.Information));
        for (MettleExplainReport.BackendGroup group : backend.groups) {
            DefaultMutableTreeNode node = new DefaultMutableTreeNode(MettleExplainNode.backendGroup(group));
            for (Map.Entry<String, Integer> member : group.members.entrySet()) {
                node.add(new DefaultMutableTreeNode(
                        MettleExplainNode.backendMember(member.getKey(), member.getValue())));
            }
            section.add(node);
        }
        root.add(section);
    }

    private void addMemory(@NotNull MettleExplainReport report) {
        if (report.memory.isEmpty()) return;
        DefaultMutableTreeNode section = new DefaultMutableTreeNode(MettleExplainNode.section(
                "Memory report", report.memory.size() + " findings", AllIcons.General.Warning));
        for (MettleExplainReport.MemoryNote note : report.memory) {
            section.add(new DefaultMutableTreeNode(MettleExplainNode.memory(note)));
        }
        root.add(section);
    }

    /** What the pipeline did to one function, in a line. */
    private static String functionSummary(MettleExplainReport.@Nullable FunctionRow row,
                                          int refused, int total) {
        StringBuilder text = new StringBuilder();
        text.append(refused == 0 ? "all clear" : refused + " of " + total + " refused");
        if (row != null && row.instructionsBefore > 0) {
            text.append("   ").append(row.instructionsBefore).append(" → ")
                    .append(row.instructionsAfter).append(" instructions");
            if (row.spills > 0) text.append(", ").append(row.spills).append(" spills");
            if (Boolean.FALSE.equals(row.backendOk)) text.append(", baseline codegen");
        }
        return text.toString();
    }

    private void addFunctions(@NotNull MettleExplainReport report) {
        if (report.functions.isEmpty()) return;
        int removed = report.functions.stream().mapToInt(MettleExplainReport.FunctionRow::instructionsRemoved).sum();
        DefaultMutableTreeNode section = new DefaultMutableTreeNode(MettleExplainNode.section(
                "Functions", report.functions.size() + " compiled, " + removed
                        + " instructions removed overall", AllIcons.Nodes.Folder));
        List<MettleExplainReport.FunctionRow> rows = new ArrayList<>(report.functions);
        rows.sort((a, b) -> Long.compare(b.hotCost, a.hotCost));
        for (MettleExplainReport.FunctionRow row : rows) {
            section.add(new DefaultMutableTreeNode(MettleExplainNode.function(row)));
        }
        root.add(section);
    }

    private void addPasses(@NotNull MettleExplainReport report) {
        if (report.passes.isEmpty()) return;
        int fired = (int) report.passes.stream().filter(pass -> pass.changedRuns > 0).count();
        DefaultMutableTreeNode section = new DefaultMutableTreeNode(MettleExplainNode.section(
                "Optimizer passes", fired + " of " + report.passes.size() + " changed anything",
                AllIcons.Actions.Profile));
        for (MettleExplainReport.PassRow pass : report.passes) {
            DefaultMutableTreeNode node = new DefaultMutableTreeNode(MettleExplainNode.pass(pass));
            for (MettleExplainReport.PassSite site : pass.sites) {
                node.add(new DefaultMutableTreeNode(MettleExplainNode.passSite(site)));
            }
            section.add(node);
        }
        root.add(section);
    }

    private void addCallGraph(@NotNull MettleExplainReport report) {
        if (report.callGraph.isEmpty()) return;
        int refused = report.callGraph.stream().mapToInt(edge -> edge.refused).sum();
        DefaultMutableTreeNode section = new DefaultMutableTreeNode(MettleExplainNode.section(
                "Call graph", report.callGraph.size() + " edges, " + refused + " sites left as calls",
                AllIcons.Hierarchy.Subtypes));
        List<MettleExplainReport.CallEdge> edges = new ArrayList<>(report.callGraph);
        edges.sort((a, b) -> Integer.compare(b.refused * b.calleeInstructions,
                a.refused * a.calleeInstructions));
        for (MettleExplainReport.CallEdge edge : edges) {
            section.add(new DefaultMutableTreeNode(MettleExplainNode.callEdge(edge)));
        }
        root.add(section);
    }

    private @Nullable MettleExplainNode selectedNode() {
        TreePath path = tree.getSelectionPath();
        if (path == null) return null;
        Object last = path.getLastPathComponent();
        if (!(last instanceof DefaultMutableTreeNode)) return null;
        Object value = ((DefaultMutableTreeNode) last).getUserObject();
        return value instanceof MettleExplainNode ? (MettleExplainNode) value : null;
    }

    // ------------------------------------------------------------- detail

    private void renderSummary() {
        if (snapshot == null) {
            summary.setText("");
            return;
        }
        if (snapshot.isFailure()) {
            summary.setText("<html><b>" + escape(snapshot.error == null ? "The report failed." : snapshot.error)
                    + "</b><br/><span style='color:" + hex(UIUtil.getInactiveTextColor()) + "'>"
                    + escape(firstLines(snapshot.output, 3)) + "</span></html>");
            return;
        }
        MettleExplainReport report = snapshot.report;
        MettleExplainReport.Stats stats = report.stats;
        String changes = !report.hasBaseline
                ? "first build, nothing to compare against"
                : (stats.changesRegressed > 0
                ? "<b style='color:" + hex(JBColor.RED) + "'>" + stats.changesRegressed + " regressed</b>, "
                + stats.changesImproved + " improved"
                : stats.changesImproved + " improved, none regressed");
        int removed = report.functions.stream()
                .mapToInt(MettleExplainReport.FunctionRow::instructionsRemoved).sum();
        String pipeline = report.functions.isEmpty() ? ""
                : " &nbsp;|&nbsp; the pipeline removed <b>" + removed + "</b> instructions across "
                + report.functions.size() + " functions";
        String worst = report.hotspots.isEmpty() ? ""
                : " &nbsp;|&nbsp; heaviest: <b>" + escape(report.hotspots.get(0).function) + ":"
                + report.hotspots.get(0).line + "</b>";
        summary.setText("<html><b>" + escape(report.source) + "</b> &nbsp; "
                + "<span style='color:" + hex(green()) + "'>" + stats.loopsVectorized + " loops vectorized</span>, "
                + stats.loopsScalar + " scalar &nbsp;|&nbsp; "
                + stats.callsInlined + " calls inlined, " + stats.callsRefused + " refused &nbsp;|&nbsp; "
                + stats.fixesVerified + " verified fixes &nbsp;|&nbsp; "
                + "backend " + report.backend.coveragePercent() + "%" + pipeline + worst
                + " &nbsp;|&nbsp; " + changes + "</html>");
    }

    private void showDetail(@Nullable MettleExplainNode node) {
        applyFix.setEnabled(false);
        if (node == null) {
            detail.setText(html(snapshot == null || snapshot.isFailure()
                    ? "<p style='color:" + hex(UIUtil.getInactiveTextColor()) + "'>"
                    + escape(snapshot == null ? "Nothing compiled yet." : snapshot.output) + "</p>"
                    : "<p style='color:" + hex(UIUtil.getInactiveTextColor())
                    + "'>Select a loop or a call to see why the optimizer decided what it did.</p>"));
            detail.setCaretPosition(0);
            return;
        }
        StringBuilder body = new StringBuilder();
        if (node.remark != null) {
            MettleExplainReport.Remark remark = node.remark;
            body.append("<h3 style='margin:0'>").append(escape(remark.function)).append(" &mdash; ")
                    .append(escape(remark.title())).append("</h3>");
            body.append("<p style='color:").append(hex(remark.positive ? green() : JBColor.foreground()))
                    .append("'><b>").append(escape(remark.headline)).append("</b></p>");
            appendField(body, "reason", remark.reason, null);
            appendField(body, "fix", remark.fix, null);
            appendField(body, "verified", remark.verified, green());
            if (remark.count > 1) {
                body.append("<p><i>").append(remark.count).append(" identical decisions, lines ")
                        .append(remark.line).append(" to ").append(remark.lineEnd).append("</i></p>");
            }
            MettleExplainFix fix = synthesize(remark);
            if (fix != null) {
                applyFix.setEnabled(true);
                applyFix.setText(fix.title());
            }
        } else if (node.change != null) {
            body.append("<h3 style='margin:0'>").append(escape(node.label)).append("</h3>");
            body.append("<p><b>").append(escape(node.detail)).append("</b></p>");
            appendField(body, "reason", node.change.reason, null);
        } else if (node.group != null) {
            body.append("<h3 style='margin:0'>Baseline codegen</h3>");
            appendField(body, "reason", node.group.reason, null);
            appendField(body, "consequence", node.group.consequence, null);
            appendField(body, "fix", node.group.fix, null);
        } else if (node.note != null) {
            body.append("<h3 style='margin:0'>").append(node.note.error ? "error" : "warning")
                    .append(" at line ").append(node.note.line).append("</h3>");
            body.append("<p><b>").append(escape(node.note.headline)).append("</b></p>");
            appendField(body, "fix", node.note.fix, null);
        } else if (node.pass != null) {
            appendPassDetail(body, node.pass);
        } else {
            body.append("<h3 style='margin:0'>").append(escape(node.label)).append("</h3>");
            if (node.detail != null) body.append("<p>").append(escape(node.detail)).append("</p>");
        }
        detail.setText(html(body.toString()));
        detail.setCaretPosition(0);
        if (!applyFix.isEnabled()) applyFix.setText("Apply fix");
    }

    /**
     * A pass, described rather than counted: what it did, then the lines it did it at with the
     * source that is on them. A column of bare "-1"s is data the reader still has to go and look up.
     */
    private void appendPassDetail(StringBuilder body, MettleExplainReport.@NotNull PassRow pass) {
        body.append("<h3 style='margin:0'>").append(escape(pass.pass)).append("</h3>");
        body.append("<p style='margin:6px 0'><b>").append(escape(pass.describeWork()))
                .append("</b></p>");
        body.append("<p style='margin:0;color:").append(hex(UIUtil.getInactiveTextColor()))
                .append("'>fired on ").append(pass.changedRuns).append(" of ").append(pass.runs)
                .append(pass.runs == 1 ? " run" : " runs");
        if (pass.instructionsRemoved != 0) {
            body.append(" &nbsp;&middot;&nbsp; net ").append(pass.instructionsRemoved > 0 ? "-" : "+")
                    .append(Math.abs(pass.instructionsRemoved)).append(" instructions");
        }
        body.append("</p>");

        if (pass.effects.size() > 1) {
            body.append("<p style='margin:6px 0;color:").append(hex(UIUtil.getInactiveTextColor()))
                    .append("'>").append(escape(pass.describeEffects(20))).append("</p>");
        }
        if (pass.sites.isEmpty()) return;

        Map<String, List<MettleExplainReport.PassSite>> grouped = pass.sitesByFunction();
        body.append("<p style='margin:10px 0 4px 0'>").append(pass.sites.size())
                .append(pass.sites.size() == 1 ? " line changed" : " lines changed")
                .append(grouped.size() > 1 ? " in " + grouped.size() + " functions" : "")
                .append("</p><table style='border:0'>");
        for (Map.Entry<String, List<MettleExplainReport.PassSite>> entry : grouped.entrySet()) {
            body.append("<tr><td colspan='3' style='padding-top:4px'><b>")
                    .append(escape(entry.getKey())).append("</b></td></tr>");
            for (MettleExplainReport.PassSite site : entry.getValue()) {
                body.append("<tr><td align='right' style='color:")
                        .append(hex(UIUtil.getInactiveTextColor())).append("'>")
                        .append(site.line).append("</td><td style='color:")
                        .append(hex(site.delta > 0 ? green() : JBColor.foreground()))
                        .append("'>&nbsp;").append(site.delta > 0 ? "-" : "+")
                        .append(Math.abs(site.delta)).append("&nbsp;</td><td><tt>")
                        .append(escape(sourceLine(site.line))).append("</tt></td></tr>");
            }
        }
        body.append("</table>");
    }

    /** The text on a line of the reported file, trimmed for the table. */
    private String sourceLine(int line) {
        if (file == null || line < 1) return "";
        Document document = FileDocumentManager.getInstance().getDocument(file);
        if (document == null || line > document.getLineCount()) return "";
        String text = document.getText(new TextRange(document.getLineStartOffset(line - 1),
                document.getLineEndOffset(line - 1))).trim();
        return text.length() > 64 ? text.substring(0, 63) + "\u2026" : text;
    }

    private void appendField(StringBuilder body, String label, @Nullable String value,
                             @Nullable java.awt.Color color) {
        if (value == null || value.isBlank()) return;
        body.append("<p><span style='color:").append(hex(UIUtil.getInactiveTextColor()))
                .append("'>").append(label).append(":</span> ");
        if (color != null) body.append("<span style='color:").append(hex(color)).append("'>");
        body.append(escape(value));
        if (color != null) body.append("</span>");
        body.append("</p>");
    }

    private void renderEmpty(@NotNull String message) {
        summary.setText("<html>" + escape(message) + "</html>");
        detail.setText(html("<p style='color:" + hex(UIUtil.getInactiveTextColor()) + "'>"
                + escape(message) + "</p>"));
    }

    // ---------------------------------------------------------------- fix

    private @Nullable MettleExplainFix synthesize(MettleExplainReport.@NotNull Remark remark) {
        PsiFile psiFile = psiFile();
        return psiFile == null ? null : MettleExplainFix.synthesize(psiFile, remark);
    }

    private @Nullable PsiFile psiFile() {
        return file == null ? null : PsiManager.getInstance(project).findFile(file);
    }

    private List<MettleExplainReport.Remark> fixableRemarks() {
        List<MettleExplainReport.Remark> fixable = new ArrayList<>();
        if (snapshot == null || snapshot.report == null || psiFile() == null) return fixable;
        for (MettleExplainReport.Remark remark : snapshot.report.remarks) {
            if (synthesize(remark) != null) fixable.add(remark);
        }
        return fixable;
    }

    private void applySelectedFix() {
        MettleExplainNode node = selectedNode();
        if (node == null || node.remark == null) return;
        MettleExplainFix fix = synthesize(node.remark);
        if (fix == null) return;
        fix.apply(project);
        applyFix.setEnabled(false);
        refresh();
    }

    /** Re-synthesizes before each edit, so one fix cannot invalidate the next one's offsets. */
    private void applyAllFixes() {
        List<MettleExplainReport.Remark> fixable = fixableRemarks();
        if (fixable.isEmpty()) return;
        PsiFile psiFile = psiFile();
        if (psiFile == null) return;
        WriteCommandAction.writeCommandAction(project, psiFile)
                .withName("Apply Verified Mettle Fixes")
                .run(() -> {
                    for (MettleExplainReport.Remark remark : fixable) {
                        MettleExplainFix fix = MettleExplainFix.synthesize(psiFile, remark);
                        if (fix != null) fix.applyEdits(project);
                    }
                });
        refresh();
    }

    private void navigateTo(@Nullable MettleExplainNode node) {
        if (node == null || !node.isNavigable() || file == null) return;
        new OpenFileDescriptor(project, file, Math.max(0, node.line - 1), 0).navigate(true);
    }

    // ------------------------------------------------------------ helpers

    private static String html(String body) {
        return "<html><body style='font-family:" + UIUtil.getLabelFont().getFamily()
                + "; font-size:" + UIUtil.getLabelFont().getSize() + "pt; margin:6px'>"
                + body + "</body></html>";
    }

    private static String hex(java.awt.Color color) {
        return "#" + ColorUtil.toHex(color);
    }

    private static JBColor green() {
        return new JBColor(new java.awt.Color(0x2E7D32), new java.awt.Color(0x6AAB73));
    }

    private static String firstLines(@Nullable String text, int count) {
        if (text == null || text.isBlank()) return "";
        String[] lines = text.split("\\R");
        StringBuilder result = new StringBuilder();
        for (int i = 0; i < Math.min(count, lines.length); i++) {
            if (i > 0) result.append(" / ");
            result.append(lines[i]);
        }
        return result.toString();
    }

    private static String escape(@Nullable String text) {
        if (text == null) return "";
        return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;");
    }

    @Override
    public void dispose() {
        // The message bus connection is parented to this panel, so the platform unhooks it here.
    }

    /** Paints the tree rows: label, then the compiler's own one-line verdict in grey. */
    private static class MettleExplainTreeRenderer extends com.intellij.ui.ColoredTreeCellRenderer {
        @Override
        public void customizeCellRenderer(@NotNull JTree tree, Object value, boolean selected,
                                          boolean expanded, boolean leaf, int row, boolean hasFocus) {
            if (!(value instanceof DefaultMutableTreeNode)) return;
            Object user = ((DefaultMutableTreeNode) value).getUserObject();
            if (!(user instanceof MettleExplainNode)) return;
            MettleExplainNode node = (MettleExplainNode) user;
            setIcon(node.icon);
            append(node.label);
            if (node.detail != null && !node.detail.isBlank()) {
                append("   " + node.detail, com.intellij.ui.SimpleTextAttributes.GRAYED_ATTRIBUTES);
            }
            setBorder(BorderFactory.createEmptyBorder());
        }
    }
}
