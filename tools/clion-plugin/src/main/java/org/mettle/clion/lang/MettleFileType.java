package org.mettle.clion.lang;

import com.intellij.openapi.fileTypes.LanguageFileType;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.NonNls;

import javax.swing.Icon;

public final class MettleFileType extends LanguageFileType {
    public static final MettleFileType INSTANCE = new MettleFileType();
    public static final String DEFAULT_EXTENSION = "mettle";

    private MettleFileType() {
        super(MettleLanguage.INSTANCE);
    }

    @Override
    public @NonNls @NotNull String getName() {
        return "Mettle";
    }

    @Override
    public @NotNull String getDescription() {
        return "Mettle source file";
    }

    @Override
    public @NotNull String getDefaultExtension() {
        return DEFAULT_EXTENSION;
    }

    @Override
    public Icon getIcon() {
        return MettleIcons.FILE;
    }
}
