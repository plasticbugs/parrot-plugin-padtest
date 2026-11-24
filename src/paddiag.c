// paddiag -- what the cart's USB host sees of a game pad, as text on the TV
// (PADTEST.CVP). It runs sdk/cvpad exactly as the emulator plugins do, and
// shows what cvpad knows, so a pad that isn't read in a game can be read
// out here, line by line:
//
//    0  PAD TEST TINYUSB 0.21+     123S   which TinyUSB; seconds running
//    1  ATT 3 REM 1 EP 0 CH 2 0.05MS      attach / remove events from the
//                                         port and the hubs; answers from
//                                         the hubs' status endpoints;
//                                         changes the port polls found
//                                         (sdk/tinyusb hub.c); the task's
//                                         longest run
//    2  A1 057E:2009 1IF L41              the two newest devices: address,
//    3  0:030000 81I01O                   VID:PID, interfaces, the length of
//    4                                    its configuration; then each
//    5  A2 ...                            interface: number, class subclass
//    6                                    protocol, its endpoints (address,
//    7                                    I interrupt B bulk S isochronous)
//    8  P1 SWITCH 1.0 READY T1 R0         pad 1: driver, address.interface;
//                                         a Switch pad's step, tries at it,
//                                         times it started over
//    9  RPT 125/S GAP  9MS AGE  2MS       reports a second; the longest
//                                         gap between two in that second;
//                                         the newest one's age now
//   10  30:1234 21:5 81:3 ?:0             reports by kind (a HID pad: OK
//                                         read as its state, ? not)
//   11  ST 11111111 0.43S E0 F0 B0        the Switch start: tries at each
//                                         of its 8 steps (X: given up, .:
//                                         not reached), how long it took;
//                                         E empty reports, F receives
//                                         refused, B sends refused
//   12  40:30A1810000...                  the last report: its length, then
//                                         its first 14 bytes, all in hex
//   13  BTN A B UP ...                    what was read from it (no button:
//                                         the sticks)
//   14-19  P2 ...                         pad 2, the same
//   20-23  123.4 PAD1 1.0 D203 SWITCH     the log's last four lines, each
//                                         with its time
//
// Devices: tuh_mount_cb() (any device, whatever its class) asks for its
// configuration descriptor and lists the interfaces. All USB work runs in
// cvpad's timer (cvpad.h); the loop here only draws.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "tusb.h"
#include "host/hcd.h"

#include "cvkernel.h"
#include "cvpad.h"
#include "cvscreen.h"
#include "handoff.h"
#include "../../midihello/mailbox.h"
#include "rom_image.h"
#include "swpro.h"

#define UI_US   50000                   // the screen is brought up to date this often
#define ROWS    24
#define LOGN    4

// ---- the log (written from the timer's interrupt) -----------------------------------
static char logs[LOGN][33];
static volatile uint32_t log_n;

static void log_line(const char *s) {
    const uint32_t irq = save_and_disable_interrupts();
    const uint32_t ds = time_us_32() / 100000;
    char *l = logs[log_n % LOGN];
    snprintf(l, 33, "%3lu.%lu %s", (unsigned long)(ds / 10 % 1000), (unsigned long)(ds % 10), s);
    for (char *c = l; *c; c++)
        if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
    log_n = log_n + 1;
    restore_interrupts(irq);
}

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    char b[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    log_line(b);
}

// TinyUSB's own lines (CFG_TUSB_DEBUG 1: failed asserts and errors), a line
// at a time, a path cut to its file
int paddiag_printf(const char *fmt, ...) {
    static char cur[96];
    static unsigned n;
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k < 0) return k;
    if (k >= (int)sizeof tmp) k = sizeof tmp - 1;
    const uint32_t irq = save_and_disable_interrupts();
    for (int i = 0; i < k; i++) {
        char c = tmp[i];
        if (c == '\r') continue;
        if (c != '\n') {
            if (n < sizeof cur - 1) cur[n++] = c;
            continue;
        }
        cur[n] = 0;
        n = 0;
        const char *s = strrchr(cur, '/');
        if (cur[0]) log_line(s ? s + 1 : cur);
    }
    restore_interrupts(irq);
    return k;
}

// ---- the devices ------------------------------------------------------------------
typedef struct {
    uint8_t daddr;                      // 0: none
    uint16_t vid, pid, total;           // its configuration's wTotalLength
    uint8_t nif;
    bool have;                          // its interfaces are read
    char ifs[2][33];
} udev_t;
static udev_t devs[2];                   // [0] the newest
static uint8_t want_daddr;              // a configuration to ask for (the control pipe was busy)
static uint8_t cfg_buf[512] __attribute__((aligned(4)));
static volatile uint32_t n_attach, n_remove, n_hub_ep, n_hub_change;

static udev_t *dev_of(uint8_t daddr) {
    for (int i = 0; i < 2; i++)
        if (devs[i].daddr == daddr) return &devs[i];
    return NULL;
}

static void cfg_cb(tuh_xfer_t *x) {
    udev_t *d = dev_of((uint8_t)x->user_data);
    if (!d) return;
    if (x->result != XFER_RESULT_SUCCESS) {
        logf_("A%u CONFIGURATION: NO ANSWER", d->daddr);
        return;
    }
    const tusb_desc_configuration_t *c = (const tusb_desc_configuration_t *)cfg_buf;
    uint16_t total = tu_le16toh(c->wTotalLength);
    d->total = total;
    d->nif = c->bNumInterfaces;
    if (total > sizeof cfg_buf) total = sizeof cfg_buf;
    char *o = NULL;
    int line = -1, on_line = 0;
    for (const uint8_t *p = cfg_buf; p + 2 <= cfg_buf + total && p[0] >= 2 && p + p[0] <= cfg_buf + total; p += p[0]) {
        if (p[1] == TUSB_DESC_INTERFACE && p[0] >= 9 && p[3] == 0) {     // (alternate setting 0)
            if (line < 0 || on_line == 2 || (o && o - d->ifs[line] > 16)) {
                if (++line >= 2) break;
                o = d->ifs[line];
                on_line = 0;
            }
            if (on_line) *o++ = ' ';
            o += snprintf(o, (size_t)(d->ifs[line] + 33 - o), "%u:%02X%02X%02X", p[2], p[5], p[6], p[7]);
            on_line++;
        } else if (p[1] == TUSB_DESC_ENDPOINT && p[0] >= 7 && o && line >= 0 && line < 2) {
            static const char kind[] = "CSBI";
            o += snprintf(o, (size_t)(d->ifs[line] + 33 - o), " %02X%c", p[2], kind[p[3] & 3]);
        }
        if (o && o > d->ifs[line] + 32) o = d->ifs[line] + 32;
    }
    d->have = true;
}

static void ask_cfg(uint8_t daddr) {
    if (tuh_descriptor_get_configuration(daddr, 0, cfg_buf, sizeof cfg_buf, cfg_cb, daddr)) want_daddr = 0;
    else want_daddr = daddr;
}

// (in cvpad's timer, after TinyUSB's work)
static void tick(void) {
    if (want_daddr) ask_cfg(want_daddr);
}

void tuh_mount_cb(uint8_t daddr) {
    devs[1] = devs[0];
    memset(&devs[0], 0, sizeof devs[0]);
    devs[0].daddr = daddr;
    tuh_vid_pid_get(daddr, &devs[0].vid, &devs[0].pid);
    logf_("MOUNT A%u %04X:%04X", daddr, devs[0].vid, devs[0].pid);
    ask_cfg(daddr);
}

void tuh_umount_cb(uint8_t daddr) {
    logf_("UNMOUNT A%u", daddr);
    udev_t *d = dev_of(daddr);
    if (d) d->daddr = 0;
    if (want_daddr == daddr) want_daddr = 0;
}

// the hub's start (sdk/tinyusb hub.c): how many ports and the wait, then each port's status and change bits
// (status bit 0: a device is there; change bit 0: its connection changed)
void tuh_hub_scan_cb(uint8_t daddr, uint8_t port, uint16_t status, uint16_t change) {
    if (port == 0) logf_("HUB A%u %u PORTS, WAIT %uMS", daddr, status, change);
    else if (status == 0xFFFF && change == 0xFFFF) logf_("HUB A%u P%u NO ANSWER", daddr, port);
    else logf_("HUB A%u P%u %04X %04X%s", daddr, port, status, change, status & 1 ? " DEVICE" : "");
}

// what the hubs' status endpoints and port polls see (sdk/tinyusb hub.c says what each is)
void tuh_hub_diag_cb(uint8_t daddr, uint8_t what, uint32_t value) {
    switch (what) {
    case 1:
        if (n_hub_ep == 0) logf_("HUB A%u ENDPOINT ANSWERS %02X R%lu", daddr, (unsigned)(value & 0xFF),
                                 (unsigned long)(value >> 8));
        n_hub_ep = n_hub_ep + 1;
        break;
    case 2:
        n_hub_change = n_hub_change + 1;
        logf_("HUB A%u P%lu CHANGE %04lX", daddr, (unsigned long)(value >> 16), (unsigned long)(value & 0xFFFF));
        break;
    case 3: logf_("HUB A%u ENDPOINT NOT ARMED %lu", daddr, (unsigned long)value); break;
    case 4: logf_("HUB A%u CHANGE DROPPED", daddr); break;
    }
}

void tuh_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr) {
    (void)rhport;
    (void)in_isr;
    if (eventid == HCD_EVENT_DEVICE_ATTACH) n_attach = n_attach + 1;
    if (eventid == HCD_EVENT_DEVICE_REMOVE) n_remove = n_remove + 1;
}

// ---- the screen -------------------------------------------------------------------
static cvs_t scr;
static uint8_t list[CVS_LIST_MAX];
static char row[ROWS][33], shown[ROWS][33];
static uint8_t colour[ROWS], shown_colour[ROWS];

static void rowf(int r, uint8_t col, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void rowf(int r, uint8_t col, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(row[r], sizeof row[r], fmt, ap);
    va_end(ap);
    colour[r] = col;
}

static uint32_t rate[CVPAD_MAX], rate_last[CVPAD_MAX], gap_shown[CVPAD_MAX], rate_at;

static void pad_rows(int n, int r0) {
    cvpad_info_t i;
    if (!cvpad_info(n, &i)) {
        rowf(r0, C_GRAY, "P%d -", n + 1);
        for (int k = 1; k < 6; k++) rowf(r0 + k, C_GRAY, "%s", "");
        return;
    }
    const bool sw = i.driver == CVPAD_SWITCH;
    if (sw) {
        const bool ready = i.step == SWPRO_RUN;
        rowf(r0, ready ? C_LGREEN : C_LYELLOW, "P%d SWITCH %u.%u %.11s T%u R%lu", n + 1, i.daddr, i.idx,
             ready && !i.calibrated ? "READY NOCAL" : swpro_step_name(i.step), i.tries, (unsigned long)i.restarts);
    } else {
        rowf(r0, i.live ? C_LGREEN : C_LYELLOW, "P%d HID %u.%u %04X:%04X D%u%s", n + 1, i.daddr, i.idx, i.vid, i.pid,
             i.desc_len, i.live ? "" : " WAIT");
    }
    const uint32_t age = (i.now_us - i.last_us) / 1000;
    rowf(r0 + 1, C_WHITE, "RPT %3lu/S GAP %3luMS AGE %3luMS", (unsigned long)rate[n],
         (unsigned long)MIN(gap_shown[n] / 1000, 999u), (unsigned long)MIN(age, 999u));
    if (sw) {
        rowf(r0 + 2, C_WHITE, "30:%lu 21:%lu 81:%lu ?:%lu", (unsigned long)i.by_id[0], (unsigned long)i.by_id[1],
             (unsigned long)i.by_id[2], (unsigned long)i.by_id[3]);
        char st[9];
        for (int k = 0; k < 8; k++) {
            const uint8_t v = i.result[k];
            st[k] = v & SWPRO_SKIPPED ? 'X' : !v && k >= i.step ? '.' : (char)('0' + MIN(v & 0x7F, 9));
        }
        st[8] = 0;
        char took[12] = "-";              // the start's length, once it is done
        if (i.step == SWPRO_RUN)
            snprintf(took, sizeof took, "%lu.%02luS", (unsigned long)(i.start_us / 1000000),
                     (unsigned long)(i.start_us / 10000 % 100));
        rowf(r0 + 3, C_WHITE, "ST %s %s E%lu F%lu B%lu", st, took, (unsigned long)i.empty,
             (unsigned long)i.rearm_failed, (unsigned long)MIN(i.send_busy, 99999u));
    } else {
        rowf(r0 + 2, C_WHITE, "OK:%lu ?:%lu", (unsigned long)i.by_id[0], (unsigned long)i.by_id[3]);
        rowf(r0 + 3, C_WHITE, "E%lu F%lu", (unsigned long)i.empty, (unsigned long)i.rearm_failed);
    }
    char hex[33], *o = hex;
    o += sprintf(o, "%02X:", i.raw_len);
    for (int k = 0; k < 14 && k < i.raw_len; k++) o += sprintf(o, "%02X", i.raw[k]);
    rowf(r0 + 4, C_CYAN, "%s", i.raw_len ? hex : "NO REPORT YET");
    static const char *const name[GP_BUTTONS] = {"UP", "DN", "LT", "RT", "B", "A", "Y", "X", "L", "R", "ZL", "ZR",
                                                 "-", "+", "HOME", "CAP", "LS", "RS"};
    char b[64] = "BTN";
    o = b + 3;
    for (int k = 0; k < GP_BUTTONS; k++)
        if (i.pad.buttons >> k & 1) o += sprintf(o, " %s", name[k]);
    if (!i.live) rowf(r0 + 5, C_GRAY, "%s", "NO STATE READ YET");
    else if (i.pad.buttons) rowf(r0 + 5, C_LYELLOW, "%s", b);
    else rowf(r0 + 5, C_GRAY, "STK %5d %5d %5d %5d", i.pad.lx, i.pad.ly, i.pad.rx, i.pad.ry);
}

static void rows(uint32_t t) {
    if (t - rate_at >= 1000000) {
        rate_at = t;
        for (int n = 0; n < CVPAD_MAX; n++) {
            cvpad_info_t i;
            if (cvpad_info(n, &i)) {
                rate[n] = i.reports - rate_last[n];
                rate_last[n] = i.reports;
                gap_shown[n] = i.gap_max_us;
                cvpad_info_lap(n);
            } else {
                rate[n] = rate_last[n] = gap_shown[n] = 0;
            }
        }
    }
    const uint32_t us = cvpad_task_max_us();
    rowf(0, C_LRED, "PAD TEST TINYUSB %-6s %6luS", PADDIAG_TUSB, (unsigned long)(t / 1000000));
    rowf(1, C_WHITE, "ATT %lu REM %lu EP %lu CH %lu %lu.%02luMS", (unsigned long)n_attach, (unsigned long)n_remove,
         (unsigned long)n_hub_ep, (unsigned long)n_hub_change, (unsigned long)(us / 1000), (unsigned long)(us / 10 % 100));
    for (int k = 0; k < 2; k++) {
        const uint32_t irq = save_and_disable_interrupts();
        const udev_t d = devs[k];
        restore_interrupts(irq);
        const int r = 2 + 3 * k;
        if (!d.daddr) {
            rowf(r, C_GRAY, "%s", k ? "" : "NO DEVICE BUT HUBS");
            rowf(r + 1, C_GRAY, "%s", "");
            rowf(r + 2, C_GRAY, "%s", "");
            continue;
        }
        if (d.have) rowf(r, C_LBLUE, "A%u %04X:%04X %uIF L%u", d.daddr, d.vid, d.pid, d.nif, d.total);
        else rowf(r, C_LBLUE, "A%u %04X:%04X", d.daddr, d.vid, d.pid);
        rowf(r + 1, C_LBLUE, "%s", d.ifs[0]);
        rowf(r + 2, C_LBLUE, "%s", d.ifs[1]);
    }
    pad_rows(0, 8);
    pad_rows(1, 14);
    const uint32_t irq = save_and_disable_interrupts();
    const uint32_t n = log_n;
    for (int k = 0; k < LOGN; k++) {
        const uint32_t at = n + (uint32_t)k;            // oldest first
        const char *l = n >= LOGN ? logs[at % LOGN] : (uint32_t)k < n ? logs[k] : "";
        snprintf(row[20 + k], sizeof row[0], "%s", l);
        colour[20 + k] = C_GRAY;
    }
    restore_interrupts(irq);
}

static void draw(canvas_t *c, bool all, void *ctx) {
    (void)ctx;
    for (int r = 0; r < ROWS; r++) {
        if (!all && colour[r] == shown_colour[r] && !strcmp(row[r], shown[r])) continue;
        cvs_line(c, 0, r * 8, VDP_W, row[r], colour[r] ? colour[r] : C_GRAY, C_BLACK, 1);
        memcpy(shown[r], row[r], sizeof shown[r]);
        shown_colour[r] = colour[r];
    }
}

// one pass of the loop: the console (power, RESET, the kernel's reports), the screen
static void pass(bool usb) {
    static uint32_t t_ui;
    const uint32_t t = time_us_32();
    cvk_task(t);
    if (usb) cvpad_task();              // (TinyUSB 0.18: no timer)
    int k;
    uint8_t bits;
    while (cvk_report(t, &k, &bits))
        if (k == REPORT_KIND_ACK || k == REPORT_KIND_HELLO) cvs_report(&scr, k == REPORT_KIND_HELLO, bits & 31, t);
    if (t - t_ui >= UI_US) {
        t_ui = t;
        rows(t);
        const size_t n = cvs_update(&scr, draw, NULL, list, sizeof list, t);
        if (n) cvk_list(list, n, scr.out_full, scr.seq);
    }
}

// panic() (CMakeLists.txt: PICO_PANIC_FUNCTION), TinyUSB's host driver's
// among them: its message as the log's last line, and the screen kept up
// from here (wherever here is: an interrupt too), USB stopped. RESET still
// goes back to the menu.
void __attribute__((noreturn)) paddiag_panic(const char *fmt, ...) {
    char b[64] = "PANIC ";
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b + 6, sizeof b - 6, fmt, ap);
    va_end(ap);
    for (char *c = b; *c; c++)
        if (*c == '\n') *c = ' ';
    save_and_disable_interrupts();
    log_line(b);
    cvk_note("Pad Test: panic");
    for (;;) pass(false);
}

int main(void) {
    static const cvk_config_t cfg = {rom_image, addr_line, data_bit, handoff_bin, HANDOFF_BIN_LEN};
    cvk_start(&cfg);
    cvs_init(&scr);
    cvk_note("Pad Test");
    cvpad_set_log(log_line);
    cvpad_set_tick(tick);
    log_line("RESET: BACK TO THE MENU");
    cvpad_init();
    for (;;) pass(true);
}
