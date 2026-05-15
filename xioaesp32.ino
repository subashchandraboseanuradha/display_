/*Using LVGL with Arduino requires some extra steps:
 *Be sure to read the docs here: https://docs.lvgl.io/master/integration/framework/arduino.html  */

/*

 * LVGL + Seeed Studio Round Display for XIAO 1.28" (GC9A01 + CST816S)
 * Target: XIAO ESP32S3
 *
 * Library install order (Arduino Library Manager):
 *   1. Seeed_Arduino_RoundDisplay
 *   2. LVGL (v8.x)
 *
 * CRITICAL - lv_conf.h settings:
 *   #define LV_COLOR_DEPTH 16
 *   #define LV_USE_TFT_ESPI 0   <-- disable, we drive TFT manually
 *
 * TFT_eSPI User_Setup.h:
 *   Use the one inside Seeed_Arduino_RoundDisplay/src/lv_xiao_round_screen/
 *   Do NOT use the default TFT_eSPI User_Setup.h
 */

#include "driver.h"
#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#define USE_TFT_ESPI_LIBRARY
#include "lv_xiao_round_screen.h"  // Seeed's LVGL helper: touch + display utils
#include "ui.h"
#include "sdcard.h"
#include "i2s_mic.h"

#define TFT_HOR_RES  240
#define TFT_VER_RES  240

static lv_disp_draw_buf_t draw_buf_dsc;
static lv_color_t buf[TFT_HOR_RES * TFT_VER_RES / 10];

#if LV_USE_LOG != 0
void my_print(const char *buf)
{
    Serial.println(buf);
    Serial.flush();
}
#endif

void lvgl_touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    static lv_coord_t last_x = 0, last_y = 0;
    if (!chsc6x_is_pressed()) {
        data->state = LV_INDEV_STATE_REL;
    } else {
        lv_coord_t x = 0, y = 0;
        chsc6x_get_xy(&x, &y);
        if (x != 0 || y != 0) {
            last_x = x;
            last_y = y;
        }
        data->state = LV_INDEV_STATE_PR;
        data->point.x = last_x;
        data->point.y = last_y;
    }
}

/* Display flushing — skips TFT during recording to keep GPIO7 as FSPI CLK */
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    if (is_recording()) {
        lv_disp_flush_ready(disp);
        return;
    }

    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)&color_p->full, w * h, true);
    tft.endWrite();

    lv_disp_flush_ready(disp);
}

void setup()
{
    Serial.begin(115200);
    init_sd_card();

    // Backlight on (Seeed Round Display uses D6 as BL pin)
    pinMode(D6, OUTPUT);
    digitalWrite(D6, HIGH);

    // TFT init (GC9A01, SPI pins from Seeed's User_Setup.h)
    tft.begin();
    tft.setRotation(0);
    tft.fillScreen(TFT_BLACK);

    lv_init();

#if LV_USE_LOG != 0
    lv_log_register_print_cb(my_print);
#endif

    lv_disp_draw_buf_init(&draw_buf_dsc, buf, NULL, TFT_HOR_RES * TFT_VER_RES / 10);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = TFT_HOR_RES;
    disp_drv.ver_res = TFT_VER_RES;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf_dsc;
    lv_disp_drv_register(&disp_drv);

    Wire.begin();
    pinMode(TOUCH_INT, INPUT_PULLUP);
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_read;
    lv_indev_drv_register(&indev_drv);

    Serial.println("Setup done");
    ui_init();
}

void loop()
{
    static uint32_t last_tick = 0;
    uint32_t now = millis();
    lv_tick_inc(now - last_tick);
    last_tick = now;

    lv_timer_handler();

    // Force screen redraw when recording ends (TFT was skipped during recording)
    static bool was_rec = false;
    bool is_rec = is_recording();
    if (was_rec && !is_rec) {
        lv_obj_invalidate(lv_scr_act());
    }
    was_rec = is_rec;

    // UI recording requests — handled here, never inside LVGL callbacks
    extern volatile int ui_rec_request;
    if (ui_rec_request == 1) {
        ui_rec_request = 0;
        Serial.println("[UI] start req"); Serial.flush();
        char path[20];
        Serial.println("[L] get filename"); Serial.flush();
        get_next_recording_filename(path, sizeof(path));
        Serial.printf("[L] path=%s\n", path); Serial.flush();
        start_i2s_recording(path);
    } else if (ui_rec_request == 2) {
        ui_rec_request = 0;
        Serial.println("[UI] stop req");
        stop_i2s_recording();
    }

    if (Serial.available()) {
        char cmd = Serial.read();
        if (cmd == 'r' && !is_recording()) {
            char path[20];
            get_next_recording_filename(path, sizeof(path));
            start_i2s_recording(path);
        } else if (cmd == 's' && is_recording()) {
            stop_i2s_recording();
        }
    }

    delay(5);
}
