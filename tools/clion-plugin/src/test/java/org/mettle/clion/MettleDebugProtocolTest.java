package org.mettle.clion;

import junit.framework.TestCase;
import org.mettle.clion.debug.MettleDebugProtocol;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * Drives a real instrumented program through the real debug pipe.
 *
 * <p>This is the part of the plugin that cannot be checked by reading it: named pipe creation,
 * the peek-then-read loop, line framing, and the order the runtime expects commands in. The test
 * compiles {@code tests/debug_demo.mettle} with {@code --debug-hooks}, sets a breakpoint, reads
 * variables, writes one back, and lets the program finish.
 *
 * <p>Skips itself when the toolchain, the compiler or Windows is missing.
 */
public class MettleDebugProtocolTest extends TestCase {

    private static final long TIMEOUT_MS = 20000;

    public void testFullDebugSession() throws Exception {
        Path repository = Paths.get(System.getProperty("mettle.repo", ""));
        if (!System.getProperty("os.name", "").toLowerCase().contains("win")) {
            System.out.println("skipped: the debug runtime transport is Windows only");
            return;
        }
        Path compiler = repository.resolve("bin").resolve("mettle.exe");
        Path source = repository.resolve("tests").resolve("debug_demo.mettle");
        if (!Files.isRegularFile(compiler) || !Files.isRegularFile(source)) {
            System.out.println("skipped: no compiler at " + compiler + " or fixture at " + source);
            return;
        }
        System.out.println("debugging " + source + " with " + compiler);

        Path executable = Files.createTempDirectory("mettle-debug-test").resolve("demo.exe");
        Process compile = new ProcessBuilder(compiler.toString(), source.toString(),
                "-o", executable.toString(), "--build", "--debug-hooks")
                .directory(repository.toFile())
                .redirectErrorStream(true)
                .start();
        String compilerOutput = new String(compile.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
        assertTrue("compile did not finish", compile.waitFor(120, TimeUnit.SECONDS));
        assertEquals("debug build failed:\n" + compilerOutput, 0, compile.exitValue());
        assertTrue("no executable was produced", Files.isRegularFile(executable));

        MettleDebugProtocol protocol = MettleDebugProtocol.host();
        BlockingQueue<MettleDebugProtocol.StopEvent> stops = new LinkedBlockingQueue<>();
        List<String> disconnects = new ArrayList<>();
        protocol.start(new MettleDebugProtocol.Listener() {
            @Override
            public void stopped(MettleDebugProtocol.StopEvent event) {
                stops.add(event);
            }

            @Override
            public void disconnected() {
                disconnects.add("gone");
            }
        });

        ProcessBuilder runner = new ProcessBuilder(executable.toString());
        runner.environment().put("METTLE_DBG_PIPE", protocol.pipeName());
        runner.directory(new File(repository.toString()));
        runner.redirectErrorStream(true);
        Process program = runner.start();

        try {
            // handshake: hello, the file and function tables, then the entry stop
            String hello = protocol.awaitLine(TIMEOUT_MS);
            assertNotNull("the program never connected to the pipe", hello);
            assertTrue("unexpected greeting: " + hello, hello.startsWith("hello\t"));

            Map<String, Integer> fileIds = new HashMap<>();
            Map<Integer, String> functionNames = new HashMap<>();
            for (;;) {
                String line = protocol.awaitLine(TIMEOUT_MS);
                assertNotNull("the table stream ended early", line);
                if (line.equals("tablesdone")) break;
                String[] fields = line.split("\t", -1);
                if (fields[0].equals("file")) {
                    fileIds.put(fields[2].replace('\\', '/').toLowerCase(), Integer.parseInt(fields[1]));
                } else if (fields[0].equals("fn")) {
                    functionNames.put(Integer.parseInt(fields[1]), fields[3]);
                }
            }
            assertFalse("no files in the debug tables", fileIds.isEmpty());
            assertTrue("main is missing from the function table", functionNames.containsValue("main"));

            MettleDebugProtocol.StopEvent entry = stops.poll(TIMEOUT_MS, TimeUnit.MILLISECONDS);
            assertNotNull("no entry stop", entry);
            assertEquals("entry", entry.reason);

            Integer fileId = fileIds.entrySet().stream()
                    .filter(entry2 -> entry2.getKey().endsWith("debug_demo.mettle"))
                    .map(Map.Entry::getValue)
                    .findFirst()
                    .orElse(null);
            assertNotNull("debug_demo.mettle is not in the file table: " + fileIds.keySet(), fileId);

            int breakpointLine = lineOf(source, "i = i + 1;");
            protocol.send("clearall");
            protocol.send("setbp\t" + fileId + "\t" + breakpointLine);
            protocol.send("go");

            MettleDebugProtocol.StopEvent hit = stops.poll(TIMEOUT_MS, TimeUnit.MILLISECONDS);
            assertNotNull("the breakpoint was never hit", hit);
            assertEquals("breakpoint", hit.reason);
            assertEquals(breakpointLine, hit.line);

            List<String> frames = protocol.query("stack", "framesdone", TIMEOUT_MS);
            assertFalse("no stack frames", frames.isEmpty());
            assertTrue("unexpected frame line: " + frames.get(0), frames.get(0).startsWith("frame\t0\t"));

            List<String> variables = protocol.query("vars\t0", "varsdone", TIMEOUT_MS);
            Map<String, String> values = new HashMap<>();
            for (String line : variables) {
                String[] fields = line.split("\t", -1);
                if (fields.length >= 6 && fields[0].equals("var")) values.put(fields[1], fields[5]);
            }
            assertTrue("locals are missing: " + values.keySet(), values.containsKey("total"));
            assertEquals("1.5", values.get("ratio"));
            assertTrue("the struct local should expand", variables.stream()
                    .anyMatch(line -> line.startsWith("var\tcorner\t") && line.split("\t")[4].equals("1")));

            // paths resolve through fields, pointers and array elements
            assertEquals("7", evaluated(protocol, "corner.x"));
            assertEquals("40", evaluated(protocol, "box.max.y"));
            assertEquals("9", evaluated(protocol, "pp->y"));
            assertEquals("12", evaluated(protocol, "grid[2]"));

            List<String> children = protocol.query("expand\t0\tbox", "varsdone", TIMEOUT_MS);
            assertTrue("expanding a struct should list its fields: " + children,
                    children.stream().anyMatch(line -> line.startsWith("var\tmin\t")));

            // writing through the live pointer really changes the program
            String written = protocol.queryOne("set\t0\ttotal\t1000", TIMEOUT_MS);
            assertNotNull(written);
            assertTrue("set failed: " + written, written.startsWith("setr\t1\t"));
            assertEquals("1000", evaluated(protocol, "total"));

            protocol.send("clearall");
            protocol.send("go");
            assertTrue("the program did not finish", program.waitFor(30, TimeUnit.SECONDS));
            assertEquals(0, program.exitValue());
        } finally {
            protocol.close();
            program.destroy();
        }
    }

    private static String evaluated(MettleDebugProtocol protocol, String path) {
        String reply = protocol.queryOne("eval\t0\t" + path, TIMEOUT_MS);
        assertNotNull("no reply evaluating " + path, reply);
        String[] fields = reply.split("\t", -1);
        assertTrue("evaluating " + path + " failed: " + reply,
                fields.length >= 5 && fields[0].equals("evalr") && fields[1].equals("1"));
        return fields[4];
    }

    private static int lineOf(Path source, String needle) throws Exception {
        List<String> lines = Files.readAllLines(source, StandardCharsets.UTF_8);
        for (int i = 0; i < lines.size(); i++) {
            if (lines.get(i).contains(needle)) return i + 1;
        }
        throw new AssertionError("'" + needle + "' is not in " + source);
    }
}
