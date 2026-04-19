#include "display.h"
#include "pincfg.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "freertos/semphr.h"
#include "vendor/esp_lcd_st77916.h"

static const char *TAG = "display";

#define BACKLIGHT_PWM_FREQ_HZ   5000
#define BACKLIGHT_LEDC_TIMER    LEDC_TIMER_13_BIT
#define BACKLIGHT_MAX_DUTY      ((1 << 13) - 1)

// One row-strip that we reuse for display_fill / display_fill_rect. 360 px
// × 20 rows × 2 bytes = 14 400 B, comfortable inside internal DMA RAM.
#define STRIP_ROWS              20
#define STRIP_BYTES             (DISPLAY_WIDTH * STRIP_ROWS * sizeof(uint16_t))

static esp_lcd_panel_handle_t    lcd_panel = NULL;
static esp_lcd_panel_io_handle_t lcd_io    = NULL;
static uint16_t                 *strip_buf = NULL;  // DMA-capable scratch
static SemaphoreHandle_t         trans_done_sem = NULL;

// Fires from the panel-IO ISR each time a color transfer finishes.
// Gives the trans_done_sem so display_draw_rect can block until the
// caller's source buffer is truly free to reuse (the "async DMA keeps
// reading after draw_bitmap returned" race is the whole reason
// Info-page row blits were getting corrupted).
static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                          esp_lcd_panel_io_event_data_t *data,
                                          void *user_ctx) {
  (void)io; (void)data; (void)user_ctx;
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(trans_done_sem, &woken);
  return woken == pdTRUE;
}

// Vendor init sequence lifted verbatim from the JC3636W518EN reference
// firmware's display.c — it's tuned for this specific panel lot and
// differs from the esp_lcd_st77916 default table.
static const st77916_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xF0, (const uint8_t[]){0x28}, 1, 0},
    {0xF2, (const uint8_t[]){0x28}, 1, 0},
    {0x73, (const uint8_t[]){0xF0}, 1, 0},
    {0x7C, (const uint8_t[]){0xD1}, 1, 0},
    {0x83, (const uint8_t[]){0xE0}, 1, 0},
    {0x84, (const uint8_t[]){0x61}, 1, 0},
    {0xF2, (const uint8_t[]){0x82}, 1, 0},
    {0xF0, (const uint8_t[]){0x00}, 1, 0},
    {0xF0, (const uint8_t[]){0x01}, 1, 0},
    {0xF1, (const uint8_t[]){0x01}, 1, 0},
    {0xB0, (const uint8_t[]){0x56}, 1, 0},
    {0xB1, (const uint8_t[]){0x4D}, 1, 0},
    {0xB2, (const uint8_t[]){0x24}, 1, 0},
    {0xB4, (const uint8_t[]){0x87}, 1, 0},
    {0xB5, (const uint8_t[]){0x44}, 1, 0},
    {0xB6, (const uint8_t[]){0x8B}, 1, 0},
    {0xB7, (const uint8_t[]){0x40}, 1, 0},
    {0xB8, (const uint8_t[]){0x86}, 1, 0},
    {0xBA, (const uint8_t[]){0x00}, 1, 0},
    {0xBB, (const uint8_t[]){0x08}, 1, 0},
    {0xBC, (const uint8_t[]){0x08}, 1, 0},
    {0xBD, (const uint8_t[]){0x00}, 1, 0},
    {0xC0, (const uint8_t[]){0x80}, 1, 0},
    {0xC1, (const uint8_t[]){0x10}, 1, 0},
    {0xC2, (const uint8_t[]){0x37}, 1, 0},
    {0xC3, (const uint8_t[]){0x80}, 1, 0},
    {0xC4, (const uint8_t[]){0x10}, 1, 0},
    {0xC5, (const uint8_t[]){0x37}, 1, 0},
    {0xC6, (const uint8_t[]){0xA9}, 1, 0},
    {0xC7, (const uint8_t[]){0x41}, 1, 0},
    {0xC8, (const uint8_t[]){0x01}, 1, 0},
    {0xC9, (const uint8_t[]){0xA9}, 1, 0},
    {0xCA, (const uint8_t[]){0x41}, 1, 0},
    {0xCB, (const uint8_t[]){0x01}, 1, 0},
    {0xD0, (const uint8_t[]){0x91}, 1, 0},
    {0xD1, (const uint8_t[]){0x68}, 1, 0},
    {0xD2, (const uint8_t[]){0x68}, 1, 0},
    {0xF5, (const uint8_t[]){0x00, 0xA5}, 2, 0},
    {0xDD, (const uint8_t[]){0x4F}, 1, 0},
    {0xDE, (const uint8_t[]){0x4F}, 1, 0},
    {0xF1, (const uint8_t[]){0x10}, 1, 0},
    {0xF0, (const uint8_t[]){0x00}, 1, 0},
    {0xF0, (const uint8_t[]){0x02}, 1, 0},
    {0xE0, (const uint8_t[]){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (const uint8_t[]){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (const uint8_t[]){0x10}, 1, 0},
    {0xF3, (const uint8_t[]){0x10}, 1, 0},
    {0xE0, (const uint8_t[]){0x07}, 1, 0},
    {0xE1, (const uint8_t[]){0x00}, 1, 0},
    {0xE2, (const uint8_t[]){0x00}, 1, 0},
    {0xE3, (const uint8_t[]){0x00}, 1, 0},
    {0xE4, (const uint8_t[]){0xE0}, 1, 0},
    {0xE5, (const uint8_t[]){0x06}, 1, 0},
    {0xE6, (const uint8_t[]){0x21}, 1, 0},
    {0xE7, (const uint8_t[]){0x01}, 1, 0},
    {0xE8, (const uint8_t[]){0x05}, 1, 0},
    {0xE9, (const uint8_t[]){0x02}, 1, 0},
    {0xEA, (const uint8_t[]){0xDA}, 1, 0},
    {0xEB, (const uint8_t[]){0x00}, 1, 0},
    {0xEC, (const uint8_t[]){0x00}, 1, 0},
    {0xED, (const uint8_t[]){0x0F}, 1, 0},
    {0xEE, (const uint8_t[]){0x00}, 1, 0},
    {0xEF, (const uint8_t[]){0x00}, 1, 0},
    {0xF8, (const uint8_t[]){0x00}, 1, 0},
    {0xF9, (const uint8_t[]){0x00}, 1, 0},
    {0xFA, (const uint8_t[]){0x00}, 1, 0},
    {0xFB, (const uint8_t[]){0x00}, 1, 0},
    {0xFC, (const uint8_t[]){0x00}, 1, 0},
    {0xFD, (const uint8_t[]){0x00}, 1, 0},
    {0xFE, (const uint8_t[]){0x00}, 1, 0},
    {0xFF, (const uint8_t[]){0x00}, 1, 0},
    {0x60, (const uint8_t[]){0x40}, 1, 0},
    {0x61, (const uint8_t[]){0x04}, 1, 0},
    {0x62, (const uint8_t[]){0x00}, 1, 0},
    {0x63, (const uint8_t[]){0x42}, 1, 0},
    {0x64, (const uint8_t[]){0xD9}, 1, 0},
    {0x65, (const uint8_t[]){0x00}, 1, 0},
    {0x66, (const uint8_t[]){0x00}, 1, 0},
    {0x67, (const uint8_t[]){0x00}, 1, 0},
    {0x68, (const uint8_t[]){0x00}, 1, 0},
    {0x69, (const uint8_t[]){0x00}, 1, 0},
    {0x6A, (const uint8_t[]){0x00}, 1, 0},
    {0x6B, (const uint8_t[]){0x00}, 1, 0},
    {0x70, (const uint8_t[]){0x40}, 1, 0},
    {0x71, (const uint8_t[]){0x03}, 1, 0},
    {0x72, (const uint8_t[]){0x00}, 1, 0},
    {0x73, (const uint8_t[]){0x42}, 1, 0},
    {0x74, (const uint8_t[]){0xD8}, 1, 0},
    {0x75, (const uint8_t[]){0x00}, 1, 0},
    {0x76, (const uint8_t[]){0x00}, 1, 0},
    {0x77, (const uint8_t[]){0x00}, 1, 0},
    {0x78, (const uint8_t[]){0x00}, 1, 0},
    {0x79, (const uint8_t[]){0x00}, 1, 0},
    {0x7A, (const uint8_t[]){0x00}, 1, 0},
    {0x7B, (const uint8_t[]){0x00}, 1, 0},
    {0x80, (const uint8_t[]){0x48}, 1, 0},
    {0x81, (const uint8_t[]){0x00}, 1, 0},
    {0x82, (const uint8_t[]){0x06}, 1, 0},
    {0x83, (const uint8_t[]){0x02}, 1, 0},
    {0x84, (const uint8_t[]){0xD6}, 1, 0},
    {0x85, (const uint8_t[]){0x04}, 1, 0},
    {0x86, (const uint8_t[]){0x00}, 1, 0},
    {0x87, (const uint8_t[]){0x00}, 1, 0},
    {0x88, (const uint8_t[]){0x48}, 1, 0},
    {0x89, (const uint8_t[]){0x00}, 1, 0},
    {0x8A, (const uint8_t[]){0x08}, 1, 0},
    {0x8B, (const uint8_t[]){0x02}, 1, 0},
    {0x8C, (const uint8_t[]){0xD8}, 1, 0},
    {0x8D, (const uint8_t[]){0x04}, 1, 0},
    {0x8E, (const uint8_t[]){0x00}, 1, 0},
    {0x8F, (const uint8_t[]){0x00}, 1, 0},
    {0x90, (const uint8_t[]){0x48}, 1, 0},
    {0x91, (const uint8_t[]){0x00}, 1, 0},
    {0x92, (const uint8_t[]){0x0A}, 1, 0},
    {0x93, (const uint8_t[]){0x02}, 1, 0},
    {0x94, (const uint8_t[]){0xDA}, 1, 0},
    {0x95, (const uint8_t[]){0x04}, 1, 0},
    {0x96, (const uint8_t[]){0x00}, 1, 0},
    {0x97, (const uint8_t[]){0x00}, 1, 0},
    {0x98, (const uint8_t[]){0x48}, 1, 0},
    {0x99, (const uint8_t[]){0x00}, 1, 0},
    {0x9A, (const uint8_t[]){0x0C}, 1, 0},
    {0x9B, (const uint8_t[]){0x02}, 1, 0},
    {0x9C, (const uint8_t[]){0xDC}, 1, 0},
    {0x9D, (const uint8_t[]){0x04}, 1, 0},
    {0x9E, (const uint8_t[]){0x00}, 1, 0},
    {0x9F, (const uint8_t[]){0x00}, 1, 0},
    {0xA0, (const uint8_t[]){0x48}, 1, 0},
    {0xA1, (const uint8_t[]){0x00}, 1, 0},
    {0xA2, (const uint8_t[]){0x05}, 1, 0},
    {0xA3, (const uint8_t[]){0x02}, 1, 0},
    {0xA4, (const uint8_t[]){0xD5}, 1, 0},
    {0xA5, (const uint8_t[]){0x04}, 1, 0},
    {0xA6, (const uint8_t[]){0x00}, 1, 0},
    {0xA7, (const uint8_t[]){0x00}, 1, 0},
    {0xA8, (const uint8_t[]){0x48}, 1, 0},
    {0xA9, (const uint8_t[]){0x00}, 1, 0},
    {0xAA, (const uint8_t[]){0x07}, 1, 0},
    {0xAB, (const uint8_t[]){0x02}, 1, 0},
    {0xAC, (const uint8_t[]){0xD7}, 1, 0},
    {0xAD, (const uint8_t[]){0x04}, 1, 0},
    {0xAE, (const uint8_t[]){0x00}, 1, 0},
    {0xAF, (const uint8_t[]){0x00}, 1, 0},
    {0xB0, (const uint8_t[]){0x48}, 1, 0},
    {0xB1, (const uint8_t[]){0x00}, 1, 0},
    {0xB2, (const uint8_t[]){0x09}, 1, 0},
    {0xB3, (const uint8_t[]){0x02}, 1, 0},
    {0xB4, (const uint8_t[]){0xD9}, 1, 0},
    {0xB5, (const uint8_t[]){0x04}, 1, 0},
    {0xB6, (const uint8_t[]){0x00}, 1, 0},
    {0xB7, (const uint8_t[]){0x00}, 1, 0},
    {0xB8, (const uint8_t[]){0x48}, 1, 0},
    {0xB9, (const uint8_t[]){0x00}, 1, 0},
    {0xBA, (const uint8_t[]){0x0B}, 1, 0},
    {0xBB, (const uint8_t[]){0x02}, 1, 0},
    {0xBC, (const uint8_t[]){0xDB}, 1, 0},
    {0xBD, (const uint8_t[]){0x04}, 1, 0},
    {0xBE, (const uint8_t[]){0x00}, 1, 0},
    {0xBF, (const uint8_t[]){0x00}, 1, 0},
    {0xC0, (const uint8_t[]){0x10}, 1, 0},
    {0xC1, (const uint8_t[]){0x47}, 1, 0},
    {0xC2, (const uint8_t[]){0x56}, 1, 0},
    {0xC3, (const uint8_t[]){0x65}, 1, 0},
    {0xC4, (const uint8_t[]){0x74}, 1, 0},
    {0xC5, (const uint8_t[]){0x88}, 1, 0},
    {0xC6, (const uint8_t[]){0x99}, 1, 0},
    {0xC7, (const uint8_t[]){0x01}, 1, 0},
    {0xC8, (const uint8_t[]){0xBB}, 1, 0},
    {0xC9, (const uint8_t[]){0xAA}, 1, 0},
    {0xD0, (const uint8_t[]){0x10}, 1, 0},
    {0xD1, (const uint8_t[]){0x47}, 1, 0},
    {0xD2, (const uint8_t[]){0x56}, 1, 0},
    {0xD3, (const uint8_t[]){0x65}, 1, 0},
    {0xD4, (const uint8_t[]){0x74}, 1, 0},
    {0xD5, (const uint8_t[]){0x88}, 1, 0},
    {0xD6, (const uint8_t[]){0x99}, 1, 0},
    {0xD7, (const uint8_t[]){0x01}, 1, 0},
    {0xD8, (const uint8_t[]){0xBB}, 1, 0},
    {0xD9, (const uint8_t[]){0xAA}, 1, 0},
    {0xF3, (const uint8_t[]){0x01}, 1, 0},
    {0xF0, (const uint8_t[]){0x00}, 1, 0},
    {0x21, (const uint8_t[]){0x00}, 1, 0},
    {0x11, (const uint8_t[]){0x00}, 1, 120},
    {0x29, (const uint8_t[]){0x00}, 1, 0},
};

static void backlight_init(void)
{
    const ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BACKLIGHT_LEDC_TIMER,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = BACKLIGHT_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    const ledc_channel_config_t ch_cfg = {
        .gpio_num   = TFT_BLK,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));
}

static esp_err_t lcd_init(void)
{
    ESP_LOGI(TAG, "QSPI bus init");
    const spi_bus_config_t bus_cfg = ST77916_PANEL_BUS_QSPI_CONFIG(
        TFT_SCK, TFT_SDA0, TFT_SDA1, TFT_SDA2, TFT_SDA3,
        DISPLAY_WIDTH * STRIP_ROWS * sizeof(uint16_t)
    );
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Panel IO");
    esp_lcd_panel_io_spi_config_t io_cfg =
        ST77916_PANEL_IO_QSPI_CONFIG(TFT_CS, on_color_trans_done, NULL);
    // Depth 1 caps in-flight transfers; combined with the
    // on_color_trans_done callback + semaphore below, display_draw_rect
    // becomes fully synchronous — the caller's buffer is safe to reuse
    // the moment the call returns.
    io_cfg.trans_queue_depth = 1;
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &lcd_io);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Panel create");
    const st77916_vendor_config_t vendor_cfg = {
        .init_cmds      = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = { .use_qspi_interface = 1 },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = TFT_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config  = (void *)&vendor_cfg,
    };
    err = esp_lcd_new_panel_st77916(lcd_io, &panel_cfg, &lcd_panel);
    if (err != ESP_OK) return err;

    ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(lcd_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd_panel, true));
    return ESP_OK;
}

bool display_init(void)
{
    backlight_init();

    strip_buf = (uint16_t *)heap_caps_malloc(STRIP_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!strip_buf) {
        ESP_LOGE(TAG, "strip buffer alloc failed");
        return false;
    }

    trans_done_sem = xSemaphoreCreateBinary();
    if (!trans_done_sem) {
        ESP_LOGE(TAG, "sem alloc failed");
        return false;
    }

    esp_err_t err = lcd_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_init failed: %s", esp_err_to_name(err));
        return false;
    }

    display_set_brightness(40);
    return true;
}

void display_set_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint32_t duty = (BACKLIGHT_MAX_DUTY * percent) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// Swap bytes so the 16-bit value arrives on the wire as ST77916 expects
// (high byte first). The panel was set up with .swap_bytes = 1 in the
// reference firmware via LVGL port; since we're driving it ourselves we
// do the swap in software here.
static inline uint16_t swap16(uint16_t v) { return (v >> 8) | (v << 8); }

void display_fill(uint16_t rgb565)
{
    display_fill_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, rgb565);
}

void display_fill_rect(int x, int y, int w, int h, uint16_t rgb565)
{
    if (!lcd_panel || !strip_buf) return;
    if (w <= 0 || h <= 0) return;
    if (x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) return;
    if (x + w > DISPLAY_WIDTH)  w = DISPLAY_WIDTH  - x;
    if (y + h > DISPLAY_HEIGHT) h = DISPLAY_HEIGHT - y;

    const uint16_t v = swap16(rgb565);
    const int max_rows_per_chunk = STRIP_BYTES / (w * sizeof(uint16_t));
    const int rows = (max_rows_per_chunk > STRIP_ROWS) ? STRIP_ROWS : max_rows_per_chunk;

    for (int i = 0; i < w * rows; i++) strip_buf[i] = v;

    int y_cur = y;
    int remaining = h;
    while (remaining > 0) {
        int chunk = remaining < rows ? remaining : rows;
        esp_lcd_panel_draw_bitmap(lcd_panel, x, y_cur, x + w, y_cur + chunk, strip_buf);
        xSemaphoreTake(trans_done_sem, portMAX_DELAY);  // sync to keep strip_buf reusable
        y_cur += chunk;
        remaining -= chunk;
    }
}

void display_draw_rect(int x, int y, int w, int h, const uint16_t *pixels)
{
    if (!lcd_panel || !pixels) return;
    if (w <= 0 || h <= 0) return;
    esp_lcd_panel_draw_bitmap(lcd_panel, x, y, x + w, y + h, pixels);
    // Block until the panel ISR reports the DMA is fully done — only
    // then is it safe for the caller to refill or free `pixels`.
    xSemaphoreTake(trans_done_sem, portMAX_DELAY);
}
