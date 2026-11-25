package com.rconnect.android.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.horizontalScroll
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
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Clear
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.composed
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Fill
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntOffset
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
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.sin

@Composable
fun healthColor(state: ServerState): Color {
    val c = appColors
    return when (state.health) {
        Health.Free -> c.green
        Health.InUse -> c.busy
        Health.Problem -> when (state.errorKind) {
            ServerState.ErrorKind.Unreachable -> c.text3
            else -> c.orange
        }
        Health.Pending -> c.text2
    }
}

// ---- motion ------------------------------------------------------------------------------------

/** Click (and optional long press) with a subtle press response. */
@OptIn(ExperimentalFoundationApi::class)
fun Modifier.pressable(onLongClick: (() -> Unit)? = null, onClick: () -> Unit): Modifier = composed {
    val source = remember { MutableInteractionSource() }
    val pressed by source.collectIsPressedAsState()
    val scale by animateFloatAsState(if (pressed) 0.985f else 1f, spring(dampingRatio = 0.7f, stiffness = Spring.StiffnessMedium), label = "press")
    val fade by animateFloatAsState(if (pressed) 0.82f else 1f, tween(80), label = "pressFade")
    this
        .graphicsLayer { scaleX = scale; scaleY = scale }
        .alpha(fade)
        .combinedClickable(interactionSource = source, indication = null, onClick = onClick, onLongClick = onLongClick)
}

@Composable
fun StatusDot(color: Color, size: Int = 8, pulse: Boolean = false, borderColor: Color? = null) {
    val shown by animateColorAsState(color, tween(250), label = "dot")
    val breathing = rememberInfiniteTransition(label = "pulse")
    val level by breathing.animateFloat(1f, 0.35f, infiniteRepeatable(tween(900), RepeatMode.Reverse), label = "level")
    Box(
        Modifier
            .size(size.dp)
            .alpha(if (pulse) level else 1f)
            .clip(CircleShape)
            .background(shown, CircleShape)
            .then(if (borderColor != null) Modifier.border(1.5.dp, borderColor, CircleShape) else Modifier),
    )
}

/** Monitor icon tile with the status dot on its lower-right corner. */
@Composable
fun MonitorTile(dotColor: Color, cardFill: Color, modifier: Modifier = Modifier, pulse: Boolean = false) {
    val c = appColors
    Box(modifier.size(40.dp)) {
        Box(
            Modifier
                .size(38.dp)
                .clip(RadiusControl)
                .background(c.accentSoft),
            contentAlignment = Alignment.Center,
        ) {
            Canvas(Modifier.size(20.dp)) {
                val stroke = Stroke(width = 1.5.dp.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round)
                val w = size.width
                val h = size.height
                drawRoundRect(
                    color = c.accentText,
                    topLeft = Offset(w * 0.1f, h * 0.16f),
                    size = Size(w * 0.8f, h * 0.54f),
                    cornerRadius = CornerRadius(2.dp.toPx(), 2.dp.toPx()),
                    style = stroke,
                )
                drawLine(c.accentText, Offset(w * 0.5f, h * 0.70f), Offset(w * 0.5f, h * 0.85f), strokeWidth = stroke.width, cap = StrokeCap.Round)
                drawLine(c.accentText, Offset(w * 0.32f, h * 0.86f), Offset(w * 0.68f, h * 0.86f), strokeWidth = stroke.width, cap = StrokeCap.Round)
            }
        }
        Box(Modifier.align(Alignment.BottomEnd)) {
            StatusDot(dotColor, size = 10, pulse = pulse, borderColor = cardFill)
        }
    }
}

@Composable
fun FavoriteStar(filled: Boolean, onClick: () -> Unit, modifier: Modifier = Modifier) {
    val c = appColors
    val fillColor = c.favorite
    val outlineColor = if (filled) (if (c.isDark) c.favorite else c.orange) else c.text3
    val desc = stringResource(R.string.favorite)
    Box(
        modifier
            .size(30.dp)
            .semantics { contentDescription = desc }
            .pressable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Canvas(Modifier.size(16.dp)) {
            val cx = size.width / 2f
            val cy = size.height / 2f
            val outerR = size.minDimension * 0.46f
            val innerR = outerR * 0.44f
            val path = Path()
            for (k in 0 until 10) {
                val r = if (k % 2 == 1) innerR else outerR
                val a = -PI / 2.0 + k * PI / 5.0
                val x = cx + (r * cos(a)).toFloat()
                val y = cy + (r * sin(a)).toFloat()
                if (k == 0) path.moveTo(x, y) else path.lineTo(x, y)
            }
            path.close()
            if (filled) drawPath(path, color = fillColor, style = Fill)
            drawPath(path, color = outlineColor, style = Stroke(width = 1.25.dp.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round))
        }
    }
}

/** Four squares for cards, three lines for the list. */
@Composable
fun ViewModeToggle(listMode: Boolean, onSelect: (Boolean) -> Unit, modifier: Modifier = Modifier) {
    val c = appColors
    Row(
        modifier
            .clip(RadiusCard)
            .background(c.surfaceAlt)
            .border(1.dp, c.border, RadiusCard)
            .padding(3.dp),
        horizontalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        for (isList in listOf(false, true)) {
            val active = isList == listMode
            val bg by animateColorAsState(if (active) c.accentSoft else Color.Transparent, tween(160), label = "viewBg")
            val fg by animateColorAsState(if (active) c.accentText else c.text2, tween(160), label = "viewFg")
            Box(
                Modifier
                    .size(width = 32.dp, height = 28.dp)
                    .clip(RadiusControl)
                    .background(bg)
                    .pressable { onSelect(isList) },
                contentAlignment = Alignment.Center,
            ) {
                Canvas(Modifier.size(14.dp)) {
                    val stroke = Stroke(width = 1.3.dp.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round)
                    val s = size.minDimension
                    if (!isList) {
                        val box = s * 0.36f
                        val gap = s * 0.56f
                        val r = CornerRadius(1.dp.toPx(), 1.dp.toPx())
                        drawRoundRect(fg, Offset(s * 0.08f, s * 0.08f), Size(box, box), r, stroke)
                        drawRoundRect(fg, Offset(gap, s * 0.08f), Size(box, box), r, stroke)
                        drawRoundRect(fg, Offset(s * 0.08f, gap), Size(box, box), r, stroke)
                        drawRoundRect(fg, Offset(gap, gap), Size(box, box), r, stroke)
                    } else {
                        for (yf in listOf(0.22f, 0.5f, 0.78f)) {
                            val y = s * yf
                            drawCircle(fg, radius = 0.9.dp.toPx(), center = Offset(s * 0.16f, y))
                            drawLine(fg, Offset(s * 0.36f, y), Offset(s * 0.88f, y), strokeWidth = stroke.width, cap = StrokeCap.Round)
                        }
                    }
                }
            }
        }
    }
}

// ---- layout ------------------------------------------------------------------------------------

fun rowShape(index: Int, count: Int): Shape = when {
    count <= 1 -> RadiusCard
    index == 0 -> RoundedCornerShape(topStart = 4.dp, topEnd = 4.dp)
    index == count - 1 -> RoundedCornerShape(bottomStart = 4.dp, bottomEnd = 4.dp)
    else -> RoundedCornerShape(0.dp)
}

/** A row of a grouped list, with an outer border and a thin separator to the next row. */
@Composable
fun GroupedRow(
    index: Int,
    count: Int,
    modifier: Modifier = Modifier,
    backgroundColor: Color? = null,
    content: @Composable () -> Unit,
) {
    val c = appColors
    val shape = rowShape(index, count)
    val fill = backgroundColor ?: c.surface
    Column(
        modifier
            .fillMaxWidth()
            .clip(shape)
            .background(fill)
            .border(1.dp, c.border, shape),
    ) {
        content()
        if (index < count - 1) HorizontalDivider(color = c.separator)
    }
}

@Composable
fun DesktopGroupHeader(
    title: String,
    count: Int,
    collapsed: Boolean,
    modifier: Modifier = Modifier,
    subtitle: String? = null,
    onClick: () -> Unit,
) {
    val c = appColors
    val turn by animateFloatAsState(if (collapsed) -90f else 0f, tween(180), label = "chevron")
    Row(
        modifier
            .fillMaxWidth()
            .pressable(onClick = onClick)
            .padding(top = 16.dp, bottom = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Canvas(Modifier.size(16.dp).rotate(turn)) {
            val path = Path().apply {
                moveTo(size.width * 0.25f, size.height * 0.38f)
                lineTo(size.width * 0.5f, size.height * 0.65f)
                lineTo(size.width * 0.75f, size.height * 0.38f)
            }
            drawPath(path, color = c.text2, style = Stroke(width = 1.5.dp.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round))
        }
        Spacer(Modifier.width(6.dp))
        Text(title, style = SectionTitle, color = c.text, maxLines = 1, overflow = TextOverflow.Ellipsis)
        Spacer(Modifier.width(7.dp))
        Box(
            Modifier
                .clip(RadiusControl)
                .background(c.accentSoft)
                .padding(horizontal = 7.dp, vertical = 2.dp),
            contentAlignment = Alignment.Center,
        ) {
            Text(count.toString(), fontSize = 11.sp, fontWeight = FontWeight.SemiBold, color = c.text2)
        }
        if (!subtitle.isNullOrBlank()) {
            Spacer(Modifier.width(8.dp))
            Text(subtitle, fontSize = 11.sp, color = c.text3, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        Spacer(Modifier.width(10.dp))
        HorizontalDivider(Modifier.weight(1f), color = c.separator)
    }
}

@Composable
fun SectionHeader(text: String, modifier: Modifier = Modifier) {
    val c = appColors
    Row(modifier.fillMaxWidth().padding(top = 16.dp, bottom = 6.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(text, style = SectionTitle, color = c.text2)
        Spacer(Modifier.width(10.dp))
        HorizontalDivider(Modifier.weight(1f), color = c.separator)
    }
}

@Composable
fun ToolButton(
    icon: ImageVector,
    description: String,
    modifier: Modifier = Modifier,
    tint: Color? = null,
    onClick: () -> Unit,
) {
    val c = appColors
    Box(
        modifier
            .size(36.dp)
            .pressable(onClick = onClick)
            .clip(RadiusControl)
            .background(c.button)
            .border(1.dp, c.border, RadiusControl),
        contentAlignment = Alignment.Center,
    ) {
        Icon(icon, description, tint = tint ?: c.text2, modifier = Modifier.size(19.dp))
    }
}

@Composable
fun PrimaryActionButton(text: String, onClick: () -> Unit, modifier: Modifier = Modifier) {
    val c = appColors
    Box(
        modifier
            .height(36.dp)
            .pressable(onClick = onClick)
            .clip(RadiusControl)
            .background(c.accent)
            .padding(horizontal = 14.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(text, color = c.onAccent, fontSize = 13.sp, fontWeight = FontWeight.SemiBold, maxLines = 1)
    }
}

/** The page heading with its toolbar actions on the right. */
@Composable
fun LargeTitleBar(title: String, modifier: Modifier = Modifier, subtitle: String? = null, actions: @Composable RowScope.() -> Unit = {}) {
    Row(
        modifier
            .fillMaxWidth()
            .padding(start = 16.dp, end = 16.dp, top = 14.dp, bottom = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, style = LargeTitle, color = appColors.text, maxLines = 1, overflow = TextOverflow.Ellipsis)
            if (subtitle != null) Muted(subtitle, maxLines = 1)
        }
        Spacer(Modifier.width(8.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically, content = actions)
    }
}

@Composable
fun Muted(text: String, modifier: Modifier = Modifier, maxLines: Int = Int.MAX_VALUE) =
    Text(text, modifier = modifier, style = MaterialTheme.typography.bodySmall, color = appColors.text2,
        maxLines = maxLines, overflow = TextOverflow.Ellipsis)

@Composable
fun SearchField(value: String, onChange: (String) -> Unit, placeholder: String, modifier: Modifier = Modifier) {
    val c = appColors
    var focused by remember { mutableStateOf(false) }
    val stroke by animateColorAsState(if (focused) c.focus else c.border, tween(140), label = "searchBorder")
    BasicTextField(
        value = value,
        onValueChange = onChange,
        singleLine = true,
        modifier = modifier
            .fillMaxWidth()
            .onFocusChanged { focused = it.isFocused },
        textStyle = TextStyle(color = c.text, fontSize = 14.sp),
        cursorBrush = SolidColor(c.accent),
        decorationBox = { inner ->
            Row(
                Modifier
                    .height(38.dp)
                    .clip(RadiusControl)
                    .background(c.field)
                    .border(1.dp, stroke, RadiusControl)
                    .padding(horizontal = 10.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(Icons.Default.Search, null, tint = c.text2, modifier = Modifier.size(18.dp))
                Spacer(Modifier.width(8.dp))
                Box(Modifier.weight(1f)) {
                    if (value.isEmpty()) Text(placeholder, color = c.text3, fontSize = 14.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    inner()
                }
                AnimatedVisibility(value.isNotEmpty(), enter = fadeIn(), exit = fadeOut()) {
                    Icon(
                        Icons.Default.Clear,
                        stringResource(R.string.clear),
                        tint = c.text2,
                        modifier = Modifier.size(18.dp).pressable { onChange("") },
                    )
                }
            }
        },
    )
}

/** Two to four choices in one bar; the highlight slides to the chosen one. */
@Composable
fun Segmented(options: List<String>, selected: Int, onSelect: (Int) -> Unit, modifier: Modifier = Modifier) {
    val c = appColors
    BoxWithConstraints(
        modifier
            .fillMaxWidth()
            .clip(RadiusCard)
            .background(c.surfaceAlt)
            .border(1.dp, c.border, RadiusCard)
            .padding(3.dp),
    ) {
        val width = maxWidth / options.size
        val x by animateDpAsState(width * selected, spring(dampingRatio = 0.85f, stiffness = Spring.StiffnessMediumLow), label = "segment")
        Box(
            Modifier
                .offset { IntOffset(x.roundToPx(), 0) }
                .width(width)
                .height(30.dp)
                .clip(RadiusControl)
                .background(c.accentSoft),
        )
        Row {
            options.forEachIndexed { i, label ->
                val color by animateColorAsState(if (i == selected) c.accentText else c.text2, tween(160), label = "segmentText")
                Box(Modifier.weight(1f).height(30.dp).pressable { onSelect(i) }, contentAlignment = Alignment.Center) {
                    Text(label, color = color, fontSize = 12.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
            }
        }
    }
}

/** Horizontally scrollable segmented bar for filter choices. */
@Composable
fun ScrollableSegmented(options: List<String>, selected: Int, onSelect: (Int) -> Unit, modifier: Modifier = Modifier) {
    val c = appColors
    Row(
        modifier
            .horizontalScroll(rememberScrollState())
            .clip(RadiusCard)
            .background(c.surfaceAlt)
            .border(1.dp, c.border, RadiusCard)
            .padding(3.dp),
        horizontalArrangement = Arrangement.spacedBy(2.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        options.forEachIndexed { i, label ->
            val active = i == selected
            val bg by animateColorAsState(if (active) c.accentSoft else Color.Transparent, tween(160), label = "segBg")
            val fg by animateColorAsState(if (active) c.accentText else c.text2, tween(160), label = "segFg")
            Box(
                Modifier
                    .height(28.dp)
                    .clip(RadiusControl)
                    .background(bg)
                    .pressable { onSelect(i) }
                    .padding(horizontal = 10.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(label, color = fg, fontSize = 12.sp, fontWeight = FontWeight.SemiBold, maxLines = 1)
            }
        }
    }
}

@Composable
fun desktopTextFieldColors() = OutlinedTextFieldDefaults.colors(
    focusedTextColor = appColors.text,
    unfocusedTextColor = appColors.text,
    focusedContainerColor = appColors.field,
    unfocusedContainerColor = appColors.field,
    focusedBorderColor = appColors.focus,
    unfocusedBorderColor = appColors.border,
    focusedLabelColor = appColors.text2,
    unfocusedLabelColor = appColors.text3,
    cursorColor = appColors.accent,
    errorBorderColor = appColors.red,
    errorLabelColor = appColors.red,
)

class TabItem(val icon: ImageVector, val label: String)

@Composable
fun TabBar(items: List<TabItem>, selected: Int, onSelect: (Int) -> Unit) {
    val c = appColors
    Column(Modifier.fillMaxWidth().background(c.sidebar).navigationBarsPadding()) {
        HorizontalDivider(color = c.border)
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 10.dp, vertical = 6.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            items.forEachIndexed { i, item ->
                val active = i == selected
                val bg by animateColorAsState(if (active) c.accentSoft else Color.Transparent, tween(160), label = "tabBg")
                val tint by animateColorAsState(if (active) c.accentText else c.text2, tween(160), label = "tabTint")
                Row(
                    Modifier
                        .weight(1f)
                        .height(40.dp)
                        .clip(RadiusControl)
                        .background(bg)
                        .pressable { onSelect(i) }
                        .padding(horizontal = 8.dp),
                    horizontalArrangement = Arrangement.Center,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Icon(item.icon, null, tint = tint, modifier = Modifier.size(18.dp))
                    Spacer(Modifier.width(6.dp))
                    Text(item.label, color = tint, fontSize = 12.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
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
        ServerState.ErrorKind.NoWinRm -> stringResource(R.string.status_no_winrm)
        ServerState.ErrorKind.AuthFailed -> stringResource(R.string.status_auth)
        else -> stringResource(R.string.status_error)
    }
}

@Composable
fun errorExplanation(s: ServerState): String = when (s.errorKind) {
    ServerState.ErrorKind.Unreachable -> stringResource(R.string.err_unreachable)
    ServerState.ErrorKind.NoWinRm -> stringResource(R.string.err_no_winrm)
    ServerState.ErrorKind.AuthFailed -> stringResource(R.string.err_auth)
    else -> s.error?.takeIf { it.isNotBlank() } ?: stringResource(R.string.err_other)
}

@Composable
fun idleLabel(s: RemoteSession): Pair<String, Color> {
    val c = appColors
    return when {
        !s.connected -> stringResource(R.string.not_connected) to c.text3
        s.idleMinutes < 1 -> stringResource(R.string.active) to c.green
        s.idleMinutes < 60 -> stringResource(R.string.idle_min, s.idleMinutes) to c.orange
        else -> stringResource(R.string.idle_hours, s.idleMinutes / 60) to c.orange
    }
}

/** Short activity label (connection duration and idle time) for a busy server card. */
@Composable
fun compactActivityText(s: RemoteSession, serverNowUtcMs: Long): String {
    val parts = ArrayList<String>(2)
    val now = if (serverNowUtcMs > 0) serverNowUtcMs else System.currentTimeMillis()
    val since = if (s.connectedUtcMs > 0) s.connectedUtcMs else s.logonUtcMs
    if (s.connected && since > 0 && now >= since) {
        val mins = (now - since) / 60_000L
        val dur = when {
            mins == 0L -> stringResource(R.string.dur_under_min)
            mins < 60L -> stringResource(R.string.dur_min, mins.toInt())
            mins < 24 * 60L -> stringResource(R.string.dur_hm, (mins / 60).toInt(), (mins % 60).toInt())
            else -> stringResource(R.string.dur_dh, (mins / (24 * 60)).toInt(), ((mins / 60) % 24).toInt())
        }
        parts += stringResource(R.string.connected_duration, dur)
    }
    if (s.connected) {
        parts += when {
            s.idleMinutes < 1 -> stringResource(R.string.active_short)
            s.idleMinutes < 60 -> stringResource(R.string.idle_min, s.idleMinutes)
            else -> stringResource(R.string.idle_hours, s.idleMinutes / 60)
        }
    }
    return parts.joinToString(" · ")
}

/** One person on a server. Long press names them. */
@Composable
fun SessionRow(s: RemoteSession, nickname: String? = null, onLongClick: (() -> Unit)? = null) {
    val (label, color) = idleLabel(s)
    Row(
        Modifier
            .fillMaxWidth()
            .then(if (onLongClick != null) Modifier.pressable(onLongClick = onLongClick) {} else Modifier)
            .padding(horizontal = 14.dp, vertical = 12.dp),
        verticalAlignment = Alignment.Top,
    ) {
        Box(Modifier.padding(top = 6.dp)) { StatusDot(color, 8) }
        Spacer(Modifier.width(10.dp))
        Column(Modifier.weight(1f)) {
            val user = s.user.substringAfter('\\')
            Text(nickname ?: s.client.ifBlank { user }, fontWeight = FontWeight.SemiBold, color = appColors.text)
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

// ---- natural sort ------------------------------------------------------------------------------

private val naturalChunk = Regex("(\\d+|\\D+)")

/** Compares two strings with numbers in natural order ("srv2" before "srv10"). */
fun naturalCompare(a: String, b: String): Int {
    val la = a.lowercase(Locale.ROOT)
    val lb = b.lowercase(Locale.ROOT)
    val pa = naturalChunk.findAll(la).map { it.value }.toList()
    val pb = naturalChunk.findAll(lb).map { it.value }.toList()
    val n = minOf(pa.size, pb.size)
    for (i in 0 until n) {
        val ca = pa[i]
        val cb = pb[i]
        if (ca == cb) continue
        val na = ca.toBigIntegerOrNull()
        val nb = cb.toBigIntegerOrNull()
        val cmp = if (na != null && nb != null) na.compareTo(nb) else ca.compareTo(cb)
        if (cmp != 0) return cmp
    }
    return pa.size.compareTo(pb.size)
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
