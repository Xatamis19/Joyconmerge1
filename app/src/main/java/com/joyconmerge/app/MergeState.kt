package com.joyconmerge.app

import android.content.Context
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

    /** Modalita' Xbox 360 (XInput): salvata nelle preferenze dell'app. */
    fun isXinput(ctx: Context): Boolean =
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(KEY_XINPUT, false)

    fun setXinput(ctx: Context, value: Boolean) {
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putBoolean(KEY_XINPUT, value).apply()
    }

    /** Sorgente: Joy-Con (L+R) oppure DualShock 4. */
    fun isDs4(ctx: Context): Boolean =
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY_SOURCE, SOURCE_JOYCON) == SOURCE_DS4

    fun setDs4(ctx: Context, value: Boolean) {
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putString(KEY_SOURCE, if (value) SOURCE_DS4 else SOURCE_JOYCON).apply()
    }

    /** DualShock 4: lascia il touchpad ad Android come mouse (comodo nei menu). */
    fun isKeepTouchpad(ctx: Context): Boolean =
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(KEY_TOUCHPAD, true)

    fun setKeepTouchpad(ctx: Context, value: Boolean) {
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putBoolean(KEY_TOUCHPAD, value).apply()
    }

    private const val KEY_TOUCHPAD = "keep_touchpad"
    private const val PREFS = "settings"
    private const val KEY_XINPUT = "xinput"
    private const val KEY_SOURCE = "source"
    private const val SOURCE_JOYCON = "joycon"
    private const val SOURCE_DS4 = "ds4"

    fun addListener(l: () -> Unit) = main.post { listeners.add(l); l() }
    fun removeListener(l: () -> Unit) = main.post { listeners.remove(l) }

    private fun notifyListeners() = main.post { listeners.toList().forEach { it() } }
}
