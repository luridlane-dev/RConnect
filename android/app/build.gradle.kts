plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

// The app's version is the repository's VERSION file, like the Windows program's.
val appVersion = rootProject.file("../VERSION").takeIf { it.exists() }?.readText()?.trim().orEmpty().ifEmpty { "1.0.0" }
val versionParts = appVersion.split('.').map { it.toIntOrNull() ?: 0 } + listOf(0, 0, 0)

// Release signing comes from the environment (the release workflow sets it); without it the debug key is used.
val keystoreFile = System.getenv("ANDROID_KEYSTORE_FILE").orEmpty()

android {
    namespace = "com.rconnect.android"
    compileSdk = 37

    defaultConfig {
        applicationId = "com.rconnect.android"
        minSdk = 26
        targetSdk = 35
        versionCode = versionParts[0] * 10000 + versionParts[1] * 100 + versionParts[2]
        versionName = appVersion
    }

    signingConfigs {
        if (keystoreFile.isNotEmpty()) {
            create("release") {
                storeFile = file(keystoreFile)
                storePassword = System.getenv("ANDROID_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("ANDROID_KEY_ALIAS")
                keyPassword = System.getenv("ANDROID_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.getByName(if (keystoreFile.isNotEmpty()) "release" else "debug")
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }
    buildFeatures {
        compose = true
        buildConfig = true
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.material.icons)
    implementation(libs.androidx.work.runtime)

}
