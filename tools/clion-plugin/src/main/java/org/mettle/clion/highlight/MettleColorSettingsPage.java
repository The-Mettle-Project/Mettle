package org.mettle.clion.highlight;

import com.intellij.openapi.editor.colors.TextAttributesKey;
import com.intellij.openapi.fileTypes.SyntaxHighlighter;
import com.intellij.openapi.options.colors.AttributesDescriptor;
import com.intellij.openapi.options.colors.ColorDescriptor;
import com.intellij.openapi.options.colors.ColorSettingsPage;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.lang.MettleIcons;

import javax.swing.Icon;
import java.util.HashMap;
import java.util.Map;

public class MettleColorSettingsPage implements ColorSettingsPage {

    private static final AttributesDescriptor[] DESCRIPTORS = {
            new AttributesDescriptor("Keyword", MettleColors.KEYWORD),
            new AttributesDescriptor("Built-in type", MettleColors.BUILTIN_TYPE),
            new AttributesDescriptor("Decorator", MettleColors.DECORATOR),
            new AttributesDescriptor("Number", MettleColors.NUMBER),
            new AttributesDescriptor("String", MettleColors.STRING),
            new AttributesDescriptor("Character", MettleColors.CHARACTER),
            new AttributesDescriptor("Comments//Line comment", MettleColors.LINE_COMMENT),
            new AttributesDescriptor("Comments//Block comment", MettleColors.BLOCK_COMMENT),
            new AttributesDescriptor("Braces and operators//Operator", MettleColors.OPERATOR),
            new AttributesDescriptor("Braces and operators//Parentheses", MettleColors.PARENTHESES),
            new AttributesDescriptor("Braces and operators//Braces", MettleColors.BRACES),
            new AttributesDescriptor("Braces and operators//Brackets", MettleColors.BRACKETS),
            new AttributesDescriptor("Braces and operators//Semicolon", MettleColors.SEMICOLON),
            new AttributesDescriptor("Braces and operators//Comma", MettleColors.COMMA),
            new AttributesDescriptor("Braces and operators//Dot", MettleColors.DOT),
            new AttributesDescriptor("Identifiers//Function declaration", MettleColors.FUNCTION_DECLARATION),
            new AttributesDescriptor("Identifiers//Function call", MettleColors.FUNCTION_CALL),
            new AttributesDescriptor("Identifiers//Type name", MettleColors.TYPE_NAME),
            new AttributesDescriptor("Identifiers//Type parameter", MettleColors.TYPE_PARAMETER),
            new AttributesDescriptor("Identifiers//Field", MettleColors.FIELD),
            new AttributesDescriptor("Identifiers//Parameter", MettleColors.PARAMETER),
            new AttributesDescriptor("Identifiers//Local variable", MettleColors.LOCAL_VARIABLE),
            new AttributesDescriptor("Identifiers//Global variable", MettleColors.GLOBAL_VARIABLE),
            new AttributesDescriptor("Identifiers//Constant", MettleColors.CONSTANT),
            new AttributesDescriptor("Identifiers//Enum variant", MettleColors.ENUM_VARIANT),
            new AttributesDescriptor("Identifiers//Built-in name", MettleColors.BUILTIN_NAME),
            new AttributesDescriptor("Identifiers//GPU built-in", MettleColors.GPU_BUILTIN),
            new AttributesDescriptor("Bad character", MettleColors.BAD_CHARACTER),
    };

    private static final Map<String, TextAttributesKey> TAGS = new HashMap<>();

    static {
        TAGS.put("fn", MettleColors.FUNCTION_DECLARATION);
        TAGS.put("call", MettleColors.FUNCTION_CALL);
        TAGS.put("type", MettleColors.TYPE_NAME);
        TAGS.put("field", MettleColors.FIELD);
        TAGS.put("param", MettleColors.PARAMETER);
        TAGS.put("local", MettleColors.LOCAL_VARIABLE);
        TAGS.put("global", MettleColors.GLOBAL_VARIABLE);
        TAGS.put("const", MettleColors.CONSTANT);
        TAGS.put("variant", MettleColors.ENUM_VARIANT);
        TAGS.put("builtin", MettleColors.BUILTIN_NAME);
        TAGS.put("gpu", MettleColors.GPU_BUILTIN);
        TAGS.put("decorator", MettleColors.DECORATOR);
    }

    @Override
    public @Nullable Icon getIcon() {
        return MettleIcons.FILE;
    }

    @Override
    public @NotNull SyntaxHighlighter getHighlighter() {
        return new MettleSyntaxHighlighter();
    }

    @Override
    public @NotNull String getDemoText() {
        return """
                import "std/io";

                const <const>LIMIT</const>: int32 = 64;
                var <global>counter</global>: int64 = 0;

                enum <type>Status</type> {
                  <variant>Ok</variant> = 0,
                  <variant>Error</variant> = 1
                }

                struct <type>Point</type> {
                  <field>x</field>: int32;
                  <field>y</field>: int32;

                  method <fn>length_squared</fn>() -> int32 {
                    return this.<field>x</field> * this.<field>x</field>
                         + this.<field>y</field> * this.<field>y</field>;
                  }
                }

                <decorator>@simd!</decorator> <decorator>@pure</decorator>
                fn <fn>dot</fn>(<param>a</param>: int8*, <param>b</param>: int8*, <param>n</param>: int32) -> int32 {
                  var <local>sum</local>: int32 = 0;      // running total
                  /* nested /* block */ comment */
                  for <local>i</local> in 0..<param>n</param> {
                    <local>sum</local> = <local>sum</local> + (int32)<param>a</param>[<local>i</local>] * (int32)<param>b</param>[<local>i</local>];
                  }
                  return <local>sum</local>;
                }

                kernel <fn>vadd</fn>(<param>out</param>: float32*, <param>n</param>: int32) {
                  var <local>i</local>: int32 = <gpu>block</gpu>.x * <gpu>block_dim</gpu>.x + <gpu>thread</gpu>.x;
                  if (<local>i</local> < <param>n</param>) { <param>out</param>[<local>i</local>] = 1.5; }
                }

                fn <fn>main</fn>() -> int32 {
                  var <local>p</local>: <type>Point</type>* = new <type>Point</type>;
                  <local>p</local>-><field>x</field> = <const>LIMIT</const>;
                  <call>println</call>("hello\\n");
                  return (int32)<builtin>sizeof</builtin>(<type>Point</type>);
                }
                """;
    }

    @Override
    public @Nullable Map<String, TextAttributesKey> getAdditionalHighlightingTagToDescriptorMap() {
        return TAGS;
    }

    @Override
    public AttributesDescriptor @NotNull [] getAttributeDescriptors() {
        return DESCRIPTORS;
    }

    @Override
    public ColorDescriptor @NotNull [] getColorDescriptors() {
        return ColorDescriptor.EMPTY_ARRAY;
    }

    @Override
    public @NotNull String getDisplayName() {
        return "Mettle";
    }
}
