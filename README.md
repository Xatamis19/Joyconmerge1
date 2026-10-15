# Joy-Con Merge

App Android che unisce **Joy-Con (L)** e **Joy-Con (R)** in un unico gamepad virtuale,
così gli emulatori (Bachata-S4/shadPS4, Eden, Dolphin…) li vedono come un controller normale.

## Requisiti
- Kernel con driver `hid-nintendo` (i Joy-Con compaiono come "Joy-Con (L)" e "Joy-Con (R)")
- [Shizuku](https://shizuku.rikka.app/) attivo

## Uso
1. Abbina entrambi i Joy-Con via Bluetooth.
2. Apri l'app, concedi il permesso Shizuku, premi **Avvia**.
3. Nell'emulatore scegli il controller **"Joy-Con Combined (merge)"**.
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

## Come funziona
L'app avvia tramite Shizuku un piccolo programma C (`app/src/main/cpp/joycon_merge.c`)
con i permessi della shell: legge i due dispositivi evdev, li blocca con `EVIOCGRAB`
e crea un gamepad tramite `/dev/uhid`.

## Build
Ogni push su `main` compila l'APK con GitHub Actions e lo pubblica nella Release **latest**.
