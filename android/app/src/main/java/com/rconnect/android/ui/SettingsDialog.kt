package com.rconnect.android.ui

import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
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
import com.rconnect.android.R
import com.rconnect.android.ThemeMode
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** App-wide settings: appearance, notifications, import/export, groups, logs and about. */
@Composable
fun SettingsDialog(
    vm: AppViewModel,
    onDismiss: () -> Unit,
    onImportServers: () -> Unit,
    onExportServers: () -> Unit,
    onNotifications: () -> Unit,
    onAbout: () -> Unit,
) {
    val c = appColors
    val context = LocalContext.current
    val resources = LocalResources.current
    val scope = rememberCoroutineScope()
    val themeMode by vm.themeMode.collectAsStateWithLifecycle()
    val servers by vm.servers.collectAsStateWithLifecycle()
    val contacts by vm.contacts.collectAsStateWithLifecycle()
    val hasLogsFlow = remember(vm) { vm.logs.map { it.isNotEmpty() }.distinctUntilChanged() }
    val hasLogs by hasLogsFlow.collectAsStateWithLifecycle(initialValue = vm.logs.value.isNotEmpty())
    val groups = remember(servers) {
        servers.map { it.group }.filter { it.isNotBlank() }.distinct().sortedWith(::naturalCompare)
    }

    var renamingGroup by remember { mutableStateOf<String?>(null) }

    val exportContactsLauncher = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/json")) { uri ->
        if (uri != null) {
            scope.launch {
                val ok = withContext(Dispatchers.IO) {
                    val data = vm.exportContacts().toByteArray(Charsets.UTF_8)
                    runCatching { context.contentResolver.openOutputStream(uri)?.use { it.write(data) } != null }.getOrDefault(false)
                }
                if (!ok) Toast.makeText(context, R.string.contacts_file_error, Toast.LENGTH_LONG).show()
            }
        }
    }
    val importContactsLauncher = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) {
            scope.launch {
                val text = withContext(Dispatchers.IO) {
                    runCatching {
                        context.contentResolver.openInputStream(uri)?.use { stream ->
                            readBoundedBytes(stream)?.let { String(it, Charsets.UTF_8) }
                        }
                    }.getOrNull()
                }
                val count = text?.let { withContext(Dispatchers.Default) { vm.importContacts(it) } }
                Toast.makeText(
                    context,
                    if (count == null) resources.getString(R.string.contacts_not_a_list) else resources.getString(R.string.contacts_imported, count),
                    Toast.LENGTH_LONG,
                ).show()
            }
        }
    }

    @Composable
    fun SettingsRowButton(text: String, enabled: Boolean = true, destructive: Boolean = false, onClick: () -> Unit) {
        Row(
            Modifier
                .fillMaxWidth()
                .then(if (enabled) Modifier.pressable(onClick = onClick) else Modifier)
                .padding(horizontal = 12.dp, vertical = 11.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text,
                color = when {
                    !enabled -> c.text3
                    destructive -> c.red
                    else -> c.text
                },
                fontSize = 14.sp,
            )
        }
    }

    @Composable
    fun Block(content: @Composable () -> Unit) {
        Column(
            Modifier
                .fillMaxWidth()
                .clip(RadiusControl)
                .background(c.surface)
                .border(1.dp, c.border, RadiusControl),
        ) {
            content()
        }
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = c.solid,
        shape = RadiusCard,
        title = { Text(stringResource(R.string.settings), color = c.text) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(stringResource(R.string.appearance), style = SectionTitle, color = c.text2)
                Segmented(
                    options = listOf(stringResource(R.string.theme_dark), stringResource(R.string.theme_light)),
                    selected = if (themeMode == ThemeMode.Dark) 0 else 1,
                    onSelect = { vm.setThemeMode(if (it == 0) ThemeMode.Dark else ThemeMode.Light) },
                )

                Text(stringResource(R.string.notifications_title), style = SectionTitle, color = c.text2)
                Block {
                    SettingsRowButton(stringResource(R.string.notifications_title)) {
                        onDismiss()
                        onNotifications()
                    }
                }

                Text(stringResource(R.string.settings_data), style = SectionTitle, color = c.text2)
                Block {
                    SettingsRowButton(stringResource(R.string.import_title)) { onDismiss(); onImportServers() }
                    HorizontalDivider(color = c.separator)
                    SettingsRowButton(stringResource(R.string.export_title), enabled = servers.isNotEmpty()) { onDismiss(); onExportServers() }
                    HorizontalDivider(color = c.separator)
                    SettingsRowButton(stringResource(R.string.contacts_import)) {
                        importContactsLauncher.launch(arrayOf("application/json", "text/plain", "*/*"))
                    }
                    HorizontalDivider(color = c.separator)
                    SettingsRowButton(stringResource(R.string.contacts_export), enabled = contacts.isNotEmpty()) {
                        exportContactsLauncher.launch("rconnect-contacts.json")
                    }
                }

                Text(stringResource(R.string.settings_groups), style = SectionTitle, color = c.text2)
                Block {
                    if (groups.isEmpty()) {
                        Muted(stringResource(R.string.settings_no_groups), Modifier.padding(12.dp))
                    } else {
                        groups.forEachIndexed { idx, g ->
                            Row(
                                Modifier.fillMaxWidth().padding(start = 12.dp, end = 4.dp, top = 4.dp, bottom = 4.dp),
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                Text(g, color = c.text, fontSize = 14.sp, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                                TextButton(onClick = { renamingGroup = g }) {
                                    Text(stringResource(R.string.rename), color = c.accentText, fontSize = 12.sp)
                                }
                                TextButton(onClick = { vm.renameGroup(g, "") }) {
                                    Text(stringResource(R.string.remove), color = c.red, fontSize = 12.sp)
                                }
                            }
                            if (idx < groups.lastIndex) HorizontalDivider(color = c.separator)
                        }
                    }
                }

                Text(stringResource(R.string.logs_title), style = SectionTitle, color = c.text2)
                Block {
                    SettingsRowButton(
                        stringResource(R.string.logs_clear),
                        enabled = hasLogs,
                        destructive = true,
                    ) {
                        vm.clearLogs()
                        Toast.makeText(context, R.string.logs_cleared, Toast.LENGTH_SHORT).show()
                    }
                }

                Block {
                    SettingsRowButton(stringResource(R.string.menu_about)) { onDismiss(); onAbout() }
                }
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.done), color = c.accentText) } },
    )

    renamingGroup?.let { oldName ->
        var nextName by remember(oldName) { mutableStateOf(oldName) }
        val valid = nextName.trim().isNotEmpty() && nextName.trim() != oldName
        AlertDialog(
            onDismissRequest = { renamingGroup = null },
            containerColor = c.solid,
            shape = RadiusCard,
            title = { Text(stringResource(R.string.rename_group_title), color = c.text) },
            text = {
                OutlinedTextField(
                    value = nextName,
                    onValueChange = { nextName = it },
                    label = { Text(stringResource(R.string.group_name)) },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                    colors = desktopTextFieldColors(),
                    shape = RadiusControl,
                )
            },
            confirmButton = {
                TextButton(enabled = valid, onClick = { vm.renameGroup(oldName, nextName.trim()); renamingGroup = null }) {
                    Text(stringResource(R.string.save), color = if (valid) c.accentText else c.text3)
                }
            },
            dismissButton = {
                TextButton(onClick = { renamingGroup = null }) { Text(stringResource(R.string.cancel), color = c.text2) }
            },
        )
    }
}
