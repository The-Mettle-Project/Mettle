package org.mettle.clion;

import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiErrorElement;
import com.intellij.psi.PsiFile;
import com.intellij.psi.util.PsiTreeUtil;
import com.intellij.testFramework.ParsingTestCase;
import org.mettle.clion.lang.MettleParserDefinition;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.stream.Stream;

/**
 * Parses the toolchain's own Mettle sources.
 *
 * <p>Snippets pin the constructs that are easy to get wrong; the corpus test then parses every
 * known-good {@code .mettle} file in the repository and insists the grammar covers all of them.
 */
public class MettleParserTest extends ParsingTestCase {

    /** Sources the compiler is meant to reject, so the plugin's parser rejects them too. */
    private static final List<String> INVALID_FIXTURES = List.of(
            "bad_syntax_module.mettle",      // missing statement, by design
            "diag_parser_no_cascade.mettle", // `if x > 0 {` with no parentheses
            "tmp_match_hang.mettle");        // a hang repro written in another language's syntax

    public MettleParserTest() {
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

    // ------------------------------------------------------------ snippets

    public void testMinimalProgram() {
        assertParses("fn main() -> int32 {\n  return 0;\n}\n");
    }

    public void testImportForms() {
        assertParses("""
                import "std/io";
                import "router" as router;
                import { send_404, send_all } from "http_util";
                import "std/net" if windows;
                import "std/net_posix" if linux;
                """);
    }

    public void testDeclarations() {
        assertParses("""
                const MAX: int32 = 100;
                const STEP = 4;
                var counter: int64 = 0;
                extern fn puts(msg: cstring) -> int32 = "puts";
                extern var errno_value: int32 = "errno";
                fn forward(a: int32, b: int32) -> int32;
                @inline! @pure fn fast(x: int32) -> int32 { return x * 3; }
                fn greet() { }
                """);
    }

    public void testStructsEnumsTraitsAndImpls() {
        assertParses("""
                struct Point { x: int32; y: int32;
                  method length_squared() -> int32 { return this.x * this.x + this.y * this.y; }
                }
                struct Pair<A, B> { first: A; second: B; }
                enum Direction { North, East = 2, South, West = -5 }
                enum Option { Some(int32), None }
                enum Result<T> { Ok(T), Err }
                trait Addable;
                trait Incrementable { fn next_value(self: Self) -> Self; }
                impl Addable for int32;
                impl Incrementable for int32 { fn next_value(self: Self) -> Self { return self + 1; } }
                fn bump<T: Incrementable + Addable>(value: T) -> T { return value.next_value(); }
                """);
    }

    public void testStatementsAndExpressions() {
        assertParses("""
                struct Pt { x: float64; y: float64; }
                const SHIFTS: int32[8] = [7, 12, 17, 22, 5, 9, 14, 20];
                const SCRATCH: uint8[64] = [0; 64];
                const CORNERS: Pt[2] = [{ x: 0.0, y: 0.0 }, { x: 1.0, y: 1.0 }];

                fn main() -> int32 {
                  var f: float64 = 3.14;
                  var i: int32 = (int32)f;
                  var p: int32* = (int32*)0;
                  var address: int64 = (int64)p;
                  var q: Pt* = new Pt;
                  q->x = 1.0;
                  var local: int32[4] = [1, 2, 3, 4];
                  local[0] += 1;
                  i = i << 2 | 1;
                  if (i > 0 && f < 1.0) { i = -i; } else { i = ~i; }
                  outer: while (i < 5) {
                    i = i + 1;
                    if (i == 3) { continue outer; }
                    break outer;
                  }
                  for (var k: int64 = 0; k < 3; k = k + 1) { i = i + (int32)k; }
                  @simd! for j in 0..8 { i = i + j; }
                  switch (i) {
                    case 0:
                      break;
                    case 3..9:
                      break;
                    default:
                      break;
                  }
                  defer cleanup();
                  return i;
                }
                fn cleanup() { }
                """);
    }

    public void testGenericCallsAreNotComparisons() {
        assertParses("""
                fn swap<T>(a: T*, b: T*) -> void { var tmp: T = *a; *a = *b; *b = tmp; }
                fn main() -> int32 {
                  var x: int32 = 10;
                  var y: int32 = 20;
                  swap<int32>(&x, &y);
                  var less: int32 = 0;
                  if (x < y) { less = 1; }
                  if (x < y == less > 0) { less = 2; }
                  return x + y + less;
                }
                """);
    }

    public void testMatchStatementAndExpression() {
        assertParses("""
                enum Option { Some(int32), None }
                fn unwrap_or(o: Option, fallback: int32) -> int32 {
                  return match (o) {
                    case Some(value): value
                    case None: fallback
                  };
                }
                fn run(o: Option) -> int32 {
                  match (o) {
                    case Some(v): { return v; }
                    case None: { return 0; }
                  }
                  return 0;
                }
                """);
    }

    public void testGpuKernelAndDispatch() {
        assertParses("""
                kernel vadd(a: float32*, b: float32*, c: float32*, n: int32) {
                  workgroup var tile: float32[256];
                  private var scratch: int32[4];
                  var i: int32 = block.x * block_dim.x + thread.x;
                  if (thread.x < 256 && i < n) { tile[thread.x] = a[i] + b[i]; }
                  barrier(workgroup, acq_rel);
                  var ticket: uint32 = atomic_fetch_add(a, i, 1, order: acq_rel, scope: device);
                }
                fn host(da: float32*, db: float32*, dc: float32*, n: int32) {
                  dispatch vadd[(n + 255) / 256, 256](da, db, dc, n);
                  dispatch vadd[grid: (1, 1, 1), block: (32, 1, 1), shared: 0, stream: 0](da, db, dc, n);
                }
                """);
    }

    public void testFunctionPointerTypesAndImportStr() {
        assertParses("""
                struct Ops { fast: fn(int32) -> int32; slow: fn(int32) -> int32; }
                fn triple(v: int32) -> int32 { return v * 3; }
                const DISPATCH: Ops = { fast: &triple, slow: &triple };
                var page: string = import_str "index.html";
                """);
    }

    public void testClosuresAndLambdas() {
        assertParses("""
                struct Ops {
                  combine: fn(int32, int32) -> int32;
                  shaper: Fn(int32) -> int32;
                }
                fn apply(f: fn(int32, int32) -> int32, a: int32, b: int32) -> int32 { return f(a, b); }
                fn adapt(f: Fn(int32) -> int32, v: int32) -> int32 { return f(v); }
                fn main() -> int32 {
                  var base: int32 = 10;
                  var add: Fn(int32) -> int32 = fn(x: int32) -> int32 { return x + base; };
                  var raw: int64 = 0;
                  var proc: fn(int64) -> int64 = (fn(int64) -> int64)raw;
                  return apply(fn(x: int32, y: int32) -> int32 { return x * y; }, 6, 7) + add(1);
                }
                """);
    }

    public void testWhereClausesAndNestedGenerics() {
        assertParses("""
                trait Addable;
                trait SignedNumber;
                struct Box<T> where T: Addable + SignedNumber { value: T; }
                struct Pair<A, B> { first: A; second: B; }
                fn identity<T>(value: T) -> T where T: Addable + SignedNumber { return value; }
                fn main() -> int32 {
                  var a: Pair<Box<int32>, Box<int32>>;
                  a.first.value = 3;
                  return a.first.value;
                }
                """);
    }

    public void testSemicolonsAreOptionalAtEndOfLine() {
        assertParses("""
                import "std/io"

                fn main() -> int32 {
                  var content: string = "hi"
                  println(cstr(content))
                  return 0
                }
                """);
    }

    public void testQualifiedNamesFromNamespacedImports() {
        assertParses("""
                import "namespaced_math" as math;
                fn main() -> int32 {
                  var kind: math.Magic = math.FortyTwo;
                  var pair: math.Pair* = math.make_pair(math.answer(), math.FortyTwo - 40);
                  return kind;
                }
                """);
    }

    public void testTopLevelCompileTimeStatements() {
        assertParses("""
                struct BudgetedEntity { hot_ptr: int64; id: int32; }
                static_assert(sizeof(BudgetedEntity) <= 24);
                static_assert(sizeof(int64) == 8);
                """);
    }

    public void testTypedRangeLoopVariable() {
        assertParses("""
                fn main() -> int32 {
                  var c: int64 = 0;
                  var n: int64 = 4;
                  for i: int64 in 0..n { c = c + i; }
                  return (int32)c;
                }
                """);
    }

    // -------------------------------------------------------------- corpus

    public void testRepositoryCorpusParsesCleanly() throws IOException {
        Path repository = Paths.get(System.getProperty("mettle.repo", ""));
        if (!Files.isDirectory(repository)) {
            // Running outside the toolchain checkout: the snippet tests still cover the grammar.
            return;
        }
        List<Path> sources = new ArrayList<>();
        for (String directory : List.of("stdlib", "examples", "tests")) {
            Path root = repository.resolve(directory);
            if (!Files.isDirectory(root)) continue;
            try (Stream<Path> walk = Files.walk(root)) {
                walk.filter(Files::isRegularFile)
                        .filter(path -> path.getFileName().toString().endsWith(".mettle"))
                        // err_*.mettle and these three are the compiler's negative fixtures:
                        // deliberately malformed sources it is supposed to reject.
                        .filter(path -> !path.getFileName().toString().startsWith("err_"))
                        .filter(path -> !INVALID_FIXTURES.contains(path.getFileName().toString()))
                        // The IDE itself builds no PSI past its intellisense size limit, so a
                        // multi-megabyte stress fixture is out of scope for any plugin.
                        .filter(MettleParserTest::withinIntellisenseLimit)
                        .forEach(sources::add);
            }
        }
        assertFalse("no Mettle sources found under " + repository, sources.isEmpty());

        List<String> failures = new ArrayList<>();
        int index = 0;
        for (Path source : sources) {
            // The corpus mixes CRLF and LF, and some files carry a BOM; PSI wants neither.
            String text = com.intellij.openapi.util.text.StringUtil.convertLineSeparators(
                    new String(Files.readAllBytes(source), StandardCharsets.UTF_8)
                            .replace("﻿", ""));
            PsiFile file = createPsiFile("corpus" + index++, text);
            assertNotNull("could not create PSI for " + source, file);
            ensureParsed(file);
            Collection<PsiErrorElement> errors =
                    PsiTreeUtil.findChildrenOfType(file, PsiErrorElement.class);
            for (PsiErrorElement error : errors) {
                failures.add(repository.relativize(source) + ":" + lineOf(text, error)
                        + " " + error.getErrorDescription()
                        + " near '" + preview(text, error) + "'");
                break; // one report per file keeps the failure readable
            }
        }
        assertTrue(sources.size() + " sources, " + failures.size() + " failed to parse:\n"
                + String.join("\n", failures), failures.isEmpty());
    }

    // ------------------------------------------------------------- helpers

    private void assertParses(String code) {
        PsiFile file = createPsiFile("snippet", code);
        ensureParsed(file);
        PsiErrorElement error = PsiTreeUtil.findChildOfType(file, PsiErrorElement.class);
        if (error != null) {
            fail("line " + lineOf(code, error) + ": " + error.getErrorDescription()
                    + " near '" + preview(code, error) + "'");
        }
    }

    private static boolean withinIntellisenseLimit(Path path) {
        try {
            return Files.size(path) <= com.intellij.openapi.vfs.PersistentFSConstants
                    .getMaxIntellisenseFileSize();
        } catch (IOException failure) {
            return false;
        }
    }

    private static int lineOf(String text, PsiElement element) {
        int offset = Math.min(element.getTextRange().getStartOffset(), text.length());
        int line = 1;
        for (int i = 0; i < offset; i++) {
            if (text.charAt(i) == '\n') line++;
        }
        return line;
    }

    private static String preview(String text, PsiElement element) {
        int start = Math.min(element.getTextRange().getStartOffset(), text.length());
        int end = Math.min(start + 40, text.length());
        return text.substring(start, end).replace('\n', ' ').trim();
    }
}
