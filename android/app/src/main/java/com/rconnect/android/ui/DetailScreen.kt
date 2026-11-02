package com.rconnect.android.ui

import androidx.compose.foundation.layout.Arrangement
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
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.rconnect.android.AppViewModel
import com.rconnect.android.Health
import com.rconnect.android.R
import com.rconnect.android.ServerState
import com.rconnect.android.data.ServerEntry
import com.rconnect.android.winrm.AccessEvent

private enum class Section { Now, History, Failed }

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DetailScreen(vm: AppViewModel, server: ServerEntry) {
    val states by vm.states.collectAsStateWithLifecycle()
    val state = states[server.id] ?: ServerState()
    var section by remember { mutableStateOf(Section.Now) }
    var editing by remember { mutableStateOf(false) }
    var deleting by remember { mutableStateOf(false) }
    val (failed, history) = state.events.orEmpty().partition { it.kind == AccessEvent.Kind.FailedSignIn }

    Column(Modifier.fillMaxSize()) {
        TopAppBar(
            title = { Text(server.title, maxLines = 1) },
            navigationIcon = { IconButton(onClick = { vm.select(null) }) { Icon(Icons.Default.ArrowBack, stringResource(R.string.back)) } },
            actions = {
                IconButton(onClick = { vm.refresh(server.id, force = true) }) { Icon(Icons.Default.Refresh, stringResource(R.string.refresh)) }
                IconButton(onClick = { editing = true }) { Icon(Icons.Default.Edit, stringResource(R.string.edit)) }
                IconButton(onClick = { deleting = true }) { Icon(Icons.Default.Delete, stringResource(R.string.delete)) }
            },
        )
        LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(16.dp)) {
            item(key = "summary") { Summary(server, state) }
            item(key = "chips") {
                Row(Modifier.padding(vertical = 12.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Chip(stringResource(R.string.tab_now, state.connected.size), section == Section.Now) { section = Section.Now }
                    Chip(stringResource(R.string.tab_history), section == Section.History) { section = Section.History }
                    Chip(stringResource(R.string.tab_failed, failed.size), section == Section.Failed) { section = Section.Failed }
                }
            }
            when (section) {
                Section.Now -> {
                    val sessions = state.sessions
                    when {
                        sessions == null -> item { Muted(stringResource(R.string.checking)) }
                        sessions.isEmpty() -> item { Muted(stringResource(R.string.nobody)) }
                        else -> items(sessions, key = { "s-${it.id}" }) { SessionRow(it); HorizontalDivider() }
                    }
                }
                Section.History -> when {
                    state.events == null -> item { Muted(stringResource(R.string.checking)) }
                    history.isEmpty() -> item { Muted(stringResource(R.string.no_history)) }
                    else -> {
                        var day = ""
                        for (e in history) {
                            val d = dayLabel(e.timeUtcMs)
                            if (d != day) {
                                day = d
                                item(key = "day-$d") { Text(d, fontWeight = FontWeight.SemiBold, modifier = Modifier.padding(top = 14.dp, bottom = 4.dp)) }
                            }
                            item(key = "e-${e.recordId}-${e.timeUtcMs}-${e.kind}") { EventRow(e) }
                        }
                    }
                }
                Section.Failed -> when {
                    state.events == null -> item { Muted(stringResource(R.string.checking)) }
                    failed.isEmpty() -> item { Muted(stringResource(R.string.no_failed)) }
                    else -> {
                        item { Muted(stringResource(R.string.failed_note), modifier = Modifier.padding(bottom = 8.dp)) }
                        items(failed.take(60), key = { "f-${it.recordId}-${it.timeUtcMs}" }) { e ->
                            Column(Modifier.fillMaxWidth().padding(vertical = 6.dp)) {
                                Row {
                                    Text(e.user.ifBlank { "?" }, modifier = Modifier.weight(1f))
                                    Text(dateTime(e.timeUtcMs), color = MaterialTheme.colorScheme.onSurfaceVariant)
                                }
                                Muted(listOfNotNull(e.address.ifBlank { null }, e.detail.ifBlank { null }).joinToString(" · "))
                            }
                            HorizontalDivider()
                        }
                    }
                }
            }
        }
    }
    if (editing) ServerDialog(server, vm.groups(), onDismiss = { editing = false }) { vm.save(it); editing = false }
    if (deleting) AlertDialog(
        onDismissRequest = { deleting = false },
        title = { Text(stringResource(R.string.delete_title, server.title)) },
        confirmButton = { TextButton(onClick = { vm.delete(server.id) }) { Text(stringResource(R.string.delete)) } },
        dismissButton = { TextButton(onClick = { deleting = false }) { Text(stringResource(R.string.cancel)) } },
    )
}

@Composable
private fun Chip(label: String, selected: Boolean, onClick: () -> Unit) =
    FilterChip(selected = selected, onClick = onClick, label = { Text(label, maxLines = 1) })

@Composable
private fun Summary(server: ServerEntry, state: ServerState) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                StatusDot(healthColor(state.health), 14)
                Spacer(Modifier.width(10.dp))
                Text(healthLabel(state), style = MaterialTheme.typography.titleMedium, color = healthColor(state.health), modifier = Modifier.weight(1f))
                if (state.checking) CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
            }
            Spacer(Modifier.size(6.dp))
            Muted(listOfNotNull(server.host, server.group.ifBlank { null }, server.user).joinToString(" · "))
            if (state.checkedAt > 0) Muted(stringResource(R.string.checked_ago, ago(state.checkedAt)))
            if (state.health == Health.Problem) Text(errorExplanation(state), color = Red, modifier = Modifier.padding(top = 8.dp))
        }
    }
}

@Composable
private fun EventRow(e: AccessEvent) {
    val (label, color) = when (e.kind) {
        AccessEvent.Kind.SignIn -> stringResource(R.string.ev_signin) to Green
        AccessEvent.Kind.SignOut -> stringResource(R.string.ev_signout) to Grey
        AccessEvent.Kind.Disconnect -> stringResource(R.string.ev_disconnect) to Orange
        AccessEvent.Kind.Reconnect -> stringResource(R.string.ev_reconnect) to Green
        AccessEvent.Kind.FailedSignIn -> stringResource(R.string.ev_failed) to Red
        AccessEvent.Kind.Logon -> stringResource(R.string.ev_signin) to Green
    }
    Row(Modifier.fillMaxWidth().padding(vertical = 10.dp), verticalAlignment = Alignment.Top) {
        Box(Modifier.padding(top = 6.dp)) { StatusDot(color, 10) }
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            // The computer tells people apart: they all use the same administrator account.
            val user = e.user.substringAfter('\\').ifBlank { null }
            Text(e.client.ifBlank { user ?: e.address }, fontWeight = FontWeight.SemiBold)
            Text(label, color = color, style = MaterialTheme.typography.bodyMedium)
            val extra = listOfNotNull(user.takeIf { e.client.isNotBlank() }, e.address.ifBlank { null }?.takeIf { it != "LOCAL" })
            if (extra.isNotEmpty()) Muted(extra.joinToString(" · "))
        }
        Text(clock(e.timeUtcMs), color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
    HorizontalDivider()
}
