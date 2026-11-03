package com.rconnect.android.ui

import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.rconnect.android.AppViewModel
import com.rconnect.android.R
import com.rconnect.android.data.Contact

/** The address book: nicknames for computers, for example the computer LENOVO is "Elif". */
@Composable
fun ContactsScreen(vm: AppViewModel) {
    val contacts by vm.contacts.collectAsStateWithLifecycle()
    val context = LocalContext.current
    val resources = LocalResources.current
    var query by remember { mutableStateOf("") }
    var editing by remember { mutableStateOf<Contact?>(null) }
    var menu by remember { mutableStateOf(false) }

    val exportLauncher = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/json")) { uri ->
        if (uri != null) runCatching {
            context.contentResolver.openOutputStream(uri)?.use { it.write(vm.exportContacts().toByteArray()) }
        }.onFailure { Toast.makeText(context, R.string.contacts_file_error, Toast.LENGTH_LONG).show() }
    }
    val importLauncher = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) {
            val text = runCatching { context.contentResolver.openInputStream(uri)?.use { String(it.readBytes()) } }.getOrNull()
            val count = text?.let { vm.importContacts(it) }
            Toast.makeText(
                context,
                if (count == null) resources.getString(R.string.contacts_not_a_list) else resources.getString(R.string.contacts_imported, count),
                Toast.LENGTH_LONG,
            ).show()
        }
    }

    val needle = query.trim().lowercase()
    val shown = contacts.filter {
        needle.isEmpty() || listOf(it.name, it.device, it.user).any { f -> f.lowercase().contains(needle) }
    }.sortedBy { it.name.lowercase() }

    Column(Modifier.fillMaxSize()) {
        LargeTitleBar(stringResource(R.string.contacts_title)) {
            ToolButton(Icons.Default.Add, stringResource(R.string.contacts_add)) { editing = Contact() }
            Box {
                ToolButton(Icons.Default.MoreVert, stringResource(R.string.more)) { menu = true }
                DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                    DropdownMenuItem(text = { Text(stringResource(R.string.contacts_import)) },
                        onClick = { menu = false; importLauncher.launch(arrayOf("application/json", "text/plain", "*/*")) })
                    DropdownMenuItem(text = { Text(stringResource(R.string.contacts_export)) }, enabled = contacts.isNotEmpty(),
                        onClick = { menu = false; exportLauncher.launch("rconnect-contacts.json") })
                }
            }
        }
        SearchField(query, { query = it }, stringResource(R.string.contacts_search), Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
        if (shown.isEmpty()) {
            Box(Modifier.fillMaxSize().padding(32.dp), contentAlignment = Alignment.Center) {
                Text(
                    stringResource(if (contacts.isEmpty()) R.string.contacts_empty else R.string.contacts_no_match),
                    color = Palette.LabelSecondary,
                )
            }
        } else LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 16.dp)) {
            items(shown.size, key = { shown[it].id }) { i ->
                val c = shown[i]
                GroupedRow(i, shown.size, Modifier.animateItem()) {
                    Column(Modifier.fillMaxWidth().pressable { editing = c }.padding(horizontal = 16.dp, vertical = 12.dp)) {
                        Text(c.name, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        Muted(listOf(c.device, c.user).filter { it.isNotBlank() }.joinToString(" · "), maxLines = 1)
                    }
                }
            }
        }
    }
    editing?.let { ContactDialog(it, isNew = contacts.none { c -> c.id == it.id }, onDismiss = { editing = null }, vm = vm) }
}

/** Edits one entry; used from the address book and from a long press on a person in the other lists. */
@Composable
fun ContactDialog(initial: Contact, isNew: Boolean, onDismiss: () -> Unit, vm: AppViewModel) {
    var name by remember { mutableStateOf(initial.name) }
    var device by remember { mutableStateOf(initial.device) }
    var user by remember { mutableStateOf(initial.user) }
    val entry = initial.copy(name = name.trim(), device = device.trim(), user = user.trim())

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(if (isNew) R.string.contacts_add else R.string.contacts_edit)) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(name, { name = it }, label = { Text(stringResource(R.string.contacts_nickname)) }, singleLine = true,
                    modifier = Modifier.fillMaxWidth(), placeholder = { Text("Elif") })
                OutlinedTextField(device, { device = it }, label = { Text(stringResource(R.string.contacts_device)) }, singleLine = true,
                    modifier = Modifier.fillMaxWidth(), placeholder = { Text("LENOVO") })
                OutlinedTextField(user, { user = it }, label = { Text(stringResource(R.string.contacts_account)) }, singleLine = true,
                    modifier = Modifier.fillMaxWidth())
                Muted(stringResource(R.string.contacts_hint))
            }
        },
        confirmButton = {
            TextButton(enabled = entry.valid, onClick = { vm.saveContact(entry); onDismiss() }) { Text(stringResource(R.string.save)) }
        },
        dismissButton = {
            Row {
                if (!isNew) TextButton(onClick = { vm.deleteContact(initial.id); onDismiss() }) { Text(stringResource(R.string.delete), color = Palette.Red) }
                TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) }
            }
        },
    )
}
