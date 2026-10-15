package com.joyconmerge.app

import android.os.Handler
import android.os.Looper

/** Stato condiviso tra servizio e schermata, notificato sempre sul thread principale. */
object MergeState {
    private val main = Handler(Looper.getMainLooper())
    private val listeners = mutableSetOf<() -> Unit>()
    private val lines = ArrayDeque<String>()

    @Volatile
    var running: Boolean = false
        private set

    val log: String
        get() = synchronized(lines) { lines.joinToString("\n") }

    fun setRunning(value: Boolean) {
        running = value
        notifyListeners()
    }

    fun append(line: String) {
        synchronized(lines) {
            lines.addLast(line)
            while (lines.size > 200) lines.removeFirst()
        }
        notifyListeners()
    }

    fun clearLog() {
        synchronized(lines) { lines.clear() }
        notifyListeners()
    }

    fun addListener(l: () -> Unit) = main.post { listeners.add(l); l() }
    fun removeListener(l: () -> Unit) = main.post { listeners.remove(l) }

    private fun notifyListeners() = main.post { listeners.toList().forEach { it() } }
}
