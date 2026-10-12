import java.util.Properties

plugins {
    id("com.android.application")
}

// Firma de la versión publicada: android/keystore.properties (fuera de git) apunta al .jks guardado fuera del repo.
val signing = Properties().apply {
    val f = rootProject.file("keystore.properties")
    if (f.isFile) f.inputStream().use { load(it) }
}

android {
    namespace = "com.lancast.android"
    compileSdk = 37

    defaultConfig {
        applicationId = "com.lancast.android"
        minSdk = 26
        targetSdk = 37
        versionCode = 3
        versionName = "1.7.0"
    }

    signingConfigs {
        if (signing.containsKey("storeFile")) {
            create("release") {
                storeFile = file(signing.getProperty("storeFile"))
                storePassword = signing.getProperty("storePassword")
                keyAlias = signing.getProperty("keyAlias")
                keyPassword = signing.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
            signingConfig = signingConfigs.findByName("release")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    implementation(project(":core"))
}
