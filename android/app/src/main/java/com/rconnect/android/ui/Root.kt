package com.rconnect.android.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Person
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.repeatOnLifecycle
import com.rconnect.android.AppViewModel
import com.rconnect.android.R
import com.rconnect.android.Tab
import kotlinx.coroutines.delay

private const val REFRESH_MS = 60_000L

@Composable
fun RConnectApp(vm: AppViewModel) {
    val selected by vm.selected.collectAsStateWithLifecycle()
    val servers by vm.servers.collectAsStateWithLifecycle()
    val tab by vm.tab.collectAsStateWithLifecycle()

    // Everything is re-checked every minute while the app is on screen (servers answered in the last 30 s are skipped).
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    LaunchedEffect(lifecycle) {
        lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
            while (true) {
                vm.refreshAll()
                selected?.let { vm.refresh(it, force = true) }
                delay(REFRESH_MS)
            }
        }
    }

    val entry = servers.firstOrNull { it.id == selected }
    BackHandler(enabled = entry != null) { vm.select(null) }
    if (entry != null) {
        Surface(Modifier.fillMaxSize()) { DetailScreen(vm, entry) }
        return
    }
    Scaffold(
        bottomBar = {
            NavigationBar {
                NavigationBarItem(selected = tab == Tab.Servers, onClick = { vm.tab.value = Tab.Servers },
                    icon = { Icon(Icons.Default.Home, null) }, label = { Text(stringResource(R.string.tab_servers)) })
                NavigationBarItem(selected = tab == Tab.People, onClick = { vm.tab.value = Tab.People },
                    icon = { Icon(Icons.Default.Person, null) }, label = { Text(stringResource(R.string.tab_people)) })
            }
        },
    ) { padding ->
        Box(Modifier.fillMaxSize().padding(padding)) {
            if (tab == Tab.Servers) ServersScreen(vm) else PeopleScreen(vm)
        }
    }
}
