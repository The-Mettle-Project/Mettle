package org.mettle.clion.explain;

import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.util.MiniJson;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * The compiler's {@code --explain-json} sidecar, as data.
 *
 * <p>Schema 1: every optimization decision in the compiled file, what changed since the last
 * explain build, the memory findings, and how much of the program reached the register-allocating
 * backend. Fields the compiler may omit are nullable here rather than defaulted, so the UI can
 * tell "no reason given" from "reason: none".
 */
public class MettleExplainReport {

    /** One loop or call the optimizer decided something about. */
    public static class Remark {
        public final String kind;        // "loop" or "call"
        public final String function;
        public final String entity;      // "loop", "call", ...
        public final int line;
        public final boolean positive;
        public final String headline;
        public final @Nullable String reason;
        public final @Nullable String fix;
        public final @Nullable String verified;
        public final @Nullable String callee;
        public final int count;          // >1 when a run of identical refusals was folded
        public final int lineEnd;
        public final int depth;          // loop nesting depth, 0 when not a loop

        // schema 2
        /** Stable decision id: "control-flow", "callee-noinline", "inlined", ... */
        public final @Nullable String code;
        public final int column;
        /** Last line of the construct, so a whole loop can be highlighted. 0 when unknown. */
        public final int endLine;
        /** Routine housekeeping - a one-line stdlib inline - that a reader can collapse. */
        public final boolean trivial;
        /** Whatever the pass measured: unroll factor, callee weight, trip count. */
        public final Map<String, Integer> quantities;

        @SuppressWarnings("unchecked")
        Remark(Map<String, Object> json) {
            kind = orEmpty(MiniJson.string(json, "kind"));
            function = orEmpty(MiniJson.string(json, "fn"));
            entity = orEmpty(MiniJson.string(json, "entity"));
            line = MiniJson.integer(json, "line", 0);
            positive = Boolean.TRUE.equals(json.get("positive"));
            headline = orEmpty(MiniJson.string(json, "headline"));
            reason = MiniJson.string(json, "reason");
            fix = MiniJson.string(json, "fix");
            verified = MiniJson.string(json, "verified");
            callee = MiniJson.string(json, "callee");
            count = MiniJson.integer(json, "count", 1);
            lineEnd = MiniJson.integer(json, "lineEnd", line);
            depth = MiniJson.integer(json, "depth", 0);

            code = MiniJson.string(json, "code");
            column = MiniJson.integer(json, "column", 0);
            endLine = MiniJson.integer(json, "endLine", 0);
            trivial = Boolean.TRUE.equals(json.get("trivial"));
            Map<String, Integer> measured = new LinkedHashMap<>();
            Object raw = json.get("quantities");
            if (raw instanceof Map) {
                for (Map.Entry<String, Object> entry : ((Map<String, Object>) raw).entrySet()) {
                    if (entry.getValue() instanceof Double) {
                        measured.put(entry.getKey(), (int) Math.round((Double) entry.getValue()));
                    }
                }
            }
            quantities = measured;
        }

        public int quantity(@NotNull String name, int fallback) {
            Integer value = quantities.get(name);
            return value == null ? fallback : value;
        }

        public boolean isLoop() {
            return "loop".equals(kind);
        }

        /** The compiler applied the suggested fix to a clone and re-ran the optimizer. */
        public boolean isVerified() {
            return verified != null && !verified.isBlank();
        }

        public @NotNull String title() {
            String what = isLoop() ? "loop" : (callee == null ? "call" : "call to " + callee);
            String where = count > 1 ? "lines " + line + "-" + lineEnd : "line " + line;
            return what + " @ " + where;
        }
    }

    /** A decision that flipped since the previous explain build. */
    public static class Change {
        public final String kind;
        public final String function;
        public final int line;
        public final boolean improved;
        public final @Nullable String reason;

        Change(Map<String, Object> json) {
            kind = orEmpty(MiniJson.string(json, "kind"));
            function = orEmpty(MiniJson.string(json, "fn"));
            line = MiniJson.integer(json, "line", 0);
            improved = "improved".equals(MiniJson.string(json, "direction"));
            reason = MiniJson.string(json, "reason");
        }
    }

    /** A compile-time memory-safety finding, as the explain report renders it. */
    public static class MemoryNote {
        public final boolean error;
        public final int line;
        public final String headline;
        public final @Nullable String fix;

        MemoryNote(Map<String, Object> json) {
            error = "error".equals(MiniJson.string(json, "severity"));
            line = MiniJson.integer(json, "line", 0);
            headline = orEmpty(MiniJson.string(json, "headline"));
            fix = MiniJson.string(json, "fix");
        }
    }

    /** Functions that fell back to baseline codegen, grouped by why. */
    public static class BackendGroup {
        public final String reason;
        public final int functions;
        public final int instructions;
        public final @Nullable String consequence;
        public final @Nullable String fix;
        public final Map<String, Integer> members = new LinkedHashMap<>();

        @SuppressWarnings("unchecked")
        BackendGroup(Map<String, Object> json) {
            reason = orEmpty(MiniJson.string(json, "reason"));
            functions = MiniJson.integer(json, "functions", 0);
            instructions = MiniJson.integer(json, "instructions", 0);
            consequence = MiniJson.string(json, "consequence");
            fix = MiniJson.string(json, "fix");
            for (Object member : MiniJson.array(json, "members")) {
                if (!(member instanceof Map)) continue;
                Map<String, Object> entry = (Map<String, Object>) member;
                members.put(orEmpty(MiniJson.string(entry, "fn")),
                        MiniJson.integer(entry, "instructions", 0));
            }
        }
    }

    public static class Backend {
        public final int ok;
        public final int total;
        public final int instructions;
        public final int okInstructions;
        public final List<BackendGroup> groups = new ArrayList<>();

        @SuppressWarnings("unchecked")
        Backend(@Nullable Map<String, Object> json) {
            ok = MiniJson.integer(json, "ok", 0);
            total = MiniJson.integer(json, "total", 0);
            instructions = MiniJson.integer(json, "instructions", 0);
            okInstructions = MiniJson.integer(json, "okInstructions", 0);
            for (Object group : MiniJson.array(json, "groups")) {
                if (group instanceof Map) groups.add(new BackendGroup((Map<String, Object>) group));
            }
        }

        /** Share of optimized IR instructions that reached the register-allocating backend. */
        public int coveragePercent() {
            return instructions == 0 ? 100 : (int) Math.round(100.0 * okInstructions / instructions);
        }
    }

    public static class Stats {
        public final int loopsVectorized;
        public final int loopsScalar;
        public final int fixesVerified;
        public final int callsInlined;
        public final int callsRefused;
        public final int changesImproved;
        public final int changesRegressed;
        public final boolean hadBaseline;

        Stats(@Nullable Map<String, Object> json) {
            loopsVectorized = MiniJson.integer(json, "loopsVectorized", 0);
            loopsScalar = MiniJson.integer(json, "loopsScalar", 0);
            fixesVerified = MiniJson.integer(json, "fixesVerified", 0);
            callsInlined = MiniJson.integer(json, "callsInlined", 0);
            callsRefused = MiniJson.integer(json, "callsRefused", 0);
            changesImproved = MiniJson.integer(json, "changesImproved", 0);
            changesRegressed = MiniJson.integer(json, "changesRegressed", 0);
            hadBaseline = Boolean.TRUE.equals(json == null ? null : json.get("hadBaseline"));
        }
    }

    /** A function's whole story: what it weighed, what happened to it, what it costs. */
    public static class FunctionRow {
        public final String name;
        public final int line;
        public final int instructionsBefore;
        public final int instructionsAfter;
        public final int loops;
        public final int loopsVectorized;
        public final int callsInlined;
        public final int callsRefused;
        public final @Nullable Boolean backendOk;
        public final @Nullable String backendReason;
        public final int spills;
        public final int regsUsed;
        public final int throughput;   // centicycles of reciprocal throughput
        public final long hotCost;
        public final int vectorOps;

        FunctionRow(Map<String, Object> json) {
            name = orEmpty(MiniJson.string(json, "fn"));
            line = MiniJson.integer(json, "line", 0);
            instructionsBefore = MiniJson.integer(json, "instructionsBefore", 0);
            instructionsAfter = MiniJson.integer(json, "instructionsAfter", 0);
            loops = MiniJson.integer(json, "loops", 0);
            loopsVectorized = MiniJson.integer(json, "loopsVectorized", 0);
            callsInlined = MiniJson.integer(json, "callsInlined", 0);
            callsRefused = MiniJson.integer(json, "callsRefused", 0);
            Object ok = json.get("backendOk");
            backendOk = ok instanceof Boolean ? (Boolean) ok : null;
            backendReason = MiniJson.string(json, "backendReason");
            spills = MiniJson.integer(json, "spills", 0);
            regsUsed = MiniJson.integer(json, "regsUsed", 0);
            throughput = MiniJson.integer(json, "throughput", 0);
            hotCost = MiniJson.integer(json, "hotCost", 0);
            vectorOps = MiniJson.integer(json, "vectorOps", 0);
        }

        /** What the whole pipeline achieved on this function. */
        public int instructionsRemoved() {
            return instructionsBefore - instructionsAfter;
        }
    }

    /** What the backend measured about one loop, whatever the optimizer said about it. */
    public static class LoopCost {
        public final String function;
        public final int line;
        public final int endLine;
        public final int depth;
        public final int cyclesPerIter;   // centicycles: 720 is 7.2 cycles
        public final @Nullable String bottleneck;
        public final boolean hasKernel;
        public final boolean estimated;

        LoopCost(Map<String, Object> json) {
            function = orEmpty(MiniJson.string(json, "fn"));
            line = MiniJson.integer(json, "line", 0);
            endLine = MiniJson.integer(json, "endLine", 0);
            depth = MiniJson.integer(json, "depth", 0);
            cyclesPerIter = MiniJson.integer(json, "cyclesPerIter", 0);
            bottleneck = MiniJson.string(json, "bottleneck");
            hasKernel = Boolean.TRUE.equals(json.get("hasKernel"));
            estimated = Boolean.TRUE.equals(json.get("estimated"));
        }

        public @NotNull String cycles() {
            return String.format("%.2f cycles/iter", cyclesPerIter / 100.0);
        }
    }

    /** One (function, line) a pass moved instructions at. */
    public static class PassSite {
        public final String function;
        public final int line;
        public final int delta;   // positive = instructions removed here

        PassSite(Map<String, Object> json) {
            function = orEmpty(MiniJson.string(json, "fn"));
            line = MiniJson.integer(json, "line", 0);
            delta = MiniJson.integer(json, "delta", 0);
        }
    }

    /** One optimization pass's effect on this file. */
    public static class PassRow {
        public final String pass;
        public final int runs;
        public final int changedRuns;
        public final int instructionsRemoved;
        /** Per-opcode net change: positive removed it, negative introduced it. */
        public final Map<String, Integer> effects = new LinkedHashMap<>();
        /** Where it happened, heaviest first. */
        public final List<PassSite> sites = new ArrayList<>();

        @SuppressWarnings("unchecked")
        PassRow(Map<String, Object> json) {
            pass = orEmpty(MiniJson.string(json, "pass"));
            runs = MiniJson.integer(json, "runs", 0);
            changedRuns = MiniJson.integer(json, "changedRuns", 0);
            instructionsRemoved = MiniJson.integer(json, "instructionsRemoved", 0);
            Object raw = json.get("effects");
            if (raw instanceof Map) {
                for (Map.Entry<String, Object> entry : ((Map<String, Object>) raw).entrySet()) {
                    if (entry.getValue() instanceof Double) {
                        effects.put(entry.getKey(), (int) Math.round((Double) entry.getValue()));
                    }
                }
            }
            for (Object site : MiniJson.array(json, "sites")) {
                if (site instanceof Map) sites.add(new PassSite((Map<String, Object>) site));
            }
        }

        /** The headline effects, as "-8 load, -8 store, +4 assign". */
        public @NotNull String describeEffects(int limit) {
            List<Map.Entry<String, Integer>> ordered = new ArrayList<>(effects.entrySet());
            ordered.sort((a, b) -> Integer.compare(Math.abs(b.getValue()), Math.abs(a.getValue())));
            StringBuilder text = new StringBuilder();
            for (int i = 0; i < ordered.size() && i < limit; i++) {
                if (i > 0) text.append(", ");
                int value = ordered.get(i).getValue();
                text.append(value > 0 ? "-" : "+").append(Math.abs(value)).append(' ')
                        .append(ordered.get(i).getKey());
            }
            if (ordered.size() > limit) {
                text.append(", ").append(ordered.size() - limit).append(" more");
            }
            return text.toString();
        }

        /**
         * What the pass did, as a sentence. A pass that turns 37 binaries into 37 assignments has
         * a net delta of zero and has plainly done something; "changed something" describes
         * neither. Positive effects are opcodes it removed, negative ones it introduced.
         */
        public @NotNull String describeWork() {
            List<Map.Entry<String, Integer>> removed = new ArrayList<>();
            List<Map.Entry<String, Integer>> added = new ArrayList<>();
            for (Map.Entry<String, Integer> effect : effects.entrySet()) {
                if (effect.getValue() > 0) removed.add(effect);
                else if (effect.getValue() < 0) added.add(effect);
            }
            removed.sort((a, b) -> Integer.compare(b.getValue(), a.getValue()));
            added.sort((a, b) -> Integer.compare(a.getValue(), b.getValue()));

            if (changedRuns == 0) return "Never fired.";
            if (removed.isEmpty() && added.isEmpty()) return "Rewrote instructions in place.";
            if (added.isEmpty()) return "Removed " + list(removed, 3) + ".";
            if (removed.isEmpty()) return "Introduced " + list(added, 3) + ".";
            return "Replaced " + list(removed, 3) + " with " + list(added, 2) + ".";
        }

        private static String list(List<Map.Entry<String, Integer>> entries, int limit) {
            StringBuilder text = new StringBuilder();
            int shown = Math.min(entries.size(), limit);
            for (int i = 0; i < shown; i++) {
                if (i > 0) text.append(i == shown - 1 && entries.size() <= limit ? " and " : ", ");
                text.append(Math.abs(entries.get(i).getValue())).append(' ')
                        .append(entries.get(i).getKey());
            }
            if (entries.size() > limit) {
                text.append(" and ").append(entries.size() - limit).append(" other kinds");
            }
            return text.toString();
        }

        /** The lines it changed, grouped by the function they are in. */
        public @NotNull Map<String, List<PassSite>> sitesByFunction() {
            Map<String, List<PassSite>> grouped = new LinkedHashMap<>();
            for (PassSite site : sites) {
                grouped.computeIfAbsent(site.function, name -> new ArrayList<>()).add(site);
            }
            for (List<PassSite> group : grouped.values()) {
                group.sort((a, b) -> Integer.compare(a.line, b.line));
            }
            return grouped;
        }
    }

    /** One caller/callee pair and what became of its call sites. */
    public static class CallEdge {
        public final String caller;
        public final String callee;
        public final int inlined;
        public final int refused;
        public final int calleeInstructions;

        CallEdge(Map<String, Object> json) {
            caller = orEmpty(MiniJson.string(json, "caller"));
            callee = orEmpty(MiniJson.string(json, "callee"));
            inlined = MiniJson.integer(json, "inlined", 0);
            refused = MiniJson.integer(json, "refused", 0);
            calleeInstructions = MiniJson.integer(json, "calleeInstructions", 0);
        }
    }

    /** A decision with a modelled cost, for ranking. */
    public static class Hotspot {
        public final String function;
        public final int line;
        public final String kind;
        public final @Nullable String code;
        public final long cost;

        Hotspot(Map<String, Object> json) {
            function = orEmpty(MiniJson.string(json, "fn"));
            line = MiniJson.integer(json, "line", 0);
            kind = orEmpty(MiniJson.string(json, "kind"));
            code = MiniJson.string(json, "code");
            cost = MiniJson.integer(json, "cost", 0);
        }
    }

    public final int schema;
    public final String source;
    public final boolean hasBaseline;
    public final List<Change> changes = new ArrayList<>();
    public final List<Remark> remarks = new ArrayList<>();
    public final List<MemoryNote> memory = new ArrayList<>();
    public final List<FunctionRow> functions = new ArrayList<>();
    public final List<LoopCost> loopCosts = new ArrayList<>();
    public final List<PassRow> passes = new ArrayList<>();
    public final List<CallEdge> callGraph = new ArrayList<>();
    public final List<Hotspot> hotspots = new ArrayList<>();
    public final Backend backend;
    public final Stats stats;

    @SuppressWarnings("unchecked")
    private MettleExplainReport(@NotNull Map<String, Object> json) {
        schema = MiniJson.integer(json, "schema", 0);
        source = orEmpty(MiniJson.string(json, "source"));

        Object changesValue = json.get("changes");
        Map<String, Object> changesJson =
                changesValue instanceof Map ? (Map<String, Object>) changesValue : null;
        hasBaseline = changesJson != null && Boolean.TRUE.equals(changesJson.get("baseline"));
        for (Object entry : MiniJson.array(changesJson, "entries")) {
            if (entry instanceof Map) changes.add(new Change((Map<String, Object>) entry));
        }
        // regressions first: they are the reason anyone opens this panel after a build
        changes.sort((a, b) -> Boolean.compare(a.improved, b.improved));

        for (Object remark : MiniJson.array(json, "remarks")) {
            if (remark instanceof Map) remarks.add(new Remark((Map<String, Object>) remark));
        }
        for (Object note : MiniJson.array(json, "memory")) {
            if (note instanceof Map) memory.add(new MemoryNote((Map<String, Object>) note));
        }
        for (Object row : MiniJson.array(json, "functions")) {
            if (row instanceof Map) functions.add(new FunctionRow((Map<String, Object>) row));
        }
        for (Object row : MiniJson.array(json, "loops")) {
            if (row instanceof Map) loopCosts.add(new LoopCost((Map<String, Object>) row));
        }
        for (Object row : MiniJson.array(json, "passes")) {
            if (row instanceof Map) passes.add(new PassRow((Map<String, Object>) row));
        }
        for (Object row : MiniJson.array(json, "callGraph")) {
            if (row instanceof Map) callGraph.add(new CallEdge((Map<String, Object>) row));
        }
        for (Object row : MiniJson.array(json, "hotspots")) {
            if (row instanceof Map) hotspots.add(new Hotspot((Map<String, Object>) row));
        }

        Object backendValue = json.get("backend");
        backend = new Backend(backendValue instanceof Map ? (Map<String, Object>) backendValue : null);
        Object statsValue = json.get("stats");
        stats = new Stats(statsValue instanceof Map ? (Map<String, Object>) statsValue : null);
    }

    /** The backend's measurement for the loop at {@code line}, when it made one. */
    public @Nullable LoopCost costAt(@NotNull String function, int line) {
        for (LoopCost cost : loopCosts) {
            if (cost.line == line && cost.function.equals(function)) return cost;
        }
        return null;
    }

    /** The modelled cost of a decision, for ranking; 0 when nothing measured it. */
    public long costOf(@NotNull Remark remark) {
        for (Hotspot hotspot : hotspots) {
            if (hotspot.line == remark.line && hotspot.function.equals(remark.function)
                    && hotspot.kind.equals(remark.isLoop() ? "loop" : "call")) {
                return hotspot.cost;
            }
        }
        return 0;
    }

    public @Nullable FunctionRow function(@NotNull String name) {
        for (FunctionRow row : functions) {
            if (row.name.equals(name)) return row;
        }
        return null;
    }

    public static @Nullable MettleExplainReport parse(@NotNull String json) {
        Map<String, Object> root = MiniJson.parseObject(json);
        if (root == null || !root.containsKey("remarks")) return null;
        return new MettleExplainReport(root);
    }

    /** Remarks grouped by the function they belong to, in source order. */
    public @NotNull Map<String, List<Remark>> byFunction() {
        Map<String, List<Remark>> grouped = new LinkedHashMap<>();
        List<Remark> sorted = new ArrayList<>(remarks);
        sorted.sort((a, b) -> a.line - b.line);
        for (Remark remark : sorted) {
            grouped.computeIfAbsent(remark.function, name -> new ArrayList<>()).add(remark);
        }
        return grouped;
    }

    /** The remarks on one source line, for gutter icons and inlay hints. */
    public @NotNull List<Remark> at(int line) {
        List<Remark> hits = new ArrayList<>();
        for (Remark remark : remarks) {
            if (remark.line == line) hits.add(remark);
        }
        return hits;
    }

    public boolean isEmpty() {
        return remarks.isEmpty() && memory.isEmpty() && backend.groups.isEmpty();
    }

    private static @NotNull String orEmpty(@Nullable String value) {
        return value == null ? "" : value;
    }
}
