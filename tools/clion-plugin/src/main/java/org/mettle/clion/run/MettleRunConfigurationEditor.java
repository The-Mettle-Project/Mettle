package org.mettle.clion.run;

import com.intellij.openapi.fileChooser.FileChooser;
import com.intellij.openapi.fileChooser.FileChooserDescriptor;
import com.intellij.openapi.options.SettingsEditor;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.ui.ComboBox;
import com.intellij.openapi.ui.TextFieldWithBrowseButton;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.ui.components.JBCheckBox;
import com.intellij.ui.components.JBTextField;
import com.intellij.util.ui.FormBuilder;
import org.jetbrains.annotations.NotNull;

import javax.swing.JComponent;
import javax.swing.JPanel;

public class MettleRunConfigurationEditor extends SettingsEditor<MettleRunConfiguration> {

    private final Project project;
    private final TextFieldWithBrowseButton file = new TextFieldWithBrowseButton();
    private final TextFieldWithBrowseButton output = new TextFieldWithBrowseButton();
    private final TextFieldWithBrowseButton workingDirectory = new TextFieldWithBrowseButton();
    private final JBTextField programArguments = new JBTextField();
    private final JBTextField compilerArguments = new JBTextField();
    private final JBCheckBox release = new JBCheckBox("Optimized build (--release)");
    private final JBCheckBox stopAtEntry =
            new JBCheckBox("Debug: stop at the first line (the debug build is always unoptimized)");
    private final ComboBox<MettleRunConfiguration.Mode> mode =
            new ComboBox<>(MettleRunConfiguration.Mode.values());

    public MettleRunConfigurationEditor(@NotNull Project project) {
        this.project = project;
        browseForFile(file, "Select the Mettle source file");
        browseForFile(output, "Select the output executable");
        browseForDirectory(workingDirectory, "Select the working directory");
        mode.setRenderer(com.intellij.ui.SimpleListCellRenderer.create("",
                value -> value == null ? "" : value.label()));
    }

    private void browseForFile(@NotNull TextFieldWithBrowseButton field, @NotNull String title) {
        FileChooserDescriptor descriptor = new FileChooserDescriptor(true, false, false, false, false, false);
        descriptor.setTitle(title);
        field.addActionListener(event -> {
            VirtualFile chosen = FileChooser.chooseFile(descriptor, project, null);
            if (chosen != null) field.setText(chosen.getPath());
        });
    }

    private void browseForDirectory(@NotNull TextFieldWithBrowseButton field, @NotNull String title) {
        FileChooserDescriptor descriptor = new FileChooserDescriptor(false, true, false, false, false, false);
        descriptor.setTitle(title);
        field.addActionListener(event -> {
            VirtualFile chosen = FileChooser.chooseFile(descriptor, project, null);
            if (chosen != null) field.setText(chosen.getPath());
        });
    }

    @Override
    protected void resetEditorFrom(@NotNull MettleRunConfiguration configuration) {
        file.setText(configuration.getFilePath());
        output.setText(configuration.getOutputPath());
        workingDirectory.setText(configuration.getWorkingDirectory());
        programArguments.setText(configuration.getProgramArguments());
        compilerArguments.setText(configuration.getCompilerArguments());
        release.setSelected(configuration.isRelease());
        stopAtEntry.setSelected(configuration.isStopAtEntry());
        mode.setSelectedItem(configuration.getMode());
    }

    @Override
    protected void applyEditorTo(@NotNull MettleRunConfiguration configuration) {
        configuration.setFilePath(file.getText().trim());
        configuration.setOutputPath(output.getText().trim());
        configuration.setWorkingDirectory(workingDirectory.getText().trim());
        configuration.setProgramArguments(programArguments.getText().trim());
        configuration.setCompilerArguments(compilerArguments.getText().trim());
        configuration.setRelease(release.isSelected());
        configuration.setStopAtEntry(stopAtEntry.isSelected());
        Object selected = mode.getSelectedItem();
        configuration.setMode(selected instanceof MettleRunConfiguration.Mode
                ? (MettleRunConfiguration.Mode) selected : MettleRunConfiguration.Mode.RUN);
    }

    @Override
    protected @NotNull JComponent createEditor() {
        JPanel panel = FormBuilder.createFormBuilder()
                .addLabeledComponent("Mettle file:", file)
                .addLabeledComponent("Action:", mode)
                .addLabeledComponent("Output executable:", output)
                .addLabeledComponent("Program arguments:", programArguments)
                .addLabeledComponent("Working directory:", workingDirectory)
                .addLabeledComponent("Extra compiler arguments:", compilerArguments)
                .addComponent(release)
                .addComponent(stopAtEntry)
                .addComponentFillVertically(new JPanel(), 0)
                .getPanel();
        return panel;
    }
}
