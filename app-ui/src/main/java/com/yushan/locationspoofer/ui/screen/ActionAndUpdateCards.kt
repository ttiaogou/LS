package com.yushan.locationspoofer.ui.screen

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.yushan.locationspoofer.data.model.AppState
import com.yushan.locationspoofer.ui.R
import com.yushan.locationspoofer.ui.theme.AccentBlue
import com.yushan.locationspoofer.viewmodel.MainViewModel
import com.yushan.locationspoofer.viewmodel.stopSpoofing

@Composable
fun ActionButtons(
    viewModel: MainViewModel,
    uiState: AppState,
    onOpenMap: () -> Unit,
    onStartFixedSpoofing: () -> Unit
) {
    if (uiState.isSpoofingActive) {
        val stopColor by animateColorAsState(
            targetValue = MaterialTheme.colorScheme.error,
            animationSpec = tween(300), label = "stop_color"
        )
        Button(
            onClick = { viewModel.stopSpoofing() },
            modifier = Modifier
                .fillMaxWidth()
                .height(50.dp),
            shape = RoundedCornerShape(14.dp),
            colors = ButtonDefaults.buttonColors(containerColor = stopColor)
        ) {
            Icon(Icons.Rounded.Stop, null, modifier = Modifier.size(20.dp))
            Spacer(Modifier.width(8.dp))
            Text(
                stringResource(R.string.stop_simulation),
                fontSize = 15.sp,
                fontWeight = FontWeight.SemiBold
            )
        }
    } else {
        Button(
            onClick = onStartFixedSpoofing,
            enabled = !uiState.isSavingConfig,
            modifier = Modifier
                .fillMaxWidth()
                .height(50.dp),
            shape = RoundedCornerShape(14.dp),
            colors = ButtonDefaults.buttonColors(containerColor = AccentBlue)
        ) {
            if (uiState.isSavingConfig) {
                CircularProgressIndicator(
                    modifier = Modifier.size(18.dp),
                    color = MaterialTheme.colorScheme.onPrimary,
                    strokeWidth = 2.dp
                )
                Spacer(Modifier.width(8.dp))
                Text(stringResource(R.string.starting_ellipsis), fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
            } else {
                Icon(Icons.Rounded.MyLocation, null, modifier = Modifier.size(18.dp))
                Spacer(Modifier.width(6.dp))
                Text(
                    stringResource(R.string.fixed_simulation),
                    fontSize = 14.sp,
                    fontWeight = FontWeight.SemiBold
                )
            }
        }
    }
}
