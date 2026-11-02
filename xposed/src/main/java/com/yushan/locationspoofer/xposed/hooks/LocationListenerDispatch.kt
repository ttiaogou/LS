package com.yushan.locationspoofer.xposed.hooks

import java.lang.reflect.InvocationTargetException

/** A successful return means invocation completed, not that the client accepted the fix. */
internal fun invokeLocationListener(listener: Any, location: Any, callbackFactory: () -> Any) {
    val methods = listener.javaClass.methods.filter { it.name == "onLocationChanged" }
    val single = methods.firstOrNull {
        it.parameterCount == 1 && it.parameterTypes[0].isInstance(location)
    }
    fun invoke(method: java.lang.reflect.Method, args: Array<Any?>) {
        try {
            method.invoke(listener, *args)
        } catch (error: InvocationTargetException) {
            throw (error.targetException ?: error)
        }
    }
    if (single != null) {
        invoke(single, arrayOf(location))
        return
    }
    val batch = methods.firstOrNull {
        it.parameterCount in 1..2 && it.parameterTypes[0].isInstance(listOf(location)) &&
            List::class.java.isAssignableFrom(it.parameterTypes[0]) &&
            (it.parameterCount == 1 || !it.parameterTypes[1].isPrimitive)
    } ?: throw NoSuchMethodException("${listener.javaClass.name}.onLocationChanged: unsupported signature")
    val args = arrayOfNulls<Any>(batch.parameterCount)
    args[0] = listOf(location)
    if (args.size == 1) {
        invoke(batch, args)
        return
    }
    try {
        args[1] = callbackFactory()
        invoke(batch, args)
    } catch (first: Throwable) {
        args[1] = null
        try {
            invoke(batch, args)
        } catch (last: Throwable) {
            if (last !== first) last.addSuppressed(first)
            throw last
        }
    }
}
