package com.rconnect.android.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.widget.Toast
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.animateContentSize
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
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
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
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
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.rconnect.android.AppViewModel
import com.rconnect.android.Health
import com.rconnect.android.R
import com.rconnect.android.ServerState
import com.rconnect.android.data.Contact
import com.rconnect.android.data.ContactBook
import com.rconnect.android.data.ContactIndex
import com.rconnect.android.data.ServerEntry
import com.rconnect.android.winrm.AccessEvent
import com.rconnect.android.winrm.RemoteSession
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch

@Composable
fun DetailScreen(vm: AppViewModel, server: ServerEntry) {
    val c = appColors
    val context = LocalContext.current
    val resources = LocalResources.current
    val scope = rememberCoroutineScope()
    // Observe only this server's state so background refreshes of other servers do not recompose DetailScreen.
    val stateFlow = remember(server.id) { vm.states.map { it[server.id] ?: ServerState() }.distinctUntilChanged() }
    val state by stateFlow.collectAsStateWithLifecycle(initialValue = vm.states.value[server.id] ?: ServerState())
    var section by remember { mutableIntStateOf(0) }
    var editing by remember { mutableStateOf(false) }
    var deleting by remember { mutableStateOf(false) }
    var messaging by remember { mutableStateOf<Pair<RemoteSession, String>?>(null) }
    val (failed, history) = remember(state.events) {
        state.events.orEmpty().partition { it.kind == AccessEvent.Kind.FailedSignIn }
    }
    val historyByDay = remember(history) { history.groupBy { dayLabel(it.timeUtcMs) } }
    val shownFailed = remember(failed) { failed.take(80) }
    val contacts by vm.contacts.collectAsStateWithLifecycle()
    val contactIndex = remember(contacts) { ContactIndex(contacts) }
    var naming by remember { mutableStateOf<Contact?>(null) }
    var tick by remember { mutableLongStateOf(0L) }

    LaunchedEffect(Unit) {
        while (true) {
            delay(30_000L)
            tick++
        }
    }

    Column(Modifier.fillMaxSize().navigationBarsPadding()) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            ToolButton(Icons.AutoMirrored.Filled.ArrowBack, stringResource(R.string.back)) { vm.select(null) }
            Text(
                server.title,
                modifier = Modifier.weight(1f).padding(horizontal = 10.dp),
                style = MaterialTheme.typography.titleLarge,
                color = c.text,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            FavoriteStar(filled = server.favorite, onClick = { vm.toggleFavorite(server.id) })
            Spacer(Modifier.width(4.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                ToolButton(Icons.Default.Refresh, stringResource(R.string.refresh)) { vm.refresh(server.id, force = true, forceEvents = true) }
                ToolButton(Icons.Default.Edit, stringResource(R.string.edit)) { editing = true }
                ToolButton(Icons.Default.Delete, stringResource(R.string.delete), tint = c.red) { deleting = true }
            }
        }
        LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(start = 16.dp, end = 16.dp, bottom = 24.dp)) {
            item(key = "summary") {
                // Read tick so relative time refreshes every 30 s.
                val unusedTick = tick
                Summary(server, state, unusedTick) {
                    val diag = buildString {
                        appendLine("${server.title} (${server.host}:${server.port})")
                        appendLine("User: ${server.user}")
                        appendLine("Status: ${state.errorKind}")
                        if (!state.error.isNullOrBlank()) appendLine("Error: ${state.error}")
                    }
                    val cm = context.getSystemService(ClipboardManager::class.java)
                    cm?.setPrimaryClip(ClipData.newPlainText("RConnect", diag))
                    Toast.makeText(context, R.string.copied, Toast.LENGTH_SHORT).show()
                }
            }
            item(key = "chips") {
                Segmented(
                    listOf(
                        stringResource(R.string.tab_now, state.connected.size),
                        stringResource(R.string.tab_history),
                        stringResource(R.string.tab_failed, failed.size),
                    ),
                    section,
                    { section = it },
                    Modifier.padding(vertical = 14.dp),
                )
            }
            when (section) {
                0 -> {
                    val sessions = state.sessions
                    when {
                        sessions == null && state.checking -> item { Muted(stringResource(R.string.checking), Modifier.padding(8.dp)) }
                        sessions == null && state.error != null -> item { Muted(stringResource(R.string.no_details_on_error), Modifier.padding(8.dp)) }
                        sessions == null -> item { Muted(stringResource(R.string.checking), Modifier.padding(8.dp)) }
                        sessions.isEmpty() -> item { Muted(stringResource(R.string.nobody), Modifier.padding(8.dp)) }
                        else -> items(sessions.size, key = { "s-${sessions[it].id}" }) { i ->
                            val s = sessions[i]
                            val nick = contactIndex.nameFor(s.client, s.user)
                            val display = nick ?: s.client.ifBlank { s.user.substringAfter('\\') }
                            GroupedRow(i, sessions.size, Modifier.animateItem()) {
                                SessionRow(
                                    s = s,
                                    nickname = nick,
                                    onLongClick = {
                                        if (ContactBook.canName(contacts, s.client, s.user)) naming = ContactBook.draft(contacts, s.client, s.user)
                                    },
                                    trailing = if (s.connected && s.id > 0) {
                                        {
                                            Box(
                                                Modifier
                                                    .clip(RadiusControl)
                                                    .background(c.button)
                                                    .border(1.dp, c.border, RadiusControl)
                                                    .pressable { messaging = s to display }
                                                    .padding(horizontal = 8.dp, vertical = 4.dp),
                                            ) {
                                                Text(stringResource(R.string.send_message), fontSize = 11.sp, color = c.text2, fontWeight = FontWeight.Medium)
                                            }
                                        }
                                    } else null,
                                )
                            }
                        }
                    }
                }
                1 -> when {
                    state.events == null && state.checking -> item { Muted(stringResource(R.string.checking), Modifier.padding(8.dp)) }
                    state.events == null && state.error != null -> item { Muted(stringResource(R.string.no_details_on_error), Modifier.padding(8.dp)) }
                    state.events == null -> item { Muted(stringResource(R.string.checking), Modifier.padding(8.dp)) }
                    history.isEmpty() -> item { Muted(stringResource(R.string.no_history), Modifier.padding(8.dp)) }
                    else -> {
                        for ((day, list) in historyByDay) {
                            item(key = "day-$day") { SectionHeader(day) }
                            items(list.size, key = { i ->
                                val e = list[i]
                                if (e.recordId > 0) "e-${e.kind.ordinal}-${e.recordId}" else "e-$day-$i-${e.timeUtcMs}-${e.kind.ordinal}"
                            }) { i ->
                                val e = list[i]
                                GroupedRow(i, list.size, Modifier.animateItem()) {
                                    EventRow(e, contactIndex.nameFor(e.client, e.user)) {
                                        if (ContactBook.canName(contacts, e.client, e.user)) naming = ContactBook.draft(contacts, e.client, e.user)
                                    }
                                }
                            }
                        }
                    }
                }
                else -> when {
                    state.events == null && state.checking -> item { Muted(stringResource(R.string.checking), Modifier.padding(8.dp)) }
                    state.events == null && state.error != null -> item { Muted(stringResource(R.string.no_details_on_error), Modifier.padding(8.dp)) }
                    state.events == null -> item { Muted(stringResource(R.string.checking), Modifier.padding(8.dp)) }
                    failed.isEmpty() -> item { Muted(stringResource(R.string.no_failed), Modifier.padding(8.dp)) }
                    else -> {
                        item { Muted(stringResource(R.string.failed_note), Modifier.padding(start = 4.dp, end = 4.dp, bottom = 10.dp)) }
                        items(shownFailed.size, key = { i ->
                            val e = shownFailed[i]
                            if (e.recordId > 0) "f-${e.recordId}" else "f-$i-${e.timeUtcMs}"
                        }) { i ->
                            val e = shownFailed[i]
                            val reason = localizedEventDetail(resources, e)
                            GroupedRow(i, shownFailed.size, Modifier.animateItem()) {
                                Column(Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 12.dp)) {
                                    Row {
                                        Text(e.user.ifBlank { "?" }, modifier = Modifier.weight(1f), fontWeight = FontWeight.Medium, color = c.text)
                                        Text(dateTime(e.timeUtcMs), fontSize = 12.sp, color = c.text2)
                                    }
                                    Muted(listOfNotNull(e.address.ifBlank { null }, reason.ifBlank { null }).joinToString(" · "))
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    naming?.let { ContactDialog(it, isNew = contacts.none { c0 -> c0.id == it.id }, onDismiss = { naming = null }, vm = vm) }
    messaging?.let { (session, recipient) ->
        SendMessageDialog(
            recipient = recipient,
            onDismiss = { messaging = null },
            onSend = { text ->
                messaging = null
                scope.launch {
                    val res = vm.sendMessage(server.id, session.id, text)
                    Toast.makeText(
                        context,
                        if (res.isSuccess) resources.getString(R.string.message_sent) else resources.getString(R.string.message_failed),
                        Toast.LENGTH_LONG,
                    ).show()
                }
            },
        )
    }
    if (editing) ServerDialog(server, vm.groups(), onDismiss = { editing = false }) { vm.save(it); editing = false }
    if (deleting) AlertDialog(
        onDismissRequest = { deleting = false },
        containerColor = c.solid,
        shape = RadiusCard,
        title = { Text(stringResource(R.string.delete_title, server.title), color = c.text) },
        confirmButton = {
            TextButton(onClick = { deleting = false; vm.delete(server.id) }) {
                Text(stringResource(R.string.delete), color = c.red)
            }
        },
        dismissButton = { TextButton(onClick = { deleting = false }) { Text(stringResource(R.string.cancel), color = c.text2) } },
    )
}

@Composable
private fun Summary(server: ServerEntry, state: ServerState, tick: Long, onCopyDiagnostics: () -> Unit) {
    val c = appColors
    val fill = if (state.health == Health.InUse) c.busySurface else c.surface
    val dotColor = healthColor(state)
    val labelColor = when (state.health) {
        Health.InUse -> c.busyText
        Health.Free -> c.green
        Health.Problem -> if (state.errorKind == ServerState.ErrorKind.Unreachable) c.text2 else c.red
        Health.Pending -> c.text2
    }
    Column(
        Modifier
            .fillMaxWidth()
            .clip(RadiusCard)
            .background(fill)
            .border(1.dp, c.border, RadiusCard)
            .padding(14.dp)
            .animateContentSize(),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            StatusDot(dotColor, 10, pulse = state.checking && state.sessions == null)
            Spacer(Modifier.width(10.dp))
            AnimatedContent(
                healthLabel(state),
                transitionSpec = { fadeIn(tween(200)) togetherWith fadeOut(tween(150)) },
                label = "health",
                modifier = Modifier.weight(1f),
            ) { label ->
                Text(label, style = MaterialTheme.typography.titleMedium, color = labelColor)
            }
            if (state.latencyMs >= 0 && state.health != Health.Problem && !state.checking) {
                Text(stringResource(R.string.ms_format, state.latencyMs), fontSize = 12.sp, color = c.text3)
            }
            if (state.checking) CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp, color = c.accent)
        }
        Spacer(Modifier.size(6.dp))
        val hostLabel = if (server.port == 3389) server.host else "${server.host}:${server.port}"
        Muted(listOfNotNull(hostLabel, server.group.ifBlank { null }, server.user).joinToString(" · "))
        if (state.checkedAt > 0 && tick >= 0L) Muted(stringResource(R.string.checked_ago, ago(state.checkedAt)))
        if (state.health == Health.Problem) {
            Text(errorExplanation(state), color = c.red, fontSize = 13.sp, modifier = Modifier.padding(top = 8.dp))
            Spacer(Modifier.height(8.dp))
            Box(
                Modifier
                    .clip(RadiusControl)
                    .background(c.button)
                    .border(1.dp, c.border, RadiusControl)
                    .pressable(onClick = onCopyDiagnostics)
                    .padding(horizontal = 10.dp, vertical = 6.dp),
            ) {
                Text(stringResource(R.string.copy_diagnostics), fontSize = 12.sp, color = c.text, fontWeight = FontWeight.Medium)
            }
        }
    }
}

@Composable
private fun EventRow(e: AccessEvent, nickname: String?, onLongClick: () -> Unit) {
    val c = appColors
    val resources = LocalResources.current
    val label = eventKindTitle(resources, e.kind)
    val color = eventKindColor(e.kind)
    val detail = localizedEventDetail(resources, e)
    Row(
        Modifier
            .fillMaxWidth()
            .pressable(onLongClick = onLongClick) {}
            .padding(horizontal = 14.dp, vertical = 12.dp),
        verticalAlignment = Alignment.Top,
    ) {
        Box(Modifier.padding(top = 6.dp)) { StatusDot(color, 8) }
        Spacer(Modifier.width(10.dp))
        Column(Modifier.weight(1f)) {
            val user = e.user.substringAfter('\\').ifBlank { null }
            Text(nickname ?: e.client.ifBlank { user ?: e.address }, fontWeight = FontWeight.SemiBold, color = c.text)
            Text(label, color = color, style = MaterialTheme.typography.bodyMedium)
            val extra = listOfNotNull(
                e.client.takeIf { nickname != null && it.isNotBlank() },
                user.takeIf { e.client.isNotBlank() || nickname != null },
                e.address.ifBlank { null }?.takeIf { it != "LOCAL" },
                detail.ifBlank { null },
            )
            if (extra.isNotEmpty()) Muted(extra.joinToString(" · "))
        }
        Text(clock(e.timeUtcMs), fontSize = 12.sp, color = c.text2)
    }
}
