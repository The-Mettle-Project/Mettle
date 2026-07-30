package org.mettle.clion.explain;

import com.intellij.openapi.command.WriteCommandAction;
import com.intellij.openapi.editor.Document;
import com.intellij.openapi.project.Project;
import com.intellij.psi.PsiDocumentManager;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiFile;
import com.intellij.psi.tree.IElementType;
import com.intellij.psi.util.PsiTreeUtil;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.lang.MettleTypes;
import org.mettle.clion.psi.MettleDeclaration;
import org.mettle.clion.psi.MettlePsiUtil;
import org.mettle.clion.psi.MettleResolver;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Turns a compiler {@code fix:} line into an edit.
 *
 * <p>Only the fixes the compiler <em>verified</em> - it applied them to a clone, re-ran its own
 * optimizer, and reported the result - are worth offering as a button, and only when the edit can
 * be derived from the syntax tree rather than guessed at with a text search. Everything else stays
 * advice for the reader.
 */
public final class MettleExplainFix {

    private static final Pattern NAMED_ACCUMULATOR =
            Pattern.compile("declare the accumulator `(\\w+)` as int64");
    private static final Pattern BYTE_SUM_ACCUMULATOR =
            Pattern.compile("declare the accumulator as int64 \\(sum bytes");
    private static final Pattern REMOVE_NOINLINE =
            Pattern.compile("remove `@noinline` from `(\\w+)`");
    private static final Pattern MARK_INLINE =
            Pattern.compile("make `(\\w+)` inline-eligible.*mark it @inline");
    private static final Pattern NARROW_INT =
            Pattern.compile("u?int(8|16|32)");

    /** One replacement in one file. */
    private static class Edit {
        final PsiFile file;
        final int start;
        final int end;
        final String text;

        Edit(PsiFile file, int start, int end, String text) {
            this.file = file;
            this.start = start;
            this.end = end;
            this.text = text;
        }
    }

    private final String title;
    private final List<Edit> edits;

    private MettleExplainFix(@NotNull String title, @NotNull List<Edit> edits) {
        this.title = title;
        this.edits = edits;
    }

    public @NotNull String title() {
        return title;
    }

    /**
     * The edit for {@code remark}, or null when there is nothing mechanical to do - the advice may
     * be prose ("use int32 elements"), or the declaration it names may not be in reach.
     */
    public static @Nullable MettleExplainFix synthesize(@NotNull PsiFile file,
                                                        MettleExplainReport.@NotNull Remark remark) {
        if (remark.positive || remark.fix == null || !remark.isVerified()) return null;

        // Schema 2 names the diagnosis, so the family is chosen by id rather than by
        // matching the compiler's prose. Only the identifiers inside the message are
        // still read from it - those are data, not wording.
        String code = remark.code == null ? "" : remark.code;

        Matcher named = NAMED_ACCUMULATOR.matcher(remark.fix);
        if ("int32-sum-narrow-acc".equals(code) || named.find()) {
            String accumulator = named.reset().find() ? named.group(1) : accumulatorInLoop(file, remark);
            return accumulator == null ? null : widenAccumulator(file, remark, accumulator, false);
        }
        if ("byte-sum-narrow-acc".equals(code) || BYTE_SUM_ACCUMULATOR.matcher(remark.fix).find()) {
            String accumulator = accumulatorInLoop(file, remark);
            return accumulator == null ? null : widenAccumulator(file, remark, accumulator, true);
        }
        Matcher noinline = REMOVE_NOINLINE.matcher(remark.fix);
        if (noinline.find()) {
            return removeDecorator(file, remark, noinline.group(1), "@noinline");
        }
        Matcher inline = MARK_INLINE.matcher(remark.fix);
        if (inline.find()) {
            return addInlineDecorator(file, remark, inline.group(1));
        }
        if (remark.fix.contains("mark the callee @inline") && remark.callee != null) {
            return addInlineDecorator(file, remark, remark.callee);
        }
        return null;
    }

    // ------------------------------------------------------- accumulators

    /** {@code var s: int32} becomes {@code var s: int64}, plus the cast the byte kernel needs. */
    private static @Nullable MettleExplainFix widenAccumulator(@NotNull PsiFile file,
                                                               MettleExplainReport.@NotNull Remark remark,
                                                               @NotNull String name,
                                                               boolean byteSum) {
        PsiElement loop = loopAt(file, remark.line);
        if (loop == null) return null;
        PsiElement anchor = PsiTreeUtil.findChildOfType(loop, PsiElement.class);
        PsiElement declaration = MettleResolver.resolveSymbol(anchor == null ? loop : anchor, name);
        if (!(declaration instanceof MettleDeclaration)) return null;

        PsiElement type = MettlePsiUtil.declaredType(declaration);
        if (type == null || !NARROW_INT.matcher(type.getText().trim()).matches()) return null;

        List<Edit> edits = new ArrayList<>();
        edits.add(new Edit(declaration.getContainingFile(),
                type.getTextRange().getStartOffset(), type.getTextRange().getEndOffset(), "int64"));

        if (byteSum) {
            Edit cast = widenAccumulationCast(loop, name, file);
            if (cast == null) return null;   // without the cast the kernel still refuses
            edits.add(cast);
        }
        return new MettleExplainFix("Declare accumulator '" + name + "' as int64", edits);
    }

    /** In {@code total = total + data[i]}, make the summed operand {@code (int64)data[i]}. */
    private static @Nullable Edit widenAccumulationCast(@NotNull PsiElement loop, @NotNull String name,
                                                        @NotNull PsiFile file) {
        for (PsiElement statement : PsiTreeUtil.findChildrenOfType(loop, PsiElement.class)) {
            if (MettlePsiUtil.elementType(statement) != MettleTypes.ASSIGN_STMT) continue;
            PsiElement target = MettleResolver.firstExpression(statement);
            if (target == null || !name.equals(target.getText().trim())) continue;

            PsiElement sum = lastExpression(statement);
            if (MettlePsiUtil.elementType(sum) != MettleTypes.BINARY_EXPR) continue;
            PsiElement addend = summedOperand(sum, name);
            if (addend == null) continue;

            if (MettlePsiUtil.elementType(addend) == MettleTypes.CAST_EXPR) {
                PsiElement castType = MettlePsiUtil.childOfType(addend, MettleTypes.TYPE_REF);
                if (castType == null) continue;
                if ("int64".equals(castType.getText().trim())) return null;
                return new Edit(file, castType.getTextRange().getStartOffset(),
                        castType.getTextRange().getEndOffset(), "int64");
            }
            return new Edit(file, addend.getTextRange().getStartOffset(),
                    addend.getTextRange().getStartOffset(), "(int64)");
        }
        return null;
    }

    /** The operand of {@code acc + x} that is not the accumulator itself. */
    private static @Nullable PsiElement summedOperand(@NotNull PsiElement binary, @NotNull String name) {
        PsiElement candidate = null;
        for (PsiElement child = binary.getFirstChild(); child != null; child = child.getNextSibling()) {
            IElementType type = MettlePsiUtil.elementType(child);
            if (type == MettleTypes.PLUS || type == null) continue;
            if (child.getText().isBlank()) continue;
            if (MettlePsiUtil.elementType(child) == MettleTypes.REF_EXPR
                    && name.equals(child.getText().trim())) {
                continue;
            }
            if (isExpression(type)) candidate = child;
        }
        return candidate;
    }

    /** The accumulator of a {@code x = x + ...} statement inside the loop. */
    private static @Nullable String accumulatorInLoop(@NotNull PsiFile file,
                                                      MettleExplainReport.@NotNull Remark remark) {
        PsiElement loop = loopAt(file, remark.line);
        if (loop == null) return null;
        for (PsiElement statement : PsiTreeUtil.findChildrenOfType(loop, PsiElement.class)) {
            if (MettlePsiUtil.elementType(statement) != MettleTypes.ASSIGN_STMT) continue;
            PsiElement target = MettleResolver.firstExpression(statement);
            if (MettlePsiUtil.elementType(target) != MettleTypes.REF_EXPR) continue;
            String name = target.getText().trim();
            PsiElement sum = lastExpression(statement);
            if (MettlePsiUtil.elementType(sum) == MettleTypes.BINARY_EXPR
                    && sum.getText().contains(name)) {
                return name;
            }
        }
        return null;
    }

    // --------------------------------------------------------- decorators

    private static @Nullable MettleExplainFix removeDecorator(@NotNull PsiFile file,
                                                              MettleExplainReport.@NotNull Remark remark,
                                                              @NotNull String function,
                                                              @NotNull String decorator) {
        MettleDeclaration target = function(file, remark, function);
        if (target == null) return null;
        for (PsiElement element : MettlePsiUtil.childrenOfType(target, MettleTypes.DECORATOR)) {
            if (!element.getText().startsWith(decorator)) continue;
            int start = element.getTextRange().getStartOffset();
            int end = element.getTextRange().getEndOffset();
            PsiElement next = element.getNextSibling();
            if (next != null && next.getText().isBlank()) end = next.getTextRange().getEndOffset();
            return new MettleExplainFix("Remove " + decorator + " from '" + function + "'",
                    List.of(new Edit(target.getContainingFile(), start, end, "")));
        }
        return null;
    }

    private static @Nullable MettleExplainFix addInlineDecorator(@NotNull PsiFile file,
                                                                 MettleExplainReport.@NotNull Remark remark,
                                                                 @NotNull String function) {
        MettleDeclaration target = function(file, remark, function);
        if (target == null) return null;
        List<PsiElement> decorators = MettlePsiUtil.childrenOfType(target, MettleTypes.DECORATOR);
        for (PsiElement decorator : decorators) {
            String text = decorator.getText();
            if (text.startsWith("@inline") || text.startsWith("@noinline")) return null;
        }
        int offset = decorators.isEmpty()
                ? target.getTextRange().getStartOffset()
                : decorators.get(decorators.size() - 1).getTextRange().getEndOffset();
        String text = decorators.isEmpty() ? "@inline " : " @inline";
        return new MettleExplainFix("Mark '" + function + "' @inline",
                List.of(new Edit(target.getContainingFile(), offset, offset, text)));
    }

    private static @Nullable MettleDeclaration function(@NotNull PsiFile file,
                                                        MettleExplainReport.@NotNull Remark remark,
                                                        @NotNull String name) {
        PsiElement anchor = anchorAt(file, remark.line);
        PsiElement resolved = MettleResolver.resolveSymbol(anchor == null ? file : anchor, name);
        if (!(resolved instanceof MettleDeclaration)) return null;
        MettleDeclaration declaration = (MettleDeclaration) resolved;
        IElementType kind = declaration.getKind();
        if (kind != MettleTypes.FUNCTION_DECL && kind != MettleTypes.METHOD_DECL) return null;
        // An extern or forward declaration has no body to inline.
        return MettlePsiUtil.childOfType(declaration, MettleTypes.BLOCK) == null ? null : declaration;
    }

    // ------------------------------------------------------------- lookup

    private static @Nullable PsiElement loopAt(@NotNull PsiFile file, int line) {
        PsiElement anchor = anchorAt(file, line);
        return PsiTreeUtil.findFirstParent(anchor, false, element -> {
            IElementType type = MettlePsiUtil.elementType(element);
            return type == MettleTypes.FOR_STMT || type == MettleTypes.WHILE_STMT;
        });
    }

    /**
     * The first real element on a 1-based source line. Counted from the file's own text rather
     * than a Document, so a fix can be synthesized for a file that has no editor open.
     */
    private static @Nullable PsiElement anchorAt(@NotNull PsiFile file, int line) {
        if (line < 1) return null;
        CharSequence text = file.getText();
        int start = 0;
        for (int current = 1; current < line; current++) {
            int newline = indexOfNewline(text, start);
            if (newline < 0) return null;
            start = newline + 1;
        }
        int end = indexOfNewline(text, start);
        if (end < 0) end = text.length();
        for (int offset = start; offset < end; offset++) {
            PsiElement element = file.findElementAt(offset);
            if (element != null && !element.getText().isBlank()) return element;
        }
        return start < text.length() ? file.findElementAt(start) : null;
    }

    private static int indexOfNewline(@NotNull CharSequence text, int from) {
        for (int i = from; i < text.length(); i++) {
            if (text.charAt(i) == '\n') return i;
        }
        return -1;
    }

    private static @Nullable PsiElement lastExpression(@NotNull PsiElement parent) {
        PsiElement last = null;
        for (PsiElement child = parent.getFirstChild(); child != null; child = child.getNextSibling()) {
            if (isExpression(MettlePsiUtil.elementType(child))) last = child;
        }
        return last;
    }

    private static boolean isExpression(@Nullable IElementType type) {
        return type == MettleTypes.BINARY_EXPR || type == MettleTypes.UNARY_EXPR
                || type == MettleTypes.CAST_EXPR || type == MettleTypes.CALL_EXPR
                || type == MettleTypes.INDEX_EXPR || type == MettleTypes.FIELD_EXPR
                || type == MettleTypes.PAREN_EXPR || type == MettleTypes.REF_EXPR
                || type == MettleTypes.LITERAL_EXPR || type == MettleTypes.NEW_EXPR
                || type == MettleTypes.AGGREGATE_EXPR;
    }

    // -------------------------------------------------------------- apply

    /** Applies every edit as one undoable command. */
    public void apply(@NotNull Project project) {
        PsiFile[] affected = files().toArray(PsiFile.EMPTY_ARRAY);
        WriteCommandAction.writeCommandAction(project, affected)
                .withName(title)
                .run(() -> applyEdits(project));
    }

    /** The raw edit, for a caller that already owns a write command. */
    public void applyEdits(@NotNull Project project) {
        Map<PsiFile, List<Edit>> byFile = new LinkedHashMap<>();
        for (Edit edit : edits) {
            byFile.computeIfAbsent(edit.file, key -> new ArrayList<>()).add(edit);
        }
        PsiDocumentManager documents = PsiDocumentManager.getInstance(project);
        for (Map.Entry<PsiFile, List<Edit>> entry : byFile.entrySet()) {
            Document document = documents.getDocument(entry.getKey());
            if (document == null) continue;
            List<Edit> ordered = new ArrayList<>(entry.getValue());
            // back to front, so an earlier edit cannot shift a later offset
            ordered.sort(Comparator.comparingInt((Edit edit) -> edit.start).reversed());
            for (Edit edit : ordered) {
                document.replaceString(edit.start, edit.end, edit.text);
            }
            documents.commitDocument(document);
        }
    }

    /** What {@code file} would read like after this fix, without touching the document. */
    public @NotNull String preview(@NotNull PsiFile file) {
        List<Edit> ordered = new ArrayList<>();
        for (Edit edit : edits) {
            if (edit.file.equals(file)) ordered.add(edit);
        }
        ordered.sort(Comparator.comparingInt((Edit edit) -> edit.start).reversed());
        StringBuilder text = new StringBuilder(file.getText());
        for (Edit edit : ordered) {
            text.replace(edit.start, edit.end, edit.text);
        }
        return text.toString();
    }

    @NotNull List<PsiFile> files() {
        List<PsiFile> files = new ArrayList<>();
        for (Edit edit : edits) {
            if (!files.contains(edit.file)) files.add(edit.file);
        }
        return files;
    }
}
