package com.rconnect.android.ui

import androidx.compose.foundation.clickable
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
import androidx.compose.material.icons.filled.Clear
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.pulltorefresh.PullToRefreshBox
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.rconnect.android.AppViewModel
import com.rconnect.android.R
import com.rconnect.android.winrm.RemoteSession

private data class Person(val session: RemoteSession, val serverId: String, val server: String, val group: String)

/** Everyone who is signed in anywhere, in one list: the quickest way to answer "who is on what". */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PeopleScreen(vm: AppViewModel) {
    val servers by vm.servers.collectAsStateWithLifecycle()
    val states by vm.states.collectAsStateWithLifecycle()
    val query by vm.query.collectAsStateWithLifecycle()
    val needle = query.trim().lowercase()

    val people = servers.flatMap { s ->
        states[s.id]?.connected.orEmpty().map { Person(it, s.id, s.title, s.group) }
    }.filter { p ->
        needle.isEmpty() || p.session.user.lowercase().contains(needle) || p.session.client.lowercase().contains(needle) ||
            p.server.lowercase().contains(needle) || p.group.lowercase().contains(needle)
    }.sortedWith(compareBy({ it.session.user.substringAfter('\\').lowercase() }, { it.server.lowercase() }))
    val checking = states.values.count { it.checking }
    val answered = states.values.count { it.sessions != null }
    var pulled by remember { mutableStateOf(false) }
    LaunchedEffect(checking) { if (checking == 0) pulled = false }

    Column(Modifier.fillMaxSize()) {
        TopAppBar(
            title = { Text(stringResource(R.string.people_title)) },
            actions = { IconButton(onClick = { vm.refreshAll(force = true) }) { Icon(Icons.Default.Refresh, stringResource(R.string.refresh)) } },
        )
        OutlinedTextField(
            query, { vm.query.value = it }, singleLine = true,
            modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
            placeholder = { Text(stringResource(R.string.search_people)) },
            leadingIcon = { Icon(Icons.Default.Search, null) },
            trailingIcon = { if (query.isNotEmpty()) IconButton(onClick = { vm.query.value = "" }) { Icon(Icons.Default.Clear, stringResource(R.string.clear)) } },
        )
        Muted(stringResource(R.string.people_summary, people.size, answered, servers.size), modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
        PullToRefreshBox(isRefreshing = pulled && checking > 0, onRefresh = { pulled = true; vm.refreshAll(force = true) }, modifier = Modifier.fillMaxSize()) {
            if (people.isEmpty()) {
                Box(Modifier.fillMaxSize().padding(32.dp), contentAlignment = Alignment.Center) {
                    Text(stringResource(if (answered == 0) R.string.checking else R.string.nobody_anywhere), color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            } else LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(bottom = 16.dp)) {
                items(people, key = { it.serverId + "/" + it.session.id }) { p ->
                    val (label, color) = idleLabel(p.session)
                    Row(Modifier.fillMaxWidth().clickable { vm.select(p.serverId) }.padding(horizontal = 16.dp, vertical = 12.dp)) {
                        Box(Modifier.padding(top = 6.dp)) { StatusDot(color, 10) }
                        Spacer(Modifier.width(12.dp))
                        Column(Modifier.weight(1f)) {
                            val user = p.session.user.substringAfter('\\')
                            Text(p.session.client.ifBlank { user }, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                            Muted(buildString {
                                append(p.server)
                                if (p.group.isNotBlank()) append(" · ").append(p.group)
                                if (p.session.client.isNotBlank()) append(" · ").append(user)
                            }, maxLines = 1)
                            val from = listOfNotNull(
                                when {
                                    p.session.connected && p.session.connectedUtcMs > 0 -> stringResource(R.string.connected_at, dateTime(p.session.connectedUtcMs))
                                    p.session.logonUtcMs > 0 -> stringResource(R.string.since, dateTime(p.session.logonUtcMs))
                                    else -> null
                                })
                            if (from.isNotEmpty()) Muted(from.joinToString(" · "), maxLines = 1)
                        }
                        Spacer(Modifier.width(8.dp))
                        Text(label, color = color, style = MaterialTheme.typography.labelLarge)
                    }
                    HorizontalDivider()
                }
            }
        }
    }
}
