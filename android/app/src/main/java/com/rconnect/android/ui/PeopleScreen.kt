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
import com.rconnect.android.data.ContactIndex
import com.rconnect.android.winrm.RemoteSession
import java.util.Locale

private data class Person(
    val session: RemoteSession,
    val serverId: String,
    val server: String,
    val group: String,
    val nickname: String?,
    val displayName: String,
)

/** Everyone who is signed in anywhere, in one list: the quickest way to answer "who is on what". */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PeopleScreen(vm: AppViewModel) {
    val c = appColors
    val servers by vm.servers.collectAsStateWithLifecycle()
    val states by vm.states.collectAsStateWithLifecycle()
    val query by vm.peopleQuery.collectAsStateWithLifecycle()
    val contacts by vm.contacts.collectAsStateWithLifecycle()
    var editing by remember { mutableStateOf<Contact?>(null) }

    val contactIndex = remember(contacts) { ContactIndex(contacts) }
    val people = remember(servers, states, contactIndex, query) {
        val needle = query.trim().lowercase(Locale.ROOT)
        servers.flatMap { s ->
            states[s.id]?.connected.orEmpty().map { session ->
                val nick = contactIndex.nameFor(session.client, session.user)
                val display = nick ?: session.client.ifBlank { session.user.substringAfter('\\') }
                Person(session, s.id, s.title, s.group, nick, display)
            }
        }.filter { p ->
            needle.isEmpty() ||
                p.session.user.lowercase(Locale.ROOT).contains(needle) ||
                p.session.client.lowercase(Locale.ROOT).contains(needle) ||
                p.nickname.orEmpty().lowercase(Locale.ROOT).contains(needle) ||
                p.server.lowercase(Locale.ROOT).contains(needle) ||
                p.group.lowercase(Locale.ROOT).contains(needle)
        }.sortedWith(
            Comparator { a, b ->
                val cmp = naturalCompare(a.displayName, b.displayName)
                if (cmp != 0) cmp else naturalCompare(a.server, b.server)
            },
        )
    }
    val (checking, answered) = remember(states) {
        var chk = 0
        var ans = 0
        for (st in states.values) {
            if (st.checking) chk++
            if (st.sessions != null) ans++
        }
        chk to ans
    }
    var pulled by remember { mutableStateOf(false) }
    LaunchedEffect(checking) { if (checking == 0) pulled = false }

    editing?.let { ContactDialog(it, isNew = contacts.none { c0 -> c0.id == it.id }, onDismiss = { editing = null }, vm = vm) }
    Column(Modifier.fillMaxSize()) {
        LargeTitleBar(stringResource(R.string.people_title), subtitle = stringResource(R.string.people_summary, people.size, answered, servers.size)) {
            ToolButton(Icons.Default.Refresh, stringResource(R.string.refresh)) { vm.refreshAll(force = true) }
        }
        SearchField(query, { vm.peopleQuery.value = it }, stringResource(R.string.search_people), Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
        if (contacts.isEmpty() && people.isNotEmpty())
            Muted(stringResource(R.string.people_name_hint), Modifier.padding(horizontal = 18.dp, vertical = 6.dp))
        PullToRefreshBox(
            isRefreshing = pulled && checking > 0,
            onRefresh = { if (servers.isNotEmpty()) { pulled = true; vm.refreshAll(force = true) } },
            modifier = Modifier.weight(1f).fillMaxWidth(),
        ) {
            if (people.isEmpty()) {
                Box(Modifier.fillMaxSize().padding(32.dp), contentAlignment = Alignment.Center) {
                    Text(
                        stringResource(if (answered == 0 && checking > 0) R.string.checking else R.string.nobody_anywhere),
                        color = c.text2,
                    )
                }
            } else {
                LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 16.dp)) {
                    items(people.size, key = { "${people[it].serverId}/${people[it].session.id}" }) { i ->
                        val p = people[i]
                        val (label, color) = idleLabel(p.session)
                        val nick = p.nickname
                        GroupedRow(i, people.size, Modifier.animateItem()) {
                            Row(
                                Modifier
                                    .fillMaxWidth()
                                    .pressable(onLongClick = {
                                        if (ContactBook.canName(contacts, p.session.client, p.session.user))
                                            editing = ContactBook.draft(contacts, p.session.client, p.session.user)
                                    }) { vm.select(p.serverId) }
                                    .padding(horizontal = 14.dp, vertical = 12.dp),
                            ) {
                                Box(Modifier.padding(top = 6.dp)) { StatusDot(color, 8) }
                                Spacer(Modifier.width(10.dp))
                                Column(Modifier.weight(1f)) {
                                    val user = p.session.user.substringAfter('\\')
                                    Text(
                                        p.displayName,
                                        fontWeight = FontWeight.SemiBold,
                                        color = c.text,
                                        maxLines = 1,
                                        overflow = TextOverflow.Ellipsis,
                                    )
                                    Muted(buildString {
                                        append(p.server)
                                        if (p.group.isNotBlank()) append(" · ").append(p.group)
                                        if (nick != null && p.session.client.isNotBlank()) append(" · ").append(p.session.client)
                                        if (p.session.client.isNotBlank() || nick != null) append(" · ").append(user)
                                    }, maxLines = 1)
                                    if (p.session.connected && p.session.connectedUtcMs > 0) {
                                        Muted(stringResource(R.string.connected_at, dateTime(p.session.connectedUtcMs)), maxLines = 1)
                                    }
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
}
