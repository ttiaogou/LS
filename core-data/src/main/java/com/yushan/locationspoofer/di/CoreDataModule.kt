package com.yushan.locationspoofer.di

import com.yushan.locationspoofer.data.db.AppDatabase
import com.yushan.locationspoofer.data.motion.ConfigMotionSink
import com.yushan.locationspoofer.data.motion.MotionController
import com.yushan.locationspoofer.data.state.SpoofingState
import com.yushan.locationspoofer.data.repository.LocationRepository
import com.yushan.locationspoofer.data.repository.SettingsRepository
import com.yushan.locationspoofer.data.repository.WifiRepository
import com.yushan.locationspoofer.utils.ConfigManager
import com.yushan.locationspoofer.utils.EnvironmentScanner
import com.yushan.locationspoofer.utils.LSPosedManager
import com.yushan.locationspoofer.utils.OpenCellIdClient
import com.yushan.locationspoofer.utils.RootManager
import com.yushan.locationspoofer.utils.SettingsManager
import com.yushan.locationspoofer.utils.WigleClient
import com.yushan.locationspoofer.data.repository.HookStatusRepository
import org.koin.android.ext.koin.androidContext
import org.koin.dsl.module

val coreDataModule = module {
    single { RootManager() }
    single { ConfigManager(androidContext(), get()) }
    single { LSPosedManager() }
    single { SettingsManager(androidContext()) }
    single { EnvironmentScanner(androidContext()) }

    single { WigleClient() }
    single { OpenCellIdClient() }
    single { WifiRepository(get()) }

    single { LocationRepository(get(), get(), get(), get(), get(), get()) }
    single { SettingsRepository(get()) }
    single { HookStatusRepository(get()) }
    single {
        val settings = get<SettingsManager>()
        MotionController(
            sink = ConfigMotionSink(get(), get()),
            realismParams = {
                (SpoofingState.realismLevel.takeIf { it >= 0 } ?: settings.realismLevel) to
                    (SpoofingState.speedFluctuationPct.takeIf { it >= 0 } ?: settings.speedFluctuationPct)
            }
        )
    }

    single { AppDatabase.getDatabase(androidContext()) }
    single { get<AppDatabase>().environmentDao() }
    single { get<AppDatabase>().savedRouteDao() }
}
