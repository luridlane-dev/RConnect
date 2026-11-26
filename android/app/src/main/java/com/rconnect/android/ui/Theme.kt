package com.rconnect.android.ui

import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Surface
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.rconnect.android.ThemeMode

/** Colours of the app for dark and light modes. */
@Immutable
data class AppColors(
    val isDark: Boolean,
    val bg: Color,
    val topBar: Color,
    val sidebar: Color,
    val surface: Color,
    val surfaceAlt: Color,
    val button: Color,
    val field: Color,
    val border: Color,
    val focus: Color,
    val solid: Color,
    val solidBorder: Color,
    val text: Color,
    val text2: Color,
    val text3: Color,
    val hover: Color,
    val press: Color,
    val accent: Color,
    val accentText: Color,
    val accentSoft: Color,
    val onAccent: Color,
    val green: Color,
    val orange: Color,
    val favorite: Color,
    val red: Color,
    val busy: Color,
    val busyText: Color,
    val busySurface: Color,
    val connectedSurface: Color,
    val separator: Color,
)

val DarkAppColors = AppColors(
    isDark = true,
    bg = Color(0xFF080808),
    topBar = Color(0xFF101010),
    sidebar = Color(0xFF101010),
    surface = Color(0xFF151515),
    surfaceAlt = Color(0xFF111111),
    button = Color(0xFF212121),
    field = Color(0xFF141414),
    border = Color(0xFF262626),
    focus = Color(0xFF777777),
    solid = Color(0xFF1C1C1C),
    solidBorder = Color(0xFF333333),
    text = Color(0xFFF5F5F5),
    text2 = Color(0xFFC2C2C2),
    text3 = Color(0xFF9D9D9D),
    hover = Color(0xFF2A2A2A),
    press = Color(0xFF353535),
    accent = Color(0xFFEDEDED),
    accentText = Color(0xFFEEEEEE),
    accentSoft = Color(0xFF282828),
    onAccent = Color(0xFF141414),
    green = Color(0xFF65DBAB),
    orange = Color(0xFFFFDF42),
    favorite = Color(0xFFFFE34D),
    red = Color(0xFFFB8393),
    busy = Color(0xFF9385F7),
    busyText = Color(0xFFC8C0FF),
    busySurface = Color(0xFF191826),
    connectedSurface = Color(0xFF172820),
    separator = Color(0xFF222222),
)

val LightAppColors = AppColors(
    isDark = false,
    bg = Color(0xFFF4F6FC),
    topBar = Color(0xFFFFFFFF),
    sidebar = Color(0xFFEEF1F9),
    surface = Color(0xFFFFFFFF),
    surfaceAlt = Color(0xFFF0F3FA),
    button = Color(0xFFFFFFFF),
    field = Color(0xFFFFFFFF),
    border = Color(0xFFE1E6F0),
    focus = Color(0xFF6878C4),
    solid = Color(0xFFFFFFFF),
    solidBorder = Color(0xFFC5D0E3),
    text = Color(0xFF19233A),
    text2 = Color(0xFF4C5974),
    text3 = Color(0xFF606E88),
    hover = Color(0xFFE9EDF8),
    press = Color(0xFFDCE4F6),
    accent = Color(0xFF414EB3),
    accentText = Color(0xFF3746A3),
    accentSoft = Color(0xFFE3E9FF),
    onAccent = Color(0xFFFFFFFF),
    green = Color(0xFF087554),
    orange = Color(0xFF795800),
    favorite = Color(0xFFE5B500),
    red = Color(0xFFBE3A54),
    busy = Color(0xFF5D53CC),
    busyText = Color(0xFF473EB0),
    busySurface = Color(0xFFF3F2FC),
    connectedSurface = Color(0xFFE5F3EB),
    separator = Color(0xFFE1E7F2),
)

val LocalAppColors = staticCompositionLocalOf { DarkAppColors }

val appColors: AppColors
    @Composable get() = LocalAppColors.current

val RadiusControl = RoundedCornerShape(3.dp)
val RadiusCard = RoundedCornerShape(4.dp)

private fun toMaterialScheme(c: AppColors) = if (c.isDark) {
    darkColorScheme(
        primary = c.accent,
        onPrimary = c.onAccent,
        primaryContainer = c.accentSoft,
        onPrimaryContainer = c.accentText,
        secondary = c.accent,
        onSecondary = c.onAccent,
        background = c.bg,
        onBackground = c.text,
        surface = c.surface,
        onSurface = c.text,
        surfaceVariant = c.surfaceAlt,
        onSurfaceVariant = c.text2,
        surfaceTint = Color.Transparent,
        surfaceContainerLowest = c.bg,
        surfaceContainerLow = c.surfaceAlt,
        surfaceContainer = c.surface,
        surfaceContainerHigh = c.solid,
        surfaceContainerHighest = c.button,
        outline = c.border,
        outlineVariant = c.separator,
        error = c.red,
        onError = c.onAccent,
    )
} else {
    lightColorScheme(
        primary = c.accent,
        onPrimary = c.onAccent,
        primaryContainer = c.accentSoft,
        onPrimaryContainer = c.accentText,
        secondary = c.accent,
        onSecondary = c.onAccent,
        background = c.bg,
        onBackground = c.text,
        surface = c.surface,
        onSurface = c.text,
        surfaceVariant = c.surfaceAlt,
        onSurfaceVariant = c.text2,
        surfaceTint = Color.Transparent,
        surfaceContainerLowest = c.bg,
        surfaceContainerLow = c.surfaceAlt,
        surfaceContainer = c.surface,
        surfaceContainerHigh = c.solid,
        surfaceContainerHighest = c.button,
        outline = c.border,
        outlineVariant = c.separator,
        error = c.red,
        onError = c.onAccent,
    )
}

private val AppShapes = Shapes(
    extraSmall = RadiusControl,
    small = RadiusControl,
    medium = RadiusCard,
    large = RadiusCard,
    extraLarge = RadiusCard,
)

private val AppTypography = Typography().let {
    it.copy(
        titleLarge = it.titleLarge.copy(fontSize = 20.sp, fontWeight = FontWeight.SemiBold, letterSpacing = (-0.2).sp),
        titleMedium = it.titleMedium.copy(fontSize = 15.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 0.sp),
        bodyLarge = it.bodyLarge.copy(fontSize = 15.sp, letterSpacing = 0.sp),
        bodyMedium = it.bodyMedium.copy(fontSize = 14.sp, letterSpacing = 0.sp),
        bodySmall = it.bodySmall.copy(fontSize = 12.sp, letterSpacing = 0.sp),
        labelLarge = it.labelLarge.copy(fontSize = 13.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 0.sp),
    )
}

/** The big heading at the top of a screen. */
val LargeTitle = TextStyle(fontSize = 26.sp, fontWeight = FontWeight.SemiBold, letterSpacing = (-0.3).sp)

/** Small heading above a group of rows. */
val SectionTitle = TextStyle(fontSize = 13.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 0.sp)

private val DarkMaterialScheme = toMaterialScheme(DarkAppColors)
private val LightMaterialScheme = toMaterialScheme(LightAppColors)

@Composable
fun RConnectTheme(mode: ThemeMode = ThemeMode.Dark, content: @Composable () -> Unit) {
    val light = mode == ThemeMode.Light
    val colors = if (light) LightAppColors else DarkAppColors
    val scheme = if (light) LightMaterialScheme else DarkMaterialScheme
    CompositionLocalProvider(LocalAppColors provides colors) {
        MaterialTheme(colorScheme = scheme, shapes = AppShapes, typography = AppTypography) {
            Surface(color = colors.bg, contentColor = colors.text, content = content)
        }
    }
}
