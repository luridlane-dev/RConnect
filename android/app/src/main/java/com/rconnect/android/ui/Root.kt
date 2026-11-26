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
import androidx.compose.foundation.layout.displayCutoutPadding
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.List
import androidx.compose.material.icons.filled.AccountCircle
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Person
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
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

private const val REFRESH_MS = 20_000L

@Composable
fun RConnectApp(vm: AppViewModel) {
    val selected by vm.selected.collectAsStateWithLifecycle()
    val servers by vm.servers.collectAsStateWithLifecycle()
    val tab by vm.tab.collectAsStateWithLifecycle()
    val bg = appColors.bg

    // Re-check every 20 seconds while on screen: only poll the open server on DetailScreen,
    // and pause full-list polling while browsing Contacts or Activity Logs.
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    LaunchedEffect(lifecycle) {
        lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
            while (true) {
                val sel = vm.selected.value
                if (sel != null) {
                    vm.refresh(sel, force = true)
                } else if (vm.tab.value == Tab.Servers || vm.tab.value == Tab.People) {
                    vm.refreshAll()
                }
                delay(REFRESH_MS)
            }
        }
    }

    BackHandler(enabled = selected != null) { vm.select(null) }

    Box(Modifier.fillMaxSize().background(bg)) {
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
            Box(Modifier.fillMaxSize().background(bg).statusBarsPadding().displayCutoutPadding()) {
                if (entry != null) DetailScreen(vm, entry) else MainTabs(vm, tab)
            }
        }
    }
}

@Composable
private fun MainTabs(vm: AppViewModel, tab: Tab) {
    val lServers = stringResource(R.string.tab_servers)
    val lPeople = stringResource(R.string.tab_people)
    val lLogs = stringResource(R.string.tab_logs)
    val lContacts = stringResource(R.string.tab_contacts)
    val items = remember(lServers, lPeople, lLogs, lContacts) {
        listOf(
            TabItem(Icons.Default.Home, lServers),
            TabItem(Icons.Default.Person, lPeople),
            TabItem(Icons.AutoMirrored.Filled.List, lLogs),
            TabItem(Icons.Default.AccountCircle, lContacts),
        )
    }
    Column(Modifier.fillMaxSize()) {
        Box(Modifier.weight(1f)) {
            AnimatedContent(
                targetState = tab,
                transitionSpec = {
                    val forward = targetState.ordinal > initialState.ordinal
                    (fadeIn(tween(200)) + slideInHorizontally(tween(240)) { if (forward) it / 14 else -it / 14 } + scaleIn(tween(240), 0.99f)) togetherWith
                        (fadeOut(tween(130)) + scaleOut(tween(180), 0.99f))
                },
                label = "tab",
            ) { t ->
                when (t) {
                    Tab.Servers -> ServersScreen(vm)
                    Tab.People -> PeopleScreen(vm)
                    Tab.Logs -> ActivityScreen(vm)
                    Tab.Contacts -> ContactsScreen(vm)
                }
            }
        }
        TabBar(items, tab.ordinal) { vm.tab.value = Tab.entries[it] }
    }
}
