package com.rconnect.android.ui

import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.animateContentSize
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.ui.graphics.RectangleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.rconnect.android.AppViewModel
import com.rconnect.android.Health
import com.rconnect.android.R
import com.rconnect.android.ServerState
import com.rconnect.android.data.Contact
import com.rconnect.android.data.ContactBook
import com.rconnect.android.data.ServerEntry
import com.rconnect.android.winrm.AccessEvent

@Composable
fun DetailScreen(vm: AppViewModel, server: ServerEntry) {
    val states by vm.states.collectAsStateWithLifecycle()
    val state = states[server.id] ?: ServerState()
    var section by remember { mutableIntStateOf(0) }
    var editing by remember { mutableStateOf(false) }
    var deleting by remember { mutableStateOf(false) }
    val (failed, history) = state.events.orEmpty().partition { it.kind == AccessEvent.Kind.FailedSignIn }
    val contacts by vm.contacts.collectAsStateWithLifecycle()
    var naming by remember { mutableStateOf<Contact?>(null) }

    Column(Modifier.fillMaxSize()) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp), verticalAlignment = Alignment.CenterVertically) {
            ToolButton(Icons.Default.ArrowBack, stringResource(R.string.back)) { vm.select(null) }
            Text(server.title, modifier = Modifier.weight(1f).padding(horizontal = 12.dp), style = MaterialTheme.typography.titleLarge,
                maxLines = 1, overflow = TextOverflow.Ellipsis)
            Row(horizontalArrangement = androidx.compose.foundation.layout.Arrangement.spacedBy(8.dp)) {
                ToolButton(Icons.Default.Refresh, stringResource(R.string.refresh)) { vm.refresh(server.id, force = true) }
                ToolButton(Icons.Default.Edit, stringResource(R.string.edit)) { editing = true }
                ToolButton(Icons.Default.Delete, stringResource(R.string.delete), tint = Palette.Red) { deleting = true }
            }
        }
        LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(start = 16.dp, end = 16.dp, bottom = 24.dp)) {
            item(key = "summary") { Summary(server, state) }
            item(key = "chips") {
                Segmented(
                    listOf(stringResource(R.string.tab_now, state.connected.size), stringResource(R.string.tab_history), stringResource(R.string.tab_failed, failed.size)),
                    section, { section = it }, Modifier.padding(vertical = 14.dp),
                )
            }
            when (section) {
                0 -> {
                    val sessions = state.sessions
                    when {
                        sessions == null -> item { Muted(stringResource(R.string.checking), modifier = Modifier.padding(8.dp)) }
                        sessions.isEmpty() -> item { Muted(stringResource(R.string.nobody), modifier = Modifier.padding(8.dp)) }
                        else -> items(sessions.size, key = { "s-${sessions[it].id}" }) { i ->
                            val s = sessions[i]
                            GroupedRow(i, sessions.size, Modifier.animateItem()) {
                                SessionRow(s, ContactBook.nameFor(contacts, s.client, s.user)) {
                                    if (ContactBook.canName(contacts, s.client, s.user)) naming = ContactBook.draft(contacts, s.client, s.user)
                                }
                            }
                        }
                    }
                }
                1 -> when {
                    state.events == null -> item { Muted(stringResource(R.string.checking), modifier = Modifier.padding(8.dp)) }
                    history.isEmpty() -> item { Muted(stringResource(R.string.no_history), modifier = Modifier.padding(8.dp)) }
                    else -> {
                        // One panel per day.
                        val days = history.groupBy { dayLabel(it.timeUtcMs) }
                        for ((day, list) in days) {
                            item(key = "day-$day") { SectionHeader(day) }
                            items(list.size, key = { "e-$day-${list[it].recordId}-${list[it].timeUtcMs}-${list[it].kind}" }) { i ->
                                val e = list[i]
                                GroupedRow(i, list.size, Modifier.animateItem()) {
                                    EventRow(e, ContactBook.nameFor(contacts, e.client, e.user)) {
                                        if (ContactBook.canName(contacts, e.client, e.user)) naming = ContactBook.draft(contacts, e.client, e.user)
                                    }
                                }
                            }
                        }
                    }
                }
                else -> when {
                    state.events == null -> item { Muted(stringResource(R.string.checking), modifier = Modifier.padding(8.dp)) }
                    failed.isEmpty() -> item { Muted(stringResource(R.string.no_failed), modifier = Modifier.padding(8.dp)) }
                    else -> {
                        item { Muted(stringResource(R.string.failed_note), modifier = Modifier.padding(start = 8.dp, end = 8.dp, bottom = 10.dp)) }
                        val shown = failed.take(60)
                        items(shown.size, key = { "f-${shown[it].recordId}-${shown[it].timeUtcMs}" }) { i ->
                            val e = shown[i]
                            GroupedRow(i, shown.size, Modifier.animateItem()) {
                                Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp)) {
                                    Row {
                                        Text(e.user.ifBlank { "?" }, modifier = Modifier.weight(1f), fontWeight = FontWeight.Medium)
                                        Text(dateTime(e.timeUtcMs), color = Palette.LabelSecondary)
                                    }
                                    Muted(listOfNotNull(e.address.ifBlank { null }, e.detail.ifBlank { null }).joinToString(" · "))
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    naming?.let { ContactDialog(it, isNew = contacts.none { c -> c.id == it.id }, onDismiss = { naming = null }, vm = vm) }
    if (editing) ServerDialog(server, vm.groups(), onDismiss = { editing = false }) { vm.save(it); editing = false }
    if (deleting) AlertDialog(
        onDismissRequest = { deleting = false },
        title = { Text(stringResource(R.string.delete_title, server.title)) },
        confirmButton = { TextButton(onClick = { vm.delete(server.id) }) { Text(stringResource(R.string.delete), color = Palette.Red) } },
        dismissButton = { TextButton(onClick = { deleting = false }) { Text(stringResource(R.string.cancel)) } },
    )
}

@Composable
private fun Summary(server: ServerEntry, state: ServerState) {
    Column(Modifier.fillMaxWidth().clip(RectangleShape).background(Palette.Panel).padding(16.dp).animateContentSize()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            StatusDot(healthColor(state.health), 14, pulse = state.checking && state.sessions == null)
            Spacer(Modifier.width(10.dp))
            AnimatedContent(healthLabel(state), transitionSpec = { fadeIn(tween(200)) togetherWith fadeOut(tween(150)) }, label = "health",
                modifier = Modifier.weight(1f)) { label ->
                Text(label, style = MaterialTheme.typography.titleMedium, color = healthColor(state.health))
            }
            if (state.checking) CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp, color = Palette.Accent)
        }
        Spacer(Modifier.size(6.dp))
        Muted(listOfNotNull(server.host, server.group.ifBlank { null }, server.user).joinToString(" · "))
        if (state.checkedAt > 0) Muted(stringResource(R.string.checked_ago, ago(state.checkedAt)))
        if (state.health == Health.Problem) Text(errorExplanation(state), color = Red, modifier = Modifier.padding(top = 8.dp))
    }
}

@Composable
private fun EventRow(e: AccessEvent, nickname: String?, onLongClick: () -> Unit) {
    val (label, color) = when (e.kind) {
        AccessEvent.Kind.SignIn -> stringResource(R.string.ev_signin) to Green
        AccessEvent.Kind.SignOut -> stringResource(R.string.ev_signout) to Grey
        AccessEvent.Kind.Disconnect -> stringResource(R.string.ev_disconnect) to Orange
        AccessEvent.Kind.Reconnect -> stringResource(R.string.ev_reconnect) to Green
        AccessEvent.Kind.FailedSignIn -> stringResource(R.string.ev_failed) to Red
        AccessEvent.Kind.Logon -> stringResource(R.string.ev_signin) to Green
    }
    Row(Modifier.fillMaxWidth().pressable(onLongClick = onLongClick) {}.padding(horizontal = 16.dp, vertical = 12.dp), verticalAlignment = Alignment.Top) {
        Box(Modifier.padding(top = 6.dp)) { StatusDot(color, 10) }
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            // The computer tells people apart: they all use the same administrator account.
            val user = e.user.substringAfter('\\').ifBlank { null }
            Text(nickname ?: e.client.ifBlank { user ?: e.address }, fontWeight = FontWeight.SemiBold)
            Text(label, color = color, style = MaterialTheme.typography.bodyMedium)
            val extra = listOfNotNull(e.client.takeIf { nickname != null && it.isNotBlank() }, user.takeIf { e.client.isNotBlank() || nickname != null }, e.address.ifBlank { null }?.takeIf { it != "LOCAL" })
            if (extra.isNotEmpty()) Muted(extra.joinToString(" · "))
        }
        Text(clock(e.timeUtcMs), color = Palette.LabelSecondary)
    }
}
