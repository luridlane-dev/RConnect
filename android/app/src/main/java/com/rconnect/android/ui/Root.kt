package com.rconnect.android.ui

import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.scaleOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.AccountCircle
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Person
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.repeatOnLifecycle
import com.rconnect.android.AppViewModel
import com.rconnect.android.R
import com.rconnect.android.Tab
import com.rconnect.android.notify.Change
import com.rconnect.android.notify.Notifier
import kotlinx.coroutines.delay

private const val REFRESH_MS = 60_000L
private const val BANNER_MS = 4_000L

@Composable
fun RConnectApp(vm: AppViewModel) {
    val selected by vm.selected.collectAsStateWithLifecycle()
    val servers by vm.servers.collectAsStateWithLifecycle()
    val tab by vm.tab.collectAsStateWithLifecycle()
    val context = LocalContext.current

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

    // What changed on the servers while the app is open appears for a few seconds at the top.
    var banner by remember { mutableStateOf<Pair<String, Change.Kind>?>(null) }
    LaunchedEffect(vm) {
        vm.changes.collect { c ->
            banner = "${c.server} · ${Notifier.text(context, c)}" to c.kind
            delay(BANNER_MS)
            banner = null
        }
    }

    BackHandler(enabled = selected != null) { vm.select(null) }

    Box(Modifier.fillMaxSize().background(Palette.Background)) {
        // A server page slides in over the lists; going back slides it out again.
        AnimatedContent(
            targetState = selected,
            transitionSpec = {
                if (targetState != null) {
                    (slideInHorizontally(spring(stiffness = Spring.StiffnessMediumLow)) { it / 3 } + fadeIn(tween(220))) togetherWith
                        (slideOutHorizontally(tween(260)) { -it / 5 } + fadeOut(tween(200)))
                } else {
                    (slideInHorizontally(spring(stiffness = Spring.StiffnessMediumLow)) { -it / 5 } + fadeIn(tween(220))) togetherWith
                        (slideOutHorizontally(tween(260)) { it / 3 } + fadeOut(tween(200)))
                }
            },
            label = "page",
        ) { id ->
            val entry = servers.firstOrNull { it.id == id }
            Box(Modifier.fillMaxSize().background(Palette.Background).statusBarsPadding()) {
                if (entry != null) DetailScreen(vm, entry) else MainTabs(vm, tab)
            }
        }
        Box(Modifier.statusBarsPadding().align(Alignment.TopCenter)) {
            Banner(banner?.first, banner?.second.let { kind ->
                when (kind) {
                    Change.Kind.Problem, Change.Kind.Stranger -> Red
                    Change.Kind.Connected -> Orange
                    Change.Kind.Recovered -> Green
                    else -> Palette.Accent
                }
            })
        }
    }
}

@Composable
private fun MainTabs(vm: AppViewModel, tab: Tab) {
    val items = listOf(
        TabItem(Icons.Default.Home, stringResource(R.string.tab_servers)),
        TabItem(Icons.Default.Person, stringResource(R.string.tab_people)),
        TabItem(Icons.Default.AccountCircle, stringResource(R.string.tab_contacts)),
    )
    Column(Modifier.fillMaxSize()) {
        Box(Modifier.weight(1f)) {
            AnimatedContent(
                targetState = tab,
                transitionSpec = {
                    val forward = targetState.ordinal > initialState.ordinal
                    (fadeIn(tween(220)) + slideInHorizontally(tween(260)) { if (forward) it / 12 else -it / 12 } + scaleIn(tween(260), 0.98f)) togetherWith
                        (fadeOut(tween(140)) + scaleOut(tween(200), 0.98f))
                },
                label = "tab",
            ) { t ->
                when (t) {
                    Tab.Servers -> ServersScreen(vm)
                    Tab.People -> PeopleScreen(vm)
                    Tab.Contacts -> ContactsScreen(vm)
                }
            }
        }
        TabBar(items, tab.ordinal) { vm.tab.value = Tab.entries[it] }
    }
}
