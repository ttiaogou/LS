package com.yushan.locationspoofer.ui.screen.managedata

import com.yushan.locationspoofer.data.db.CompleteLocation
import com.yushan.locationspoofer.data.db.CompleteCollectionRoute

data class ManageDataUiState(
    val dataList: List<CompleteLocation> = emptyList(),
    val collectionRoutes: List<CompleteCollectionRoute> = emptyList(),
    val isLoading: Boolean = false
)
