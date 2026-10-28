plugins {
    id("com.android.application") version "9.1.0" apply false
    id("org.jetbrains.kotlin.plugin.compose") version "2.2.10" apply false
}
layout.buildDirectory.set(rootProject.layout.projectDirectory.dir("../../build/android/root"))
subprojects { layout.buildDirectory.set(rootProject.layout.projectDirectory.dir("../../build/android/$name")) }
