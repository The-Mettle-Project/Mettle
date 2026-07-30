package org.mettle.clion.editor;

import com.intellij.lang.refactoring.NamesValidator;
import com.intellij.openapi.project.Project;
import org.jetbrains.annotations.NotNull;
import org.mettle.clion.lang.MettleTypes;

public class MettleNamesValidator implements NamesValidator {

    @Override
    public boolean isKeyword(@NotNull String name, Project project) {
        return MettleTypes.KEYWORDS.containsKey(name);
    }

    @Override
    public boolean isIdentifier(@NotNull String name, Project project) {
        if (name.isEmpty() || isKeyword(name, project)) return false;
        char first = name.charAt(0);
        if (first != '_' && !isAsciiLetter(first)) return false;
        for (int i = 1; i < name.length(); i++) {
            char c = name.charAt(i);
            if (c != '_' && !isAsciiLetter(c) && (c < '0' || c > '9')) return false;
        }
        return true;
    }

    private static boolean isAsciiLetter(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }
}
