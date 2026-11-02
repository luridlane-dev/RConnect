package com.rconnect.android.ui

import androidx.compose.foundation.clickable
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
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Clear
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.pulltorefresh.PullToRefreshBox
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.rconnect.android.AppViewModel
import com.rconnect.android.Health
import com.rconnect.android.R
import com.rconnect.android.ServerState
import com.rconnect.android.data.ServerEntry

private data class Item(val entry: ServerEntry, val state: ServerState)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ServersScreen(vm: AppViewModel) {
    val servers by vm.servers.collectAsStateWithLifecycle()
    val states by vm.states.collectAsStateWithLifecycle()
    val filter by vm.filter.collectAsStateWithLifecycle()
    val query by vm.query.collectAsStateWithLifecycle()
    var adding by remember { mutableStateOf(false) }
    var importing by remember { mutableStateOf(false) }
    var menu by remember { mutableStateOf(false) }
    val collapsed = remember { mutableStateListOf<String>() }
    var pulled by remember { mutableStateOf(false) }

    val rows = servers.map { Item(it, states[it.id] ?: ServerState()) }
    val counts = Health.entries.associateWith { h -> rows.count { it.state.health == h } }
    val checking = rows.count { it.state.checking }
    val needle = query.trim().lowercase()
    val visible = rows.filter { r ->
        (filter == null || r.state.health == filter) &&
            (needle.isEmpty() || matches(r, needle))
    }.sortedWith(compareBy({ it.entry.group.isBlank() }, { it.entry.group.lowercase() }, { it.entry.title.lowercase() }))
    LaunchedEffect(checking) { if (checking == 0) pulled = false }
    val grouped = visible.groupBy { it.entry.group }
    val showGroups = servers.any { it.group.isNotBlank() }

    Column(Modifier.fillMaxSize()) {
        TopAppBar(
            title = { Text(stringResource(R.string.app_name)) },
            actions = {
                IconButton(onClick = { menu = true }) { Icon(Icons.Default.MoreVert, stringResource(R.string.more)) }
                DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                    DropdownMenuItem(text = { Text(stringResource(R.string.refresh_all)) }, onClick = { menu = false; vm.refreshAll(force = true) })
                    DropdownMenuItem(text = { Text(stringResource(R.string.import_title)) }, onClick = { menu = false; importing = true })
                }
            },
        )
        if (checking > 0) LinearProgressIndicator(progress = { 1f - checking.toFloat() / rows.size.coerceAtLeast(1) }, modifier = Modifier.fillMaxWidth())
        else Spacer(Modifier.size(4.dp))

        if (servers.isEmpty()) {
            Box(Modifier.fillMaxSize().padding(32.dp), contentAlignment = Alignment.Center) {
                Text(stringResource(R.string.empty_hint), color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        } else {
            // The four numbers are also the filter buttons.
            Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                SummaryTile(rows.size, stringResource(R.string.tile_all), MaterialTheme.colorScheme.primary, filter == null, Modifier.weight(1f)) { vm.filter.value = null }
                SummaryTile(counts[Health.InUse] ?: 0, stringResource(R.string.tile_in_use), Orange, filter == Health.InUse, Modifier.weight(1f)) { vm.filter.value = toggle(filter, Health.InUse) }
                SummaryTile(counts[Health.Free] ?: 0, stringResource(R.string.tile_free), Green, filter == Health.Free, Modifier.weight(1f)) { vm.filter.value = toggle(filter, Health.Free) }
                SummaryTile(counts[Health.Problem] ?: 0, stringResource(R.string.tile_problem), Red, filter == Health.Problem, Modifier.weight(1f)) { vm.filter.value = toggle(filter, Health.Problem) }
            }
            OutlinedTextField(
                query, { vm.query.value = it }, singleLine = true,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp),
                placeholder = { Text(stringResource(R.string.search_servers)) },
                leadingIcon = { Icon(Icons.Default.Search, null) },
                trailingIcon = { if (query.isNotEmpty()) IconButton(onClick = { vm.query.value = "" }) { Icon(Icons.Default.Clear, stringResource(R.string.clear)) } },
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
            )
            Box(Modifier.weight(1f)) {
                PullToRefreshBox(isRefreshing = pulled && checking > 0, onRefresh = { pulled = true; vm.refreshAll(force = true) }, modifier = Modifier.fillMaxSize()) {
                    if (visible.isEmpty()) {
                        Box(Modifier.fillMaxSize().padding(32.dp), contentAlignment = Alignment.Center) {
                            Text(stringResource(R.string.no_match), color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    } else LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = 88.dp)) {
                        for ((group, list) in grouped) {
                            if (showGroups) item(key = "group-$group") {
                                GroupHeader(group.ifBlank { stringResource(R.string.no_group) }, list, group in collapsed) {
                                    if (group in collapsed) collapsed.remove(group) else collapsed.add(group)
                                }
                            }
                            if (group !in collapsed) items(list, key = { it.entry.id }) { r ->
                                ServerRow(r.entry, r.state) { vm.select(r.entry.id) }
                                HorizontalDivider(Modifier.padding(start = 40.dp))
                            }
                        }
                    }
                }
                FloatingActionButton(onClick = { adding = true }, modifier = Modifier.align(Alignment.BottomEnd).padding(16.dp)) {
                    Icon(Icons.Default.Add, stringResource(R.string.add_server))
                }
            }
        }
        if (servers.isEmpty()) Box(Modifier.fillMaxWidth().padding(16.dp), contentAlignment = Alignment.BottomEnd) {
            FloatingActionButton(onClick = { adding = true }) { Icon(Icons.Default.Add, stringResource(R.string.add_server)) }
        }
    }
    if (adding) ServerDialog(null, vm.groups(), onDismiss = { adding = false }) { vm.save(it); adding = false }
    if (importing) ImportDialog(onDismiss = { importing = false }) { vm.import(it) }
}

private fun toggle(current: Health?, wanted: Health): Health? = if (current == wanted) null else wanted

private fun matches(r: Item, needle: String): Boolean =
    r.entry.title.lowercase().contains(needle) || r.entry.host.contains(needle) || r.entry.group.lowercase().contains(needle) ||
        r.state.sessions.orEmpty().any { it.user.lowercase().contains(needle) || it.client.lowercase().contains(needle) }

@Composable
private fun GroupHeader(title: String, list: List<Item>, collapsed: Boolean, onClick: () -> Unit) {
    val inUse = list.count { it.state.health == Health.InUse }
    val problems = list.count { it.state.health == Health.Problem }
    Row(
        Modifier.fillMaxWidth().clickable(onClick = onClick).padding(horizontal = 16.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(if (collapsed) "▸" else "▾", color = MaterialTheme.colorScheme.primary)
        Spacer(Modifier.width(8.dp))
        Text(title, fontWeight = FontWeight.SemiBold, color = MaterialTheme.colorScheme.primary, modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
        Muted(buildString {
            append(pluralStringResource(R.plurals.group_count, list.size, list.size))
            if (inUse > 0) append(" · ").append(stringResource(R.string.group_in_use, inUse))
            if (problems > 0) append(" · ").append(stringResource(R.string.group_problems, problems))
        })
    }
}

@Composable
private fun ServerRow(server: ServerEntry, state: ServerState, onClick: () -> Unit) {
    Row(Modifier.fillMaxWidth().clickable(onClick = onClick).padding(horizontal = 16.dp, vertical = 12.dp), verticalAlignment = Alignment.Top) {
        Box(Modifier.padding(top = 5.dp)) { StatusDot(healthColor(state.health)) }
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(server.title, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                Spacer(Modifier.width(8.dp))
                Text(healthLabel(state), color = healthColor(state.health), style = MaterialTheme.typography.labelLarge, maxLines = 1)
            }
            if (server.name.isNotBlank()) Muted(server.host, maxLines = 1)
            when (state.health) {
                Health.InUse -> Text(
                    state.connected.joinToString("  ·  ") { s -> if (s.client.isNotBlank()) s.client else s.user.substringAfter('\\') },
                    style = MaterialTheme.typography.bodyMedium, maxLines = 2, overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.padding(top = 2.dp),
                )
                Health.Problem -> if (state.errorKind == ServerState.ErrorKind.Other) Muted(errorExplanation(state), maxLines = 2)
                else -> {}
            }
        }
    }
}
