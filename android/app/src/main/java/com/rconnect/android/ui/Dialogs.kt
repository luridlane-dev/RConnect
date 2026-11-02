package com.rconnect.android.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import com.rconnect.android.R
import com.rconnect.android.data.ServerEntry

@Composable
fun ServerDialog(existing: ServerEntry?, groups: List<String>, onDismiss: () -> Unit, onSave: (ServerEntry) -> Unit) {
    var name by remember { mutableStateOf(existing?.name.orEmpty()) }
    var host by remember { mutableStateOf(existing?.host.orEmpty()) }
    var user by remember { mutableStateOf(existing?.user.orEmpty()) }
    var password by remember { mutableStateOf(existing?.password.orEmpty()) }
    var group by remember { mutableStateOf(existing?.group.orEmpty()) }
    var showPassword by remember { mutableStateOf(false) }
    val valid = host.isNotBlank() && user.isNotBlank() && password.isNotEmpty()

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(if (existing == null) R.string.add_server else R.string.edit)) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(host, { host = it }, label = { Text(stringResource(R.string.ip_address)) }, singleLine = true,
                    modifier = Modifier.fillMaxWidth(), keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri))
                OutlinedTextField(user, { user = it }, label = { Text(stringResource(R.string.user_name)) }, singleLine = true,
                    modifier = Modifier.fillMaxWidth(), keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Ascii))
                OutlinedTextField(
                    password, { password = it }, label = { Text(stringResource(R.string.password)) }, singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                    visualTransformation = if (showPassword) VisualTransformation.None else PasswordVisualTransformation(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                    trailingIcon = {
                        TextButton(onClick = { showPassword = !showPassword }) {
                            Text(stringResource(if (showPassword) R.string.hide else R.string.show))
                        }
                    },
                )
                OutlinedTextField(name, { name = it }, label = { Text(stringResource(R.string.name_optional)) }, singleLine = true,
                    modifier = Modifier.fillMaxWidth())
                OutlinedTextField(group, { group = it }, label = { Text(stringResource(R.string.group_optional)) }, singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                    supportingText = if (groups.isNotEmpty()) ({ Text(groups.joinToString(", "), maxLines = 1) }) else null)
            }
        },
        confirmButton = {
            TextButton(enabled = valid, onClick = {
                onSave((existing ?: ServerEntry(name = "", host = "", user = "", password = ""))
                    .copy(name = name.trim(), host = host.trim(), user = user.trim(), password = password, group = group.trim()))
            }) { Text(stringResource(R.string.save)) }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) } },
    )
}

/** Paste a list (for example rows copied from a spreadsheet) to add many servers at once. */
@Composable
fun ImportDialog(onDismiss: () -> Unit, onImport: (String) -> Pair<Int, Int>) {
    var text by remember { mutableStateOf("") }
    var result by remember { mutableStateOf<Pair<Int, Int>?>(null) }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.import_title)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                val done = result
                if (done == null) {
                    Text(stringResource(R.string.import_help), style = androidx.compose.material3.MaterialTheme.typography.bodySmall)
                    OutlinedTextField(
                        text, { text = it }, modifier = Modifier.fillMaxWidth().heightIn(min = 140.dp),
                        placeholder = { Text("10.0.0.5;Administrator;password;SRV-01;Ankara") },
                    )
                } else {
                    Text(stringResource(R.string.import_result, done.first, done.second))
                }
            }
        },
        confirmButton = {
            if (result == null) TextButton(enabled = text.isNotBlank(), onClick = { result = onImport(text) }) {
                Text(stringResource(R.string.import_action))
            } else TextButton(onClick = onDismiss) { Text(stringResource(R.string.done)) }
        },
        dismissButton = { if (result == null) TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) } },
    )
}
