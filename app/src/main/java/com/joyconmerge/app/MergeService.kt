package com.joyconmerge.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder

class MergeService : Service() {

    @Volatile
    private var proc: ShizukuHelper.RemoteProc? = null

    @Volatile
    private var stopping = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                stopEngine()
                return START_NOT_STICKY
            }
            else -> startEngine()
        }
        return START_NOT_STICKY
    }

    private fun startEngine() {
        goForeground()
        if (proc != null) return
        stopping = false
        MergeState.clearLog()
        MergeState.setRunning(true)

        Thread({
            try {
                if (!ShizukuHelper.hasPermission) {
                    MergeState.append("Shizuku non attivo o permesso non concesso.")
                    return@Thread
                }
                val src = applicationInfo.nativeLibraryDir + "/libjoycon_merge.so"
                // La shell non puo' eseguire file dentro la cartella dell'app: lo copiamo in /data/local/tmp
                val copy = ShizukuHelper.exec("cp '$src' $ENGINE && chmod 755 $ENGINE 2>&1")
                if (copy == null) {
                    MergeState.append("Impossibile avviare la shell di Shizuku.")
                    return@Thread
                }
                val copyOut = copy.output.bufferedReader().readText().trim()
                if (copy.waitFor() != 0) {
                    MergeState.append("Copia del motore fallita: $copyOut")
                    return@Thread
                }

                val p = ShizukuHelper.exec("exec $ENGINE 2>&1")
                if (p == null) {
                    MergeState.append("Impossibile avviare il motore.")
                    return@Thread
                }
                proc = p
                p.output.bufferedReader().useLines { seq ->
                    seq.forEach { line -> if (line.isNotBlank()) MergeState.append(line) }
                }
                p.waitFor()
            } catch (t: Throwable) {
                MergeState.append("Errore: ${t.message ?: t.javaClass.simpleName}")
            } finally {
                proc = null
                if (!stopping) MergeState.append("Motore terminato.")
                MergeState.setRunning(false)
                stopForegroundCompat()
                stopSelf()
            }
        }, "joycon-merge").start()
    }

    private fun stopEngine() {
        stopping = true
        val p = proc
        if (p != null) {
            Thread({ p.destroy() }, "joycon-stop").start()
            MergeState.append("Arresto…")
        } else {
            MergeState.setRunning(false)
            stopForegroundCompat()
            stopSelf()
        }
    }

    override fun onDestroy() {
        stopping = true
        proc?.destroy()
        super.onDestroy()
    }

    private fun goForeground() {
        val nm = getSystemService(NotificationManager::class.java)
        if (Build.VERSION.SDK_INT >= 26) {
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL, "Joy-Con Merge", NotificationManager.IMPORTANCE_LOW)
            )
        }
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        val stop = PendingIntent.getService(
            this, 1, Intent(this, MergeService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        val n: Notification = Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_notify)
            .setContentTitle("Joy-Con uniti")
            .setContentText("Gamepad virtuale attivo")
            .setContentIntent(open)
            .setOngoing(true)
            .addAction(Notification.Action.Builder(null, "Ferma", stop).build())
            .build()

        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
        } else {
            startForeground(NOTIF_ID, n)
        }
    }

    private fun stopForegroundCompat() {
        stopForeground(STOP_FOREGROUND_REMOVE)
    }

    companion object {
        private const val CHANNEL = "merge"
        private const val NOTIF_ID = 1
        private const val ENGINE = "/data/local/tmp/.joycon_merge"
        const val ACTION_START = "com.joyconmerge.app.START"
        const val ACTION_STOP = "com.joyconmerge.app.STOP"

        fun start(ctx: Context) {
            ctx.startForegroundService(Intent(ctx, MergeService::class.java).setAction(ACTION_START))
        }

        fun stop(ctx: Context) {
            ctx.startService(Intent(ctx, MergeService::class.java).setAction(ACTION_STOP))
        }
    }
}
