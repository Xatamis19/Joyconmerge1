# Joy-Con Merge

App Android che unisce **Joy-Con (L)** e **Joy-Con (R)** in un unico gamepad virtuale,
così gli emulatori (Bachata-S4/shadPS4, Eden, Dolphin…) li vedono come un controller normale.

## Requisiti
- Kernel con driver `hid-nintendo` (i Joy-Con compaiono come "Joy-Con (L)" e "Joy-Con (R)")
- [Shizuku](https://shizuku.rikka.app/) attivo

## Uso
1. Abbina entrambi i Joy-Con via Bluetooth.
2. Apri l'app, concedi il permesso Shizuku, premi **Avvia**.
3. Nell'emulatore scegli il controller **"Joy-Con Combined (merge)"**
   (o **"Microsoft X-Box 360 pad"** se hai attivato la modalità XInput).
4. Per fermare: pulsante **Ferma** nell'app o nella notifica.

## Mappatura
| Joy-Con | Gamepad virtuale |
|---|---|
| Stick sinistro | X / Y |
| Stick destro | Z / RZ |
| Croce | D-pad (hat) |
| A B X Y, L R, ZL ZR, −, +, Home, Cattura, click stick | pulsanti standard |
| ZL / ZR | anche assi Brake / Gas |

SL/SR vengono ignorati per evitare conflitti con L/R.

## Modalità Xbox 360 (XInput)
Con l'interruttore **Modalità Xbox 360 (XInput)** (o `joycon_merge --xinput` da shell) il gamepad
virtuale usa l'ID di un controller Xbox 360 (`045E:028E`) e la disposizione assi del driver `xpad`:

| Joy-Con | Gamepad Xbox virtuale |
|---|---|
| Stick sinistro | X / Y |
| Stick destro | RX / RY (Android: Z / RZ) |
| ZL / ZR | grilletti LT / RT (assi Z / RZ, 0 o pieno) |
| B A Y X (posizione) | A B X Y Xbox |
| −, +, Home | Back, Start, Guide |

Android carica il suo layout Xbox già pronto, quindi emulatori e giochi che riconoscono solo
controller "XInput"/Xbox lo vedono come tale. I pulsanti seguono la **posizione** (quello in basso
è A), non le lettere stampate sul Joy-Con. La vibrazione non viene ancora inoltrata.

## Come funziona
L'app avvia tramite Shizuku un piccolo programma C (`app/src/main/cpp/joycon_merge.c`)
con i permessi della shell: legge i due dispositivi evdev, li blocca con `EVIOCGRAB`
e crea un gamepad tramite `/dev/uhid`.

## Build
Ogni push su `main` compila l'APK con GitHub Actions e lo pubblica nella Release **latest**.
