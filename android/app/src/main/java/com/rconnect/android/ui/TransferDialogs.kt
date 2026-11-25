package com.rconnect.android.ui

import android.Manifest
import android.content.Intent
import android.os.Build
import android.provider.Settings
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import com.rconnect.android.AppViewModel
import com.rconnect.android.R
import com.rconnect.android.data.ServerFile
import com.rconnect.android.notify.Notifier
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

enum class Transfer { Import, Export }

// Notepad and PowerShell on Windows may save as UTF-16; read that as well as UTF-8.
private fun decodeText(bytes: ByteArray): String = when {
    bytes.size >= 2 && bytes[0] == 0xFF.toByte() && bytes[1] == 0xFE.toByte() -> String(bytes, 2, bytes.size - 2, Charsets.UTF_16LE)
    bytes.size >= 2 && bytes[0] == 0xFE.toByte() && bytes[1] == 0xFF.toByte() -> String(bytes, 2, bytes.size - 2, Charsets.UTF_16BE)
    else -> String(bytes, Charsets.UTF_8)
}

private sealed interface Step {
    data object Choose : Step
    data object Paste : Step
    data class Passphrase(val text: String, val wrong: Boolean) : Step
    data object ExportForm : Step
    data object Working : Step
}

/**
 * Import and export of the server list, the same file and the same steps as in the Windows program: pick a file
 * (asking for the passphrase when it is encrypted), or write one (a passphrase also carries the saved passwords).
 */
@Composable
fun TransferHost(vm: AppViewModel, transfer: Transfer, onDone: () -> Unit) {
    val c = appColors
    val context = LocalContext.current
    val resources = LocalResources.current
    val scope = rememberCoroutineScope()
    var step by remember { mutableStateOf<Step>(if (transfer == Transfer.Import) Step.Choose else Step.ExportForm) }
    var exportPass by remember { mutableStateOf("") }

    fun say(text: String) = Toast.makeText(context, text, Toast.LENGTH_LONG).show()

    fun finishRead(result: ServerFile.Read, text: String) {
        when (result) {
            is ServerFile.Read.Ok -> {
                val (added, skipped) = vm.addServers(result.servers, result.unusable)
                say(resources.getString(R.string.import_result, added, skipped))
                onDone()
            }
            ServerFile.Read.NeedPassphrase -> step = Step.Passphrase(text, wrong = false)
            ServerFile.Read.WrongPassphrase -> step = Step.Passphrase(text, wrong = true)
            ServerFile.Read.Invalid -> {
                // A plain list saved as a file ("ip;user;password;name;group") works too.
                val (added, skipped) = vm.import(text)
                if (added == 0) say(resources.getString(R.string.import_unreadable))
                else say(resources.getString(R.string.import_result, added, skipped))
                onDone()
            }
        }
    }

    val openLauncher = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null) { onDone(); return@rememberLauncherForActivityResult }
        step = Step.Working
        scope.launch {
            val text = withContext(Dispatchers.IO) {
                runCatching { context.contentResolver.openInputStream(uri)?.use { decodeText(it.readBytes()) } }.getOrNull()
            }
            if (text == null) {
                say(resources.getString(R.string.import_unreadable))
                onDone()
            } else {
                val result = vm.readServerFileAsync(text)
                finishRead(result, text)
            }
        }
    }
    val saveLauncher = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/json")) { uri ->
        if (uri == null) { onDone(); return@rememberLauncherForActivityResult }
        step = Step.Working
        scope.launch {
            val ok = runCatching {
                val payload = vm.exportServersAsync(exportPass).toByteArray(Charsets.UTF_8)
                withContext(Dispatchers.IO) {
                    context.contentResolver.openOutputStream(uri)?.use { it.write(payload) } != null
                }
            }.getOrDefault(false)
            say(resources.getString(if (!ok) R.string.contacts_file_error else if (exportPass.isEmpty()) R.string.export_done_plain else R.string.export_done))
            onDone()
        }
    }

    when (val s = step) {
        Step.Choose -> AlertDialog(
            onDismissRequest = onDone,
            containerColor = c.solid,
            shape = RadiusCard,
            title = { Text(stringResource(R.string.import_title), color = c.text) },
            text = { Text(stringResource(R.string.import_choose_help), color = c.text2) },
            confirmButton = {
                TextButton(onClick = { openLauncher.launch(arrayOf("application/json", "text/plain", "*/*")) }) {
                    Text(stringResource(R.string.import_from_file), color = c.accentText)
                }
            },
            dismissButton = {
                Row {
                    TextButton(onClick = { step = Step.Paste }) { Text(stringResource(R.string.import_paste), color = c.text) }
                    TextButton(onClick = onDone) { Text(stringResource(R.string.cancel), color = c.text2) }
                }
            },
        )
        Step.Paste -> ImportDialog(onDismiss = onDone) { vm.import(it) }
        is Step.Passphrase -> PassphraseDialog(
            title = stringResource(R.string.import_title),
            message = stringResource(if (s.wrong) R.string.passphrase_wrong else R.string.passphrase_needed),
            confirm = stringResource(R.string.import_action),
            askTwice = false,
            onDismiss = onDone,
        ) { pass ->
            step = Step.Working
            scope.launch {
                val result = vm.readServerFileAsync(s.text, pass)
                finishRead(result, s.text)
            }
        }
        Step.ExportForm -> PassphraseDialog(
            title = stringResource(R.string.export_title),
            message = stringResource(R.string.export_help),
            confirm = stringResource(R.string.export_action),
            askTwice = true,
            onDismiss = onDone,
        ) { pass ->
            exportPass = pass
            saveLauncher.launch("rconnect-servers-${SimpleDateFormat("yyyyMMdd", Locale.US).format(Date())}.json")
        }
        Step.Working -> AlertDialog(
            onDismissRequest = {},
            containerColor = c.solid,
            shape = RadiusCard,
            text = {
                Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(vertical = 8.dp)) {
                    CircularProgressIndicator(Modifier.size(22.dp), strokeWidth = 2.dp, color = c.accent)
                    Spacer(Modifier.width(14.dp))
                    Text(stringResource(R.string.working), color = c.text)
                }
            },
            confirmButton = {},
        )
    }
}

@Composable
private fun PassphraseDialog(
    title: String,
    message: String,
    confirm: String,
    askTwice: Boolean,
    onDismiss: () -> Unit,
    onConfirm: (String) -> Unit,
) {
    val c = appColors
    val fieldColors = desktopTextFieldColors()
    var pass by remember { mutableStateOf("") }
    var again by remember { mutableStateOf("") }
    val mismatch = askTwice && pass.isNotEmpty() && again.isNotEmpty() && pass != again
    // Importing needs a passphrase; exporting may leave it empty (no passwords in the file).
    val ready = if (askTwice) pass == again else pass.isNotEmpty()
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = c.solid,
        shape = RadiusCard,
        title = { Text(title, color = c.text) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(message, style = MaterialTheme.typography.bodyMedium, color = c.text2)
                OutlinedTextField(
                    pass, { pass = it },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                    label = { Text(stringResource(R.string.passphrase)) },
                    visualTransformation = PasswordVisualTransformation(),
                    keyboardOptions = KeyboardOptions(autoCorrectEnabled = false, keyboardType = KeyboardType.Password),
                    colors = fieldColors,
                    shape = RadiusControl,
                )
                if (askTwice) {
                    OutlinedTextField(
                        again, { again = it },
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth(),
                        label = { Text(stringResource(R.string.passphrase_again)) },
                        isError = mismatch,
                        visualTransformation = PasswordVisualTransformation(),
                        keyboardOptions = KeyboardOptions(autoCorrectEnabled = false, keyboardType = KeyboardType.Password),
                        colors = fieldColors,
                        shape = RadiusControl,
                    )
                }
            }
        },
        confirmButton = {
            TextButton(enabled = ready, onClick = { onConfirm(pass) }) {
                Text(confirm, color = if (ready) c.accentText else c.text3)
            }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel), color = c.text2) } },
    )
}

/** What to be told about, and the permission Android 13 and later asks for. */
@Composable
fun NotificationsDialog(vm: AppViewModel, onDismiss: () -> Unit) {
    val c = appColors
    val context = LocalContext.current
    val prefs = vm.notificationPrefs
    var enabled by remember { mutableStateOf(prefs.enabled && Notifier.canPost(context)) }
    var onConnect by remember { mutableStateOf(prefs.onConnect) }
    var onLeave by remember { mutableStateOf(prefs.onLeave) }
    var onProblem by remember { mutableStateOf(prefs.onProblem) }
    var onStranger by remember { mutableStateOf(prefs.onStranger) }
    var watch by remember { mutableStateOf(prefs.watch) }
    var denied by remember { mutableStateOf(false) }

    val permission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        enabled = granted
        denied = !granted
        vm.setNotificationsEnabled(granted)
    }

    @Composable
    fun Option(label: String, checked: Boolean, enabledSwitch: Boolean = true, change: (Boolean) -> Unit) {
        Row(Modifier.fillMaxWidth().padding(vertical = 2.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(label, color = if (enabledSwitch) c.text else c.text3, modifier = Modifier.weight(1f))
            Spacer(Modifier.width(8.dp))
            Switch(
                checked = checked,
                onCheckedChange = change,
                enabled = enabledSwitch,
                colors = SwitchDefaults.colors(
                    checkedThumbColor = c.onAccent,
                    checkedTrackColor = c.accent,
                    uncheckedThumbColor = c.text2,
                    uncheckedTrackColor = c.surfaceAlt,
                    uncheckedBorderColor = c.border,
                ),
            )
        }
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = c.solid,
        shape = RadiusCard,
        title = { Text(stringResource(R.string.notifications_title), color = c.text) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                Option(stringResource(R.string.notifications_on), enabled) { on ->
                    if (on && Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU && !Notifier.canPost(context)) {
                        permission.launch(Manifest.permission.POST_NOTIFICATIONS)
                    } else {
                        enabled = on
                        denied = false
                        vm.setNotificationsEnabled(on)
                    }
                }
                Spacer(Modifier.height(4.dp))
                Option(stringResource(R.string.notify_connect), onConnect, enabled) { onConnect = it; prefs.onConnect = it }
                Option(stringResource(R.string.notify_stranger), onStranger, enabled) { onStranger = it; prefs.onStranger = it }
                Option(stringResource(R.string.notify_leave), onLeave, enabled) { onLeave = it; prefs.onLeave = it }
                Option(stringResource(R.string.notify_problem), onProblem, enabled) { onProblem = it; prefs.onProblem = it }
                Option(stringResource(R.string.notify_watch), watch, enabled) { watch = it; vm.setWatching(it) }
                Spacer(Modifier.height(8.dp))
                Muted(stringResource(if (denied) R.string.notifications_denied else R.string.notifications_help))
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.done), color = c.accentText) } },
        dismissButton = if (denied) {
            {
                TextButton(onClick = {
                    val intent = Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS)
                        .putExtra(Settings.EXTRA_APP_PACKAGE, context.packageName)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    runCatching { context.startActivity(intent) }
                }) { Text(stringResource(R.string.open_settings), color = c.text) }
            }
        } else null,
    )
}
