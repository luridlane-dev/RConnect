package com.rconnect.android.ui

import androidx.compose.animation.animateContentSize
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.LinearProgressIndicator
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
import androidx.compose.ui.draw.clip
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.rconnect.android.AppViewModel
import com.rconnect.android.Health
import com.rconnect.android.R
import com.rconnect.android.ServerFilter
import com.rconnect.android.ServerState
import com.rconnect.android.data.ContactIndex
import com.rconnect.android.data.ServerEntry
import java.util.Locale

private data class ServerItem(val entry: ServerEntry, val state: ServerState)
private data class ServerSummaryCounts(
    val rows: List<ServerItem>,
    val favCount: Int,
    val availableCount: Int,
    val inUseCount: Int,
    val offlineCount: Int,
    val checking: Int,
)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ServersScreen(vm: AppViewModel) {
    val c = appColors
    val servers by vm.servers.collectAsStateWithLifecycle()
    val contacts by vm.contacts.collectAsStateWithLifecycle()
    val states by vm.states.collectAsStateWithLifecycle()
    val filter by vm.filter.collectAsStateWithLifecycle()
    val query by vm.serverQuery.collectAsStateWithLifecycle()
    val listView by vm.listView.collectAsStateWithLifecycle()

    var adding by remember { mutableStateOf(false) }
    var transfer by remember { mutableStateOf<Transfer?>(null) }
    var notifying by remember { mutableStateOf(false) }
    var about by remember { mutableStateOf(false) }
    var settings by remember { mutableStateOf(false) }
    val collapsed = remember { mutableStateListOf<String>() }
    var pulled by remember { mutableStateOf(false) }

    val contactIndex = remember(contacts) { ContactIndex(contacts) }
    val summary = remember(servers, states) {
        val list = ArrayList<ServerItem>(servers.size)
        var fav = 0
        var avail = 0
        var inUse = 0
        var offline = 0
        var chk = 0
        for (s in servers) {
            val st = states[s.id] ?: ServerState()
            list += ServerItem(s, st)
            if (s.favorite) fav++
            when (st.health) {
                Health.Free -> avail++
                Health.InUse -> inUse++
                Health.Problem -> offline++
                Health.Pending -> {}
            }
            if (st.checking) chk++
        }
        ServerSummaryCounts(list, fav, avail, inUse, offline, chk)
    }
    val rows = summary.rows
    val favCount = summary.favCount
    val availableCount = summary.availableCount
    val inUseCount = summary.inUseCount
    val offlineCount = summary.offlineCount
    val checking = summary.checking

    val visible = remember(rows, filter, query, contactIndex) {
        val needle = query.trim().lowercase(Locale.ROOT)
        rows.filter { r ->
            val modeOk = when (filter) {
                ServerFilter.All -> true
                ServerFilter.Favorites -> r.entry.favorite
                ServerFilter.Available -> r.state.health == Health.Free
                ServerFilter.InUse -> r.state.health == Health.InUse
                ServerFilter.Offline -> r.state.health == Health.Problem
            }
            modeOk && (needle.isEmpty() || matches(r, needle, contactIndex))
        }.sortedWith(
            Comparator { a, b ->
                // Named groups first, favourites at the top of each group, then natural order by name.
                val ga = a.entry.group
                val gb = b.entry.group
                if (ga != gb) {
                    if (ga.isEmpty() || gb.isEmpty()) return@Comparator if (gb.isEmpty()) -1 else 1
                    val gc = naturalCompare(ga, gb)
                    if (gc != 0) return@Comparator gc
                }
                if (a.entry.favorite != b.entry.favorite) return@Comparator if (a.entry.favorite) -1 else 1
                naturalCompare(a.entry.title, b.entry.title)
            },
        )
    }
    LaunchedEffect(checking) { if (checking == 0) pulled = false }
    val grouped = remember(visible) { visible.groupBy { it.entry.group } }
    val showGroups = remember(servers) { servers.any { it.group.isNotBlank() } }
    val allGroups = remember(servers) { servers.map { it.group }.filter { it.isNotBlank() }.distinct().sorted() }

    Column(Modifier.fillMaxSize()) {
        LargeTitleBar(stringResource(R.string.tab_servers)) {
            PrimaryActionButton(stringResource(R.string.add_server_cta), onClick = { adding = true })
            if (servers.isNotEmpty()) {
                ToolButton(Icons.Default.Refresh, stringResource(R.string.refresh_all)) { vm.refreshAll(force = true) }
            }
            ToolButton(Icons.Default.Settings, stringResource(R.string.settings)) { settings = true }
        }

        if (checking > 0) {
            LinearProgressIndicator(
                progress = { 1f - checking.toFloat() / rows.size.coerceAtLeast(1) },
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp).height(2.dp),
                color = c.accent,
                trackColor = c.surfaceAlt,
            )
        } else {
            Spacer(Modifier.height(2.dp))
        }

        if (servers.isEmpty()) {
            Box(Modifier.fillMaxSize().padding(32.dp), contentAlignment = Alignment.Center) {
                Text(stringResource(R.string.empty_hint), color = c.text2, textAlign = TextAlign.Center)
            }
        } else {
            SearchField(
                value = query,
                onChange = { vm.serverQuery.value = it },
                placeholder = stringResource(R.string.search_servers),
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
            )
            Row(
                Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, bottom = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                val filterOptions = listOf(
                    "${stringResource(R.string.tile_all)} ${rows.size}",
                    "${stringResource(R.string.tile_favorites)} $favCount",
                    "${stringResource(R.string.tile_available)} $availableCount",
                    "${stringResource(R.string.tile_in_use)} $inUseCount",
                    "${stringResource(R.string.tile_offline)} $offlineCount",
                )
                ScrollableSegmented(
                    options = filterOptions,
                    selected = filter.ordinal,
                    onSelect = { vm.filter.value = ServerFilter.entries[it] },
                    modifier = Modifier.weight(1f),
                )
                ViewModeToggle(listMode = listView, onSelect = { vm.setListView(it) })
            }

            PullToRefreshBox(
                isRefreshing = pulled && checking > 0,
                onRefresh = { if (servers.isNotEmpty()) { pulled = true; vm.refreshAll(force = true) } },
                modifier = Modifier.weight(1f).fillMaxWidth(),
            ) {
                if (visible.isEmpty()) {
                    Box(Modifier.fillMaxSize().padding(32.dp), contentAlignment = Alignment.Center) {
                        Text(stringResource(R.string.no_match), color = c.text2)
                    }
                } else {
                    LazyColumn(
                        Modifier.fillMaxSize(),
                        contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 4.dp, bottom = 16.dp),
                    ) {
                        for ((group, list) in grouped) {
                            val isCollapsed = group in collapsed
                            if (showGroups) {
                                item(key = "group-$group") {
                                    val inUse = list.count { it.state.health == Health.InUse }
                                    val problems = list.count { it.state.health == Health.Problem }
                                    val subtitle = buildList {
                                        if (inUse > 0) add(stringResource(R.string.group_in_use, inUse))
                                        if (problems > 0) add(pluralStringResource(R.plurals.group_problems, problems, problems))
                                    }.joinToString(" · ").ifEmpty { null }
                                    DesktopGroupHeader(
                                        title = group.ifBlank { stringResource(R.string.no_group) },
                                        count = list.size,
                                        collapsed = isCollapsed,
                                        subtitle = subtitle,
                                    ) {
                                        if (isCollapsed) collapsed.remove(group) else collapsed.add(group)
                                    }
                                }
                            }
                            if (!isCollapsed) {
                                items(list.size, key = { list[it].entry.id }) { i ->
                                    val r = list[i]
                                    if (listView) {
                                        val rowFill = if (r.state.health == Health.InUse) c.busySurface else c.surface
                                        GroupedRow(i, list.size, Modifier.animateItem(), backgroundColor = rowFill) {
                                            ServerListRow(
                                                server = r.entry,
                                                state = r.state,
                                                contacts = contactIndex,
                                                onFavorite = { vm.toggleFavorite(r.entry.id) },
                                                onClick = { vm.select(r.entry.id) },
                                            )
                                        }
                                    } else {
                                        Box(Modifier.animateItem().padding(bottom = 8.dp)) {
                                            ServerCard(
                                                server = r.entry,
                                                state = r.state,
                                                contacts = contactIndex,
                                                onFavorite = { vm.toggleFavorite(r.entry.id) },
                                                onClick = { vm.select(r.entry.id) },
                                            )
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (adding) ServerDialog(null, allGroups, onDismiss = { adding = false }) { vm.save(it); adding = false }
    if (settings) SettingsDialog(
        vm = vm,
        onDismiss = { settings = false },
        onImportServers = { transfer = Transfer.Import },
        onExportServers = { transfer = Transfer.Export },
        onNotifications = { notifying = true },
        onAbout = { about = true },
    )
    transfer?.let { TransferHost(vm, it) { transfer = null } }
    if (notifying) NotificationsDialog(vm) { notifying = false }
    if (about) AboutDialog { about = false }
}

private fun matches(r: ServerItem, needle: String, contacts: ContactIndex): Boolean =
    r.entry.title.lowercase(Locale.ROOT).contains(needle) ||
        r.entry.host.lowercase(Locale.ROOT).contains(needle) ||
        r.entry.group.lowercase(Locale.ROOT).contains(needle) ||
        r.entry.user.lowercase(Locale.ROOT).contains(needle) ||
        r.state.sessions.orEmpty().any {
            it.user.lowercase(Locale.ROOT).contains(needle) ||
                it.client.lowercase(Locale.ROOT).contains(needle) ||
                contacts.nameFor(it.client, it.user).orEmpty().lowercase(Locale.ROOT).contains(needle)
        }

@Composable
private fun ServerCard(
    server: ServerEntry,
    state: ServerState,
    contacts: ContactIndex,
    onFavorite: () -> Unit,
    onClick: () -> Unit,
) {
    val c = appColors
    val busy = state.health == Health.InUse
    val fill = if (busy) c.busySurface else c.surface
    val dotColor = healthColor(state)

    Row(
        Modifier
            .fillMaxWidth()
            .pressable(onClick = onClick)
            .clip(RadiusCard)
            .background(fill)
            .border(1.dp, c.border, RadiusCard)
            .padding(start = 12.dp, end = 10.dp, top = 11.dp, bottom = 11.dp)
            .animateContentSize(),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        MonitorTile(
            dotColor = dotColor,
            cardFill = fill,
            pulse = state.checking && state.sessions == null,
        )
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    server.title,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.SemiBold,
                    color = c.text,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f),
                )
                if (state.latencyMs >= 0 && state.health != Health.Problem) {
                    Spacer(Modifier.width(6.dp))
                    Text(
                        stringResource(R.string.ms_format, state.latencyMs),
                        fontSize = 11.sp,
                        color = c.text3,
                        maxLines = 1,
                    )
                }
                FavoriteStar(filled = server.favorite, onClick = onFavorite)
            }
            Text(
                if (server.port == 3389) server.host else "${server.host}:${server.port}",
                fontSize = 12.sp,
                color = c.text2,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Spacer(Modifier.height(4.dp))
            ServerStatusLine(state = state, contacts = contacts)
        }
    }
}

@Composable
private fun ServerListRow(
    server: ServerEntry,
    state: ServerState,
    contacts: ContactIndex,
    onFavorite: () -> Unit,
    onClick: () -> Unit,
) {
    val c = appColors
    Row(
        Modifier
            .fillMaxWidth()
            .pressable(onClick = onClick)
            .padding(start = 14.dp, end = 8.dp, top = 10.dp, bottom = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        StatusDot(healthColor(state), size = 8, pulse = state.checking && state.sessions == null)
        Spacer(Modifier.width(10.dp))
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    server.title,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.SemiBold,
                    color = c.text,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f),
                )
                if (state.latencyMs >= 0 && state.health != Health.Problem) {
                    Spacer(Modifier.width(6.dp))
                    Text(stringResource(R.string.ms_format, state.latencyMs), fontSize = 11.sp, color = c.text3, maxLines = 1)
                }
            }
            Text(
                if (server.port == 3389) server.host else "${server.host}:${server.port}",
                fontSize = 12.sp,
                color = c.text2,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Spacer(Modifier.height(2.dp))
            ServerStatusLine(state = state, contacts = contacts)
        }
        FavoriteStar(filled = server.favorite, onClick = onFavorite)
    }
}

@Composable
private fun ServerStatusLine(state: ServerState, contacts: ContactIndex) {
    val c = appColors
    when (state.health) {
        Health.InUse -> {
            val first = state.connected.firstOrNull()
            if (first != null) {
                val primaryName = contacts.nameFor(first.client, first.user)
                    ?: first.client.ifBlank { first.user.substringAfter('\\') }
                val who = if (state.connected.size > 1) "$primaryName +${state.connected.size - 1}" else primaryName
                val activity = compactActivityText(first, state.serverNowUtcMs)
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        who,
                        fontSize = 13.sp,
                        fontWeight = FontWeight.Bold,
                        color = c.busyText,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f),
                    )
                    if (activity.isNotEmpty()) {
                        Spacer(Modifier.width(8.dp))
                        Text(activity, fontSize = 12.sp, color = c.text2, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    }
                }
            }
        }
        Health.Problem -> {
            val isOffline = state.errorKind == ServerState.ErrorKind.Unreachable
            Text(
                healthLabel(state),
                fontSize = 12.sp,
                fontWeight = FontWeight.Medium,
                color = if (isOffline) c.text3 else c.red,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        Health.Free -> {
            Text(
                stringResource(R.string.status_free),
                fontSize = 12.sp,
                color = c.green,
                maxLines = 1,
            )
        }
        Health.Pending -> {
            Text(
                stringResource(R.string.status_pending),
                fontSize = 12.sp,
                color = c.text3,
                maxLines = 1,
            )
        }
    }
}
