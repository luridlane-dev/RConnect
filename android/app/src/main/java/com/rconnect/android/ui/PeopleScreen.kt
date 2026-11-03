package com.rconnect.android.ui

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.pulltorefresh.PullToRefreshBox
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.rconnect.android.AppViewModel
import com.rconnect.android.R
import com.rconnect.android.data.Contact
import com.rconnect.android.data.ContactBook
import com.rconnect.android.winrm.RemoteSession

private data class Person(val session: RemoteSession, val serverId: String, val server: String, val group: String)

/** Everyone who is signed in anywhere, in one list: the quickest way to answer "who is on what". */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PeopleScreen(vm: AppViewModel) {
    val servers by vm.servers.collectAsStateWithLifecycle()
    val states by vm.states.collectAsStateWithLifecycle()
    val query by vm.query.collectAsStateWithLifecycle()
    val contacts by vm.contacts.collectAsStateWithLifecycle()
    var editing by remember { mutableStateOf<Contact?>(null) }
    val needle = query.trim().lowercase()
    fun nickname(s: RemoteSession) = ContactBook.nameFor(contacts, s.client, s.user)

    val people = servers.flatMap { s ->
        states[s.id]?.connected.orEmpty().map { Person(it, s.id, s.title, s.group) }
    }.filter { p ->
        needle.isEmpty() || p.session.user.lowercase().contains(needle) || p.session.client.lowercase().contains(needle) ||
            nickname(p.session).orEmpty().lowercase().contains(needle) ||
            p.server.lowercase().contains(needle) || p.group.lowercase().contains(needle)
    }.sortedWith(compareBy({ (nickname(it.session) ?: it.session.user.substringAfter('\\')).lowercase() }, { it.server.lowercase() }))
    val checking = states.values.count { it.checking }
    val answered = states.values.count { it.sessions != null }
    var pulled by remember { mutableStateOf(false) }
    LaunchedEffect(checking) { if (checking == 0) pulled = false }

    editing?.let { ContactDialog(it, isNew = contacts.none { c -> c.id == it.id }, onDismiss = { editing = null }, vm = vm) }
    Column(Modifier.fillMaxSize()) {
        LargeTitleBar(stringResource(R.string.people_title), subtitle = stringResource(R.string.people_summary, people.size, answered, servers.size)) {
            ToolButton(Icons.Default.Refresh, stringResource(R.string.refresh)) { vm.refreshAll(force = true) }
        }
        SearchField(query, { vm.query.value = it }, stringResource(R.string.search_people), Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
        if (contacts.isEmpty() && people.isNotEmpty())
            Muted(stringResource(R.string.people_name_hint), modifier = Modifier.padding(horizontal = 20.dp, vertical = 6.dp))
        PullToRefreshBox(isRefreshing = pulled && checking > 0, onRefresh = { pulled = true; vm.refreshAll(force = true) }, modifier = Modifier.weight(1f).fillMaxWidth()) {
            if (people.isEmpty()) {
                Box(Modifier.fillMaxSize().padding(32.dp), contentAlignment = Alignment.Center) {
                    Text(stringResource(if (answered == 0) R.string.checking else R.string.nobody_anywhere), color = Palette.LabelSecondary)
                }
            } else LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 16.dp)) {
                items(people.size, key = { people[it].serverId + "/" + people[it].session.id }) { i ->
                    val p = people[i]
                    val (label, color) = idleLabel(p.session)
                    val nick = nickname(p.session)
                    GroupedRow(i, people.size, Modifier.animateItem()) {
                        Row(
                            Modifier.fillMaxWidth()
                                .pressable(onLongClick = {
                                    if (ContactBook.canName(contacts, p.session.client, p.session.user))
                                        editing = ContactBook.draft(contacts, p.session.client, p.session.user)
                                }) { vm.select(p.serverId) }
                                .padding(horizontal = 16.dp, vertical = 12.dp),
                        ) {
                            Box(Modifier.padding(top = 6.dp)) { StatusDot(color, 10) }
                            Spacer(Modifier.width(12.dp))
                            Column(Modifier.weight(1f)) {
                                val user = p.session.user.substringAfter('\\')
                                Text(nick ?: p.session.client.ifBlank { user }, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                                Muted(buildString {
                                    append(p.server)
                                    if (p.group.isNotBlank()) append(" · ").append(p.group)
                                    if (nick != null && p.session.client.isNotBlank()) append(" · ").append(p.session.client)
                                    if (p.session.client.isNotBlank() || nick != null) append(" · ").append(user)
                                }, maxLines = 1)
                                val from = when {
                                    p.session.connected && p.session.connectedUtcMs > 0 -> stringResource(R.string.connected_at, dateTime(p.session.connectedUtcMs))
                                    p.session.logonUtcMs > 0 -> stringResource(R.string.since, dateTime(p.session.logonUtcMs))
                                    else -> null
                                }
                                if (from != null) Muted(from, maxLines = 1)
                            }
                            Spacer(Modifier.width(8.dp))
                            Text(label, color = color, style = MaterialTheme.typography.labelLarge)
                        }
                    }
                }
            }
        }
    }
}
