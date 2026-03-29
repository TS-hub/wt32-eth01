/* SH1106 1.3" I2C OLED driver
 * SDA=GPIO16, SCL=GPIO17, I2C address 0x3C or 0x3D (auto-detected)
 * 128x64 px, 5x7 font (6px pitch) → 21 chars × 8 lines
 *
 * Layout:
 *   Page 0  title bar
 *   Page 1  separator
 *   Page 2  "ETH: <ip>"
 *   Page 3  (blank)
 *   Page 4  "AP:  ESP32-ETH-Setup"
 *   Page 5  (blank)
 *   Page 6  "iperf3: <status>"
 *   Page 7  (blank)
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "oled.h"

#define TAG              "oled"
#define OLED_SDA         16
#define OLED_SCL         17
#define OLED_WIDTH       128
#define OLED_PAGES       8
#define CHAR_PITCH       6    /* 5px glyph + 1px gap */
#define COL_OFFSET       2    /* SH1106 RAM starts 2 cols before display */

/* ------------------------------------------------------------------ */
/* 5×7 font, column format, LSB=top — ASCII 0x20..0x7E                */
/* ------------------------------------------------------------------ */
static const uint8_t s_font[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* 20 ' '  */
    {0x00,0x00,0x5F,0x00,0x00}, /* 21 '!'  */
    {0x00,0x07,0x00,0x07,0x00}, /* 22 '"'  */
    {0x14,0x7F,0x14,0x7F,0x14}, /* 23 '#'  */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* 24 '$'  */
    {0x23,0x13,0x08,0x64,0x62}, /* 25 '%'  */
    {0x36,0x49,0x55,0x22,0x50}, /* 26 '&'  */
    {0x00,0x05,0x03,0x00,0x00}, /* 27 '\'' */
    {0x00,0x1C,0x22,0x41,0x00}, /* 28 '('  */
    {0x00,0x41,0x22,0x1C,0x00}, /* 29 ')'  */
    {0x14,0x08,0x3E,0x08,0x14}, /* 2A '*'  */
    {0x08,0x08,0x3E,0x08,0x08}, /* 2B '+'  */
    {0x00,0x50,0x30,0x00,0x00}, /* 2C ','  */
    {0x08,0x08,0x08,0x08,0x08}, /* 2D '-'  */
    {0x00,0x60,0x60,0x00,0x00}, /* 2E '.'  */
    {0x20,0x10,0x08,0x04,0x02}, /* 2F '/'  */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 30 '0'  */
    {0x00,0x42,0x7F,0x40,0x00}, /* 31 '1'  */
    {0x42,0x61,0x51,0x49,0x46}, /* 32 '2'  */
    {0x21,0x41,0x45,0x4B,0x31}, /* 33 '3'  */
    {0x18,0x14,0x12,0x7F,0x10}, /* 34 '4'  */
    {0x27,0x45,0x45,0x45,0x39}, /* 35 '5'  */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 36 '6'  */
    {0x01,0x71,0x09,0x05,0x03}, /* 37 '7'  */
    {0x36,0x49,0x49,0x49,0x36}, /* 38 '8'  */
    {0x06,0x49,0x49,0x29,0x1E}, /* 39 '9'  */
    {0x00,0x36,0x36,0x00,0x00}, /* 3A ':'  */
    {0x00,0x56,0x36,0x00,0x00}, /* 3B ';'  */
    {0x08,0x14,0x22,0x41,0x00}, /* 3C '<'  */
    {0x14,0x14,0x14,0x14,0x14}, /* 3D '='  */
    {0x00,0x41,0x22,0x14,0x08}, /* 3E '>'  */
    {0x02,0x01,0x51,0x09,0x06}, /* 3F '?'  */
    {0x32,0x49,0x79,0x41,0x3E}, /* 40 '@'  */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 41 'A'  */
    {0x7F,0x49,0x49,0x49,0x36}, /* 42 'B'  */
    {0x3E,0x41,0x41,0x41,0x22}, /* 43 'C'  */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 44 'D'  */
    {0x7F,0x49,0x49,0x49,0x41}, /* 45 'E'  */
    {0x7F,0x09,0x09,0x09,0x01}, /* 46 'F'  */
    {0x3E,0x41,0x49,0x49,0x7A}, /* 47 'G'  */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 48 'H'  */
    {0x00,0x41,0x7F,0x41,0x00}, /* 49 'I'  */
    {0x20,0x40,0x41,0x3F,0x01}, /* 4A 'J'  */
    {0x7F,0x08,0x14,0x22,0x41}, /* 4B 'K'  */
    {0x7F,0x40,0x40,0x40,0x40}, /* 4C 'L'  */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* 4D 'M'  */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 4E 'N'  */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 4F 'O'  */
    {0x7F,0x09,0x09,0x09,0x06}, /* 50 'P'  */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 51 'Q'  */
    {0x7F,0x09,0x19,0x29,0x46}, /* 52 'R'  */
    {0x46,0x49,0x49,0x49,0x31}, /* 53 'S'  */
    {0x01,0x01,0x7F,0x01,0x01}, /* 54 'T'  */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 55 'U'  */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 56 'V'  */
    {0x3F,0x40,0x38,0x40,0x3F}, /* 57 'W'  */
    {0x63,0x14,0x08,0x14,0x63}, /* 58 'X'  */
    {0x07,0x08,0x70,0x08,0x07}, /* 59 'Y'  */
    {0x61,0x51,0x49,0x45,0x43}, /* 5A 'Z'  */
    {0x00,0x7F,0x41,0x41,0x00}, /* 5B '['  */
    {0x02,0x04,0x08,0x10,0x20}, /* 5C '\\' */
    {0x00,0x41,0x41,0x7F,0x00}, /* 5D ']'  */
    {0x04,0x02,0x01,0x02,0x04}, /* 5E '^'  */
    {0x40,0x40,0x40,0x40,0x40}, /* 5F '_'  */
    {0x00,0x01,0x02,0x04,0x00}, /* 60 '`'  */
    {0x20,0x54,0x54,0x54,0x78}, /* 61 'a'  */
    {0x7F,0x48,0x44,0x44,0x38}, /* 62 'b'  */
    {0x38,0x44,0x44,0x44,0x20}, /* 63 'c'  */
    {0x38,0x44,0x44,0x48,0x7F}, /* 64 'd'  */
    {0x38,0x54,0x54,0x54,0x18}, /* 65 'e'  */
    {0x08,0x7E,0x09,0x01,0x02}, /* 66 'f'  */
    {0x0C,0x52,0x52,0x52,0x3E}, /* 67 'g'  */
    {0x7F,0x08,0x04,0x04,0x78}, /* 68 'h'  */
    {0x00,0x44,0x7D,0x40,0x00}, /* 69 'i'  */
    {0x20,0x40,0x44,0x3D,0x00}, /* 6A 'j'  */
    {0x7F,0x10,0x28,0x44,0x00}, /* 6B 'k'  */
    {0x00,0x41,0x7F,0x40,0x00}, /* 6C 'l'  */
    {0x7C,0x04,0x18,0x04,0x7C}, /* 6D 'm'  */
    {0x7C,0x08,0x04,0x04,0x78}, /* 6E 'n'  */
    {0x38,0x44,0x44,0x44,0x38}, /* 6F 'o'  */
    {0x7C,0x14,0x14,0x14,0x08}, /* 70 'p'  */
    {0x08,0x14,0x14,0x18,0x7C}, /* 71 'q'  */
    {0x7C,0x08,0x04,0x04,0x08}, /* 72 'r'  */
    {0x48,0x54,0x54,0x54,0x20}, /* 73 's'  */
    {0x04,0x3F,0x44,0x40,0x20}, /* 74 't'  */
    {0x3C,0x40,0x40,0x20,0x7C}, /* 75 'u'  */
    {0x1C,0x20,0x40,0x20,0x1C}, /* 76 'v'  */
    {0x3C,0x40,0x30,0x40,0x3C}, /* 77 'w'  */
    {0x44,0x28,0x10,0x28,0x44}, /* 78 'x'  */
    {0x0C,0x50,0x50,0x50,0x3C}, /* 79 'y'  */
    {0x44,0x64,0x54,0x4C,0x44}, /* 7A 'z'  */
    {0x00,0x08,0x36,0x41,0x00}, /* 7B '{'  */
    {0x00,0x00,0x7F,0x00,0x00}, /* 7C '|'  */
    {0x00,0x41,0x36,0x08,0x00}, /* 7D '}'  */
    {0x10,0x08,0x08,0x10,0x08}, /* 7E '~'  */
};

/* ------------------------------------------------------------------ */

static bool              s_ready = false;
static SemaphoreHandle_t s_mutex;
static int               s_col_offset = COL_OFFSET;
static uint8_t           s_addr = 0x3C;  /* updated by bus scan; also 0x3D */

/* Framebuffer: [page][col] */
static uint8_t s_fb[OLED_PAGES][OLED_WIDTH];

/* State */
static char   s_ip[24]       = "---";
static bool   s_iperf3_active = false;
static double s_iperf3_mbps   = 0.0;

/* ------------------------------------------------------------------ */
/* Low-level I/O (legacy i2c driver)                                   */
/* ------------------------------------------------------------------ */

static void cmd(uint8_t c)
{
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (s_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(h, 0x00, true);   /* control: command */
    i2c_master_write_byte(h, c,    true);
    i2c_master_stop(h);
    i2c_master_cmd_begin(I2C_NUM_0, h, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(h);
}

static void flush_page(int page)
{
    int col = s_col_offset;
    cmd(0xB0 | (page & 0x07));
    cmd(0x00 | (col & 0x0F));
    cmd(0x10 | ((col >> 4) & 0x07));

    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (s_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(h, 0x40, true);   /* control: data */
    i2c_master_write(h, s_fb[page], OLED_WIDTH, true);
    i2c_master_stop(h);
    i2c_master_cmd_begin(I2C_NUM_0, h, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(h);
}

/* ------------------------------------------------------------------ */
/* Framebuffer helpers                                                  */
/* ------------------------------------------------------------------ */

static void fb_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

static void fb_puts(int page, int col, const char *str)
{
    while (*str && col + CHAR_PITCH <= OLED_WIDTH) {
        uint8_t c = (uint8_t)*str++;
        if (c < 0x20 || c > 0x7E) c = 0x20;
        memcpy(&s_fb[page][col], s_font[c - 0x20], 5);
        s_fb[page][col + 5] = 0x00;
        col += CHAR_PITCH;
    }
}

/* ------------------------------------------------------------------ */
/* Screen layout                                                        */
/* ------------------------------------------------------------------ */

static void redraw(void)
{
    fb_clear();

    /* Page 0 — title */
    fb_puts(0, 8, "iperf test plug");

    /* Page 1 — separator line (pixel row 3 of the page) */
    memset(s_fb[1], 0x08, OLED_WIDTH);

    /* Page 2 — Ethernet IP */
    char line[32];
    snprintf(line, sizeof(line), "ETH: %s", s_ip);
    fb_puts(2, 8, line);

    /* Page 4 — WiFi AP SSID */
    fb_puts(4, 8, "AP:  ESP32-ETH-Setup");

    /* Page 6 — iperf3 status */
    char iperf_line[32];
    if (s_iperf3_active)
        snprintf(iperf_line, sizeof(iperf_line), "iperf3: running...");
    else if (s_iperf3_mbps > 0)
        snprintf(iperf_line, sizeof(iperf_line), "iperf3: %.1f Mb/s", s_iperf3_mbps);
    else
        snprintf(iperf_line, sizeof(iperf_line), "iperf3: waiting");
    fb_puts(6, 8, iperf_line);

    for (int p = 0; p < OLED_PAGES; p++) flush_page(p);
}

/* ------------------------------------------------------------------ */
/* SH1106 init sequence                                                 */
/* ------------------------------------------------------------------ */

/* Compatible init for both SSD1306 and SH1106.
 * Uses 0x8D/0x14 charge pump (SSD1306) — SH1106 modules with external
 * charge pump accept this; pure SH1106 internal-pump modules may need
 * col_offset=2, but both are tried at startup. */
static void oled_hw_init(void)
{
    cmd(0xAE);            /* display off              */
    cmd(0xD5); cmd(0x80); /* clock / osc freq         */
    cmd(0xA8); cmd(0x3F); /* mux ratio 1:64           */
    cmd(0xD3); cmd(0x00); /* display offset = 0       */
    cmd(0x40);            /* start line 0             */
    cmd(0x8D); cmd(0x14); /* charge pump ON (SSD1306 / SH1106-ext) */
    cmd(0x20); cmd(0x02); /* page addressing mode     */
    cmd(0xA1);            /* seg remap                */
    cmd(0xC8);            /* COM scan reverse         */
    cmd(0xDA); cmd(0x12); /* COM pins config          */
    cmd(0x81); cmd(0xCF); /* contrast                 */
    cmd(0xD9); cmd(0xF1); /* pre-charge               */
    cmd(0xDB); cmd(0x40); /* VCOMH deselect level     */
    cmd(0xA4);            /* display from RAM         */
    cmd(0xA6);            /* normal (not inverted)    */
    cmd(0xAF);            /* display on               */
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void oled_init(void)
{
    s_mutex = xSemaphoreCreateMutex();

    /* Legacy I2C driver — more compatible with SSD1306/SH1106 than new master API */
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = OLED_SDA,
        .scl_io_num       = OLED_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    if (i2c_param_config(I2C_NUM_0, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed");
        return;
    }
    if (i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK) {
        ESP_LOGE(TAG, "i2c_driver_install failed");
        return;
    }

    /* Bus scan */
    ESP_LOGI(TAG, "Scanning I2C bus (SDA=%d SCL=%d)...", OLED_SDA, OLED_SCL);
    uint8_t found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        i2c_cmd_handle_t h = i2c_cmd_link_create();
        i2c_master_start(h);
        i2c_master_write_byte(h, (a << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(h);
        esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, h, pdMS_TO_TICKS(10));
        i2c_cmd_link_delete(h);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "  Device at 0x%02X", a);
            found = a;
        }
    }
    if (!found) {
        ESP_LOGE(TAG, "I2C bus empty — no device responded");
        i2c_driver_delete(I2C_NUM_0);
        return;
    }

    s_addr = found;
    vTaskDelay(pdMS_TO_TICKS(100));
    oled_hw_init();
    s_col_offset = COL_OFFSET;
    s_ready = true;
    redraw();
    ESP_LOGI(TAG, "OLED ready at 0x%02X (col_offset=%d)", found, s_col_offset);
}

void oled_set_eth_ip(const char *ip)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(s_ip, (ip && *ip) ? ip : "---", sizeof(s_ip));
    redraw();
    xSemaphoreGive(s_mutex);
}

void oled_set_iperf3_active(bool active)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_iperf3_active = active;
    redraw();
    xSemaphoreGive(s_mutex);
}

void oled_set_iperf3_result(double mbps)
{
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_iperf3_active = false;
    s_iperf3_mbps   = mbps;
    redraw();
    xSemaphoreGive(s_mutex);
}
