/*
 * Game Boy (DMG) emulator for NM-CYD-C5 (ESP32-C5)
 * Core: Peanut-GB (fetched at build time). Control + optional video over USB Serial/JTAG (Web Serial).
 *
 * STATUS: written without being able to compile or run it (no toolchain in the authoring environment).
 *         Expect to fix small compile/runtime issues on first build.
 * NOT implemented: sound, Game Boy Color mode (CGB-only games are listed but refused).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "driver/usb_serial_jtag.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "board.h"
#include "font5x7.h"

#define ENABLE_SOUND 0
#define ENABLE_LCD   1
#define PEANUT_GB_HIGH_LCD_ACCURACY 0
#include "peanut_gb.h"

static const char *TAG = "gbemu";

#define GB_W 160
#define GB_H 144
#define OUT_W GAME_OUT_W
#define OUT_H GAME_OUT_H
#define GB_X0 ((LCD_W - OUT_W) / 2)
#define GB_Y0 0
#define SCALE_ROWS 24
#define FRAME_US 16742   /* 59.73 Hz */

/* ---------- colours (pre-swapped RGB565 for the SPI wire) ---------- */
#define C565(r, g, b) ((uint16_t)(((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))))
#define SW(c) ((uint16_t)(((c) >> 8) | ((c) << 8)))
static uint16_t pal565[4];
static uint16_t COL_BG, COL_FG, COL_DIM, COL_SEL;

/* ---------- display ---------- */
static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t lcd_idle;      /* given when the last color transfer finished */
static uint16_t *fb[2];                 /* DMA-capable, internal RAM */
static uint16_t *strip;                 /* LCD_W x 16 scratch for text/fills */
#define STRIP_ROWS 16

static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ed, void *ctx)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(lcd_idle, &hp);
    return hp == pdTRUE;
}

static void lcd_wait_idle(void)
{
    xSemaphoreTake(lcd_idle, portMAX_DELAY);
    xSemaphoreGive(lcd_idle);
}

static void lcd_draw_async(int x0, int y0, int w, int h, const uint16_t *buf)
{
    xSemaphoreTake(lcd_idle, portMAX_DELAY);   /* previous transfer done */
    esp_lcd_panel_draw_bitmap(panel, x0, y0, x0 + w, y0 + h, buf);
}

static void lcd_draw_wait(int x0, int y0, int w, int h, const uint16_t *buf)
{
    lcd_draw_async(x0, y0, w, h, buf);
    lcd_wait_idle();
}

static void fill_rect(int x, int y, int w, int h, uint16_t col)
{
    for (int i = 0; i < w * STRIP_ROWS && i < LCD_W * STRIP_ROWS; i++) strip[i] = col;
    for (int row = 0; row < h; row += STRIP_ROWS) {
        int hh = (h - row < STRIP_ROWS) ? (h - row) : STRIP_ROWS;
        lcd_draw_wait(x, y + row, w, hh, strip);
    }
}

static const uint8_t *glyph(char c)
{
    c = (char)toupper((unsigned char)c);
    for (size_t i = 0; i < sizeof(FONT) / sizeof(FONT[0]); i++)
        if (FONT[i].c == c) return FONT[i].d;
    return FONT[0].d;
}

static void draw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int sc)
{
    int n = (int)strlen(s);
    int maxn = (LCD_W - x) / (6 * sc);
    if (n > maxn) n = maxn;
    if (n <= 0) return;
    int w = n * 6 * sc, h = 7 * sc;
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            int ch = c / (6 * sc), col = (c / sc) % 6;
            uint16_t p = bg;
            if (col < 5 && ((glyph(s[ch])[col] >> (r / sc)) & 1)) p = fg;
            strip[r * w + c] = p;
        }
    }
    lcd_draw_wait(x, y, w, h, strip);
}

/* nearest-neighbour scaling of the 160x144 frame to OUT_W x OUT_H, in strips so little RAM is needed */
static uint16_t *sbuf[2];
static uint16_t xmap[OUT_W];

static void draw_game(const uint16_t *src)
{
    int b = 0;
    for (int y0 = 0; y0 < OUT_H; y0 += SCALE_ROWS) {
        int rows = (OUT_H - y0 < SCALE_ROWS) ? (OUT_H - y0) : SCALE_ROWS;
        uint16_t *d = sbuf[b];
        for (int r = 0; r < rows; r++) {
            const uint16_t *row = src + ((y0 + r) * GB_H / OUT_H) * GB_W;
            for (int x = 0; x < OUT_W; x++) *d++ = row[xmap[x]];
        }
        lcd_draw_async(GB_X0, GB_Y0 + y0, OUT_W, rows, sbuf[b]);
        b ^= 1;
    }
}

static void display_init(void)
{
    lcd_idle = xSemaphoreCreateBinary();
    xSemaphoreGive(lcd_idle);

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 4,
        .on_color_trans_done = on_color_done,
        .user_ctx = NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST_ID, &io_cfg, &io));

    esp_lcd_panel_dev_config_t pc = {
        .reset_gpio_num = -1,   /* panel reset is tied to chip reset on this board */
        .rgb_ele_order = LCD_BGR ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &pc, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, LCD_INVERT));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, LCD_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, LCD_MIRROR_X, LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    gpio_config_t bl = { .pin_bit_mask = 1ULL << PIN_LCD_BL, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&bl);
    gpio_set_level(PIN_LCD_BL, 1);

    fb[0] = heap_caps_malloc(GB_W * GB_H * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    configASSERT(fb[0]);
    for (int i = 0; i < 2; i++) {
        sbuf[i] = heap_caps_malloc(OUT_W * SCALE_ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        configASSERT(sbuf[i]);
    }
    for (int x = 0; x < OUT_W; x++) xmap[x] = (uint16_t)(x * GB_W / OUT_W);
    strip = heap_caps_malloc(LCD_W * STRIP_ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    configASSERT(strip);

    COL_BG  = SW(C565(0, 0, 0));
    COL_FG  = SW(C565(255, 255, 255));
    COL_DIM = SW(C565(140, 140, 140));
    COL_SEL = SW(C565(155, 188, 15));
    /* classic DMG green shades, lightest to darkest */
    pal565[0] = SW(C565(155, 188, 15));
    pal565[1] = SW(C565(139, 172, 15));
    pal565[2] = SW(C565(48, 98, 48));
    pal565[3] = SW(C565(15, 56, 15));
}

/* ---------- SD card ---------- */
static bool sd_ok;

static void ensure_dir(const char *p) { if (mkdir(p, 0777) != 0 && errno != EEXIST) ESP_LOGW(TAG, "mkdir %s failed", p); }

static void sd_init(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = LCD_HOST_ID;
    host.max_freq_khz = 20000;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = LCD_HOST_ID;
    esp_vfs_fat_mount_config_t mc = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    esp_err_t e = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mc, &card);
    if (e != ESP_OK) { ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(e)); sd_ok = false; return; }
    sd_ok = true;
    ensure_dir("/sdcard/roms"); ensure_dir("/sdcard/roms/gb"); ensure_dir("/sdcard/roms/gbc");
    ensure_dir("/sdcard/saves"); ensure_dir("/sdcard/saves/gb"); ensure_dir("/sdcard/saves/gbc");
    ensure_dir("/sdcard/states");
}

/* ---------- ROM list ---------- */
#define MAX_ROMS 96
typedef struct { char name[64]; char path[160]; uint8_t gbc_dir; } rom_t;
static rom_t roms[MAX_ROMS];
static int n_roms;

static bool has_ext(const char *n, const char *ext)
{
    size_t a = strlen(n), b = strlen(ext);
    return a > b && strcasecmp(n + a - b, ext) == 0;
}

static void scan_dir(const char *dir, uint8_t gbc)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && n_roms < MAX_ROMS) {
        if (!has_ext(de->d_name, ".gb") && !has_ext(de->d_name, ".gbc")) continue;
        rom_t *r = &roms[n_roms++];
        snprintf(r->path, sizeof r->path, "%s/%s", dir, de->d_name);
        snprintf(r->name, sizeof r->name, "%s", de->d_name);
        char *dot = strrchr(r->name, '.');
        if (dot) *dot = 0;
        r->gbc_dir = gbc;
    }
    closedir(d);
}

static void scan_roms(void)
{
    n_roms = 0;
    if (!sd_ok) return;
    scan_dir("/sdcard/roms/gb", 0);
    scan_dir("/sdcard/roms/gbc", 1);
}

/* ---------- emulator state ---------- */
static struct gb_s gb;
static uint8_t *rom_data;
static size_t rom_size;
static uint8_t *cart_ram;
static size_t save_size;
static bool save_dirty;
static int cur_rom = -1;
static char save_path[200], state_path[200];
static volatile bool gb_failed;

static uint8_t gb_rom_read(struct gb_s *g, const uint_fast32_t addr) { return addr < rom_size ? rom_data[addr] : 0xFF; }
static uint8_t gb_cart_ram_read(struct gb_s *g, const uint_fast32_t addr) { return cart_ram[addr]; }
static void gb_cart_ram_write(struct gb_s *g, const uint_fast32_t addr, const uint8_t v) { cart_ram[addr] = v; save_dirty = true; }
static void gb_error_cb(struct gb_s *g, const enum gb_error_e err, const uint16_t addr)
{
    ESP_LOGE(TAG, "emulator error %d at %04X", (int)err, addr);
    gb_failed = true;
}

/* video state used by the line callback */
static uint16_t *cur_fb;
static uint8_t pack_buf[GB_W * GB_H / 4];
static bool skip_render, want_stream;

static void lcd_draw_line(struct gb_s *g, const uint8_t pixels[160], const uint_fast8_t line)
{
    if (skip_render) return;
    uint16_t *dst = cur_fb + line * GB_W;
    for (int x = 0; x < GB_W; x++) dst[x] = pal565[pixels[x] & 3];
    if (want_stream) {
        uint8_t *p = pack_buf + line * (GB_W / 4);
        for (int x = 0; x < GB_W; x += 4)
            *p++ = (pixels[x] & 3) | ((pixels[x + 1] & 3) << 2) | ((pixels[x + 2] & 3) << 4) | ((pixels[x + 3] & 3) << 6);
    }
}

static void make_paths(int idx)
{
    char base[64];
    snprintf(base, sizeof base, "%s", roms[idx].name);
    const char *sub = roms[idx].gbc_dir ? "gbc" : "gb";
    snprintf(save_path, sizeof save_path, "/sdcard/saves/%s/%s.sav", sub, base);
    snprintf(state_path, sizeof state_path, "/sdcard/states/%s.st", base);
}

static void sram_flush(void)
{
    if (!save_dirty || !cart_ram || !save_size) return;
    lcd_wait_idle();   /* SD and LCD share one SPI bus */
    FILE *f = fopen(save_path, "wb");
    if (f) { fwrite(cart_ram, 1, save_size, f); fclose(f); save_dirty = false; }
    else ESP_LOGE(TAG, "cannot write %s", save_path);
}

static bool state_save(void)
{
    lcd_wait_idle();
    FILE *f = fopen(state_path, "wb");
    if (!f) return false;
    fwrite(&gb, sizeof gb, 1, f);
    if (save_size) fwrite(cart_ram, 1, save_size, f);
    fclose(f);
    return true;
}

static bool state_load(void)
{
    lcd_wait_idle();
    FILE *f = fopen(state_path, "rb");
    if (!f) return false;
    struct gb_s keep = gb;                 /* keep our function pointers */
    bool ok = fread(&gb, sizeof gb, 1, f) == 1;
    if (ok && save_size) ok = fread(cart_ram, 1, save_size, f) == save_size;
    fclose(f);
    gb.gb_rom_read = keep.gb_rom_read;
    gb.gb_cart_ram_read = keep.gb_cart_ram_read;
    gb.gb_cart_ram_write = keep.gb_cart_ram_write;
    gb.gb_error = keep.gb_error;
    gb.gb_serial_tx = keep.gb_serial_tx;
    gb.gb_serial_rx = keep.gb_serial_rx;
    gb.display.lcd_draw_line = keep.display.lcd_draw_line;
    gb.direct.priv = keep.direct.priv;
    return ok;
}

static void unload_rom(void)
{
    sram_flush();
    free(cart_ram); cart_ram = NULL; save_size = 0;
    if (rom_data) { heap_caps_free(rom_data); rom_data = NULL; }
    rom_size = 0; cur_rom = -1;
}

static const char *load_rom(int idx)
{
    if (idx < 0 || idx >= n_roms) return "BAD INDEX";
    unload_rom();
    lcd_wait_idle();
    FILE *f = fopen(roms[idx].path, "rb");
    if (!f) return "OPEN FAILED";
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0x150) { fclose(f); return "TOO SMALL"; }
    rom_data = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rom_data) { fclose(f); return "NO PSRAM"; }
    uint8_t *tmp = heap_caps_malloc(16 * 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    size_t got = 0;
    while (got < (size_t)sz) {
        size_t n = fread(tmp, 1, 16 * 1024, f);
        if (n == 0) break;
        memcpy(rom_data + got, tmp, n);
        got += n;
    }
    free(tmp);
    fclose(f);
    if (got != (size_t)sz) { unload_rom(); return "READ FAILED"; }
    rom_size = sz;

    if (rom_data[0x143] == 0xC0) { unload_rom(); return "GBC ONLY - NOT SUPPORTED"; }

    enum gb_init_error_e e = gb_init(&gb, gb_rom_read, gb_cart_ram_read, gb_cart_ram_write, gb_error_cb, NULL);
    if (e != GB_INIT_NO_ERROR) { unload_rom(); return "INIT FAILED"; }
    gb_init_lcd(&gb, lcd_draw_line);

    save_size = gb_get_save_size(&gb);
    cart_ram = calloc(1, save_size ? save_size : 1);
    make_paths(idx);
    if (save_size) {
        FILE *s = fopen(save_path, "rb");
        if (s) { size_t r = fread(cart_ram, 1, save_size, s); (void)r; fclose(s); }
    }
    save_dirty = false;
    gb_failed = false;
    cur_rom = idx;
    return NULL;
}

/* ---------- USB link (Web Serial) ---------- */
#define SYNC0 0xA5
#define SYNC1 0x5A
enum { M_LIST = 0x10, M_STATUS = 0x11, M_VIDEO = 0x12 };
enum { C_JOY = 1, C_MENU, C_RESET, C_SAVESTATE, C_LOADSTATE, C_LIST, C_LOAD, C_VIDEO, C_HELLO };

static bool host_seen, stream_on = true;
static uint8_t joy_host;                 /* active-high: A,B,SEL,START,R,L,U,D = bits 0..7 */
static volatile int pend_load = -1;
static volatile bool pend_menu, pend_reset, pend_ss, pend_ls, pend_list;

static uint8_t txbuf[5 + GB_W * GB_H / 4 + 8];

static void send_msg(uint8_t type, const void *payload, uint16_t len)
{
    if (!host_seen || len > sizeof(txbuf) - 5) return;
    txbuf[0] = SYNC0; txbuf[1] = SYNC1; txbuf[2] = type; txbuf[3] = len & 0xFF; txbuf[4] = len >> 8;
    memcpy(txbuf + 5, payload, len);
    usb_serial_jtag_write_bytes(txbuf, 5 + len, 0);
}

static void send_status(const char *s) { send_msg(M_STATUS, s, (uint16_t)strlen(s)); }

static void send_list(void)
{
    static char buf[4096];
    size_t o = 0;
    for (int i = 0; i < n_roms && o < sizeof buf - 100; i++)
        o += snprintf(buf + o, sizeof buf - o, "%d|%s|%s\n", i, roms[i].name, roms[i].gbc_dir ? "GBC" : "GB");
    send_msg(M_LIST, buf, (uint16_t)o);
}

static void poll_serial(void)
{
    static uint8_t st, cmd;
    uint8_t b[64];
    int n;
    while ((n = usb_serial_jtag_read_bytes(b, sizeof b, 0)) > 0) {
        for (int i = 0; i < n; i++) {
            if (st == 0) { if (b[i] == SYNC0) st = 1; }
            else if (st == 1) { cmd = b[i]; st = 2; }
            else {
                st = 0;
                host_seen = true;
                switch (cmd) {
                case C_JOY: joy_host = b[i]; break;
                case C_MENU: pend_menu = true; break;
                case C_RESET: pend_reset = true; break;
                case C_SAVESTATE: pend_ss = true; break;
                case C_LOADSTATE: pend_ls = true; break;
                case C_LIST: pend_list = true; break;
                case C_LOAD: pend_load = b[i]; break;
                case C_VIDEO: stream_on = b[i] != 0; break;
                default: break;
                }
            }
        }
    }
}

static void usb_init(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 12288;
    cfg.rx_buffer_size = 512;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
}

/* ---------- touch (XPT2046, resistive, ONE touch point only) ---------- */
typedef struct { int16_t swap, x0, x1, y0, y1; } cal_t;
static cal_t cal;
static spi_device_handle_t touch_dev;
static bool touch_ok;
#define CAL_FILE "/sdcard/touch.cal"
#define CAL_MARGIN 15

static int xpt_read(uint8_t cmd)
{
    spi_transaction_t t = { .length = 24, .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA };
    t.tx_data[0] = cmd; t.tx_data[1] = 0; t.tx_data[2] = 0;
    spi_device_polling_transmit(touch_dev, &t);
    return (((t.rx_data[1] << 8) | t.rx_data[2]) >> 3) & 0xFFF;
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static bool touch_raw(int *rx, int *ry)
{
    if (!touch_dev || gpio_get_level(PIN_TOUCH_IRQ)) return false;   /* IRQ low = touched */
    int xs[5], ys[5];
    for (int i = 0; i < 5; i++) { xs[i] = xpt_read(0xD0); ys[i] = xpt_read(0x90); }
    qsort(xs, 5, sizeof(int), cmp_int);
    qsort(ys, 5, sizeof(int), cmp_int);
    *rx = xs[2]; *ry = ys[2];
    return *rx > 50 && *rx < 4050 && *ry > 50 && *ry < 4050;
}

static bool touch_screen(int *sx, int *sy)
{
    int rx, ry;
    if (!touch_ok || !touch_raw(&rx, &ry)) return false;
    int ax = cal.swap ? ry : rx, ay = cal.swap ? rx : ry;
    int dx = cal.x1 - cal.x0, dy = cal.y1 - cal.y0;
    if (dx == 0 || dy == 0) return false;
    int x = CAL_MARGIN + (ax - cal.x0) * (LCD_W - 2 * CAL_MARGIN) / dx;
    int y = CAL_MARGIN + (ay - cal.y0) * (LCD_H - 2 * CAL_MARGIN) / dy;
    *sx = x < 0 ? 0 : (x >= LCD_W ? LCD_W - 1 : x);
    *sy = y < 0 ? 0 : (y >= LCD_H ? LCD_H - 1 : y);
    return true;
}

/* waits for a press (returns false after timeout), then for release */
static bool cal_wait_tap(int *rx, int *ry)
{
    int waited = 0;
    int a, b;
    while (touch_raw(&a, &b)) vTaskDelay(pdMS_TO_TICKS(20));          /* wait for release first */
    for (;;) {
        if (touch_raw(&a, &b)) {
            vTaskDelay(pdMS_TO_TICKS(120));                           /* let it settle */
            if (touch_raw(rx, ry)) break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        if (++waited > 1500) return false;                            /* 30 s */
    }
    while (touch_raw(&a, &b)) vTaskDelay(pdMS_TO_TICKS(20));
    return true;
}

static bool touch_calibrate(void)
{
    const int px[3] = { CAL_MARGIN, LCD_W - CAL_MARGIN, LCD_W - CAL_MARGIN };
    const int py[3] = { CAL_MARGIN, CAL_MARGIN, LCD_H - CAL_MARGIN };
    int rx[3], ry[3];
    for (int i = 0; i < 3; i++) {
        fill_rect(0, 0, LCD_W, LCD_H, COL_BG);
        draw_text(40, 100, "TOUCH SETUP", COL_SEL, COL_BG, 2);
        draw_text(40, 130, "TAP THE CROSS", COL_FG, COL_BG, 2);
        fill_rect(px[i] - 8, py[i] - 1, 17, 3, COL_FG);
        fill_rect(px[i] - 1, py[i] - 8, 3, 17, COL_FG);
        if (!cal_wait_tap(&rx[i], &ry[i])) return false;
    }
    cal.swap = abs(ry[1] - ry[0]) > abs(rx[1] - rx[0]);
    if (!cal.swap) { cal.x0 = rx[0]; cal.x1 = rx[1]; cal.y0 = ry[0]; cal.y1 = ry[2]; }
    else           { cal.x0 = ry[0]; cal.x1 = ry[1]; cal.y0 = rx[0]; cal.y1 = rx[2]; }
    lcd_wait_idle();
    FILE *f = fopen(CAL_FILE, "wb");
    if (f) { fwrite(&cal, sizeof cal, 1, f); fclose(f); }
    return true;
}

static void touch_init(void)
{
    gpio_config_t irq = { .pin_bit_mask = 1ULL << PIN_TOUCH_IRQ, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&irq);
    spi_device_interface_config_t dc = {
        .clock_speed_hz = 1000000,
        .mode = 0,
        .spics_io_num = PIN_TOUCH_CS,
        .queue_size = 1,
    };
    if (spi_bus_add_device(LCD_HOST_ID, &dc, &touch_dev) != ESP_OK) { ESP_LOGE(TAG, "touch device failed"); touch_dev = NULL; return; }

    bool have = false;
    if (sd_ok) {
        lcd_wait_idle();
        FILE *f = fopen(CAL_FILE, "rb");
        if (f) { have = fread(&cal, sizeof cal, 1, f) == 1 && cal.x1 != cal.x0 && cal.y1 != cal.y0; fclose(f); }
    }
    touch_ok = true;
    if (!have) {
        touch_ok = touch_calibrate();
        if (!touch_ok) ESP_LOGW(TAG, "touch calibration skipped (no taps)");
    }
}

/* ---------- touch layout while playing (320x240) ----------
 *  left column  x < 60      : d-pad. Up y<80, Down y>=160, middle band: Left (x<30) / Right
 *  right column x >= 260    : A (top half) / B (bottom half), 60x120 each
 *  bottom strip y >= 180    : SELECT | START | MENU, 60 px tall
 */
#define TOUCH_MENU_FLAG 0x100
static uint16_t touch_zone(int sx, int sy)
{
    if (sx < GB_X0) {
        if (sy < 80) return 0x40;
        if (sy >= 160) return 0x80;
        return sx < GB_X0 / 2 ? 0x20 : 0x10;
    }
    if (sx >= GB_X0 + OUT_W) return sy < LCD_H / 2 ? 0x01 : 0x02;
    if (sy >= OUT_H) {
        int rel = sx - GB_X0;
        if (rel < OUT_W / 3) return 0x04;
        if (rel < 2 * OUT_W / 3) return 0x08;
        return TOUCH_MENU_FLAG;
    }
    return 0;
}

/* filled block with a 2 px gap around it so neighbouring buttons are visibly separate */
static void blk(int x, int y, int w, int h, uint16_t col) { fill_rect(x + 2, y + 2, w - 4, h - 4, col); }

/* dir: 0 up, 1 down, 2 left, 3 right */
static void draw_arrow(int cx, int cy, int dir, uint16_t col)
{
    for (int i = 0; i < 10; i++) {
        int w = 2 * i + 1;
        switch (dir) {
        case 0: fill_rect(cx - i, cy - 6 + i, w, 1, col); break;
        case 1: fill_rect(cx - i, cy + 6 - i, w, 1, col); break;
        case 2: fill_rect(cx - 6 + i, cy - i, 1, w, col); break;
        default: fill_rect(cx + 6 - i, cy - i, 1, w, col); break;
        }
    }
}

static void draw_touch_hints(void)
{
    uint16_t pad = SW(C565(60, 72, 82)), ab = SW(C565(140, 40, 75)), strip = SW(C565(45, 55, 62));

    /* d-pad column */
    blk(0, 0, 60, 80, pad);   blk(0, 160, 60, 80, pad);
    blk(0, 80, 30, 80, pad);  blk(30, 80, 30, 80, pad);
    draw_arrow(30, 40, 0, COL_FG);  draw_arrow(30, 200, 1, COL_FG);
    draw_arrow(15, 120, 2, COL_FG); draw_arrow(45, 120, 3, COL_FG);

    /* A / B column */
    blk(260, 0, 60, 120, ab);   draw_text(280, 46, "A", COL_FG, ab, 4);
    blk(260, 120, 60, 120, ab); draw_text(280, 166, "B", COL_FG, ab, 4);

    /* bottom strip */
    blk(60, 180, 66, 60, strip);  draw_text(75, 203, "SEL", COL_FG, strip, 2);
    blk(126, 180, 67, 60, strip); draw_text(129, 203, "START", COL_FG, strip, 2);
    blk(193, 180, 67, 60, strip); draw_text(202, 203, "MENU", COL_FG, strip, 2);
}

/* ---------- menu ---------- */
static int sel, top;
static bool touch_block;   /* ignore a touch that is still held from the previous screen */
static char menu_msg[48] = "";
#define MENU_ROWS 10

static void menu_draw(void)
{
    fill_rect(0, 0, LCD_W, LCD_H, COL_BG);
    draw_text(8, 6, "GB EMU NM-CYD-C5", COL_SEL, COL_BG, 2);
    if (!sd_ok) draw_text(8, 40, "NO SD CARD", COL_FG, COL_BG, 2);
    else if (n_roms == 0) draw_text(8, 40, "NO ROMS IN /ROMS/GB", COL_FG, COL_BG, 2);
    for (int r = 0; r < MENU_ROWS && top + r < n_roms; r++) {
        int i = top + r;
        char line[40];
        snprintf(line, sizeof line, "%s%s", i == sel ? ">" : " ", roms[i].name);
        line[26] = 0;
        draw_text(8, 30 + r * 18, line, i == sel ? COL_SEL : COL_DIM, COL_BG, 2);
    }
    draw_text(8, 218, menu_msg, COL_FG, COL_BG, 2);
}

static void menu_set_msg(const char *m) { snprintf(menu_msg, sizeof menu_msg, "%s", m); send_status(m); }

static bool in_play;

static void start_rom(int idx)
{
    const char *err = load_rom(idx);
    if (err) { menu_set_msg(err); menu_draw(); return; }
    fill_rect(0, 0, LCD_W, LCD_H, COL_BG);
    draw_touch_hints();
    in_play = true;
    send_status("PLAYING");
}

static void menu_tick(void)
{
    static uint8_t prev;
    uint8_t edge = joy_host & ~prev;
    prev = joy_host;
    bool dirty = false;

    /* touch: tap a row to start it, tap the title to page up, tap the bottom line to page down */
    int tx, ty;
    static bool tprev;
    if (!touch_screen(&tx, &ty)) { tprev = false; touch_block = false; }
    else if (!tprev && !touch_block) {
        tprev = true;
        if (ty < 30) { top = top > MENU_ROWS ? top - MENU_ROWS : 0; sel = top; dirty = true; }
        else if (ty >= 210) { if (top + MENU_ROWS < n_roms) top += MENU_ROWS; sel = top; dirty = true; }
        else { int i = top + (ty - 30) / 18; if (i < n_roms) { sel = i; start_rom(i); return; } }
    }

    if (edge & 0x40) { if (sel > 0) sel--; dirty = true; }          /* up */
    if (edge & 0x80) { if (sel + 1 < n_roms) sel++; dirty = true; } /* down */
    if (sel < top) top = sel;
    if (sel >= top + MENU_ROWS) top = sel - MENU_ROWS + 1;
    if (pend_list) { pend_list = false; send_list(); }
    if (pend_load >= 0) { int i = pend_load; pend_load = -1; sel = i; start_rom(i); return; }
    if ((edge & 0x01) || (edge & 0x08)) { if (n_roms) { start_rom(sel); return; } }
    if (dirty) menu_draw();
    vTaskDelay(pdMS_TO_TICKS(10));
}

/* ---------- play loop ---------- */
static void play_loop(void)
{
    int64_t next = esp_timer_get_time();
    int64_t last_save = next, fps_t = next;
    int frames = 0, shown = 0;
    unsigned fcount = 0;

    while (in_play) {
        poll_serial();
        if (pend_list) { pend_list = false; send_list(); }
        if (pend_menu || gb_failed) {
            pend_menu = false;
            sram_flush();
            in_play = false;
            if (gb_failed) menu_set_msg("EMULATOR ERROR");
            break;
        }
        if (pend_reset) { pend_reset = false; gb_reset(&gb); send_status("RESET"); }
        if (pend_ss) { pend_ss = false; send_status(state_save() ? "STATE SAVED" : "STATE SAVE FAILED"); }
        if (pend_ls) { pend_ls = false; send_status(state_load() ? "STATE LOADED" : "NO STATE"); }
        if (pend_load >= 0) { int i = pend_load; pend_load = -1; const char *e = load_rom(i); if (e) { send_status(e); in_play = false; break; } fill_rect(0, 0, LCD_W, LCD_H, COL_BG); draw_touch_hints(); }

        /* touch buttons (single touch point) are combined with the laptop input */
        static bool menu_was;
        uint16_t tz = 0;
        int tx, ty;
        if (touch_screen(&tx, &ty)) tz = touch_zone(tx, ty);
        bool menu_now = (tz & TOUCH_MENU_FLAG) != 0;
        if (menu_now && !menu_was) pend_menu = true;
        menu_was = menu_now;
        gb.direct.joypad = (uint8_t)~(joy_host | (tz & 0xFF));

        int64_t now = esp_timer_get_time();
        skip_render = (now > next + FRAME_US);   /* running late: drop this frame's drawing */
        want_stream = host_seen && stream_on && !skip_render && (fcount % STREAM_EVERY == 0);
        cur_fb = fb[0];

        gb_run_frame(&gb);
        fcount++;

        if (!skip_render) {
            draw_game(cur_fb);
            shown++;
            if (want_stream) send_msg(M_VIDEO, pack_buf, sizeof pack_buf);
        }
        frames++;

        now = esp_timer_get_time();
        if (now - last_save > 5000000) { sram_flush(); last_save = now; }
        if (now - fps_t >= 1000000) {
#if SHOW_FPS
            char t[16];
            snprintf(t, sizeof t, "FPS %d", shown);
            draw_text(8, 8, t, COL_DIM, COL_BG, 2);
#endif
            ESP_LOGI(TAG, "emu %d fps, drawn %d fps", frames, shown);
            frames = shown = 0;
            fps_t = now;
        }

        next += FRAME_US;
        now = esp_timer_get_time();
        if (next < now - 4 * FRAME_US) next = now;   /* far behind: resync instead of fast-forwarding */
        while (next - esp_timer_get_time() > 2000) vTaskDelay(1);
        while (esp_timer_get_time() < next) { }
        vTaskDelay(0);
    }
}

/* ---------- main ---------- */
void app_main(void)
{
    /* keep the touch controller off the shared bus */
    gpio_config_t tc = { .pin_bit_mask = 1ULL << PIN_TOUCH_CS, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&tc);
    gpio_set_level(PIN_TOUCH_CS, 1);

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = GB_W * GB_H * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST_ID, &bus, SPI_DMA_CH_AUTO));

    display_init();
    usb_init();

    fill_rect(0, 0, LCD_W, LCD_H, COL_BG);
    draw_text(8, 100, "GB EMU NM-CYD-C5", COL_SEL, COL_BG, 2);
    draw_text(8, 130, "READING SD...", COL_DIM, COL_BG, 2);

    sd_init();
    scan_roms();
    ESP_LOGI(TAG, "%d ROMs, PSRAM free %u", n_roms, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    touch_init();
    menu_draw();
    while (1) {
        poll_serial();
        if (in_play) { play_loop(); touch_block = true; menu_draw(); }
        else menu_tick();
        if (pend_menu && !in_play) pend_menu = false;
    }
}