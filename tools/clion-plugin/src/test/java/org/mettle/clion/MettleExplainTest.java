package org.mettle.clion;

import com.intellij.psi.PsiFile;
import com.intellij.testFramework.ParsingTestCase;
import org.mettle.clion.explain.MettleExplainFix;
import org.mettle.clion.explain.MettleExplainReport;
import org.mettle.clion.lang.MettleParserDefinition;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.TimeUnit;

/**
 * The optimization report end to end: the compiler's real {@code --explain-json} is parsed into the
 * model, and the fixes it verified are turned into edits against real syntax trees.
 */
public class MettleExplainTest extends ParsingTestCase {

    public MettleExplainTest() {
        super("", "mettle", new MettleParserDefinition());
    }

    @Override
    protected String getTestDataPath() {
        return "src/test/testData";
    }

    @Override
    protected boolean shouldContainTempFiles() {
        return false;
    }

    // -------------------------------------------------------------- model

    public void testParsesTheCompilersOwnReport() throws Exception {
        Path repository = Paths.get(System.getProperty("mettle.repo", ""));
        Path compiler = repository.resolve("bin").resolve(
                System.getProperty("os.name", "").toLowerCase().contains("win") ? "mettle.exe" : "mettle");
        Path source = repository.resolve("tests").resolve("explain_demo.mettle");
        if (!Files.isRegularFile(compiler) || !Files.isRegularFile(source)) {
            System.out.println("skipped: no compiler at " + compiler);
            return;
        }

        Path directory = Files.createTempDirectory("mettle-explain-test");
        Path output = directory.resolve("demo.obj");
        ProcessBuilder builder = new ProcessBuilder(compiler.toString(),
                "-i", source.toString(), "-o", output.toString(), "--release", "--explain-json");
        builder.directory(repository.toFile());
        builder.redirectErrorStream(true);
        Process process = builder.start();
        String output_ = new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
        assertTrue("the compile did not finish", process.waitFor(120, TimeUnit.SECONDS));
        assertEquals("explain build failed:\n" + output_, 0, process.exitValue());

        Path sidecar = directory.resolve("demo.explain.json");
        assertTrue("no sidecar at " + sidecar, Files.isRegularFile(sidecar));
        MettleExplainReport report = MettleExplainReport.parse(
                new String(Files.readAllBytes(sidecar), StandardCharsets.UTF_8));
        assertNotNull("the sidecar did not parse", report);

        assertEquals("the compiler emits schema 2", 2, report.schema);
        assertEquals("explain_demo.mettle", report.source);
        assertFalse("no remarks", report.remarks.isEmpty());
        assertTrue("no loop was reported as vectorized", report.stats.loopsVectorized > 0);
        assertTrue("no loop was reported as scalar", report.stats.loopsScalar > 0);
        assertTrue("no verified fixes", report.stats.fixesVerified > 0);
        assertTrue("no backend coverage", report.backend.total > 0);
        assertTrue("coverage out of range", report.backend.coveragePercent() >= 0
                && report.backend.coveragePercent() <= 100);

        MettleExplainReport.Remark widen = null;
        for (MettleExplainReport.Remark remark : report.remarks) {
            if ("sum_ints".equals(remark.function) && remark.isLoop() && !remark.positive) widen = remark;
        }
        assertNotNull("sum_ints should refuse to vectorize", widen);
        assertNotNull("the refusal should carry a reason", widen.reason);
        assertNotNull("the refusal should carry a fix", widen.fix);
        assertTrue("the fix should be verified", widen.isVerified());
        assertTrue("unexpected fix wording: " + widen.fix,
                widen.fix.contains("declare the accumulator `s` as int64"));

        assertFalse("remarks should group by function", report.byFunction().isEmpty());
        assertFalse("the loop line should be findable", report.at(widen.line).isEmpty());

        // Schema 2: the structured half the panel ranks and reasons with.
        assertEquals("the refusal should carry its diagnosis id",
                "int32-sum-narrow-acc", widen.code);
        assertTrue("a loop remark should carry its extent", widen.endLine > widen.line);
        assertTrue("stdlib one-liners should be marked trivial",
                report.remarks.stream().anyMatch(remark -> remark.trivial));
        assertTrue("call remarks should carry the callee weight",
                report.remarks.stream().anyMatch(r -> r.quantity("calleeInstructions", 0) > 0));

        assertFalse("no per-function table", report.functions.isEmpty());
        MettleExplainReport.FunctionRow row = report.function("sum_ints");
        assertNotNull("sum_ints missing from the function table", row);
        assertTrue("function weights missing", row.instructionsBefore > 0);

        assertFalse("no pass ledger", report.passes.isEmpty());
        assertTrue("the pass ledger recorded no work",
                report.passes.stream().anyMatch(pass -> pass.instructionsRemoved > 0));
        // A ledger entry says what it did and where, not just that it fired.
        MettleExplainReport.PassRow described = report.passes.stream()
                .filter(pass -> !pass.effects.isEmpty() && !pass.sites.isEmpty())
                .findFirst().orElse(null);
        assertNotNull("no pass reported its effects and sites", described);
        assertTrue("effects should name opcodes",
                described.describeEffects(4).matches(".*[+-]\\d+ \\w+.*"));
        assertTrue("sites should carry a line", described.sites.get(0).line > 0);
        // The ledger describes work, it does not report activity.
        String work = described.describeWork();
        assertTrue("a fired pass should describe what it did: " + work,
                work.startsWith("Removed ") || work.startsWith("Introduced ")
                        || work.startsWith("Replaced ") || work.startsWith("Rewrote "));
        assertFalse("the ledger should never say 'changed something'",
                work.contains("changed something"));
        assertFalse("sites should group by function", described.sitesByFunction().isEmpty());
        MettleExplainReport.PassRow vectorizer = report.passes.stream()
                .filter(pass -> pass.pass.equals("simd_affine_map_float"))
                .findFirst().orElse(null);
        assertNotNull("the vectorizer is missing from the ledger", vectorizer);
        assertTrue("the vectorizer should report the kernel it introduced",
                vectorizer.effects.entrySet().stream()
                        .anyMatch(e -> e.getKey().startsWith("simd_") && e.getValue() < 0));

        assertFalse("no loop cost model", report.loopCosts.isEmpty());
        MettleExplainReport.LoopCost cost = report.costAt(widen.function, widen.line);
        assertNotNull("the refused loop was not measured", cost);
        assertTrue("no cycle estimate", cost.cyclesPerIter > 0);
        assertNotNull("no bottleneck port", cost.bottleneck);

        assertFalse("no call graph", report.callGraph.isEmpty());
        assertFalse("no hotspot ranking", report.hotspots.isEmpty());
        for (int i = 1; i < report.hotspots.size(); i++) {
            assertTrue("hotspots are not ranked by cost",
                    report.hotspots.get(i - 1).cost >= report.hotspots.get(i).cost);
        }
        assertTrue("the refused loop should have a cost", report.costOf(widen) > 0);
    }

    public void testHandlesAnEmptyOrBrokenSidecar() {
        assertNull(MettleExplainReport.parse("not json"));
        assertNull(MettleExplainReport.parse("{\"schema\":1}"));
        MettleExplainReport minimal = MettleExplainReport.parse(
                "{\"schema\":1,\"source\":\"a.mettle\",\"remarks\":[],\"memory\":[],"
                        + "\"backend\":{\"ok\":0,\"total\":0,\"groups\":[]},\"stats\":{}}");
        assertNotNull(minimal);
        assertTrue(minimal.isEmpty());
    }

    public void testRegressionsSortFirst() {
        MettleExplainReport report = MettleExplainReport.parse(
                "{\"schema\":1,\"source\":\"a.mettle\",\"changes\":{\"baseline\":true,\"entries\":["
                        + "{\"kind\":\"loop\",\"fn\":\"good\",\"line\":3,\"direction\":\"improved\",\"reason\":null},"
                        + "{\"kind\":\"call\",\"fn\":\"bad\",\"line\":9,\"direction\":\"regressed\","
                        + "\"reason\":\"the callee grew\"}]},"
                        + "\"remarks\":[],\"memory\":[],\"backend\":{},\"stats\":{}}");
        assertNotNull(report);
        assertTrue(report.hasBaseline);
        assertEquals(2, report.changes.size());
        assertFalse("regressions come first", report.changes.get(0).improved);
        assertEquals("bad", report.changes.get(0).function);
        assertEquals("the callee grew", report.changes.get(0).reason);
    }

    // --------------------------------------------------------------- fixes

    public void testWidensAnInt32Accumulator() {
        String source = """
                fn sum_ints(a: int32*, n: int32) -> int32 {
                    var s: int32 = 0;
                    for i in 0..n {
                        s = s + a[i];
                    }
                    return s;
                }
                """;
        String fixed = applyFix(source, remark("loop", "sum_ints", 3,
                "declare the accumulator `s` as int64",
                "simulated that fix and re-ran the optimizer: this loop then vectorizes"));
        assertTrue("the accumulator should widen:\n" + fixed, fixed.contains("var s: int64 = 0;"));
    }

    public void testWidensAByteSumAccumulatorAndItsCast() {
        String source = """
                fn sum_bytes(data: uint8*, n: int32) -> int32 {
                    var total: int32 = 0;
                    for i in 0..n {
                        total = total + (int32)data[i];
                    }
                    return total;
                }
                """;
        String fixed = applyFix(source, remark("loop", "sum_bytes", 3,
                "declare the accumulator as int64 (sum bytes as `total = total + (int64)data[i]`)",
                "simulated that fix and re-ran the optimizer: this loop then vectorizes -> vpsadbw"));
        assertTrue("the accumulator should widen:\n" + fixed, fixed.contains("var total: int64 = 0;"));
        assertTrue("the cast should widen too:\n" + fixed, fixed.contains("total + (int64)data[i]"));
    }

    public void testRemovesNoinlineFromTheCallee() {
        String source = """
                @noinline fn damp(x: float32) -> float32 { return x * 0.5; }

                fn apply_damp(a: float32*, n: int32) {
                    for i in 0..n {
                        a[i] = damp(a[i]);
                    }
                }
                """;
        String fixed = applyFix(source, remark("loop", "apply_damp", 4,
                "remove `@noinline` from `damp` (it blocks this loop's vectorization), "
                        + "or hoist the call out of the loop",
                "simulated removing `@noinline` from `damp` ... this loop then vectorizes"));
        assertTrue("the decorator should be gone:\n" + fixed, fixed.startsWith("fn damp"));
    }

    public void testMarksACalleeInline() {
        String source = """
                fn scale(x: int32) -> int32 { return x * 3; }

                fn apply(a: int32*, n: int32) {
                    for i in 0..n {
                        a[i] = scale(a[i]);
                    }
                }
                """;
        String fixed = applyFix(source, remark("loop", "apply", 4,
                "make `scale` inline-eligible (small body, or mark it @inline), "
                        + "or hoist the call out of the loop",
                "simulated marking `scale` @inline: this loop then vectorizes"));
        assertTrue("the decorator should be added:\n" + fixed, fixed.startsWith("@inline fn scale"));
    }

    public void testOffersNothingWithoutAVerifiedFix() {
        String source = """
                fn sum_ints(a: int32*, n: int32) -> int32 {
                    var s: int32 = 0;
                    for i in 0..n {
                        s = s + a[i];
                    }
                    return s;
                }
                """;
        PsiFile file = createPsiFile("unverified", source);
        // Advice the compiler did not prove stays advice: no button, no edit.
        assertNull(MettleExplainFix.synthesize(file,
                remark("loop", "sum_ints", 3, "declare the accumulator `s` as int64", null)));
        // Prose advice has nothing mechanical behind it either.
        assertNull(MettleExplainFix.synthesize(file,
                remark("loop", "sum_ints", 3, "use int32 elements", "simulated that fix")));
        // Neither does a name that is not in the file.
        assertNull(MettleExplainFix.synthesize(file,
                remark("loop", "sum_ints", 3, "remove `@noinline` from `nowhere`", "simulated")));
    }

    // ------------------------------------------------------------ helpers

    /** Builds a Remark the way the compiler's JSON would. */
    private static MettleExplainReport.Remark remark(String kind, String function, int line,
                                                     String fix, String verified) {
        Map<String, Object> json = new LinkedHashMap<>();
        json.put("kind", kind);
        json.put("fn", function);
        json.put("entity", kind);
        json.put("line", (double) line);
        json.put("positive", Boolean.FALSE);
        json.put("headline", "NOT vectorized");
        json.put("reason", "a reason");
        json.put("fix", fix);
        json.put("verified", verified);
        MettleExplainReport report = MettleExplainReport.parse(
                "{\"schema\":1,\"source\":\"x\",\"remarks\":[" + object(json) + "],\"stats\":{}}");
        assertNotNull(report);
        return report.remarks.get(0);
    }

    private static String object(Map<String, Object> json) {
        StringBuilder text = new StringBuilder("{");
        for (Map.Entry<String, Object> entry : json.entrySet()) {
            if (text.length() > 1) text.append(',');
            text.append('"').append(entry.getKey()).append("\":");
            Object value = entry.getValue();
            if (value == null) text.append("null");
            else if (value instanceof String) text.append('"').append(escape((String) value)).append('"');
            else if (value instanceof Double) text.append(((Double) value).intValue());
            else text.append(value);
        }
        return text.append('}').toString();
    }

    private static String escape(String text) {
        return text.replace("\\", "\\\\").replace("\"", "\\\"");
    }

    /**
     * Synthesizes the fix for {@code remark} against {@code source} and returns the edited text.
     * The edits are previewed rather than committed: a light test fixture has no editor document
     * behind its files, and the offsets are what this is really checking.
     */
    private String applyFix(String source, MettleExplainReport.Remark remark) {
        PsiFile file = createPsiFile("fixture" + remark.function, source);
        MettleExplainFix fix = MettleExplainFix.synthesize(file, remark);
        assertNotNull("no fix was synthesized for: " + remark.fix, fix);
        return fix.preview(file);
    }
}
