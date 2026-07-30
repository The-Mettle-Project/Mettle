package org.mettle.clion.explain;

import com.intellij.icons.AllIcons;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import javax.swing.Icon;

/** What one row of the report tree stands for. */
public class MettleExplainNode {

    public enum Kind {
        SECTION, REMARK, CHANGE, BACKEND_GROUP, BACKEND_MEMBER, MEMORY, TEXT,
        FUNCTION, PASS, PASS_SITE, CALL_EDGE
    }

    public final Kind kind;
    public final String label;
    public final @Nullable String detail;
    public final int line;
    public final @Nullable Icon icon;
    public final @Nullable MettleExplainReport.Remark remark;
    public final @Nullable MettleExplainReport.Change change;
    public final @Nullable MettleExplainReport.BackendGroup group;
    public final @Nullable MettleExplainReport.MemoryNote note;
    /** Schema 2 rows; set by the factory that built the node. */
    public MettleExplainReport.@Nullable FunctionRow function;
    public MettleExplainReport.@Nullable PassRow pass;
    public MettleExplainReport.@Nullable CallEdge edge;

    private MettleExplainNode(Kind kind, String label, @Nullable String detail, int line,
                              @Nullable Icon icon, @Nullable MettleExplainReport.Remark remark,
                              @Nullable MettleExplainReport.Change change,
                              @Nullable MettleExplainReport.BackendGroup group,
                              @Nullable MettleExplainReport.MemoryNote note) {
        this.kind = kind;
        this.label = label;
        this.detail = detail;
        this.line = line;
        this.icon = icon;
        this.remark = remark;
        this.change = change;
        this.group = group;
        this.note = note;
    }

    public static MettleExplainNode section(@NotNull String label, @Nullable String detail, Icon icon) {
        return new MettleExplainNode(Kind.SECTION, label, detail, 0, icon, null, null, null, null);
    }

    public static MettleExplainNode text(@NotNull String label) {
        return new MettleExplainNode(Kind.TEXT, label, null, 0, null, null, null, null, null);
    }

    public static MettleExplainNode remark(MettleExplainReport.@NotNull Remark remark) {
        return remark(remark, null);
    }

    /** With the backend's measurement for that loop, when it made one. */
    public static MettleExplainNode remark(MettleExplainReport.@NotNull Remark remark,
                                           MettleExplainReport.@Nullable LoopCost cost) {
        Icon icon = remark.positive
                ? AllIcons.General.InspectionsOK
                : (remark.isVerified() ? AllIcons.Actions.IntentionBulb : AllIcons.General.BalloonInformation);
        String detail = remark.headline;
        if (cost != null && cost.cyclesPerIter > 0) {
            detail = detail + "   " + cost.cycles()
                    + (cost.bottleneck == null ? "" : " on " + cost.bottleneck);
        }
        return new MettleExplainNode(Kind.REMARK, remark.title(), detail, remark.line,
                icon, remark, null, null, null);
    }

    public static MettleExplainNode function(MettleExplainReport.@NotNull FunctionRow row) {
        StringBuilder detail = new StringBuilder();
        detail.append(row.instructionsBefore).append(" → ").append(row.instructionsAfter)
                .append(" instructions");
        if (row.loops > 0) {
            detail.append(", ").append(row.loopsVectorized).append('/').append(row.loops)
                    .append(" loops vectorized");
        }
        if (row.spills > 0) detail.append(", ").append(row.spills).append(" spills");
        if (Boolean.FALSE.equals(row.backendOk)) detail.append(", baseline codegen");
        MettleExplainNode node = new MettleExplainNode(Kind.FUNCTION, row.name, detail.toString(),
                row.line, AllIcons.Nodes.Function, null, null, null, null);
        node.function = row;
        return node;
    }

    public static MettleExplainNode pass(MettleExplainReport.@NotNull PassRow row) {
        String detail = row.describeWork();
        if (row.changedRuns > 0 && !row.sites.isEmpty()) {
            detail += "   " + row.sites.size() + (row.sites.size() == 1 ? " line" : " lines");
        }
        Icon icon = row.changedRuns > 0 ? AllIcons.Actions.Profile : AllIcons.General.InspectionsOK;
        MettleExplainNode node = new MettleExplainNode(Kind.PASS, row.pass, detail, 0, icon,
                null, null, null, null);
        node.pass = row;
        return node;
    }

    /** One line a pass changed, as a child of the pass row. */
    public static MettleExplainNode passSite(MettleExplainReport.@NotNull PassSite site) {
        String detail = (site.delta > 0 ? "-" : "+") + Math.abs(site.delta) + " instructions";
        return new MettleExplainNode(Kind.PASS_SITE, site.function + ":" + site.line, detail,
                site.line, AllIcons.General.Information, null, null, null, null);
    }

    public static MettleExplainNode callEdge(MettleExplainReport.@NotNull CallEdge edge) {
        String detail = edge.inlined + " inlined, " + edge.refused + " left as calls, callee weighs "
                + edge.calleeInstructions;
        Icon icon = edge.refused == 0 ? AllIcons.General.InspectionsOK : AllIcons.General.BalloonInformation;
        MettleExplainNode node = new MettleExplainNode(Kind.CALL_EDGE,
                edge.caller + " → " + edge.callee, detail, 0, icon, null, null, null, null);
        node.edge = edge;
        return node;
    }

    public static MettleExplainNode change(MettleExplainReport.@NotNull Change change) {
        String what = change.kind.equals("loop")
                ? (change.improved ? "now vectorized" : "vectorized, now scalar")
                : (change.improved ? "now inlined" : "inlined, now a real call");
        Icon icon = change.improved ? AllIcons.General.InspectionsOK : AllIcons.General.Warning;
        return new MettleExplainNode(Kind.CHANGE,
                change.function + " (" + change.kind + " @ line " + change.line + ")",
                what, change.line, icon, null, change, null, null);
    }

    public static MettleExplainNode backendGroup(MettleExplainReport.@NotNull BackendGroup group) {
        String label = group.functions + (group.functions == 1 ? " function, " : " functions, ")
                + group.instructions + " instructions";
        return new MettleExplainNode(Kind.BACKEND_GROUP, label, group.reason, 0,
                AllIcons.General.Information, null, null, group, null);
    }

    public static MettleExplainNode backendMember(@NotNull String function, int instructions) {
        return new MettleExplainNode(Kind.BACKEND_MEMBER, function, instructions + " instructions", 0,
                AllIcons.Nodes.Function, null, null, null, null);
    }

    public static MettleExplainNode memory(MettleExplainReport.@NotNull MemoryNote note) {
        Icon icon = note.error ? AllIcons.General.Error : AllIcons.General.Warning;
        return new MettleExplainNode(Kind.MEMORY, "line " + note.line, note.headline, note.line,
                icon, null, null, null, note);
    }

    public boolean isNavigable() {
        return line > 0;
    }

    @Override
    public String toString() {
        return label;
    }
}
