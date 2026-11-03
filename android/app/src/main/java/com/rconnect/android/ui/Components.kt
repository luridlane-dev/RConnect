package com.rconnect.android.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.animateIntAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.ui.graphics.RectangleShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Clear
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.composed
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.rconnect.android.Health
import com.rconnect.android.R
import com.rconnect.android.ServerState
import com.rconnect.android.winrm.RemoteSession
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale

val Green = Palette.Green
val Orange = Palette.Orange
val Red = Palette.Red
val Grey = Palette.Grey

fun healthColor(h: Health): Color = when (h) {
    Health.Free -> Green
    Health.InUse -> Orange
    Health.Problem -> Red
    Health.Pending -> Grey
}

// ---- motion ------------------------------------------------------------------------------------

/** Click (and optional long press) with a small, springy press effect. */
@OptIn(ExperimentalFoundationApi::class)
fun Modifier.pressable(onLongClick: (() -> Unit)? = null, onClick: () -> Unit): Modifier = composed {
    val source = remember { MutableInteractionSource() }
    val pressed by source.collectIsPressedAsState()
    val scale by animateFloatAsState(if (pressed) 0.97f else 1f, spring(dampingRatio = 0.6f, stiffness = Spring.StiffnessMedium), label = "press")
    val fade by animateFloatAsState(if (pressed) 0.75f else 1f, tween(90), label = "pressFade")
    this
        .graphicsLayer { scaleX = scale; scaleY = scale }
        .alpha(fade)
        .combinedClickable(interactionSource = source, indication = null, onClick = onClick, onLongClick = onLongClick)
}

@Composable
fun StatusDot(color: Color, size: Int = 12, pulse: Boolean = false) {
    val shown by animateColorAsState(color, tween(300), label = "dot")
    val breathing = rememberInfiniteTransition(label = "pulse")
    val level by breathing.animateFloat(1f, 0.3f, infiniteRepeatable(tween(900), RepeatMode.Reverse), label = "level")
    Box(Modifier.size(size.dp).alpha(if (pulse) level else 1f).background(shown, RectangleShape))
}

// ---- layout ------------------------------------------------------------------------------------

/** Rows are flat: no rounded corners anywhere. */
fun rowShape(index: Int, count: Int): Shape = RectangleShape

/** A row of a grouped list: panel background, flat, a thin line to the next row. */
@Composable
fun GroupedRow(index: Int, count: Int, modifier: Modifier = Modifier, content: @Composable () -> Unit) {
    Column(modifier.fillMaxWidth().clip(rowShape(index, count)).background(Palette.Panel)) {
        content()
        if (index < count - 1) HorizontalDivider(Modifier.padding(start = 16.dp), color = Palette.Separator)
    }
}

@Composable
fun SectionHeader(text: String, modifier: Modifier = Modifier) =
    Text(text.uppercase(), modifier = modifier.padding(start = 16.dp, top = 18.dp, bottom = 6.dp), style = SectionTitle,
        color = Palette.LabelSecondary)

/** Round toolbar button. */
@Composable
fun ToolButton(icon: ImageVector, description: String, modifier: Modifier = Modifier, tint: Color = Palette.Accent, onClick: () -> Unit) {
    Box(modifier.size(38.dp).pressable(onClick = onClick).background(Palette.Panel, RectangleShape), contentAlignment = Alignment.Center) {
        Icon(icon, description, tint = tint, modifier = Modifier.size(20.dp))
    }
}

/** The big heading with its toolbar buttons on the right. */
@Composable
fun LargeTitleBar(title: String, modifier: Modifier = Modifier, subtitle: String? = null, actions: @Composable RowScope.() -> Unit = {}) {
    Row(modifier.fillMaxWidth().padding(start = 20.dp, end = 16.dp, top = 12.dp, bottom = 8.dp), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(title, style = LargeTitle, maxLines = 1, overflow = TextOverflow.Ellipsis)
            if (subtitle != null) Muted(subtitle, maxLines = 1)
        }
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically, content = actions)
    }
}

@Composable
fun Muted(text: String, maxLines: Int = Int.MAX_VALUE, modifier: Modifier = Modifier) =
    Text(text, modifier = modifier, style = MaterialTheme.typography.bodySmall, color = Palette.LabelSecondary,
        maxLines = maxLines, overflow = TextOverflow.Ellipsis)

/** Search field: a rounded grey capsule. */
@Composable
fun SearchField(value: String, onChange: (String) -> Unit, placeholder: String, modifier: Modifier = Modifier) {
    BasicTextField(
        value, onChange, singleLine = true, modifier = modifier.fillMaxWidth(),
        textStyle = TextStyle(color = Palette.Label, fontSize = 16.sp),
        cursorBrush = SolidColor(Palette.Accent),
        decorationBox = { inner ->
            Row(Modifier.clip(RectangleShape).background(Palette.Panel).padding(horizontal = 12.dp, vertical = 10.dp),
                verticalAlignment = Alignment.CenterVertically) {
                Icon(Icons.Default.Search, null, tint = Palette.LabelSecondary, modifier = Modifier.size(20.dp))
                Spacer(Modifier.width(8.dp))
                Box(Modifier.weight(1f)) {
                    if (value.isEmpty()) Text(placeholder, color = Palette.LabelTertiary, fontSize = 16.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    inner()
                }
                AnimatedVisibility(value.isNotEmpty(), enter = fadeIn(), exit = fadeOut()) {
                    Icon(Icons.Default.Clear, stringResource(R.string.clear), tint = Palette.LabelSecondary,
                        modifier = Modifier.size(20.dp).pressable { onChange("") })
                }
            }
        },
    )
}

/** Two to four choices in one capsule; the highlight slides to the chosen one. */
@Composable
fun Segmented(options: List<String>, selected: Int, onSelect: (Int) -> Unit, modifier: Modifier = Modifier) {
    BoxWithConstraints(modifier.fillMaxWidth().clip(RectangleShape).background(Palette.Panel).padding(3.dp)) {
        val width = maxWidth / options.size
        val x by animateDpAsState(width * selected, spring(dampingRatio = 0.8f, stiffness = Spring.StiffnessMediumLow), label = "segment")
        Box(Modifier.offset(x).width(width).height(32.dp).clip(RectangleShape).background(Palette.PanelRaised))
        Row {
            options.forEachIndexed { i, label ->
                val color by animateColorAsState(if (i == selected) Palette.Label else Palette.LabelSecondary, tween(200), label = "segmentText")
                Box(Modifier.weight(1f).height(32.dp).pressable { onSelect(i) }, contentAlignment = Alignment.Center) {
                    Text(label, color = color, fontSize = 13.sp, fontWeight = FontWeight.Medium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
            }
        }
    }
}

class TabItem(val icon: ImageVector, val label: String)

/** The bar at the bottom: a floating capsule whose highlight glides to the chosen tab. */
@Composable
fun TabBar(items: List<TabItem>, selected: Int, onSelect: (Int) -> Unit) {
    Column(Modifier.fillMaxWidth().background(Palette.Background).navigationBarsPadding()) {
        HorizontalDivider(color = Palette.Separator)
        Row(Modifier.fillMaxWidth()) {
            items.forEachIndexed { i, item ->
                val tint by animateColorAsState(if (i == selected) Palette.Accent else Palette.LabelSecondary, tween(150), label = "tabTint")
                Column(Modifier.weight(1f).height(54.dp).pressable { onSelect(i) }, horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.Center) {
                    Icon(item.icon, null, tint = tint, modifier = Modifier.size(22.dp))
                    Text(item.label, color = tint, fontSize = 11.sp, fontWeight = FontWeight.Medium, maxLines = 1)
                }
            }
        }
    }
}

// ---- state texts -------------------------------------------------------------------------------

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

/** One person on a server. Long press names them. */
@Composable
fun SessionRow(s: RemoteSession, nickname: String? = null, onLongClick: (() -> Unit)? = null) {
    val (label, color) = idleLabel(s)
    Row(Modifier.fillMaxWidth().then(if (onLongClick != null) Modifier.pressable(onLongClick = onLongClick) {} else Modifier)
        .padding(horizontal = 16.dp, vertical = 12.dp), verticalAlignment = Alignment.Top) {
        Box(Modifier.padding(top = 6.dp)) { StatusDot(color, 10) }
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            val user = s.user.substringAfter('\\')
            Text(nickname ?: s.client.ifBlank { user }, fontWeight = FontWeight.SemiBold)
            if (nickname != null) Muted(listOf(s.client, user).filter { it.isNotBlank() }.distinct().joinToString(" · "))
            else if (s.client.isNotBlank()) Muted(user)
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
    val shown by animateIntAsState(count, tween(450), label = "count")
    val border by animateColorAsState(if (selected) color else Color.Transparent, tween(220), label = "tileBorder")
    val fill by animateColorAsState(if (selected) color.copy(alpha = 0.16f) else Palette.Panel, tween(220), label = "tileFill")
    Column(
        modifier.pressable(onClick = onClick).clip(RectangleShape).background(fill)
            .border(1.5.dp, border, RectangleShape).padding(horizontal = 12.dp, vertical = 10.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            StatusDot(color, 8)
            Text(shown.toString(), fontSize = 22.sp, fontWeight = FontWeight.Bold)
        }
        Text(label, fontSize = 12.sp, color = Palette.LabelSecondary, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

// ---- time --------------------------------------------------------------------------------------

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
