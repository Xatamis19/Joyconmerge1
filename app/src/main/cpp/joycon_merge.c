/*
 * joycon_merge - unisce Joy-Con (L) e Joy-Con (R) in un unico gamepad virtuale.
 *
 * Funziona con il driver hid-nintendo del kernel: legge i due dispositivi evdev,
 * li "blocca" (EVIOCGRAB) cosi' Android non li vede piu' separati, e crea tramite
 * /dev/uhid un gamepad standard:
 *   stick sinistro  -> X / Y
 *   stick destro    -> Z / RZ   (quello che si aspettano gli emulatori Android)
 *   croce           -> hat switch (DPAD)
 *   ZL / ZR         -> pulsanti L2/R2 + assi analogici BRAKE/GAS (0 o massimo)
 *
 * Modalita' --xinput: il gamepad si presenta come controller Xbox 360
 * (VID 045E / PID 028E) con la disposizione assi di xpad, cosi' Android usa il suo
 * layout Xbox e gli emulatori/giochi lo riconoscono come controller XInput:
 *   stick destro    -> RX / RY
 *   ZL / ZR         -> grilletti analogici Z / RZ (LTRIGGER / RTRIGGER)
 *
 * Sorgente --ds4: invece dei Joy-Con legge un DualShock 4 (o DualSense) gestito dal
 * driver hid-playstation/hid-sony e lo ripresenta come pad Xbox 360 (va usato con
 * --xinput): grilletti L2/R2 analogici, Quadrato/Triangolo -> X/Y Xbox, Share/Options/PS
 * -> Back/Start/Guide. Anche touchpad e sensori di movimento vengono "bloccati", cosi'
 * Android vede un solo controller.
 *
 * Va eseguito come utente shell (Shizuku/rish o adb shell).
 * Uso: ./joycon_merge [--ds4] [--xinput]   (Ctrl+C per uscire)
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uhid.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NAME_L "Joy-Con (L)"
#define NAME_R "Joy-Con (R)"

#define SONY_VENDOR_ID 0x054C
#define MAX_EXTRA 4

/* ID neutro (pid.codes): evita che hid-nintendo o altri driver si aggancino */
#define VENDOR_ID  0x1209
#define PRODUCT_ID 0x0001

/* Xbox 360 cablato: Android ha gia' il layout Vendor_045e_Product_028e.kl */
#define XBOX_VENDOR_ID  0x045E
#define XBOX_PRODUCT_ID 0x028E

/*
 * Descrittore HID:
 *  15 pulsanti (+1 bit padding) -> hid-input li mappa su BTN_SOUTH..BTN_THUMBR
 *  hat switch 4 bit + 4 bit padding
 *  X, Y, Z, Rz a 16 bit con segno
 *  Brake, Accelerator a 8 bit
 */
static const uint8_t rdesc_generic[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x05,       /* Usage (Game Pad) */
    0xA1, 0x01,       /* Collection (Application) */
    0x05, 0x09,       /*   Usage Page (Button) */
    0x19, 0x01,       /*   Usage Minimum (1) */
    0x29, 0x0F,       /*   Usage Maximum (15) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x01,       /*   Logical Maximum (1) */
    0x75, 0x01,       /*   Report Size (1) */
    0x95, 0x0F,       /*   Report Count (15) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */
    0x75, 0x01,       /*   Report Size (1) */
    0x95, 0x01,       /*   Report Count (1) */
    0x81, 0x03,       /*   Input (Const) padding */
    0x05, 0x01,       /*   Usage Page (Generic Desktop) */
    0x09, 0x39,       /*   Usage (Hat switch) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x07,       /*   Logical Maximum (7) */
    0x35, 0x00,       /*   Physical Minimum (0) */
    0x46, 0x3B, 0x01, /*   Physical Maximum (315) */
    0x65, 0x14,       /*   Unit (Degrees) */
    0x75, 0x04,       /*   Report Size (4) */
    0x95, 0x01,       /*   Report Count (1) */
    0x81, 0x42,       /*   Input (Data,Var,Abs,Null) */
    0x65, 0x00,       /*   Unit (None) */
    0x75, 0x04,       /*   Report Size (4) */
    0x95, 0x01,       /*   Report Count (1) */
    0x81, 0x03,       /*   Input (Const) padding */
    0x09, 0x30,       /*   Usage (X) */
    0x09, 0x31,       /*   Usage (Y) */
    0x09, 0x32,       /*   Usage (Z) */
    0x09, 0x35,       /*   Usage (Rz) */
    0x16, 0x01, 0x80, /*   Logical Minimum (-32767) */
    0x26, 0xFF, 0x7F, /*   Logical Maximum (32767) */
    0x75, 0x10,       /*   Report Size (16) */
    0x95, 0x04,       /*   Report Count (4) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */
    0x05, 0x02,       /*   Usage Page (Simulation Controls) */
    0x09, 0xC5,       /*   Usage (Brake) */
    0x09, 0xC4,       /*   Usage (Accelerator) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255) */
    0x75, 0x08,       /*   Report Size (8) */
    0x95, 0x02,       /*   Report Count (2) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */
    0xC0              /* End Collection */
};

/*
 * Descrittore "Xbox 360": stessi pulsanti e stesso formato di report del
 * generico, cambiano solo gli usage degli assi:
 *  X, Y, Rx, Ry a 16 bit con segno  -> ABS_X/Y, ABS_RX/RY (stick)
 *  Z, Rz a 8 bit                    -> ABS_Z / ABS_RZ (grilletti, come xpad)
 */
static const uint8_t rdesc_xinput[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x05,       /* Usage (Game Pad) */
    0xA1, 0x01,       /* Collection (Application) */
    0x05, 0x09,       /*   Usage Page (Button) */
    0x19, 0x01,       /*   Usage Minimum (1) */
    0x29, 0x0F,       /*   Usage Maximum (15) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x01,       /*   Logical Maximum (1) */
    0x75, 0x01,       /*   Report Size (1) */
    0x95, 0x0F,       /*   Report Count (15) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */
    0x75, 0x01,       /*   Report Size (1) */
    0x95, 0x01,       /*   Report Count (1) */
    0x81, 0x03,       /*   Input (Const) padding */
    0x05, 0x01,       /*   Usage Page (Generic Desktop) */
    0x09, 0x39,       /*   Usage (Hat switch) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x07,       /*   Logical Maximum (7) */
    0x35, 0x00,       /*   Physical Minimum (0) */
    0x46, 0x3B, 0x01, /*   Physical Maximum (315) */
    0x65, 0x14,       /*   Unit (Degrees) */
    0x75, 0x04,       /*   Report Size (4) */
    0x95, 0x01,       /*   Report Count (1) */
    0x81, 0x42,       /*   Input (Data,Var,Abs,Null) */
    0x65, 0x00,       /*   Unit (None) */
    0x75, 0x04,       /*   Report Size (4) */
    0x95, 0x01,       /*   Report Count (1) */
    0x81, 0x03,       /*   Input (Const) padding */
    0x09, 0x30,       /*   Usage (X) */
    0x09, 0x31,       /*   Usage (Y) */
    0x09, 0x33,       /*   Usage (Rx) */
    0x09, 0x34,       /*   Usage (Ry) */
    0x16, 0x01, 0x80, /*   Logical Minimum (-32767) */
    0x26, 0xFF, 0x7F, /*   Logical Maximum (32767) */
    0x75, 0x10,       /*   Report Size (16) */
    0x95, 0x04,       /*   Report Count (4) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */
    0x09, 0x32,       /*   Usage (Z)  - grilletto sinistro */
    0x09, 0x35,       /*   Usage (Rz) - grilletto destro */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00, /*   Logical Maximum (255) */
    0x75, 0x08,       /*   Report Size (8) */
    0x95, 0x02,       /*   Report Count (2) */
    0x81, 0x02,       /*   Input (Data,Var,Abs) */
    0xC0              /* End Collection */
};

static int xinput_mode = 0;
/*
 * 1 = pad Xbox con assi "alla Android generica": stick destro su Z/RZ e grilletti su
 * BRAKE/GAS. Serve quando il telefono non ha il layout Xbox (Vendor_045e_Product_028e.kl):
 * senza quel file lo stick destro RX/RY non viene letto da giochi ed emulatori.
 */
static int xinput_alt = 0;
static int ds4_mode = 0;
static int keep_touchpad = 0; /* --keep-touchpad: il touchpad resta un mouse per Android */

#pragma pack(push, 1)
struct report {
    uint16_t buttons;   /* bit n = BTN_SOUTH + n */
    uint8_t hat;        /* 0..7, 8 = centro */
    int16_t lx, ly, rx, ry;
    uint8_t brake, gas; /* in modalita' XInput: grilletti LT / RT */
};
#pragma pack(pop)

struct state {
    uint16_t buttons;
    int up, down, left, right;
    int16_t lx, ly, rx, ry;
    int analog_triggers;  /* 1 = lt/rt arrivano da assi veri (DS4) */
    uint8_t lt, rt;
};

/*
 * Driver del kernel che gestisce il DS4: con hid-playstation/hid-sony gli assi sono
 * "standard" (RX/RY = stick destro, Z/RZ = grilletti). Se il kernel non li ha, il
 * controller finisce su hid-generic e il layout e' diverso (Z/RZ = stick destro,
 * RX/RY = grilletti, pulsanti nell'ordine grezzo del DS4).
 */
static int ds4_generic_driver = 0;

/* range degli assi del DS4, letti con EVIOCGABS */
struct range { int min, max; };
static struct range ds4_rng[ABS_CNT];

static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }

static int open_by_name(const char *wanted, char *path_out, size_t path_len) {
    DIR *dir = opendir("/dev/input");
    if (!dir) { perror("opendir /dev/input"); return -1; }
    struct dirent *de;
    int found = -1;
    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        char name[256] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 && strcmp(name, wanted) == 0) {
            snprintf(path_out, path_len, "%s", path);
            found = fd;
            break;
        }
        close(fd);
    }
    closedir(dir);
    return found;
}

static int uhid_write(int fd, const struct uhid_event *ev) {
    ssize_t n = write(fd, ev, sizeof(*ev));
    if (n < 0) { perror("write /dev/uhid"); return -1; }
    if ((size_t)n != sizeof(*ev)) { fprintf(stderr, "write /dev/uhid parziale\n"); return -1; }
    return 0;
}

static int uhid_create(int fd) {
    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_CREATE2;
    int use_x = xinput_mode && !xinput_alt;
    const uint8_t *rd = use_x ? rdesc_xinput : rdesc_generic;
    size_t rd_size = use_x ? sizeof(rdesc_xinput) : sizeof(rdesc_generic);
    snprintf((char *)ev.u.create2.name, sizeof(ev.u.create2.name), "%s",
             xinput_mode ? "Microsoft X-Box 360 pad"
                         : ds4_mode ? "DualShock 4 (merge)" : "Joy-Con Combined (merge)");
    snprintf((char *)ev.u.create2.phys, sizeof(ev.u.create2.phys), "joycon-merge/0");
    snprintf((char *)ev.u.create2.uniq, sizeof(ev.u.create2.uniq), "joycon-merge-0");
    ev.u.create2.rd_size = (uint16_t)rd_size;
    ev.u.create2.bus = BUS_USB;
    ev.u.create2.vendor = xinput_mode ? XBOX_VENDOR_ID : VENDOR_ID;
    ev.u.create2.product = xinput_mode ? XBOX_PRODUCT_ID : PRODUCT_ID;
    ev.u.create2.version = xinput_mode ? 0x0114 : 1;
    ev.u.create2.country = 0;
    memcpy(ev.u.create2.rd_data, rd, rd_size);
    return uhid_write(fd, &ev);
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/*
 * Chiede ad Android (dumpsys input) quali assi vede sul pad virtuale e li scrive nel
 * registro. Ritorna 1 se lo stick destro e' arrivato come RX/RY (= manca il layout Xbox
 * e i giochi non lo leggerebbero), 0 se e' tutto a posto, -1 se non si capisce.
 */
static int check_android_axes(const char *devname) {
    FILE *f = popen("dumpsys input 2>/dev/null", "r");
    if (!f) return -1;
    char line[512], axes[256] = "";
    int in_reader = 0, in_dev = 0, has_rx = 0, has_z = 0, found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "Input Reader State")) { in_reader = 1; continue; }
        if (!in_reader) continue;
        if (strncmp(line, "  Device ", 9) == 0) {
            if (in_dev) break;
            in_dev = strstr(line, devname) != NULL;
            continue;
        }
        if (!in_dev) continue;
        char *src = strstr(line, ": source=JOYSTICK");
        if (!src) continue;
        found = 1;
        *src = 0;
        char *name = line;
        while (*name == ' ') name++;
        if (strlen(axes) + strlen(name) + 2 < sizeof(axes)) {
            strcat(axes, name);
            strcat(axes, " ");
        }
        if (strcmp(name, "RX") == 0) has_rx = 1;
        if (strcmp(name, "Z") == 0) has_z = 1;
    }
    pclose(f);
    if (!found) {
        printf("Assi visti da Android: (non trovati)\n");
        return -1;
    }
    printf("Assi visti da Android: %s\n", axes);
    return (has_rx && !has_z) ? 1 : 0;
}

static void uhid_destroy(int fd) {
    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_DESTROY;
    uhid_write(fd, &ev);
}

static uint8_t hat_from(const struct state *s) {
    int x = s->right - s->left;
    int y = s->down - s->up;
    if (x == 0 && y == 0) return 8;
    if (x == 0 && y < 0) return 0;
    if (x > 0 && y < 0) return 1;
    if (x > 0 && y == 0) return 2;
    if (x > 0 && y > 0) return 3;
    if (x == 0 && y > 0) return 4;
    if (x < 0 && y > 0) return 5;
    if (x < 0 && y == 0) return 6;
    return 7;
}

static int send_report(int fd, const struct state *s) {
    struct report r;
    r.buttons = s->buttons;
    /* L'Xbox 360 non ha pulsanti L2/R2: i grilletti sono solo assi */
    if (xinput_mode)
        r.buttons &= (uint16_t)~((1u << (BTN_TL2 - BTN_SOUTH)) | (1u << (BTN_TR2 - BTN_SOUTH)));
    r.hat = hat_from(s);
    r.lx = s->lx; r.ly = s->ly; r.rx = s->rx; r.ry = s->ry;
    if (s->analog_triggers) {
        r.brake = s->lt;
        r.gas = s->rt;
    } else {
        r.brake = (s->buttons & (1u << (BTN_TL2 - BTN_SOUTH))) ? 255 : 0;
        r.gas   = (s->buttons & (1u << (BTN_TR2 - BTN_SOUTH))) ? 255 : 0;
    }

    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_INPUT2;
    ev.u.input2.size = sizeof(r);
    memcpy(ev.u.input2.data, &r, sizeof(r));
    return uhid_write(fd, &ev);
}

static void set_button(struct state *s, int code, int pressed) {
    if (code < BTN_SOUTH || code > BTN_THUMBR) return;
    uint16_t bit = (uint16_t)(1u << (code - BTN_SOUTH));
    if (pressed) s->buttons |= bit; else s->buttons &= (uint16_t)~bit;
}

/*
 * Ogni Joy-Con ha anche SL/SR, che hid-nintendo riporta come TL/TR/TL2/TR2
 * del lato opposto. Per non avere conflitti prendiamo L/ZL solo dal sinistro
 * e R/ZR solo dal destro.
 */
static int left_owns(int code) {
    switch (code) {
    case BTN_TR: case BTN_TR2: return 0;
    default: return 1;
    }
}
static int right_owns(int code) {
    switch (code) {
    case BTN_TL: case BTN_TL2: return 0;
    default: return 1;
    }
}

/* ritorna 1 se e' arrivato un SYN_REPORT, 0 se no, -1 se il dispositivo e' sparito */
static int handle(int fd, int is_left, struct state *s) {
    struct input_event evs[64];
    int synced = 0;
    for (;;) {
        ssize_t n = read(fd, evs, sizeof(evs));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) break;
            return -1;
        }
        if (n == 0) return -1;
        size_t count = (size_t)n / sizeof(struct input_event);
        for (size_t i = 0; i < count; i++) {
            const struct input_event *e = &evs[i];
            if (e->type == EV_SYN && e->code == SYN_REPORT) {
                synced = 1;
            } else if (e->type == EV_KEY) {
                int v = e->value != 0;
                if (is_left) {
                    switch (e->code) {
                    case BTN_DPAD_UP:    s->up = v; break;
                    case BTN_DPAD_DOWN:  s->down = v; break;
                    case BTN_DPAD_LEFT:  s->left = v; break;
                    case BTN_DPAD_RIGHT: s->right = v; break;
                    default: if (left_owns(e->code)) set_button(s, e->code, v);
                    }
                } else if (right_owns(e->code)) {
                    set_button(s, e->code, v);
                }
            } else if (e->type == EV_ABS) {
                if (is_left) {
                    if (e->code == ABS_X) s->lx = (int16_t)e->value;
                    else if (e->code == ABS_Y) s->ly = (int16_t)e->value;
                } else {
                    if (e->code == ABS_RX) s->rx = (int16_t)e->value;
                    else if (e->code == ABS_RY) s->ry = (int16_t)e->value;
                }
            }
        }
    }
    return synced;
}

/* ---------------------------------------------------------------- DualShock 4 */

static int test_bit(const unsigned long *bits, int n) {
    return (bits[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1;
}

/*
 * Trova il nodo "gamepad" di un controller Sony (assi RX + pulsante SOUTH) e,
 * con lo stesso uniq/phys, gli altri nodi (touchpad, sensori) da bloccare.
 */
static int open_ds4(char *path_out, size_t path_len, int *extra, int *n_extra) {
    *n_extra = 0;
    DIR *dir = opendir("/dev/input");
    if (!dir) { perror("opendir /dev/input"); return -1; }
    struct dirent *de;
    int found = -1;
    char found_uniq[64] = "", found_phys[64] = "";
    int cand[32]; char cand_uniq[32][64]; char cand_phys[32][64]; char cand_name[32][128]; int nc = 0;

    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        struct input_id id;
        if (ioctl(fd, EVIOCGID, &id) < 0 || id.vendor != SONY_VENDOR_ID) { close(fd); continue; }

        char uniq[64] = "", phys[64] = "", dname[128] = "";
        ioctl(fd, EVIOCGNAME(sizeof(dname) - 1), dname);
        ioctl(fd, EVIOCGUNIQ(sizeof(uniq) - 1), uniq);
        ioctl(fd, EVIOCGPHYS(sizeof(phys) - 1), phys);
        /* phys e' tipo "xx:xx:..:xx/input0": teniamo solo la parte prima di '/' */
        char *slash = strchr(phys, '/');
        if (slash) *slash = 0;

        unsigned long absb[(ABS_CNT + 8 * sizeof(long) - 1) / (8 * sizeof(long))] = {0};
        unsigned long keyb[(KEY_CNT + 8 * sizeof(long) - 1) / (8 * sizeof(long))] = {0};
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absb)), absb);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyb)), keyb);
        int is_pad = test_bit(absb, ABS_RX) && test_bit(keyb, BTN_SOUTH);

        if (is_pad && found < 0) {
            snprintf(path_out, path_len, "%s", path);
            snprintf(found_uniq, sizeof(found_uniq), "%s", uniq);
            snprintf(found_phys, sizeof(found_phys), "%s", phys);
            found = fd;
        } else if (nc < 32) {
            cand[nc] = fd;
            snprintf(cand_uniq[nc], 64, "%s", uniq);
            snprintf(cand_phys[nc], 64, "%s", phys);
            snprintf(cand_name[nc], 128, "%s", dname);
            nc++;
        } else {
            close(fd);
        }
    }
    closedir(dir);

    for (int i = 0; i < nc; i++) {
        /* stesso controller: stesso uniq/phys, oppure i nodi "Touchpad"/"Motion Sensors" Sony */
        int same = found >= 0 &&
                   ((found_uniq[0] && strcmp(found_uniq, cand_uniq[i]) == 0) ||
                    (found_phys[0] && strcmp(found_phys, cand_phys[i]) == 0) ||
                    strstr(cand_name[i], "Touchpad") || strstr(cand_name[i], "Motion"));
        if (same && keep_touchpad && strstr(cand_name[i], "Touchpad")) {
            printf("Touchpad lasciato ad Android (mouse): %s\n", cand_name[i]);
            close(cand[i]);
        } else if (same && *n_extra < MAX_EXTRA) {
            printf("Nascondo anche: %s\n", cand_name[i]);
            extra[(*n_extra)++] = cand[i];
        } else {
            close(cand[i]);
        }
    }
    return found;
}

static void ds4_detect_driver(int fd, const char *path) {
    const char *ev = strrchr(path, '/');
    ev = ev ? ev + 1 : path;
    char link[300], target[300] = "";
    snprintf(link, sizeof(link), "/sys/class/input/%s/device/device/driver", ev);
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n > 0) target[n] = 0;
    const char *drv = strrchr(target, '/');
    drv = drv ? drv + 1 : target;
    if (!drv[0]) drv = "non leggibile";

    /*
     * Il layout si decide dalle capacita' del dispositivo, non da sysfs (che la shell
     * potrebbe non poter leggere): hid-playstation/hid-sony non usano mai BTN_C/BTN_Z,
     * mentre con hid-generic i pulsanti 3 e 6 del DS4 diventano proprio BTN_C e BTN_Z.
     */
    unsigned long keyb[(KEY_CNT + 8 * sizeof(long) - 1) / (8 * sizeof(long))] = {0};
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyb)), keyb);
    ds4_generic_driver = test_bit(keyb, BTN_C) || test_bit(keyb, BTN_Z);
    printf("Driver kernel: %s -> layout %s\n", drv, ds4_generic_driver ? "HID generico" : "standard");
}

/* layout hid-generic: posizione nel DS4 -> codice standard */
static int ds4_generic_button(int code) {
    switch (code) {
    case BTN_SOUTH:  return BTN_WEST;   /* 1 Quadrato  -> X Xbox (0x134 prima dello scambio) */
    case BTN_EAST:   return BTN_SOUTH;  /* 2 Croce     -> A */
    case BTN_C:      return BTN_EAST;   /* 3 Cerchio   -> B */
    case BTN_NORTH:  return BTN_NORTH;  /* 4 Triangolo -> Y (scambiato sotto) */
    case BTN_WEST:   return BTN_TL;     /* 5 L1 */
    case BTN_Z:      return BTN_TR;     /* 6 R1 */
    case BTN_TL:     return BTN_TL2;    /* 7 L2 (digitale) */
    case BTN_TR:     return BTN_TR2;    /* 8 R2 (digitale) */
    case BTN_TL2:    return BTN_SELECT; /* 9 Share */
    case BTN_TR2:    return BTN_START;  /* 10 Options */
    case BTN_SELECT: return BTN_THUMBL; /* 11 L3 */
    case BTN_START:  return BTN_THUMBR; /* 12 R3 */
    case BTN_MODE:   return BTN_MODE;   /* 13 PS */
    default:         return -1;         /* clic touchpad e altro: ignorati */
    }
}

static void ds4_read_ranges(int fd) {
    static const int axes[] = { ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ };
    for (size_t i = 0; i < sizeof(axes) / sizeof(axes[0]); i++) {
        struct input_absinfo ai;
        if (ioctl(fd, EVIOCGABS(axes[i]), &ai) == 0 && ai.maximum > ai.minimum) {
            ds4_rng[axes[i]].min = ai.minimum;
            ds4_rng[axes[i]].max = ai.maximum;
            printf("Asse %#x: %d..%d\n", axes[i], ai.minimum, ai.maximum);
        } else {
            printf("Asse %#x: non trovato, uso 0..255\n", axes[i]);
            ds4_rng[axes[i]].min = 0;
            ds4_rng[axes[i]].max = 255;
        }
    }
}

/* stick: [min,max] -> [-32767, 32767] */
static int16_t scale_stick(int code, int v) {
    int lo = ds4_rng[code].min, hi = ds4_rng[code].max;
    long span = (long)hi - lo;
    long out = ((long)(v - lo) * 65534L) / span - 32767L;
    if (out < -32767) out = -32767;
    if (out > 32767) out = 32767;
    return (int16_t)out;
}

/* grilletto: [min,max] -> [0, 255] */
static uint8_t scale_trigger(int code, int v) {
    int lo = ds4_rng[code].min, hi = ds4_rng[code].max;
    long out = ((long)(v - lo) * 255L) / ((long)hi - lo);
    if (out < 0) out = 0;
    if (out > 255) out = 255;
    return (uint8_t)out;
}

/*
 * hid-playstation/hid-sony: Quadrato = BTN_WEST (0x134), Triangolo = BTN_NORTH (0x133).
 * Sul pad Xbox virtuale 0x133 e' X (a sinistra) e 0x134 e' Y (in alto): li scambiamo,
 * cosi' i pulsanti corrispondono per posizione.
 */
static int handle_ds4(int fd, struct state *s) {
    struct input_event evs[64];
    int synced = 0;
    for (;;) {
        ssize_t n = read(fd, evs, sizeof(evs));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) break;
            return -1;
        }
        if (n == 0) return -1;
        size_t count = (size_t)n / sizeof(struct input_event);
        for (size_t i = 0; i < count; i++) {
            const struct input_event *e = &evs[i];
            if (e->type == EV_SYN && e->code == SYN_REPORT) {
                synced = 1;
            } else if (e->type == EV_KEY) {
                int v = e->value != 0;
                int code = e->code;
                if (ds4_generic_driver) {
                    code = ds4_generic_button(code);
                    if (code < 0) continue;
                }
                if (code == BTN_WEST) code = BTN_NORTH;
                else if (code == BTN_NORTH) code = BTN_WEST;
                set_button(s, code, v);
            } else if (e->type == EV_ABS) {
                int acode = e->code;
                if (ds4_generic_driver) {
                    /* Z/RZ = stick destro, RX/RY = grilletti */
                    if (acode == ABS_Z) acode = ABS_RX;
                    else if (acode == ABS_RZ) acode = ABS_RY;
                    else if (acode == ABS_RX) acode = ABS_Z;
                    else if (acode == ABS_RY) acode = ABS_RZ;
                }
                switch (acode) {
                case ABS_X:  s->lx = scale_stick(e->code, e->value); break;
                case ABS_Y:  s->ly = scale_stick(e->code, e->value); break;
                case ABS_RX: s->rx = scale_stick(e->code, e->value); break;
                case ABS_RY: s->ry = scale_stick(e->code, e->value); break;
                case ABS_Z:  s->lt = scale_trigger(e->code, e->value); break;
                case ABS_RZ: s->rt = scale_trigger(e->code, e->value); break;
                case ABS_HAT0X: s->left = e->value < 0; s->right = e->value > 0; break;
                case ABS_HAT0Y: s->up = e->value < 0; s->down = e->value > 0; break;
                }
            }
        }
    }
    return synced;
}

/* svuota i nodi extra (touchpad/sensori) bloccati: ritorna -1 se spariti */
static int drain(int fd) {
    struct input_event evs[64];
    for (;;) {
        ssize_t n = read(fd, evs, sizeof(evs));
        if (n < 0) return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
        if (n == 0) return -1;
    }
}

static int run_ds4(void) {
    char path[300] = "";
    int extra[MAX_EXTRA];
    int n_extra = 0;
    int fd = open_ds4(path, sizeof(path), extra, &n_extra);
    if (fd < 0) {
        fprintf(stderr, "DualShock 4 non trovato. Collegalo via Bluetooth (o USB) e riprova.\n");
        return 1;
    }
    char name[256] = "";
    ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
    printf("Controller: %s (%s)\n", name, path);
    ds4_detect_driver(fd, path);
    ds4_read_ranges(fd);

    int fu = open("/dev/uhid", O_RDWR | O_CLOEXEC);
    if (fu < 0) {
        perror("open /dev/uhid (serve la shell di Shizuku/adb)");
        return 1;
    }
    if (uhid_create(fu) < 0) return 1;

    if (ioctl(fd, EVIOCGRAB, 1) < 0) perror("EVIOCGRAB controller");
    for (int i = 0; i < n_extra; i++)
        if (ioctl(extra[i], EVIOCGRAB, 1) < 0) perror("EVIOCGRAB nodo extra");
    if (n_extra) printf("Bloccati anche %d nodi extra (touchpad/sensori).\n", n_extra);

    printf("DualShock 4 -> %s attivo. Ctrl+C per uscire.\n",
           xinput_mode ? "pad Xbox 360 (045E:028E)" : "gamepad generico");

    struct state s;
    memset(&s, 0, sizeof(s));
    s.analog_triggers = 1;
    int started = 0;
    long check_at = -1;      /* quando controllare gli assi visti da Android */
    int checked = 0;

    struct pollfd pfd[2 + MAX_EXTRA];
    pfd[0].fd = fd; pfd[0].events = POLLIN;
    pfd[1].fd = fu; pfd[1].events = POLLIN;
    for (int i = 0; i < n_extra; i++) { pfd[2 + i].fd = extra[i]; pfd[2 + i].events = POLLIN; }
    int nfds = 2 + n_extra;

    while (running) {
        int r = poll(pfd, (nfds_t)nfds, 300);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (pfd[1].revents & POLLIN) {
            struct uhid_event ev;
            if (read(fu, &ev, sizeof(ev)) > 0 && ev.type == UHID_START) {
                started = 1;
                if (!checked) check_at = now_ms() + 1500;
            }
        }
        if (!checked && check_at > 0 && now_ms() >= check_at) {
            checked = 1;
            if (xinput_mode && check_android_axes("Microsoft X-Box 360 pad") == 1) {
                printf("Manca il layout Xbox: ricreo il pad con stick destro su Z/RZ.\n");
                uhid_destroy(fu);
                xinput_alt = 1;
                started = 0;
                if (uhid_create(fu) < 0) break;
                check_at = -1;
            }
        }
        for (int i = 0; i < n_extra; i++) {
            if (pfd[2 + i].revents & (POLLERR | POLLHUP | POLLNVAL)) pfd[2 + i].fd = -1;
            else if ((pfd[2 + i].revents & POLLIN) && drain(pfd[2 + i].fd) < 0) pfd[2 + i].fd = -1;
        }
        if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "DualShock 4 disconnesso.\n");
            break;
        }
        if (pfd[0].revents & POLLIN) {
            int h = handle_ds4(fd, &s);
            if (h < 0) {
                fprintf(stderr, "DualShock 4 disconnesso.\n");
                break;
            }
            if (h > 0 && started && send_report(fu, &s) < 0) break;
        }
    }

    uhid_destroy(fu);
    close(fu);
    ioctl(fd, EVIOCGRAB, 0);
    close(fd);
    for (int i = 0; i < n_extra; i++) { ioctl(extra[i], EVIOCGRAB, 0); close(extra[i]); }
    printf("\nGamepad virtuale rimosso, DualShock 4 restituito ad Android.\n");
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--xinput") == 0) xinput_mode = 1;
        else if (strcmp(argv[i], "--ds4") == 0) ds4_mode = 1;
        else if (strcmp(argv[i], "--keep-touchpad") == 0) keep_touchpad = 1;
        else {
            fprintf(stderr, "Opzione sconosciuta: %s\nUso: %s [--ds4] [--keep-touchpad] [--xinput]\n", argv[i], argv[0]);
            return 2;
        }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);

    if (ds4_mode) return run_ds4();

    char pl[300] = "", pr[300] = "";
    int fl = open_by_name(NAME_L, pl, sizeof(pl));
    int fr = open_by_name(NAME_R, pr, sizeof(pr));
    if (fl < 0 || fr < 0) {
        fprintf(stderr, "Joy-Con non trovati:%s%s\n",
                fl < 0 ? " sinistro" : "", fr < 0 ? " destro" : "");
        fprintf(stderr, "Collega entrambi via Bluetooth e riprova.\n");
        return 1;
    }
    printf("Sinistro: %s\nDestro:   %s\n", pl, pr);

    int fu = open("/dev/uhid", O_RDWR | O_CLOEXEC);
    if (fu < 0) {
        perror("open /dev/uhid (serve la shell di Shizuku/adb)");
        return 1;
    }
    if (uhid_create(fu) < 0) return 1;

    if (ioctl(fl, EVIOCGRAB, 1) < 0) perror("EVIOCGRAB sinistro");
    if (ioctl(fr, EVIOCGRAB, 1) < 0) perror("EVIOCGRAB destro");

    if (xinput_mode)
        printf("Modalita' XInput: gamepad virtuale \"Xbox 360\" (045E:028E) attivo. Ctrl+C per uscire.\n");
    else
        printf("Gamepad virtuale \"Joy-Con Combined (merge)\" attivo. Ctrl+C per uscire.\n");

    struct state s;
    memset(&s, 0, sizeof(s));
    int started = 0;
    long check_at = -1;
    int checked = 0;

    struct pollfd pfd[3] = {
        { .fd = fl, .events = POLLIN },
        { .fd = fr, .events = POLLIN },
        { .fd = fu, .events = POLLIN },
    };

    while (running) {
        int r = poll(pfd, 3, 300);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (pfd[2].revents & POLLIN) {
            struct uhid_event ev;
            if (read(fu, &ev, sizeof(ev)) > 0 && ev.type == UHID_START) {
                started = 1;
                if (!checked) check_at = now_ms() + 1500;
            }
        }
        if (!checked && check_at > 0 && now_ms() >= check_at) {
            checked = 1;
            if (xinput_mode && check_android_axes("Microsoft X-Box 360 pad") == 1) {
                printf("Manca il layout Xbox: ricreo il pad con stick destro su Z/RZ.\n");
                uhid_destroy(fu);
                xinput_alt = 1;
                started = 0;
                if (uhid_create(fu) < 0) break;
            } else if (!xinput_mode) {
                check_android_axes("Joy-Con Combined (merge)");
            }
        }
        int changed = 0;
        for (int i = 0; i < 2; i++) {
            if (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                fprintf(stderr, "Joy-Con %s disconnesso.\n", i == 0 ? "sinistro" : "destro");
                running = 0;
                break;
            }
            if (pfd[i].revents & POLLIN) {
                int h = handle(pfd[i].fd, i == 0, &s);
                if (h < 0) {
                    fprintf(stderr, "Joy-Con %s disconnesso.\n", i == 0 ? "sinistro" : "destro");
                    running = 0;
                    break;
                }
                if (h > 0) changed = 1;
            }
        }
        if (running && changed && started) {
            if (send_report(fu, &s) < 0) break;
        }
    }

    uhid_destroy(fu);
    close(fu);
    ioctl(fl, EVIOCGRAB, 0);
    ioctl(fr, EVIOCGRAB, 0);
    close(fl);
    close(fr);
    printf("\nGamepad virtuale rimosso, Joy-Con restituiti ad Android.\n");
    return 0;
}
