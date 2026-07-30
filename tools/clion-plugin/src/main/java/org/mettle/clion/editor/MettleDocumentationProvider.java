package org.mettle.clion.editor;

import com.intellij.lang.documentation.AbstractDocumentationProvider;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiFile;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.lang.MettleTypes;
import org.mettle.clion.psi.MettleDeclaration;
import org.mettle.clion.psi.MettlePsiUtil;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** Quick documentation: the declaration as written, plus the comment block above it. */
public class MettleDocumentationProvider extends AbstractDocumentationProvider {

    @Override
    public @Nullable String getQuickNavigateInfo(PsiElement element, PsiElement originalElement) {
        if (!(element instanceof MettleDeclaration)) return null;
        return MettlePsiUtil.signatureText(element);
    }

    @Override
    public @Nullable String generateDoc(PsiElement element, @Nullable PsiElement originalElement) {
        if (!(element instanceof MettleDeclaration)) return null;
        MettleDeclaration declaration = (MettleDeclaration) element;

        StringBuilder html = new StringBuilder();
        html.append("<div class='definition'><pre>")
                .append(escape(MettlePsiUtil.signatureText(declaration)))
                .append("</pre></div>");

        String comment = precedingComment(declaration);
        if (comment != null && !comment.isBlank()) {
            html.append("<div class='content'>").append(escape(comment).replace("\n", "<br/>"))
                    .append("</div>");
        }

        PsiFile file = declaration.getContainingFile();
        if (file != null) {
            html.append("<table class='sections'><tr><td valign='top' class='section'><p>Defined in:</td>")
                    .append("<td valign='top'>").append(escape(file.getName())).append("</td></tr></table>");
        }
        return html.toString();
    }

    /** The run of {@code //} lines (or one block comment) directly above the declaration. */
    private static @Nullable String precedingComment(MettleDeclaration declaration) {
        List<String> lines = new ArrayList<>();
        PsiElement cursor = declaration.getPrevSibling();
        while (cursor != null) {
            if (cursor.getNode() == null) break;
            var type = cursor.getNode().getElementType();
            if (type == com.intellij.psi.TokenType.WHITE_SPACE) {
                if (cursor.getText().indexOf('\n') != cursor.getText().lastIndexOf('\n')) break; // blank line
                cursor = cursor.getPrevSibling();
                continue;
            }
            if (type == MettleTypes.LINE_COMMENT) {
                lines.add(cursor.getText().replaceFirst("^//\\s?", ""));
            } else if (type == MettleTypes.BLOCK_COMMENT) {
                String text = cursor.getText();
                text = text.replaceFirst("^/\\*+", "").replaceFirst("\\*+/$", "");
                lines.add(text.strip());
            } else {
                break;
            }
            cursor = cursor.getPrevSibling();
        }
        if (lines.isEmpty()) return null;
        Collections.reverse(lines);
        return String.join("\n", lines);
    }

    private static String escape(String text) {
        return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;");
    }
}
