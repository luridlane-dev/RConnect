package com.rconnect.android

import android.content.Intent
import android.graphics.Color
import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.SystemBarStyle
import androidx.activity.enableEdgeToEdge
import androidx.activity.viewModels
import com.rconnect.android.notify.AppVisibility
import com.rconnect.android.notify.Notifier
import com.rconnect.android.notify.WatchService
import com.rconnect.android.ui.RConnectApp
import com.rconnect.android.ui.RConnectTheme

class MainActivity : ComponentActivity() {
    private val viewModel: AppViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(Color.TRANSPARENT),
            navigationBarStyle = SystemBarStyle.dark(Color.TRANSPARENT),
        )
        // Server addresses and who is signed in are sensitive: no screenshots or task-switcher previews in release builds.
        if (!BuildConfig.DEBUG) window.setFlags(WindowManager.LayoutParams.FLAG_SECURE, WindowManager.LayoutParams.FLAG_SECURE)
        setContent {
            RConnectTheme { RConnectApp(viewModel) }
        }
        openServerFrom(intent)
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
