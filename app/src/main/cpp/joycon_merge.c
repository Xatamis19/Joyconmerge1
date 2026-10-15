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
 * Va eseguito come utente shell (Shizuku/rish o adb shell).
 * Uso: ./joycon_merge        (Ctrl+C per uscire)
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
#include <unistd.h>

#define NAME_L "Joy-Con (L)"
#define NAME_R "Joy-Con (R)"

/* ID neutro (pid.codes): evita che hid-nintendo o altri driver si aggancino */
#define VENDOR_ID  0x1209
#define PRODUCT_ID 0x0001

/*
 * Descrittore HID:
 *  15 pulsanti (+1 bit padding) -> hid-input li mappa su BTN_SOUTH..BTN_THUMBR
 *  hat switch 4 bit + 4 bit padding
 *  X, Y, Z, Rz a 16 bit con segno
 *  Brake, Accelerator a 8 bit
 */
static const uint8_t rdesc[] = {
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

#pragma pack(push, 1)
struct report {
    uint16_t buttons;   /* bit n = BTN_SOUTH + n */
    uint8_t hat;        /* 0..7, 8 = centro */
    int16_t lx, ly, rx, ry;
    uint8_t brake, gas;
};
#pragma pack(pop)

struct state {
    uint16_t buttons;
    int up, down, left, right;
    int16_t lx, ly, rx, ry;
};

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
    snprintf((char *)ev.u.create2.name, sizeof(ev.u.create2.name), "Joy-Con Combined (merge)");
    snprintf((char *)ev.u.create2.phys, sizeof(ev.u.create2.phys), "joycon-merge/0");
    snprintf((char *)ev.u.create2.uniq, sizeof(ev.u.create2.uniq), "joycon-merge-0");
    ev.u.create2.rd_size = sizeof(rdesc);
    ev.u.create2.bus = BUS_USB;
    ev.u.create2.vendor = VENDOR_ID;
    ev.u.create2.product = PRODUCT_ID;
    ev.u.create2.version = 1;
    ev.u.create2.country = 0;
    memcpy(ev.u.create2.rd_data, rdesc, sizeof(rdesc));
    return uhid_write(fd, &ev);
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
    r.hat = hat_from(s);
    r.lx = s->lx; r.ly = s->ly; r.rx = s->rx; r.ry = s->ry;
    r.brake = (s->buttons & (1u << (BTN_TL2 - BTN_SOUTH))) ? 255 : 0;
    r.gas   = (s->buttons & (1u << (BTN_TR2 - BTN_SOUTH))) ? 255 : 0;

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

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);

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

    printf("Gamepad virtuale \"Joy-Con Combined (merge)\" attivo. Ctrl+C per uscire.\n");

    struct state s;
    memset(&s, 0, sizeof(s));
    int started = 0;

    struct pollfd pfd[3] = {
        { .fd = fl, .events = POLLIN },
        { .fd = fr, .events = POLLIN },
        { .fd = fu, .events = POLLIN },
    };

    while (running) {
        int r = poll(pfd, 3, 1000);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (pfd[2].revents & POLLIN) {
            struct uhid_event ev;
            if (read(fu, &ev, sizeof(ev)) > 0 && ev.type == UHID_START) started = 1;
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
