package org.mettle.clion;

import junit.framework.TestCase;
import org.mettle.clion.diagnostics.MettleDiagnostic;

/**
 * Parses real {@code mettle --error-format=json} output, copied verbatim from a compiler run so
 * the plugin notices if the shape ever changes.
 */
public class MettleDiagnosticTest extends TestCase {

    private static final String ARGUMENT_COUNT =
            "{\"severity\":\"error\",\"code\":\"E0003\",\"message\":\"Function 'add' expects 2 arguments, got 3\","
                    + "\"file\":\"C:/tmp/bad.mettle\",\"line\":6,\"column\":18,\"length\":3,"
                    + "\"label\":\"expected 2 arguments, got 3\","
                    + "\"notes\":[{\"message\":\"function 'add' defined here\",\"file\":\"C:/tmp/bad.mettle\","
                    + "\"line\":1,\"column\":4,\"length\":3}]}";

    private static final String TYPE_MISMATCH =
            "{\"severity\":\"error\",\"code\":\"E0004\",\"message\":\"Type mismatch: expected 'int64', found 'string'\","
                    + "\"file\":\"C:/tmp/bad.mettle\",\"line\":7,\"column\":18,\"length\":14,"
                    + "\"label\":\"expected 'int64', found 'string'\","
                    + "\"help\":\"use numeric literal without quotes: 42\",\"notes\":[]}";

    private static final String UNUSED_WARNING =
            "{\"severity\":\"warning\",\"code\":\"\",\"message\":\"unused variable 'unused'\","
                    + "\"file\":\"C:/tmp/bad.mettle\",\"line\":8,\"column\":7,\"length\":6,\"notes\":[]}";

    public void testParsesAnErrorWithNotes() {
        MettleDiagnostic diagnostic = MettleDiagnostic.fromJsonLine(ARGUMENT_COUNT);
        assertNotNull(diagnostic);
        assertTrue(diagnostic.isError());
        assertEquals("E0003", diagnostic.code);
        assertEquals("C:/tmp/bad.mettle", diagnostic.file);
        assertEquals(6, diagnostic.line);
        assertEquals(18, diagnostic.column);
        assertEquals(3, diagnostic.length);
        assertEquals(1, diagnostic.notes.size());
        assertEquals("function 'add' defined here", diagnostic.notes.get(0).message);
        assertEquals(1, diagnostic.notes.get(0).line);
        assertTrue(diagnostic.displayMessage().startsWith("[E0003] Function 'add' expects 2"));
        assertTrue(diagnostic.tooltip().contains("mettle explain E0003"));
    }

    public void testParsesHelpText() {
        MettleDiagnostic diagnostic = MettleDiagnostic.fromJsonLine(TYPE_MISMATCH);
        assertNotNull(diagnostic);
        assertEquals("use numeric literal without quotes: 42", diagnostic.help);
        assertTrue(diagnostic.tooltip().contains("help:"));
        assertEquals(14, diagnostic.length);
    }

    public void testParsesWarnings() {
        MettleDiagnostic diagnostic = MettleDiagnostic.fromJsonLine(UNUSED_WARNING);
        assertNotNull(diagnostic);
        assertFalse(diagnostic.isError());
        assertTrue(diagnostic.isWarning());
        assertEquals("unused variable 'unused'", diagnostic.message);
    }

    public void testIgnoresNonDiagnosticLines() {
        assertNull(MettleDiagnostic.fromJsonLine("Compiling bad.mettle"));
        assertNull(MettleDiagnostic.fromJsonLine("{\"progress\":1}"));
        assertNull(MettleDiagnostic.fromJsonLine("{not json"));
    }
}
