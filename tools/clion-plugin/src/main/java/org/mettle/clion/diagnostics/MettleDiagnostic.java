package org.mettle.clion.diagnostics;

import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.util.MiniJson;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * One diagnostic from {@code mettle --error-format=json}: severity, stable code, 1-based position,
 * the span to underline, and the compiler's own label / help / related notes.
 */
public class MettleDiagnostic {

    public final String severity;
    public final String code;
    public final String message;
    public final String file;
    public final int line;
    public final int column;
    public final int length;
    public final String label;
    public final String help;
    public final List<MettleDiagnostic> notes;

    public MettleDiagnostic(String severity, String code, String message, String file,
                            int line, int column, int length, String label, String help,
                            List<MettleDiagnostic> notes) {
        this.severity = severity == null ? "error" : severity;
        this.code = code;
        this.message = message == null ? "" : message;
        this.file = file;
        this.line = line;
        this.column = column;
        this.length = length;
        this.label = label;
        this.help = help;
        this.notes = notes == null ? List.of() : notes;
    }

    /** Parses one NDJSON line; returns null when the line is not a diagnostic. */
    public static @Nullable MettleDiagnostic fromJsonLine(@NotNull String jsonLine) {
        Map<String, Object> object = MiniJson.parseObject(jsonLine);
        if (object == null || !object.containsKey("message")) return null;
        return fromObject(object);
    }

    @SuppressWarnings("unchecked")
    private static MettleDiagnostic fromObject(@NotNull Map<String, Object> object) {
        List<MettleDiagnostic> notes = new ArrayList<>();
        for (Object note : MiniJson.array(object, "notes")) {
            if (note instanceof Map) {
                notes.add(fromObject((Map<String, Object>) note));
            }
        }
        return new MettleDiagnostic(
                MiniJson.string(object, "severity"),
                MiniJson.string(object, "code"),
                MiniJson.string(object, "message"),
                MiniJson.string(object, "file"),
                MiniJson.integer(object, "line", 1),
                MiniJson.integer(object, "column", 1),
                MiniJson.integer(object, "length", 0),
                MiniJson.string(object, "label"),
                MiniJson.string(object, "help"),
                notes);
    }

    public boolean isError() {
        return "error".equalsIgnoreCase(severity);
    }

    public boolean isWarning() {
        return "warning".equalsIgnoreCase(severity);
    }

    /** The one-line text shown in the editor gutter and Problems view. */
    public @NotNull String displayMessage() {
        StringBuilder text = new StringBuilder();
        if (code != null && !code.isEmpty()) text.append('[').append(code).append("] ");
        text.append(message);
        if (label != null && !label.isEmpty() && !label.equals(message)) {
            text.append(" - ").append(label);
        }
        return text.toString();
    }

    /** The richer hover text: the message, the help line, and any related locations. */
    public @NotNull String tooltip() {
        StringBuilder html = new StringBuilder("<html><body>");
        html.append("<b>").append(escape(displayMessage())).append("</b>");
        if (help != null && !help.isEmpty()) {
            html.append("<br/><i>help:</i> ").append(escape(help));
        }
        for (MettleDiagnostic note : notes) {
            html.append("<br/><i>note:</i> ").append(escape(note.message));
            if (note.file != null && !note.file.isEmpty()) {
                html.append(" (").append(escape(note.file)).append(':').append(note.line).append(')');
            }
        }
        if (code != null && !code.isEmpty()) {
            html.append("<br/><br/><tt>mettle explain ").append(escape(code)).append("</tt>");
        }
        return html.append("</body></html>").toString();
    }

    private static String escape(@Nullable String text) {
        if (text == null) return "";
        return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;");
    }
}
