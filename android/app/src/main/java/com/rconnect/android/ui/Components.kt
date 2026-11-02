package com.rconnect.android.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.rconnect.android.Health
import com.rconnect.android.R
import com.rconnect.android.ServerState
import com.rconnect.android.winrm.RemoteSession
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale

val Green = Color(0xFF2FA84F)
val Orange = Color(0xFFE08A00)
val Red = Color(0xFFE5392E)
val Grey = Color(0xFF8E8E93)

fun healthColor(h: Health): Color = when (h) {
    Health.Free -> Green
    Health.InUse -> Orange
    Health.Problem -> Red
    Health.Pending -> Grey
}

@Composable
fun StatusDot(color: Color, size: Int = 12) {
    Box(Modifier.size(size.dp).background(color, CircleShape))
}

@Composable
fun Muted(text: String, maxLines: Int = Int.MAX_VALUE, modifier: Modifier = Modifier) =
    Text(text, modifier = modifier, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
        maxLines = maxLines, overflow = TextOverflow.Ellipsis)

/** The state in words, so it never depends on colour alone. */
@Composable
fun healthLabel(s: ServerState): String = when (s.health) {
    Health.Pending -> stringResource(R.string.status_pending)
    Health.Free -> stringResource(R.string.status_free)
    Health.InUse -> stringResource(R.string.status_in_use, s.connected.size)
    Health.Problem -> when (s.errorKind) {
        ServerState.ErrorKind.Unreachable -> stringResource(R.string.status_unreachable)
        ServerState.ErrorKind.AuthFailed -> stringResource(R.string.status_auth)
        else -> stringResource(R.string.status_error)
    }
}

@Composable
fun errorExplanation(s: ServerState): String = when (s.errorKind) {
    ServerState.ErrorKind.Unreachable -> stringResource(R.string.err_unreachable)
    ServerState.ErrorKind.AuthFailed -> stringResource(R.string.err_auth)
    else -> s.error?.takeIf { it.isNotBlank() } ?: stringResource(R.string.err_other)
}

@Composable
fun idleLabel(s: RemoteSession): Pair<String, Color> = when {
    !s.connected -> stringResource(R.string.not_connected) to Grey
    s.idleMinutes < 1 -> stringResource(R.string.active) to Green
    s.idleMinutes < 60 -> stringResource(R.string.idle_min, s.idleMinutes) to Orange
    else -> stringResource(R.string.idle_hours, s.idleMinutes / 60) to Orange
}

@Composable
fun SessionRow(s: RemoteSession) {
    val (label, color) = idleLabel(s)
    Row(Modifier.fillMaxWidth().padding(vertical = 10.dp), verticalAlignment = Alignment.Top) {
        Box(Modifier.padding(top = 6.dp)) { StatusDot(color, 10) }
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            val user = s.user.substringAfter('\\')
            Text(s.client.ifBlank { user }, fontWeight = FontWeight.SemiBold)
            if (s.client.isNotBlank()) Muted(user)
            val detail = listOfNotNull(
                when {
                    s.connected && s.connectedUtcMs > 0 -> stringResource(R.string.connected_at, dateTime(s.connectedUtcMs))
                    s.logonUtcMs > 0 -> stringResource(R.string.since, dateTime(s.logonUtcMs))
                    else -> null
                },
            )
            if (detail.isNotEmpty()) Muted(detail.joinToString(" · "))
        }
        Spacer(Modifier.width(8.dp))
        Text(label, color = color, style = MaterialTheme.typography.labelLarge)
    }
}

/** A number with a caption that doubles as a filter button. */
@Composable
fun SummaryTile(count: Int, label: String, color: Color, selected: Boolean, modifier: Modifier = Modifier, onClick: () -> Unit) {
    Card(
        onClick = onClick,
        modifier = modifier,
        border = if (selected) BorderStroke(2.dp, color) else null,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = if (selected) 1f else 0.5f)),
    ) {
        Column(Modifier.padding(horizontal = 10.dp, vertical = 8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                StatusDot(color, 8)
                Text(count.toString(), style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.SemiBold)
            }
            Text(label, style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
    }
}

// ---- time -----------------------------------------------------------------------------------

private val zone: ZoneId get() = ZoneId.systemDefault()

fun clock(ms: Long): String =
    DateTimeFormatter.ofPattern("HH:mm", Locale.getDefault()).format(Instant.ofEpochMilli(ms).atZone(zone))

fun dateTime(ms: Long): String =
    DateTimeFormatter.ofPattern("d MMM HH:mm", Locale.getDefault()).format(Instant.ofEpochMilli(ms).atZone(zone))

fun dayLabel(ms: Long): String =
    DateTimeFormatter.ofPattern("d MMMM EEEE", Locale.getDefault()).format(Instant.ofEpochMilli(ms).atZone(zone))

@Composable
fun ago(ms: Long): String {
    val minutes = ((System.currentTimeMillis() - ms) / 60000).toInt()
    return when {
        minutes < 1 -> stringResource(R.string.just_now)
        minutes < 60 -> stringResource(R.string.min_ago, minutes)
        else -> stringResource(R.string.hours_ago, minutes / 60)
    }
}
