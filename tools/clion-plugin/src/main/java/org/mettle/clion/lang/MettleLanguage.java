package org.mettle.clion.lang;

import com.intellij.lang.Language;

public final class MettleLanguage extends Language {
    public static final MettleLanguage INSTANCE = new MettleLanguage();

    private MettleLanguage() {
        super("Mettle");
    }

    @Override
    public boolean isCaseSensitive() {
        return true;
    }
}
