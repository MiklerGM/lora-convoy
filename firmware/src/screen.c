#include "screen.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "codec.h"

extern const uint8_t screen_font[95][20];
extern const uint8_t screen_font_large[95][45];

/* The app's short words for the states (Rider.kt, STATE_TAGS); none while riding. */
static const char *tag(uint8_t state) {
    static const char *const TAG[] = {"", "SOS", "STOP", "BREAK", "HELPING"};
    return state < sizeof(TAG) / sizeof(TAG[0]) ? TAG[state] : "?";
}

/* "now" under a minute, then whole minutes, then hours: coarse, so an e-paper is not redrawn by it. */
static void age(char *out, size_t cap, int64_t us) {
    const long long s = us / 1000000;
    if (s < 60) snprintf(out, cap, "now");
    else if (s < 3600) snprintf(out, cap, "%lldm", s / 60);
    else snprintf(out, cap, "%lldh", s / 3600);
}

/* A line, cut at the screen's width as the screen would cut it, and its trailing blanks. */
static void put(char *line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, SCREEN_COLS + 1, fmt, ap);
    va_end(ap);
    for (size_t k = strlen(line); k && line[k - 1] == ' '; k--) line[k - 1] = '\0';
}

int screen_battery_pct(int mv) {
    /* a lithium cell's resting voltage at 100%, 90%, ... 0%: Meshtastic's OCV_ARRAY (Power.h) */
    static const int OCV[] = {4190, 4050, 3990, 3890, 3800, 3720, 3630, 3530, 3420, 3300, 3100};
    if (mv >= OCV[0]) return 100;
    for (int i = 1; i < 11; i++) {
        if (mv >= OCV[i]) return 100 - 10 * i + 10 * (mv - OCV[i]) / (OCV[i - 1] - OCV[i]);
    }
    return 0;
}

void screen_text(const node_t *n, int64_t now_us, bool phone, int battery_mv, bool usb, int zone_min, screen_text_t out) {
    memset(out, 0, sizeof(screen_text_t));
    char clock[16] = "--:--", battery[16] = "";
    int64_t utc, err;
    if (node_utc_us(n, now_us, &utc, &err)) {
        const long long m = ((utc / 60000000 + zone_min) % 1440 + 1440) % 1440;
        snprintf(clock, sizeof(clock), "%02lld:%02lld", m / 60, m % 60);
    }
    /* Under 2.5 V there is no cell: a Heltec without one reads its divider's noise. Over 4.4 V, more
     * than a cell gives, the divider sees USB, not the cell (the T-Echo's, through a diode, 4.74 V),
     * so there is no charge to tell. On USB a cell under full is marked charging; at full the mark
     * goes, which is all its voltage says: no charger on these boards tells the chip it is done. */
    if (battery_mv > SCREEN_USB_MV || (usb && battery_mv < 2500)) {
        snprintf(battery, sizeof(battery), "USB");
    } else if (battery_mv >= 2500) {
        const int pct = screen_battery_pct(battery_mv);
        if (usb && pct < 100) snprintf(battery, sizeof(battery), "%c%d%%", SCREEN_CHARGING, pct);
        else snprintf(battery, sizeof(battery), "%d%%", pct);
    }
    put(out[0], "%s %-2s%4s", clock, phone ? "BT" : "", battery);
    if (n->cfg.keyed) {
        const char *t = tag(n->state);
        put(out[1], "R%u/%u%s%s", n->cfg.slot + 1u, n->cfg.slots, *t ? " " : "", t);
    } else {
        put(out[1], "no group");
    }
    const bool fix = n->fix.valid && now_us - n->fix_us <= FIX_FRESH_US;
    char sats[16] = "nofix";
    if (fix) snprintf(sats, sizeof(sats), "%usat", n->fix.sats);
    put(out[2], "%s %s", n->cfg.keyed && node_time_fits(n, now_us) ? "on air" : "silent", sats);

    int near = 0;
    int alarm = -1;
    for (unsigned i = 0; i < NODE_SLOTS; i++) {
        const rider_t *r = &n->riders[i];
        if (i == n->cfg.slot || !r->known) continue;
        if (now_us - r->rec_us < 60 * 1000000ll) near++;
        /* a call for help first, then a stop, each the lowest slot's */
        const uint8_t s = r->rec.state;
        if ((s == STATE_HELP && (alarm < 0 || n->riders[alarm].rec.state != STATE_HELP)) ||
            (s == STATE_STOP_NOW && alarm < 0)) {
            alarm = (int)i;
        }
    }
    if (alarm >= 0) {
        put(out[3], "R%d %s", alarm + 1, tag(n->riders[alarm].rec.state));
    } else {
        put(out[3], "riders %d/%u", near, n->cfg.slots ? n->cfg.slots - 1u : 0u);
    }

    int line = 4;
    for (unsigned i = 0; i < NODE_SLOTS && line < SCREEN_LINES; i++) {
        const rider_t *r = &n->riders[i];
        if (i == n->cfg.slot || !r->known) continue;
        char ago[16], via[16] = "";
        age(ago, sizeof(ago), now_us - r->rec_us);
        if (r->via != i) snprintf(via, sizeof(via), " via R%u", r->via + 1u);
        const char *t = tag(r->rec.state);
        put(out[line++], "R%u %s%s%s%s", i + 1u, ago, *t ? " " : "", t, via);
    }
}

/* The charging mark, ours, in the fonts' layout and height: a bolt over the digits' rows. */
const uint8_t screen_bolt[20] = {0x00, 0x00, 0x00, 0x00, 0x40, 0x1c, 0x60, 0x0e, 0x70, 0x07,
                                 0xf8, 0x03, 0xdc, 0x01, 0xcc, 0x00, 0x44, 0x00, 0x00, 0x00};
const uint8_t screen_bolt_large[45] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06,
                                       0x00, 0x07, 0x03, 0x80, 0x87, 0x01, 0xc0, 0xc7, 0x01, 0xe0, 0xe6, 0x00,
                                       0x70, 0x76, 0x00, 0x38, 0x3e, 0x00, 0x18, 0x1e, 0x00, 0x08, 0x0e, 0x00,
                                       0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

void screen_draw(const screen_text_t text, uint8_t *buf, int w, int h) {
    memset(buf, 0, (size_t)(w * h / 8));
    /* 10 x 16 on the OLEDs, 4 lines of 12; 15 x 24 on the e-paper, 8 lines of 13 */
    const bool large = w >= 200;
    const uint8_t *font = large ? &screen_font_large[0][0] : &screen_font[0][0];
    const int fw = large ? 15 : SCREEN_FONT_W, bands = large ? 3 : SCREEN_FONT_H / 8, per = fw * bands;
    const int rows = h / (bands * 8), cols = w / fw;
    for (int row = 0; row < rows && row < SCREEN_LINES; row++) {
        for (int col = 0; col < cols && col < SCREEN_COLS && text[row][col]; col++) {
            const unsigned char c = (unsigned char)text[row][col];
            const uint8_t *g = c == (unsigned char)SCREEN_CHARGING ? (large ? screen_bolt_large : screen_bolt)
                               : font + (c >= ' ' && c <= '~' ? c - ' ' : '?' - ' ') * per;
            for (int x = 0; x < fw; x++) {
                for (int b = 0; b < bands; b++) buf[(row * bands + b) * w + col * fw + x] = g[x * bands + b];
            }
        }
    }
}
