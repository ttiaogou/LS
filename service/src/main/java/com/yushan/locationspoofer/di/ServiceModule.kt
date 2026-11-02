package com.yushan.locationspoofer.di

import com.yushan.locationspoofer.data.repository.SpoofingServiceController
import com.yushan.locationspoofer.service.SpoofingServiceControllerImpl
import org.koin.dsl.module

val serviceModule = module {
    single<SpoofingServiceController> { SpoofingServiceControllerImpl() }
}
