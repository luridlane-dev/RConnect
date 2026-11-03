package com.rconnect.android.ui

import androidx.compose.animation.animateContentSize
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
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
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.pulltorefresh.PullToRefreshBox
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.res.pluralStringResource
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

private data class Item(val entry: ServerEntry, val state: ServerState)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ServersScreen(vm: AppViewModel) {
    val servers by vm.servers.collectAsStateWithLifecycle()
    val contacts by vm.contacts.collectAsStateWithLifecycle()
    val states by vm.states.collectAsStateWithLifecycle()
    val filter by vm.filter.collectAsStateWithLifecycle()
    val query by vm.query.collectAsStateWithLifecycle()
    var adding by remember { mutableStateOf(false) }
    var transfer by remember { mutableStateOf<Transfer?>(null) }
    var notifying by remember { mutableStateOf(false) }
    var about by remember { mutableStateOf(false) }
    var menu by remember { mutableStateOf(false) }
    val collapsed = remember { mutableStateListOf<String>() }
    var pulled by remember { mutableStateOf(false) }

    val rows = servers.map { Item(it, states[it.id] ?: ServerState()) }
    val counts = Health.entries.associateWith { h -> rows.count { it.state.health == h } }
    val checking = rows.count { it.state.checking }
    val needle = query.trim().lowercase()
    val visible = rows.filter { r ->
        (filter == null || r.state.health == filter) && (needle.isEmpty() || matches(r, needle, contacts))
    }.sortedWith(compareBy({ it.entry.group.isBlank() }, { it.entry.group.lowercase() }, { it.entry.title.lowercase() }))
    LaunchedEffect(checking) { if (checking == 0) pulled = false }
    val grouped = visible.groupBy { it.entry.group }
    val showGroups = servers.any { it.group.isNotBlank() }

    Column(Modifier.fillMaxSize()) {
        LargeTitleBar(stringResource(R.string.app_name)) {
            ToolButton(Icons.Default.Add, stringResource(R.string.add_server)) { adding = true }
            Box {
                ToolButton(Icons.Default.MoreVert, stringResource(R.string.more)) { menu = true }
                DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                    DropdownMenuItem(text = { Text(stringResource(R.string.refresh_all)) }, onClick = { menu = false; vm.refreshAll(force = true) })
                    DropdownMenuItem(text = { Text(stringResource(R.string.import_title)) }, onClick = { menu = false; transfer = Transfer.Import })
                    DropdownMenuItem(text = { Text(stringResource(R.string.export_title)) }, enabled = servers.isNotEmpty(),
                        onClick = { menu = false; transfer = Transfer.Export })
                    DropdownMenuItem(text = { Text(stringResource(R.string.notifications_title)) }, onClick = { menu = false; notifying = true })
                    DropdownMenuItem(text = { Text(stringResource(R.string.menu_about)) }, onClick = { menu = false; about = true })
                }
            }
        }
        if (checking > 0) LinearProgressIndicator(
            progress = { 1f - checking.toFloat() / rows.size.coerceAtLeast(1) },
            modifier = Modifier.fillMaxWidth().padding(horizontal = 20.dp), color = Palette.Accent, trackColor = Palette.Panel,
        ) else Spacer(Modifier.size(4.dp))

        if (servers.isEmpty()) {
            Box(Modifier.fillMaxSize().padding(32.dp), contentAlignment = Alignment.Center) {
                Text(stringResource(R.string.empty_hint), color = Palette.LabelSecondary)
            }
        } else {
            // The four numbers are also the filter buttons.
            Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                SummaryTile(rows.size, stringResource(R.string.tile_all), Palette.Accent, filter == null, Modifier.weight(1f)) { vm.filter.value = null }
                SummaryTile(counts[Health.InUse] ?: 0, stringResource(R.string.tile_in_use), Orange, filter == Health.InUse, Modifier.weight(1f)) { vm.filter.value = toggle(filter, Health.InUse) }
                SummaryTile(counts[Health.Free] ?: 0, stringResource(R.string.tile_free), Green, filter == Health.Free, Modifier.weight(1f)) { vm.filter.value = toggle(filter, Health.Free) }
                SummaryTile(counts[Health.Problem] ?: 0, stringResource(R.string.tile_problem), Red, filter == Health.Problem, Modifier.weight(1f)) { vm.filter.value = toggle(filter, Health.Problem) }
            }
            SearchField(query, { vm.query.value = it }, stringResource(R.string.search_servers), Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
            PullToRefreshBox(isRefreshing = pulled && checking > 0, onRefresh = { pulled = true; vm.refreshAll(force = true) }, modifier = Modifier.weight(1f).fillMaxWidth()) {
                if (visible.isEmpty()) {
                    Box(Modifier.fillMaxSize().padding(32.dp), contentAlignment = Alignment.Center) {
                        Text(stringResource(R.string.no_match), color = Palette.LabelSecondary)
                    }
                } else LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(start = 16.dp, end = 16.dp, bottom = 16.dp)) {
                    for ((group, list) in grouped) {
                        val isCollapsed = group in collapsed
                        if (showGroups) item(key = "group-$group") {
                            GroupHeader(group.ifBlank { stringResource(R.string.no_group) }, list, isCollapsed) {
                                if (isCollapsed) collapsed.remove(group) else collapsed.add(group)
                            }
                        }
                        if (!isCollapsed) items(list.size, key = { list[it].entry.id }) { i ->
                            val r = list[i]
                            GroupedRow(i, list.size, Modifier.animateItem()) { ServerRow(r.entry, r.state, contacts) { vm.select(r.entry.id) } }
                        }
                    }
                }
            }
        }
    }
    if (adding) ServerDialog(null, vm.groups(), onDismiss = { adding = false }) { vm.save(it); adding = false }
    transfer?.let { TransferHost(vm, it) { transfer = null } }
    if (notifying) NotificationsDialog(vm) { notifying = false }
    if (about) AboutDialog { about = false }
}

private fun toggle(current: Health?, wanted: Health): Health? = if (current == wanted) null else wanted

private fun matches(r: Item, needle: String, contacts: List<Contact>): Boolean =
    r.entry.title.lowercase().contains(needle) || r.entry.host.contains(needle) || r.entry.group.lowercase().contains(needle) ||
        r.state.sessions.orEmpty().any {
            it.user.lowercase().contains(needle) || it.client.lowercase().contains(needle) ||
                ContactBook.nameFor(contacts, it.client, it.user).orEmpty().lowercase().contains(needle)
        }

@Composable
private fun GroupHeader(title: String, list: List<Item>, collapsed: Boolean, onClick: () -> Unit) {
    val inUse = list.count { it.state.health == Health.InUse }
    val problems = list.count { it.state.health == Health.Problem }
    val turn by animateFloatAsState(if (collapsed) -90f else 0f, tween(220), label = "chevron")
    Row(
        Modifier.fillMaxWidth().pressable(onClick = onClick).padding(start = 8.dp, end = 8.dp, top = 16.dp, bottom = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(Icons.Default.KeyboardArrowDown, null, tint = Palette.Accent, modifier = Modifier.size(20.dp).rotate(turn))
        Spacer(Modifier.width(4.dp))
        Text(title, fontWeight = FontWeight.SemiBold, color = Palette.Accent, modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
        Muted(buildString {
            append(pluralStringResource(R.plurals.group_count, list.size, list.size))
            if (inUse > 0) append(" · ").append(stringResource(R.string.group_in_use, inUse))
            if (problems > 0) append(" · ").append(stringResource(R.string.group_problems, problems))
        })
    }
}

@Composable
private fun ServerRow(server: ServerEntry, state: ServerState, contacts: List<Contact>, onClick: () -> Unit) {
    Row(Modifier.fillMaxWidth().pressable(onClick = onClick).padding(horizontal = 16.dp, vertical = 12.dp), verticalAlignment = Alignment.Top) {
        Box(Modifier.padding(top = 5.dp)) { StatusDot(healthColor(state.health), pulse = state.checking && state.sessions == null) }
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f).animateContentSize()) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(server.title, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                Spacer(Modifier.width(8.dp))
                Text(healthLabel(state), color = healthColor(state.health), style = MaterialTheme.typography.labelLarge, maxLines = 1)
            }
            if (server.name.isNotBlank()) Muted(server.host, maxLines = 1)
            when (state.health) {
                Health.InUse -> Text(
                    state.connected.joinToString("  ·  ") { s ->
                        ContactBook.nameFor(contacts, s.client, s.user) ?: if (s.client.isNotBlank()) s.client else s.user.substringAfter('\\')
                    },
                    style = MaterialTheme.typography.bodyMedium, maxLines = 2, overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.padding(top = 2.dp),
                )
                Health.Problem -> if (state.errorKind == ServerState.ErrorKind.Other || state.errorKind == ServerState.ErrorKind.NoWinRm) Muted(errorExplanation(state), maxLines = 2)
                else -> {}
            }
        }
    }
}
