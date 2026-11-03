package com.rconnect.android.ui

import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Surface
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** The colours of the app: a dark, quiet window with grouped panels and one blue accent. */
object Palette {
    val Background = Color(0xFF000000)
    val Panel = Color(0xFF0F0F0F)
    val PanelRaised = Color(0xFF1C1C1C)
    val Separator = Color(0x1FFFFFFF)
    val Label = Color(0xFFE8E8E8)
    val LabelSecondary = Color(0xFF8F8F8F)
    val LabelTertiary = Color(0xFF5E5E5E)
    val Accent = Color(0xFF4A86C8)
    val Green = Color(0xFF6AA97D)
    val Orange = Color(0xFFC8975A)
    val Red = Color(0xFFC4675F)
    val Grey = Color(0xFF8E8E93)
}

private val Scheme = darkColorScheme(
    primary = Palette.Accent,
    onPrimary = Color.White,
    primaryContainer = Color(0xFF14283D),
    onPrimaryContainer = Color.White,
    secondary = Palette.Accent,
    onSecondary = Color.White,
    background = Palette.Background,
    onBackground = Palette.Label,
    surface = Palette.Background,
    onSurface = Palette.Label,
    surfaceVariant = Palette.Panel,
    onSurfaceVariant = Palette.LabelSecondary,
    surfaceTint = Color.Transparent,
    surfaceContainerLowest = Palette.Background,
    surfaceContainerLow = Palette.Background,
    surfaceContainer = Palette.Panel,
    surfaceContainerHigh = Palette.Panel,
    surfaceContainerHighest = Palette.PanelRaised,
    outline = Palette.Separator,
    outlineVariant = Palette.Separator,
    error = Palette.Red,
    onError = Color.White,
)

private val Square = RoundedCornerShape(0.dp)

private val AppShapes = Shapes(extraSmall = Square, small = Square, medium = Square, large = Square, extraLarge = Square)

private val AppTypography = Typography().let {
    it.copy(
        titleLarge = it.titleLarge.copy(fontWeight = FontWeight.SemiBold),
        titleMedium = it.titleMedium.copy(fontWeight = FontWeight.SemiBold),
        bodyLarge = it.bodyLarge.copy(fontSize = 16.sp, letterSpacing = 0.sp),
        bodyMedium = it.bodyMedium.copy(fontSize = 15.sp, letterSpacing = 0.sp),
        bodySmall = it.bodySmall.copy(fontSize = 13.sp, letterSpacing = 0.sp),
        labelLarge = it.labelLarge.copy(fontSize = 14.sp, letterSpacing = 0.sp),
    )
}

/** The big heading at the top of a screen. */
val LargeTitle = TextStyle(fontSize = 32.sp, fontWeight = FontWeight.Bold, letterSpacing = (-0.4).sp)

/** Small grey heading above a group of rows. */
val SectionTitle = TextStyle(fontSize = 13.sp, fontWeight = FontWeight.Medium, letterSpacing = 0.2.sp)

@Composable
fun RConnectTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = Scheme, shapes = AppShapes, typography = AppTypography) {
        Surface(color = Palette.Background, contentColor = Palette.Label, content = content)
    }
}
