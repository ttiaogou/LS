package com.yushan.locationspoofer.ui.screen.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.text.font.FontWeight
import com.yushan.locationspoofer.ui.BuildConfig
import com.yushan.locationspoofer.ui.R
import com.yushan.locationspoofer.ui.theme.AccentBlue
import com.yushan.locationspoofer.ui.theme.AccentOrange

/** 当前安装的是哪个模拟方案，更新页、启动更新弹窗、系统适配页共用 */
@Composable
fun SchemeBadge(modifier: Modifier = Modifier) {
    val color = if (BuildConfig.GLOBAL_SCHEME) AccentOrange else AccentBlue
    Box(
        modifier = modifier
            .clip(RoundedCornerShape(8.dp))
            .background(color.copy(alpha = 0.15f))
            .padding(horizontal = 8.dp, vertical = 4.dp)
    ) {
        Text(
            text = stringResource(if (BuildConfig.GLOBAL_SCHEME) R.string.scheme_global else R.string.scheme_scoped),
            fontSize = 12.sp,
            fontWeight = FontWeight.SemiBold,
            color = color
        )
    }
}
