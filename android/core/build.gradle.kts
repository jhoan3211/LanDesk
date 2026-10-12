plugins {
    kotlin("jvm")
}

// Bytecode compatible con Android (Java 17), aunque se compile con un JDK más nuevo.
java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

dependencies {
    testImplementation(kotlin("test-junit"))
}

tasks.test {
    // Prueba de interoperabilidad con el host real (C++): ruta del ejecutable compilado con CMake.
    val host = rootProject.layout.projectDirectory.file("../build/tests/Release/lancast-test-host.exe").asFile
    systemProperty("lancast.testHost", host.absolutePath)
    testLogging {
        events("passed", "failed", "skipped")
        showStandardStreams = true
        exceptionFormat = org.gradle.api.tasks.testing.logging.TestExceptionFormat.FULL
    }
}
