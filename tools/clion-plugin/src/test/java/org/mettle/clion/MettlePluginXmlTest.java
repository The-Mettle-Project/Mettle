package org.mettle.clion;

import junit.framework.TestCase;

import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Every class named in plugin.xml has to exist: a typo there fails at runtime, when the user
 * opens a file, rather than at build time.
 */
public class MettlePluginXmlTest extends TestCase {

    private static final Pattern CLASS_ATTRIBUTE = Pattern.compile(
            "(?:implementationClass|implementation|instance|serviceImplementation|factoryClass|class)"
                    + "=\"([^\"]+)\"");

    /** Actions and listeners name their class in an attribute; intentions in an element. */
    private static final Pattern CLASS_ELEMENT = Pattern.compile("<className>([^<]+)</className>");

    public void testEveryRegisteredClassExists() throws IOException {
        String xml = readPluginXml();
        Set<String> names = new LinkedHashSet<>();
        Matcher matcher = CLASS_ATTRIBUTE.matcher(xml);
        while (matcher.find()) {
            names.add(matcher.group(1));
        }
        Matcher elements = CLASS_ELEMENT.matcher(xml);
        while (elements.find()) {
            names.add(elements.group(1));
        }
        names.removeIf(name -> name.startsWith("com.intellij."));
        assertFalse("plugin.xml registered nothing", names.isEmpty());

        List<String> missing = new ArrayList<>();
        for (String name : names) {
            try {
                Class.forName(name, false, getClass().getClassLoader());
            } catch (ClassNotFoundException notFound) {
                missing.add(name);
            }
        }
        assertTrue("plugin.xml names classes that do not exist: " + missing, missing.isEmpty());
    }

    public void testLiveTemplateFileIsPresent() throws IOException {
        assertTrue(readPluginXml().contains("/liveTemplates/Mettle.xml"));
        try (InputStream stream = getClass().getResourceAsStream("/liveTemplates/Mettle.xml")) {
            assertNotNull("the live template file is not on the classpath", stream);
        }
    }

    public void testIconsArePresent() throws IOException {
        for (String icon : List.of("/icons/mettle.svg", "/icons/mettle_dark.svg")) {
            try (InputStream stream = getClass().getResourceAsStream(icon)) {
                assertNotNull(icon + " is missing", stream);
            }
        }
    }

    private String readPluginXml() throws IOException {
        try (InputStream stream = getClass().getResourceAsStream("/META-INF/plugin.xml")) {
            assertNotNull("plugin.xml is not on the classpath", stream);
            return new String(stream.readAllBytes(), StandardCharsets.UTF_8);
        }
    }
}
