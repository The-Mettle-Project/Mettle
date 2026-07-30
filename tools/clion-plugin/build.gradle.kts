import org.jetbrains.intellij.platform.gradle.IntelliJPlatformType
import org.jetbrains.intellij.platform.gradle.TestFrameworkType

plugins {
    id("java")
    id("org.jetbrains.intellij.platform") version "2.18.1"
}

group = "org.mettle"
version = providers.gradleProperty("pluginVersion").get()

repositories {
    mavenCentral()
    intellijPlatform {
        defaultRepositories()
    }
}

dependencies {
    intellijPlatform {
        val localPath = providers.gradleProperty("platformLocalPath").orNull
        if (localPath.isNullOrBlank()) {
            create(
                IntelliJPlatformType.fromCode(providers.gradleProperty("platformType").get()),
                providers.gradleProperty("platformVersion").get(),
            )
        } else {
            local(localPath)
        }
        testFramework(TestFrameworkType.Platform)
    }
    testImplementation("junit:junit:4.13.2")
}

// The parser tests read the toolchain's own .mettle corpus, two directories up.
tasks.test {
    useJUnit()
    systemProperty("mettle.repo", rootDir.parentFile.parentFile.absolutePath)
}

intellijPlatform {
    buildSearchableOptions = false
    instrumentCode = false

    pluginConfiguration {
        version = providers.gradleProperty("pluginVersion")
        ideaVersion {
            sinceBuild = providers.gradleProperty("pluginSinceBuild")
            // Open-ended: the plugin only uses long-lived platform API.
            untilBuild = provider { null }
        }
    }
}

// The platform wants Java 21 bytecode, but the JDK that produces it does not have to be a 21.
// Default to whichever JDK is running Gradle - always present, so no toolchain download - and let
// `-PjavaToolchain=21` pin a specific one.
java {
    toolchain {
        val requested = providers.gradleProperty("javaToolchain").orNull
        languageVersion = JavaLanguageVersion.of(
            requested?.toInt() ?: JavaVersion.current().majorVersion.toInt()
        )
    }
}

tasks.withType<JavaCompile>().configureEach {
    options.encoding = "UTF-8"
    options.release = 21
}
