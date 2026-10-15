package com.joyconmerge.app

import android.Manifest
import android.app.Activity
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Build
import android.os.Bundle
import android.util.TypedValue
import android.view.Gravity
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import rikka.shizuku.Shizuku

class MainActivity : Activity() {

    private lateinit var shizukuStatus: TextView
    private lateinit var mainButton: Button
    private lateinit var logView: TextView
    private lateinit var logScroll: ScrollView

    private val onState: () -> Unit = { render() }
    private val onBinder = Shizuku.OnBinderReceivedListener { runOnUiThread { render() } }
    private val onBinderDead = Shizuku.OnBinderDeadListener { runOnUiThread { render() } }
    private val onPermission = Shizuku.OnRequestPermissionResultListener { code, _ ->
        if (code == ShizukuHelper.REQUEST_CODE) runOnUiThread { render() }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        buildUi()

        Shizuku.addBinderReceivedListenerSticky(onBinder)
        Shizuku.addBinderDeadListener(onBinderDead)
        Shizuku.addRequestPermissionResultListener(onPermission)

        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 1)
        }
    }

    override fun onStart() {
        super.onStart()
        MergeState.addListener(onState)
    }

    override fun onStop() {
        MergeState.removeListener(onState)
        super.onStop()
    }

    override fun onDestroy() {
        Shizuku.removeBinderReceivedListener(onBinder)
        Shizuku.removeBinderDeadListener(onBinderDead)
        Shizuku.removeRequestPermissionResultListener(onPermission)
        super.onDestroy()
    }

    private fun onMainButton() {
        when {
            !ShizukuHelper.isRunning -> render()
            !ShizukuHelper.hasPermission -> Shizuku.requestPermission(ShizukuHelper.REQUEST_CODE)
            MergeState.running -> MergeService.stop(this)
            else -> MergeService.start(this)
        }
    }

    private fun render() {
        if (!::mainButton.isInitialized) return
        val running = MergeState.running
        when {
            !ShizukuHelper.isRunning -> {
                shizukuStatus.text = "Shizuku non è attivo. Avvialo dall'app Shizuku e torna qui."
                shizukuStatus.setTextColor(Color.parseColor("#FF8A80"))
                mainButton.text = "In attesa di Shizuku"
                mainButton.isEnabled = false
            }
            !ShizukuHelper.hasPermission -> {
                shizukuStatus.text = "Shizuku attivo, ma serve il permesso."
                shizukuStatus.setTextColor(Color.parseColor("#FFD180"))
                mainButton.text = "Concedi permesso Shizuku"
                mainButton.isEnabled = true
            }
            else -> {
                shizukuStatus.text = if (running) "Joy-Con uniti: gamepad virtuale attivo." else "Pronto. Collega i due Joy-Con e premi Avvia."
                shizukuStatus.setTextColor(Color.parseColor(if (running) "#69F0AE" else "#B0BEC5"))
                mainButton.text = if (running) "Ferma" else "Avvia"
                mainButton.isEnabled = true
            }
        }
        mainButton.background = pill(if (running) "#FF5252" else "#00B4E6")
        logView.text = MergeState.log.ifEmpty { "Il registro del motore comparirà qui." }
        logScroll.post { logScroll.fullScroll(ScrollView.FOCUS_DOWN) }
    }

    private fun buildUi() {
        val pad = dp(20)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, dp(32), pad, pad)
            setBackgroundColor(Color.parseColor("#121212"))
        }

        root.addView(TextView(this).apply {
            text = "Joy-Con Merge"
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 28f)
            typeface = Typeface.DEFAULT_BOLD
        })
        root.addView(TextView(this).apply {
            text = "Unisce Joy-Con (L) e (R) in un unico controller per gli emulatori."
            setTextColor(Color.parseColor("#9E9E9E"))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
            setPadding(0, dp(4), 0, dp(24))
        })

        shizukuStatus = TextView(this).apply {
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
            setPadding(0, 0, 0, dp(16))
        }
        root.addView(shizukuStatus)

        mainButton = Button(this).apply {
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f)
            isAllCaps = false
            setOnClickListener { onMainButton() }
        }
        root.addView(mainButton, LinearLayout.LayoutParams(MATCH_PARENT, dp(56)))

        root.addView(TextView(this).apply {
            text = "Registro"
            setTextColor(Color.parseColor("#9E9E9E"))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
            setPadding(0, dp(28), 0, dp(8))
        })

        logView = TextView(this).apply {
            typeface = Typeface.MONOSPACE
            setTextColor(Color.parseColor("#CFD8DC"))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
            setPadding(dp(12), dp(12), dp(12), dp(12))
            setTextIsSelectable(true)
        }
        logScroll = ScrollView(this).apply {
            background = GradientDrawable().apply {
                setColor(Color.parseColor("#1E1E1E"))
                cornerRadius = dp(12).toFloat()
            }
            addView(logView)
        }
        root.addView(logScroll, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))

        root.addView(TextView(this).apply {
            text = "Tip: lascia l'app in background mentre giochi. Ferma dalla notifica."
            setTextColor(Color.parseColor("#757575"))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
            gravity = Gravity.CENTER
            setPadding(0, dp(12), 0, 0)
        })

        // Android 15 disegna sotto le barre di sistema: ci teniamo lontani
        root.fitsSystemWindows = true
        setContentView(root, android.view.ViewGroup.LayoutParams(MATCH_PARENT, MATCH_PARENT))
    }

    private fun pill(color: String) = GradientDrawable().apply {
        setColor(Color.parseColor(color))
        cornerRadius = dp(28).toFloat()
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()
}
