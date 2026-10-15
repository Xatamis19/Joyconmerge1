package com.joyconmerge.app

import android.content.pm.PackageManager
import android.os.ParcelFileDescriptor
import moe.shizuku.server.IRemoteProcess
import moe.shizuku.server.IShizukuService
import rikka.shizuku.Shizuku
import java.io.InputStream

object ShizukuHelper {
    const val REQUEST_CODE = 4242

    val isRunning: Boolean
        get() = try { Shizuku.pingBinder() } catch (_: Throwable) { false }

    val hasPermission: Boolean
        get() = try {
            isRunning && Shizuku.checkSelfPermission() == PackageManager.PERMISSION_GRANTED
        } catch (_: Throwable) { false }

    /** Avvia un comando con i permessi della shell (uid 2000) tramite Shizuku. */
    fun exec(script: String): RemoteProc? {
        val service = IShizukuService.Stub.asInterface(Shizuku.getBinder()) ?: return null
        val p = service.newProcess(arrayOf("sh", "-c", script), null, null) ?: return null
        return RemoteProc(p)
    }

    class RemoteProc(private val p: IRemoteProcess) {
        val output: InputStream = ParcelFileDescriptor.AutoCloseInputStream(p.inputStream)

        fun waitFor(): Int = try { p.waitFor() } catch (_: Throwable) { -1 }

        fun destroy() {
            try { p.destroy() } catch (_: Throwable) {}
        }
    }
}
