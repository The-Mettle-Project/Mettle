package org.mettle.clion.util;

import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * A small JSON reader for the compiler's {@code --error-format=json} output.
 *
 * <p>Values come back as {@link Map}, {@link List}, {@link String}, {@link Double}, {@link Boolean}
 * or null. Deliberately dependency-free: the plugin should not care which JSON library the IDE
 * happens to bundle this year.
 */
public final class MiniJson {

    private final String text;
    private int position;

    private MiniJson(@NotNull String text) {
        this.text = text;
    }

    /** Parses one JSON value, or returns null if the text is not valid JSON. */
    public static @Nullable Object parse(@NotNull String text) {
        try {
            MiniJson parser = new MiniJson(text);
            parser.skipWhitespace();
            Object value = parser.readValue();
            parser.skipWhitespace();
            return parser.position == text.length() ? value : value;
        } catch (RuntimeException failure) {
            return null;
        }
    }

    @SuppressWarnings("unchecked")
    public static @Nullable Map<String, Object> parseObject(@NotNull String text) {
        Object value = parse(text);
        return value instanceof Map ? (Map<String, Object>) value : null;
    }

    public static @Nullable String string(@Nullable Map<String, Object> object, @NotNull String key) {
        Object value = object == null ? null : object.get(key);
        return value instanceof String ? (String) value : null;
    }

    public static int integer(@Nullable Map<String, Object> object, @NotNull String key, int fallback) {
        Object value = object == null ? null : object.get(key);
        if (value instanceof Double) return (int) Math.round((Double) value);
        if (value instanceof String) {
            try {
                return Integer.parseInt((String) value);
            } catch (NumberFormatException ignored) {
                return fallback;
            }
        }
        return fallback;
    }

    @SuppressWarnings("unchecked")
    public static @NotNull List<Object> array(@Nullable Map<String, Object> object, @NotNull String key) {
        Object value = object == null ? null : object.get(key);
        return value instanceof List ? (List<Object>) value : List.of();
    }

    // ------------------------------------------------------------- parsing

    private Object readValue() {
        char c = peek();
        switch (c) {
            case '{': return readObject();
            case '[': return readArray();
            case '"': return readString();
            case 't': expect("true"); return Boolean.TRUE;
            case 'f': expect("false"); return Boolean.FALSE;
            case 'n': expect("null"); return null;
            default: return readNumber();
        }
    }

    private Map<String, Object> readObject() {
        Map<String, Object> result = new LinkedHashMap<>();
        position++; // {
        skipWhitespace();
        if (peek() == '}') {
            position++;
            return result;
        }
        for (;;) {
            skipWhitespace();
            String key = readString();
            skipWhitespace();
            if (peek() != ':') throw new IllegalStateException("expected ':'");
            position++;
            skipWhitespace();
            result.put(key, readValue());
            skipWhitespace();
            char c = peek();
            position++;
            if (c == '}') return result;
            if (c != ',') throw new IllegalStateException("expected ',' or '}'");
        }
    }

    private List<Object> readArray() {
        List<Object> result = new ArrayList<>();
        position++; // [
        skipWhitespace();
        if (peek() == ']') {
            position++;
            return result;
        }
        for (;;) {
            skipWhitespace();
            result.add(readValue());
            skipWhitespace();
            char c = peek();
            position++;
            if (c == ']') return result;
            if (c != ',') throw new IllegalStateException("expected ',' or ']'");
        }
    }

    private String readString() {
        if (peek() != '"') throw new IllegalStateException("expected a string");
        position++;
        StringBuilder result = new StringBuilder();
        for (;;) {
            char c = next();
            if (c == '"') return result.toString();
            if (c != '\\') {
                result.append(c);
                continue;
            }
            char escape = next();
            switch (escape) {
                case 'n': result.append('\n'); break;
                case 't': result.append('\t'); break;
                case 'r': result.append('\r'); break;
                case 'b': result.append('\b'); break;
                case 'f': result.append('\f'); break;
                case 'u':
                    result.append((char) Integer.parseInt(read(4), 16));
                    break;
                default: result.append(escape);
            }
        }
    }

    private Double readNumber() {
        int start = position;
        while (position < text.length() && "+-.eE0123456789".indexOf(text.charAt(position)) >= 0) {
            position++;
        }
        if (start == position) throw new IllegalStateException("expected a value");
        return Double.parseDouble(text.substring(start, position));
    }

    private void expect(String literal) {
        if (!text.startsWith(literal, position)) throw new IllegalStateException("expected " + literal);
        position += literal.length();
    }

    private String read(int count) {
        String result = text.substring(position, position + count);
        position += count;
        return result;
    }

    private char peek() {
        if (position >= text.length()) throw new IllegalStateException("unexpected end of input");
        return text.charAt(position);
    }

    private char next() {
        char c = peek();
        position++;
        return c;
    }

    private void skipWhitespace() {
        while (position < text.length() && Character.isWhitespace(text.charAt(position))) position++;
    }
}
