// SPDX-License-Identifier: MIT

package dev.tessera.app

import android.app.Activity
import androidx.window.layout.WindowInfoTracker
import androidx.window.layout.WindowLayoutInfo
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.launch

/**
 * The one Kotlin file in the shell, and it exists for one reason: `WindowInfoTracker.windowLayoutInfo`
 * returns a Kotlin `Flow`, and collecting a `Flow` from Java means calling suspend functions through
 * `BuildersKt.launch$default` with a hand-written `Function2`. That compiles only while every
 * parameter type lines up exactly and breaks on any coroutines version bump.
 *
 * Kotlin is already on the classpath because `androidx.window` is written in it, so this costs no
 * new dependency. It is deliberately tiny: it starts a collection on the main dispatcher and hands
 * each layout to a Java callback. All the mapping logic stays in [TesseraFoldReader], where it is
 * readable and testable.
 */
internal class FoldObserver(private val activity: Activity) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var job: Job? = null

    /**
     * Collects window layout updates until [stop].
     *
     * The first emission is the current layout, so a caller does not need a separate synchronous
     * read: subscribing is the read.
     *
     * The callback is a `java.util.function.Consumer` rather than a Kotlin function type so the Java
     * caller can pass a lambda directly; a Kotlin `(WindowLayoutInfo) -> Unit` compiles to a
     * `Function1` and does not convert.
     */
    fun start(onLayout: java.util.function.Consumer<WindowLayoutInfo>) {
        if (job != null) {
            return
        }
        val flow: Flow<WindowLayoutInfo> = WindowInfoTracker.getOrCreate(activity)
            .windowLayoutInfo(activity)
        job = scope.launch {
            flow.collect { info -> onLayout.accept(info) }
        }
    }

    /** Stops observing. Safe twice, and safe when [start] never ran. */
    fun stop() {
        job?.cancel()
        job = null
        scope.cancel()
    }
}
