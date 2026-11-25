package com.rconnect.android

import android.content.Intent
import android.graphics.Color
import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.viewModels
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.rconnect.android.notify.AppVisibility
import com.rconnect.android.notify.Notifier
import com.rconnect.android.notify.WatchService
import com.rconnect.android.ui.RConnectApp
import com.rconnect.android.ui.RConnectTheme

class MainActivity : ComponentActivity() {
    private val viewModel: AppViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        applySystemBars(viewModel.themeMode.value)
        // Server addresses and who is signed in are sensitive: no screenshots or task-switcher previews in release builds.
        if (!BuildConfig.DEBUG) window.setFlags(WindowManager.LayoutParams.FLAG_SECURE, WindowManager.LayoutParams.FLAG_SECURE)
        setContent {
            val mode by viewModel.themeMode.collectAsStateWithLifecycle()
            LaunchedEffect(mode) { applySystemBars(mode) }
            RConnectTheme(mode = mode) { RConnectApp(viewModel) }
        }
        openServerFrom(intent)
    }

    private fun applySystemBars(mode: ThemeMode) {
        val bar = if (mode == ThemeMode.Light) {
            SystemBarStyle.light(Color.TRANSPARENT, Color.TRANSPARENT)
        } else {
            SystemBarStyle.dark(Color.TRANSPARENT)
        }
        enableEdgeToEdge(statusBarStyle = bar, navigationBarStyle = bar)
    }

    override fun onStart() {
        super.onStart()
        AppVisibility.inFront = true
        WatchService.sync(this)
    }

    override fun onStop() {
        AppVisibility.inFront = false
        super.onStop()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        openServerFrom(intent)
    }

    // Tapping a notification opens the server it is about.
    private fun openServerFrom(intent: Intent?) {
        intent?.getStringExtra(Notifier.EXTRA_SERVER)?.let { viewModel.select(it) }
    }
}
