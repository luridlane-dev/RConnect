package com.rconnect.android.ui

import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
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
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Share
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
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.rconnect.android.AppViewModel
import com.rconnect.android.R
import com.rconnect.android.data.Contact
import com.rconnect.android.data.ContactBook
import com.rconnect.android.data.ContactIndex
import com.rconnect.android.data.EventStore
import com.rconnect.android.data.StoredEvent
import com.rconnect.android.winrm.AccessEvent
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

private enum class LogCategory { All, SignIns, Failed, Leaves, Security }

private val periodHours = intArrayOf(1, 24, 24 * 7, 0)

/** Unified activity history across every server: sign-ins, disconnections, failed sign-ins and security changes. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ActivityScreen(vm: AppViewModel) {
    val c = appColors
    val context = LocalContext.current
    val resources = LocalResources.current
    val scope = rememberCoroutineScope()

    val logs by vm.logs.collectAsStateWithLifecycle()
    val fetching by vm.fetchingLogs.collectAsStateWithLifecycle()
    val servers by vm.servers.collectAsStateWithLifecycle()
    val contacts by vm.contacts.collectAsStateWithLifecycle()
    val query by vm.logQuery.collectAsStateWithLifecycle()

    var category by remember { mutableStateOf(LogCategory.All) }
    var periodIndex by remember { mutableIntStateOf(2) } // 7 d default
    var serverFilter by remember { mutableStateOf<String?>(null) }
    var serverMenu by remember { mutableStateOf(false) }
    var naming by remember { mutableStateOf<Contact?>(null) }

    // Fetch automatically the first time the tab is opened if no logs are cached yet.
    LaunchedEffect(servers.size) {
        if (servers.isNotEmpty() && logs.isEmpty()) vm.refreshAllLogs()
    }

    val contactIndex = remember(contacts) { ContactIndex(contacts) }
    val sortedServers = remember(servers) { servers.sortedWith { a, b -> naturalCompare(a.title, b.title) } }
    val visible = remember(logs, category, periodIndex, serverFilter, query, contactIndex, resources) {
        val cutoff = periodHours[periodIndex].let { h -> if (h <= 0) 0L else System.currentTimeMillis() - h * 3600_000L }
        val needle = query.trim().lowercase(Locale.ROOT)
        logs.filter { item ->
            val e = item.event
            val catOk = when (category) {
                LogCategory.All -> true
                LogCategory.SignIns -> e.kind == AccessEvent.Kind.SignIn || e.kind == AccessEvent.Kind.Reconnect
                LogCategory.Failed -> e.kind == AccessEvent.Kind.FailedSignIn
                LogCategory.Leaves -> e.kind == AccessEvent.Kind.SignOut || e.kind == AccessEvent.Kind.Disconnect
                LogCategory.Security -> e.isSecurity
            }
            if (!catOk) return@filter false
            if (cutoff > 0L && e.timeUtcMs < cutoff) return@filter false
            if (serverFilter != null && item.serverId != serverFilter) return@filter false
            if (needle.isEmpty()) return@filter true
            val nick = contactIndex.nameFor(e.client, e.user).orEmpty()
            val label = eventKindTitle(resources, e.kind)
            val detail = localizedEventDetail(resources, e)
            item.serverTitle.lowercase(Locale.ROOT).contains(needle) ||
                nick.lowercase(Locale.ROOT).contains(needle) ||
                e.client.lowercase(Locale.ROOT).contains(needle) ||
                e.user.lowercase(Locale.ROOT).contains(needle) ||
                e.address.lowercase(Locale.ROOT).contains(needle) ||
                label.lowercase(Locale.ROOT).contains(needle) ||
                detail.lowercase(Locale.ROOT).contains(needle)
        }
    }
    val days = remember(visible) { visible.take(400).groupBy { dayLabel(it.event.timeUtcMs) } }

    val exportCsvLauncher = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("text/csv")) { uri ->
        if (uri != null) {
            val snapshot = visible
            scope.launch {
                val ok = withContext(Dispatchers.IO) {
                    runCatching {
                        val csv = EventStore.toCsv(
                            snapshot,
                            contacts,
                            eventName = { eventKindTitle(resources, it) },
                            detailLabel = { localizedEventDetail(resources, it) },
                        )
                        context.contentResolver.openOutputStream(uri)?.use { it.write(csv.toByteArray(Charsets.UTF_8)) } != null
                    }.getOrDefault(false)
                }
                Toast.makeText(
                    context,
                    resources.getString(if (ok) R.string.logs_export_done else R.string.contacts_file_error),
                    Toast.LENGTH_LONG,
                ).show()
            }
        }
    }

    Column(Modifier.fillMaxSize()) {
        LargeTitleBar(stringResource(R.string.logs_title)) {
            if (visible.isNotEmpty()) {
                ToolButton(Icons.Default.Share, stringResource(R.string.logs_export_csv)) {
                    val stamp = SimpleDateFormat("yyyyMMdd-HHmm", Locale.US).format(Date())
                    exportCsvLauncher.launch("rconnect-activity-$stamp.csv")
                }
            }
            ToolButton(Icons.Default.Refresh, stringResource(R.string.logs_fetch)) { vm.refreshAllLogs() }
        }
        if (fetching) {
            LinearProgressIndicator(
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp).height(2.dp),
                color = c.accent,
                trackColor = c.surfaceAlt,
            )
        } else {
            Spacer(Modifier.height(2.dp))
        }

        SearchField(
            value = query,
            onChange = { vm.logQuery.value = it },
            placeholder = stringResource(R.string.logs_search),
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 6.dp),
        )

        ScrollableSegmented(
            options = listOf(
                stringResource(R.string.logs_cat_all),
                stringResource(R.string.logs_cat_signins),
                stringResource(R.string.logs_cat_failed),
                stringResource(R.string.logs_cat_leaves),
                stringResource(R.string.logs_cat_security),
            ),
            selected = category.ordinal,
            onSelect = { category = LogCategory.entries[it] },
            modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp),
        )

        Row(
            Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            ScrollableSegmented(
                options = listOf(
                    stringResource(R.string.logs_period_1h),
                    stringResource(R.string.logs_period_24h),
                    stringResource(R.string.logs_period_7d),
                    stringResource(R.string.logs_period_all),
                ),
                selected = periodIndex,
                onSelect = { periodIndex = it },
                modifier = Modifier.weight(1f),
            )
            Box {
                val chosenTitle = servers.firstOrNull { it.id == serverFilter }?.title
                    ?: stringResource(R.string.logs_all_servers)
                Row(
                    Modifier
                        .height(34.dp)
                        .clip(RadiusControl)
                        .background(c.button)
                        .border(1.dp, c.border, RadiusControl)
                        .pressable { serverMenu = true }
                        .padding(horizontal = 10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        chosenTitle,
                        color = c.text,
                        fontSize = 12.sp,
                        fontWeight = FontWeight.Medium,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    Spacer(Modifier.width(4.dp))
                    Icon(Icons.Default.KeyboardArrowDown, null, tint = c.text2, modifier = Modifier.size(16.dp))
                }
                DropdownMenu(
                    expanded = serverMenu,
                    onDismissRequest = { serverMenu = false },
                    containerColor = c.solid,
                    shape = RadiusControl,
                ) {
                    DropdownMenuItem(
                        text = { Text(stringResource(R.string.logs_all_servers), color = c.text) },
                        onClick = { serverFilter = null; serverMenu = false },
                    )
                    for (s in sortedServers) {
                        DropdownMenuItem(
                            text = { Text(s.title, color = c.text) },
                            onClick = { serverFilter = s.id; serverMenu = false },
                        )
                    }
                }
            }
        }

        PullToRefreshBox(
            isRefreshing = fetching,
            onRefresh = { vm.refreshAllLogs() },
            modifier = Modifier.weight(1f).fillMaxWidth(),
        ) {
            if (visible.isEmpty()) {
                Box(Modifier.fillMaxSize().padding(32.dp), contentAlignment = Alignment.Center) {
                    Text(
                        stringResource(if (logs.isEmpty()) R.string.logs_empty else R.string.logs_no_match),
                        color = c.text2,
                        textAlign = TextAlign.Center,
                    )
                }
            } else {
                LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(start = 16.dp, end = 16.dp, bottom = 16.dp)) {
                    for ((day, list) in days) {
                        item(key = "lday-$day") { SectionHeader(day) }
                        items(list.size, key = { EventStore.dedupeKey(list[it].serverId, list[it].event) }) { i ->
                            val item = list[i]
                            val e = item.event
                            val nick = contactIndex.nameFor(e.client, e.user)
                            GroupedRow(i, list.size, Modifier.animateItem()) {
                                LogEntryRow(
                                    item = item,
                                    nickname = nick,
                                    onClick = { vm.select(item.serverId) },
                                    onLongClick = {
                                        if (ContactBook.canName(contacts, e.client, e.user)) {
                                            naming = ContactBook.draft(contacts, e.client, e.user)
                                        }
                                    },
                                )
                            }
                        }
                    }
                }
            }
        }
    }

    naming?.let { ContactDialog(it, isNew = contacts.none { c0 -> c0.id == it.id }, onDismiss = { naming = null }, vm = vm) }
}

@Composable
private fun LogEntryRow(item: StoredEvent, nickname: String?, onClick: () -> Unit, onLongClick: () -> Unit) {
    val c = appColors
    val resources = LocalResources.current
    val e = item.event
    val color = eventKindColor(e.kind)
    val label = eventKindTitle(resources, e.kind)
    val detail = localizedEventDetail(resources, e)
    val user = e.user.substringAfter('\\').ifBlank { null }
    val actor = nickname ?: e.client.ifBlank { user ?: e.address.ifBlank { item.serverTitle } }

    Row(
        Modifier
            .fillMaxWidth()
            .pressable(onLongClick = onLongClick, onClick = onClick)
            .padding(horizontal = 14.dp, vertical = 11.dp),
        verticalAlignment = Alignment.Top,
    ) {
        Box(Modifier.padding(top = 6.dp)) { StatusDot(color, 8) }
        Spacer(Modifier.width(10.dp))
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    actor,
                    fontWeight = FontWeight.SemiBold,
                    color = c.text,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f, fill = false),
                )
                Spacer(Modifier.width(6.dp))
                Box(
                    Modifier
                        .clip(RadiusControl)
                        .background(c.accentSoft)
                        .padding(horizontal = 6.dp, vertical = 1.dp),
                ) {
                    Text(item.serverTitle, fontSize = 11.sp, color = c.text2, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
            }
            Text(label, color = color, style = MaterialTheme.typography.bodyMedium)
            val extra = listOfNotNull(
                e.client.takeIf { nickname != null && it.isNotBlank() },
                user.takeIf { e.client.isNotBlank() || nickname != null },
                e.address.ifBlank { null }?.takeIf { it != "LOCAL" },
                detail.ifBlank { null },
            )
            if (extra.isNotEmpty()) Muted(extra.joinToString(" · "), maxLines = 2)
        }
        Spacer(Modifier.width(8.dp))
        Text(clock(e.timeUtcMs), fontSize = 12.sp, color = c.text2)
    }
}
