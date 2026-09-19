#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_system.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "esp_camera.h"
#include "jpeg_decoder.h"
#include "fall_detector.h"
#include "care_store.h"
#include "care_portal.h"
#include "care_audio.h"
#include "care_imu.h"
#include "care_indicator.h"
#include "care_ble.h"

#define W 240
#define H 240
#define ROWS 20
#define SCLK GPIO_NUM_21
#define MOSI GPIO_NUM_47
#define DC GPIO_NUM_43
#define CS GPIO_NUM_44
#define BL GPIO_NUM_48
#define BUTTON_ADC_CHANNEL ADC_CHANNEL_0
#define CAM_PWDN GPIO_NUM_NC
#define CAM_RESET GPIO_NUM_NC
#define CAM_XCLK GPIO_NUM_15
#define CAM_SIOD GPIO_NUM_4
#define CAM_SIOC GPIO_NUM_5
#define CAM_D7 GPIO_NUM_16
#define CAM_D6 GPIO_NUM_17
#define CAM_D5 GPIO_NUM_18
#define CAM_D4 GPIO_NUM_12
#define CAM_D3 GPIO_NUM_10
#define CAM_D2 GPIO_NUM_8
#define CAM_D1 GPIO_NUM_9
#define CAM_D0 GPIO_NUM_11
#define CAM_VSYNC GPIO_NUM_6
#define CAM_HREF GPIO_NUM_7
#define CAM_PCLK GPIO_NUM_13
#define CAMERA_FRAME_W 320
#define CAMERA_FRAME_H 240
static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t done;
static SemaphoreHandle_t panel_lock;
static uint16_t *lcd_buffer;
static uint16_t *camera_lcd_buffer;
static uint8_t *camera_decode_buffer;
static volatile bool camera_ready;
static bool camera_init_failed;
static volatile bool preview_active;
static fall_detector_t fall_detector;
static SemaphoreHandle_t detector_lock;
static bool care_store_ready;
static bool manual_alert_test;
static bool reminder_due;
static uint16_t reminder_interval;
static int64_t reminder_started_us;
static lv_obj_t *monitor_label;
static uint32_t camera_frame_counter;
static uint32_t fall_alert_confirm_ms = 5000;

typedef enum {
    FALL_ALERT_IDLE,
    FALL_ALERT_COUNTDOWN,
    FALL_ALERT_ACTIVE
} fall_alert_state_t;

static volatile fall_alert_state_t fall_alert_state;
static volatile uint32_t fall_alert_started_ms;
static volatile bool alert_ui_refresh_pending;
static lv_obj_t *main_screen;
static uint32_t tick_ms(void);
static void wait_transfer(void);

static void fall_alert_start(void)
{
    xSemaphoreTake(detector_lock, portMAX_DELAY);
    if (fall_alert_state != FALL_ALERT_IDLE || (!manual_alert_test && (!fall_detector.possible_fall || !preview_active))) {
        xSemaphoreGive(detector_lock);
        return;
    }
    fall_alert_started_ms = tick_ms();
    fall_alert_state = FALL_ALERT_COUNTDOWN;
    preview_active = false;
    alert_ui_refresh_pending = true;
    if (care_store_ready) care_store_record(manual_alert_test ? CARE_EVENT_TEST_START : CARE_EVENT_CANDIDATE,
        fall_detector.posture_score, fall_detector.motion_score);
    xSemaphoreGive(detector_lock);
    care_portal_set_sharing(false);
    ESP_LOGW("eye", "Fall candidate detected; local alert countdown started");
}

static void fall_alert_cancel(void)
{
    preview_active = false;
    care_portal_set_sharing(false);
    care_config_t config;
    if (care_store_ready) care_store_get_config(&config);
    xSemaphoreTake(detector_lock, portMAX_DELAY);
    if (fall_alert_state != FALL_ALERT_IDLE && care_store_ready) {
        care_store_record(manual_alert_test ? CARE_EVENT_TEST_CANCEL : CARE_EVENT_CANCEL,
            fall_detector.posture_score, fall_detector.motion_score);
    }
    fall_alert_state = FALL_ALERT_IDLE;
    fall_alert_started_ms = 0;
    manual_alert_test = false;
    fall_detector_reset_tracking(&fall_detector);
    if (care_store_ready) {
        fall_detector.pose_threshold = config.pose_threshold;
        fall_detector.recovery_threshold = config.recovery_threshold;
        fall_detector.motion_limit = config.motion_limit;
        fall_detector.hold_samples = config.hold_samples;
        fall_alert_confirm_ms = config.confirm_seconds * 1000;
    }
    xSemaphoreGive(detector_lock);
    ESP_LOGI("eye", "Local fall alert cancelled and detector reset");
}

static void fall_alert_update(void)
{
    xSemaphoreTake(detector_lock, portMAX_DELAY);
    if (fall_alert_state != FALL_ALERT_COUNTDOWN) {
        xSemaphoreGive(detector_lock);
        return;
    }
    if (!manual_alert_test && !fall_detector.possible_fall) {
        fall_alert_state = FALL_ALERT_IDLE;
        fall_alert_started_ms = 0;
        alert_ui_refresh_pending = true;
        ESP_LOGI("eye", "Fall candidate cleared during confirmation");
        xSemaphoreGive(detector_lock);
        return;
    }
    if (tick_ms() - fall_alert_started_ms >= fall_alert_confirm_ms) {
        fall_alert_state = FALL_ALERT_ACTIVE;
        alert_ui_refresh_pending = true;
        if (care_store_ready) care_store_record(manual_alert_test ? CARE_EVENT_TEST_ALARM : CARE_EVENT_ALARM,
            fall_detector.posture_score, fall_detector.motion_score);
        ESP_LOGW("eye", "Local fall alert confirmed");
    }
    xSemaphoreGive(detector_lock);
}

static void format_alert_status(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) {
        return;
    }
    xSemaphoreTake(detector_lock, portMAX_DELAY);
    if (fall_alert_state == FALL_ALERT_COUNTDOWN) {
        uint32_t elapsed_ms = tick_ms() - fall_alert_started_ms;
        uint32_t remaining_ms = elapsed_ms >= fall_alert_confirm_ms ?
            0 : fall_alert_confirm_ms - elapsed_ms;
        uint32_t remaining_seconds = (remaining_ms + 999) / 1000;
        snprintf(buffer, buffer_size, "DEMO FALL CHECK\nConfirm in %lus\nPLAY: cancel",
            (unsigned long)remaining_seconds);
    } else if (fall_alert_state == FALL_ALERT_ACTIVE) {
        snprintf(buffer, buffer_size, "DEMO ALARM ACTIVE\nPLAY: cancel\nPose:%u%% Motion:%u%%",
            fall_detector.posture_score, fall_detector.motion_score);
    } else {
        snprintf(buffer, buffer_size, "Alert: STANDBY\nHistory on phone\nScreen-only demo");
    }
    xSemaphoreGive(detector_lock);
}

static void format_detection_status(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) {
        return;
    }
    xSemaphoreTake(detector_lock, portMAX_DELAY);
    if (!preview_active && !fall_detector.baseline_ready) {
        snprintf(buffer, buffer_size, "AI: STANDBY\nStart Camera preview\nSamples:%lu",
            (unsigned long)fall_detector.sampled_frames);
        xSemaphoreGive(detector_lock);
        return;
    }
    fall_detector_format_status(&fall_detector, buffer, buffer_size);
    xSemaphoreGive(detector_lock);
}

static void camera_start(void)
{
    camera_config_t config = {
        .pin_pwdn = CAM_PWDN, .pin_reset = CAM_RESET, .pin_xclk = CAM_XCLK,
        .pin_sccb_sda = CAM_SIOD, .pin_sccb_scl = CAM_SIOC,
        .pin_d7 = CAM_D7, .pin_d6 = CAM_D6, .pin_d5 = CAM_D5, .pin_d4 = CAM_D4,
        .pin_d3 = CAM_D3, .pin_d2 = CAM_D2, .pin_d1 = CAM_D1, .pin_d0 = CAM_D0,
        .pin_vsync = CAM_VSYNC, .pin_href = CAM_HREF, .pin_pclk = CAM_PCLK,
        .xclk_freq_hz = 20000000, .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG, .frame_size = FRAMESIZE_QVGA,
        .jpeg_quality = 12, .fb_count = 2, .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    if (camera_ready) {
        care_imu_init();
        return;
    }
    if (!camera_decode_buffer) {
        camera_decode_buffer = heap_caps_malloc(CAMERA_FRAME_W * CAMERA_FRAME_H * 3,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!camera_decode_buffer) {
            camera_init_failed = true;
            ESP_LOGE("eye", "Camera decode buffer allocation failed");
            return;
        }
    }
    esp_err_t err = esp_camera_init(&config);
    if (err == ESP_OK) {
        camera_ready = true;
        camera_init_failed = false;
        ESP_LOGI("eye", "Camera ready: OV2640 QVGA JPEG");
        esp_err_t imu_result = care_imu_init();
        if (imu_result != ESP_OK) ESP_LOGW("eye", "Accelerometer unavailable: %s", esp_err_to_name(imu_result));
    } else {
        camera_init_failed = true;
        ESP_LOGE("eye", "Camera init failed: %s", esp_err_to_name(err));
    }
}
static void camera_stop(void)
{
    if (preview_active && care_store_ready) care_store_record(CARE_EVENT_MONITOR_STOP, 0, 0);
    preview_active = false;
    care_portal_set_sharing(false);
    if (camera_ready) ESP_LOGI("eye", "Camera preview stopped");
}
static void camera_draw_rgb888(const uint8_t *frame, uint16_t width, uint16_t height)
{
    if (!camera_lcd_buffer || width < W || height < H) {
        ESP_LOGW("eye", "Decoded frame unsuitable: size=%ux%u", width, height);
        return;
    }
    int left = (width - W) / 2;
    for (int y = 0; y < H && preview_active; y += ROWS) {
        for (int row = 0; row < ROWS && y + row < height; ++row) {
            const uint8_t *source = frame + ((size_t)(y + row) * width + left) * 3;
            for (int x = 0; x < W; ++x) {
                uint8_t red = source[x * 3];
                uint8_t green = source[x * 3 + 1];
                uint8_t blue = source[x * 3 + 2];
                camera_lcd_buffer[row * W + x] =
                    ((uint16_t)(red & 0xf8) << 8) |
                    ((uint16_t)(green & 0xfc) << 3) |
                    (blue >> 3);
            }
        }
        if (xSemaphoreTake(panel_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
            ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, y, W, y + ROWS, camera_lcd_buffer));
            wait_transfer();
            xSemaphoreGive(panel_lock);
        }
    }
}
static void camera_task(void *arg)
{
    (void)arg;
    while (true) {
        if (!camera_ready || !preview_active) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW("eye", "Camera capture failed");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (fb->format == PIXFORMAT_JPEG && camera_decode_buffer) {
            esp_jpeg_image_cfg_t jpeg_cfg = {
                .indata = fb->buf,
                .indata_size = fb->len,
                .outbuf = camera_decode_buffer,
                .outbuf_size = CAMERA_FRAME_W * CAMERA_FRAME_H * 3,
                .out_format = JPEG_IMAGE_FORMAT_RGB888,
                .out_scale = JPEG_IMAGE_SCALE_0,
            };
            esp_jpeg_image_output_t image = {0};
            esp_err_t decode_err = esp_jpeg_decode(&jpeg_cfg, &image);
            if (decode_err == ESP_OK) {
                if (preview_active) care_portal_publish_jpeg(fb->buf, fb->len);
                camera_draw_rgb888(camera_decode_buffer, image.width, image.height);
                if ((++camera_frame_counter % 5) == 0) {
                    xSemaphoreTake(detector_lock, portMAX_DELAY);
                    if (preview_active) fall_detector_process_frame(&fall_detector, camera_decode_buffer, image.width, image.height);
                    bool possible_fall = fall_detector.possible_fall;
                    xSemaphoreGive(detector_lock);
                    if (possible_fall) {
                        fall_alert_start();
                    }
                }
            } else {
                ESP_LOGW("eye", "JPEG decode failed: %s", esp_err_to_name(decode_err));
            }
        } else {
            ESP_LOGW("eye", "Camera frame unsuitable: format=%d size=%ux%u", fb->format, fb->width, fb->height);
        }
        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
static uint32_t tick_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void wait_transfer(void) { ESP_ERROR_CHECK(xSemaphoreTake(done, pdMS_TO_TICKS(2000)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT); }
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p){xSemaphoreTake(panel_lock,portMAX_DELAY);ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel,a->x1,a->y1,a->x2+1,a->y2+1,p));wait_transfer();xSemaphoreGive(panel_lock);lv_display_flush_ready(d);}
static bool txdone(esp_lcd_panel_io_handle_t io,esp_lcd_panel_io_event_data_t *e,void *ctx){BaseType_t w=pdFALSE;(void)io;(void)e;(void)ctx;xSemaphoreGiveFromISR(done,&w);return w==pdTRUE;}
static void lcd_start(void){gpio_config_t g={.pin_bit_mask=1ULL<<BL,.mode=GPIO_MODE_OUTPUT};ESP_ERROR_CHECK(gpio_config(&g));gpio_set_level(BL,1);done=xSemaphoreCreateBinary();panel_lock=xSemaphoreCreateMutex();ESP_ERROR_CHECK(done && panel_lock ? ESP_OK : ESP_ERR_NO_MEM);lcd_buffer=heap_caps_malloc(W*ROWS*2,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);camera_lcd_buffer=heap_caps_malloc(W*ROWS*2,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);ESP_ERROR_CHECK(lcd_buffer && camera_lcd_buffer ? ESP_OK : ESP_ERR_NO_MEM);uint16_t *buf=lcd_buffer;spi_bus_config_t b={.sclk_io_num=SCLK,.mosi_io_num=MOSI,.miso_io_num=-1,.quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=W*ROWS*2};ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST,&b,SPI_DMA_CH_AUTO));esp_lcd_panel_io_spi_config_t io_cfg={.dc_gpio_num=DC,.cs_gpio_num=CS,.pclk_hz=10000000,.lcd_cmd_bits=8,.lcd_param_bits=8,.trans_queue_depth=1,.on_color_trans_done=txdone};esp_lcd_panel_io_handle_t io;ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,&io_cfg,&io));esp_lcd_panel_dev_config_t pc={.reset_gpio_num=-1,.rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB,.data_endian=LCD_RGB_DATA_ENDIAN_LITTLE,.bits_per_pixel=16};ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io,&pc,&panel));ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));ESP_ERROR_CHECK(esp_lcd_panel_init(panel));ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel,true));ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel,true));ESP_ERROR_CHECK(gpio_set_level(BL,0));ESP_LOGI("eye","Backlight ON; starting RGB test");for(int c=0;c<3;c++){uint16_t colors[3] = {0xf800, 0x07e0, 0x001f}; uint16_t color = colors[c];for(int i=0;i<W*ROWS;i++)buf[i]=color;for(int y=0;y<H;y+=ROWS){ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel,0,y,W,y+ROWS,buf));wait_transfer();}vTaskDelay(pdMS_TO_TICKS(500));}lv_init();lv_tick_set_cb(tick_ms);lv_display_t *d=lv_display_create(W,H);lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);lv_display_set_buffers(d,buf,NULL,W*ROWS*2,LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);}
static lv_obj_t *time_label;
static lv_obj_t *page_label;
static lv_obj_t *status_label;
static lv_obj_t *key_label;
static adc_oneshot_unit_handle_t adc_handle;
static int page_index;
static int selection;
static bool detail_open;

typedef enum {
    KEY_UNKNOWN = -1, KEY_RELEASED, KEY_MENU, KEY_PLAY, KEY_UP, KEY_DOWN
} care_key_t;

static care_key_t classify_key(int raw)
{
    if (raw >= 3900) return KEY_RELEASED;
    if (raw >= 2700 && raw <= 3100) return KEY_MENU;
    if (raw >= 2150 && raw <= 2550) return KEY_PLAY;
    if (raw >= 250 && raw <= 600) return KEY_UP;
    if (raw >= 750 && raw <= 1150) return KEY_DOWN;
    return KEY_UNKNOWN;
}

static void render_page(void)
{
    static const char *titles[] = {"HOME", "CARE", "ENVIRONMENT", "SETTINGS"};
    static const char *items[4][3] = {
        {"Care status", "Environment", "Settings"},
        {"Camera preview", "Fall detection", "Local alert"},
        {"Temperature", "Humidity", "Smoke sensor"},
        {"Network", "System", "Privacy"}
    };
    static const char *details[4][3] = {
        {"Care: NOT READY", "Sensors: NOT READY", "Settings"},
        {"Camera: NOT STARTED\nNo images captured", "AI: NOT LOADED\nDetection unavailable", "Alarm: NOT CONNECTED\nNo alert was sent"},
        {"Temperature: --\nSensor not connected", "Humidity: --\nSensor not connected", "Smoke: --\nSensor not connected"},
        {"Wi-Fi: NOT CONNECTED\nNo network configured", "ESP32-S3-EYE\nESP-IDF / LVGL 9\nNot OpenVela yet", "Video streaming: OFF\nCamera capture: OFF"}
    };
    uint32_t background_color = 0x102333;
    uint32_t status_color = 0xffffff;
    if (fall_alert_state == FALL_ALERT_COUNTDOWN) {
        background_color = 0x4b3b16;
        status_color = 0xffd166;
    } else if (fall_alert_state == FALL_ALERT_ACTIVE) {
        background_color = 0x4b1515;
        status_color = 0xff5964;
    }
    if (main_screen) {
        lv_obj_set_style_bg_color(main_screen, lv_color_hex(background_color), 0);
    }
    lv_obj_set_style_text_color(status_label, lv_color_hex(status_color), 0);
    lv_label_set_text_fmt(page_label, "%d/4  %s", page_index + 1, titles[page_index]);
    if (fall_alert_state != FALL_ALERT_IDLE || (reminder_due && !preview_active)) {
        char alert_status[96];
        if (fall_alert_state != FALL_ALERT_IDLE) format_alert_status(alert_status, sizeof(alert_status));
        else snprintf(alert_status, sizeof(alert_status), "CARE REMINDER\nPlease check in\nPLAY: acknowledge");
        lv_label_set_text(status_label, alert_status);
        return;
    }
    if (detail_open) {
        if (fall_alert_state != FALL_ALERT_IDLE) {
            char alert_status[96];
            format_alert_status(alert_status, sizeof(alert_status));
            lv_label_set_text(status_label, alert_status);
        } else if (page_index == 1 && selection == 0) {
            lv_label_set_text(status_label, preview_active ?
                "Camera: LIVE\nPLAY: stop preview" :
                camera_ready ? "Camera: READY\nPLAY: live preview" :
                camera_init_failed ? "Camera: ERROR\nCheck camera connection" :
                "Camera: STANDBY\nPLAY: start preview");
        } else if (page_index == 1 && selection == 1) {
            char detector_status[96];
            if (fall_alert_state != FALL_ALERT_IDLE) {
                format_alert_status(detector_status, sizeof(detector_status));
            } else {
                format_detection_status(detector_status, sizeof(detector_status));
            }
            lv_label_set_text(status_label, detector_status);
        } else if (page_index == 1 && selection == 2) {
            char alert_status[96];
            format_alert_status(alert_status, sizeof(alert_status));
            lv_label_set_text(status_label, alert_status);
        } else if (page_index == 0 && selection == 1) {
            care_imu_status_t imu;
            care_imu_get_status(&imu);
            lv_label_set_text_fmt(status_label, "QMA6100P: %s\nX:%d Y:%d Z:%d mg\nMag:%u Shock:%u",
                imu.ready ? "READY" : "NOT FOUND", imu.x_mg, imu.y_mg, imu.z_mg,
                imu.magnitude_mg, imu.shock_mg);
        } else if (page_index == 3 && selection == 0) {
            char network_status[96];
            care_portal_network_text(network_status, sizeof(network_status));
            lv_label_set_text(status_label, network_status);
        } else if (page_index == 3 && selection == 1) {
            lv_label_set_text_fmt(status_label, "ESP-IDF / DEMO\nHeap: %lu KB\nStore errors: %lu",
                (unsigned long)(esp_get_free_heap_size() / 1024),
                (unsigned long)care_store_error_count());
        } else if (page_index == 3 && selection == 2) {
            lv_label_set_text_fmt(status_label, "Video share: %s\nCapture: %s\nLAN only / no cloud",
                care_portal_sharing() ? "ON" : "OFF", preview_active ? "ON" : "OFF");
        } else {
            lv_label_set_text(status_label, details[page_index][selection]);
        }
    } else {
        lv_label_set_text_fmt(status_label, "%s %s\n%s %s\n%s %s",
            selection == 0 ? ">" : " ", items[page_index][0],
            selection == 1 ? ">" : " ", items[page_index][1],
            selection == 2 ? ">" : " ", items[page_index][2]);
    }
}

static void handle_key(care_key_t key, int raw)
{
    const char *name = "UNKNOWN";
    if (key == KEY_PLAY && fall_alert_state != FALL_ALERT_IDLE) {
        name = "ALERT CANCEL";
        fall_alert_cancel();
        if (page_index == 1) {
            selection = 0;
            detail_open = true;
        }
        lv_obj_invalidate(main_screen);
    } else if (key == KEY_PLAY && reminder_due) {
        name = "REMINDER ACK";
        reminder_due = false;
        reminder_started_us = esp_timer_get_time();
        if (care_store_ready) care_store_record(CARE_EVENT_REMINDER_ACK, 0, 0);
        lv_obj_invalidate(main_screen);
    } else if (key == KEY_MENU) {
        name = "MENU";
        camera_stop();
        preview_active = false;
        page_index = (page_index + 1) % 4;
        selection = 0;
        detail_open = false;
    } else if (key == KEY_PLAY) {
        name = "PLAY";
        if (page_index == 0) {
            page_index = selection + 1;
            selection = 0;
        } else if (page_index == 1 && selection == 0) {
            if (!detail_open) {
                detail_open = true;
                camera_start();
                preview_active = false;
            } else if (!camera_ready) {
                camera_start();
                preview_active = false;
            } else if (!preview_active) {
                fall_alert_cancel();
                preview_active = true;
                if (care_store_ready) care_store_record(CARE_EVENT_MONITOR_START, 0, 0);
            } else {
                detail_open = false;
                camera_stop();
            }
        } else {
            detail_open = !detail_open;
            if (!detail_open) camera_stop();
            preview_active = false;
        }
    } else if (key == KEY_UP || key == KEY_DOWN) {
        name = key == KEY_UP ? "CP+" : "CW-";
        camera_stop();
        detail_open = false;
        preview_active = false;
        selection = (selection + (key == KEY_UP ? 2 : 1)) % 3;
    }
    render_page();
    if (page_index == 1 && selection == 0 && detail_open) lv_obj_invalidate(lv_screen_active());
    lv_label_set_text_fmt(key_label, "%s  ADC:%d", name, raw);
    ESP_LOGI("eye", "Key %s ADC=%d page=%d item=%d", name, raw, page_index, selection);
}

static void button_timer(lv_timer_t *timer)
{
    (void)timer;
    static care_key_t candidate = KEY_UNKNOWN;
    static unsigned stable_samples;
    static bool armed;
    int samples[5];
    for (int index = 0; index < 5; ++index) {
        if (adc_oneshot_read(adc_handle, BUTTON_ADC_CHANNEL, &samples[index]) != ESP_OK) {
            stable_samples = 0;
            candidate = KEY_UNKNOWN;
            return;
        }
    }
    for (int index = 1; index < 5; ++index) {
        int value = samples[index];
        int position = index;
        while (position > 0 && samples[position - 1] > value) {
            samples[position] = samples[position - 1];
            --position;
        }
        samples[position] = value;
    }
    care_key_t key = classify_key(samples[2]);
    if (key != candidate) {
        candidate = key;
        stable_samples = 1;
        return;
    }
    if (stable_samples < 3) ++stable_samples;
    if (stable_samples < 3) return;
    if (key == KEY_RELEASED) {
        armed = true;
    } else if (key != KEY_UNKNOWN && armed) {
        armed = false;
        handle_key(key, samples[2]);
    }
}

static void handle_remote_commands(void)
{
    care_command_t command;
    while (care_portal_next_command(&command)) {
        ESP_LOGI("care_control", "Remote command: %d", command);
        if (command == CARE_COMMAND_START && fall_alert_state == FALL_ALERT_IDLE) {
            camera_stop();
            camera_start();
            if (camera_ready) {
                fall_alert_cancel();
                preview_active = true;
                if (care_store_ready) care_store_record(CARE_EVENT_MONITOR_START, 0, 0);
            }
            page_index = 1;
            selection = 0;
            detail_open = true;
        } else if (command == CARE_COMMAND_STOP) {
            camera_stop();
            page_index = 1;
            selection = 0;
            detail_open = true;
        } else if (command == CARE_COMMAND_CANCEL) {
            camera_stop();
            fall_alert_cancel();
            page_index = 1;
            selection = 0;
            detail_open = true;
        } else if (command == CARE_COMMAND_TEST && fall_alert_state == FALL_ALERT_IDLE) {
            camera_stop();
            fall_alert_cancel();
            manual_alert_test = true;
            fall_alert_start();
        } else if (command == CARE_COMMAND_SHARE_ON && preview_active) {
            esp_err_t result = care_portal_set_sharing(true);
            if (result != ESP_OK) ESP_LOGE("care_control", "Sharing unavailable: %s", esp_err_to_name(result));
        } else if (command == CARE_COMMAND_SHARE_OFF) {
            care_portal_set_sharing(false);
        } else if (command == CARE_COMMAND_REMINDER_ACK && reminder_due) {
            reminder_due = false;
            reminder_started_us = esp_timer_get_time();
            if (care_store_ready) care_store_record(CARE_EVENT_REMINDER_ACK, 0, 0);
        }
        render_page();
        if (!preview_active) lv_obj_invalidate(main_screen);
    }
}

static void publish_care_status(void)
{
    if (care_store_ready) {
        care_config_t config;
        care_store_get_config(&config);
        if (reminder_interval != config.reminder_minutes) {
            reminder_interval = config.reminder_minutes;
            reminder_started_us = esp_timer_get_time();
            bool was_due = reminder_due;
            reminder_due = false;
            if (was_due && !preview_active) {
                render_page();
                lv_obj_invalidate(main_screen);
            }
        }
        if (reminder_interval && !reminder_due &&
            esp_timer_get_time() - reminder_started_us >= reminder_interval * 60000000LL) {
            reminder_due = true;
            care_store_record(CARE_EVENT_REMINDER, 0, 0);
            if (!preview_active) {
                render_page();
                lv_obj_invalidate(main_screen);
            }
        }
    }
    xSemaphoreTake(detector_lock, portMAX_DELAY);
    care_audio_status_t audio_status;
    care_audio_get_status(&audio_status);
    care_imu_status_t imu_status;
    care_imu_get_status(&imu_status);
    uint32_t elapsed_ms = tick_ms() - fall_alert_started_ms;
    care_status_t status = {
        .camera_ready = camera_ready,
        .camera_error = camera_init_failed,
        .monitoring = preview_active,
        .baseline_ready = fall_detector.baseline_ready,
        .baseline_samples = fall_detector.baseline_samples,
        .pose = fall_detector.posture_score,
        .motion = fall_detector.motion_score,
        .samples = fall_detector.sampled_frames,
        .alert_state = fall_alert_state,
        .test_alert = manual_alert_test,
        .reminder_due = reminder_due,
        .audio_ready = audio_status.ready,
        .audio_level = audio_status.level,
        .audio_samples = audio_status.samples,
        .audio_errors = audio_status.errors,
        .imu_ready = imu_status.ready,
        .imu_x_mg = imu_status.x_mg,
        .imu_y_mg = imu_status.y_mg,
        .imu_z_mg = imu_status.z_mg,
        .imu_magnitude_mg = imu_status.magnitude_mg,
        .imu_shock_mg = imu_status.shock_mg,
        .imu_samples = imu_status.samples,
        .imu_errors = imu_status.errors,
        .ble_ready = care_ble_ready(),
        .confirm_remaining = fall_alert_state == FALL_ALERT_COUNTDOWN && elapsed_ms < fall_alert_confirm_ms ?
            (fall_alert_confirm_ms - elapsed_ms + 999) / 1000 : 0,
    };
    xSemaphoreGive(detector_lock);
    care_portal_publish_status(&status);
    care_indicator_set_mode(fall_alert_state == FALL_ALERT_ACTIVE ? CARE_INDICATOR_ALARM :
        fall_alert_state == FALL_ALERT_COUNTDOWN ? CARE_INDICATOR_COUNTDOWN :
        preview_active ? CARE_INDICATOR_MONITORING : CARE_INDICATOR_HOTSPOT);
    lv_label_set_text(monitor_label, care_portal_sharing() ? "VIDEO SHARED / 5min max" :
        reminder_due ? "Care reminder: PLAY" : preview_active ? "DEMO monitoring active" : "Monitoring paused / DEMO");
}

static void update_home(lv_timer_t *timer)
{
    (void)timer;
    handle_remote_commands();
    fall_alert_update();
    publish_care_status();
    uint32_t seconds = tick_ms() / 1000;
    lv_label_set_text_fmt(time_label, "UP %02lu:%02lu", (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
    if (alert_ui_refresh_pending || fall_alert_state != FALL_ALERT_IDLE) {
        alert_ui_refresh_pending = false;
        if (main_screen) {
            lv_obj_invalidate(main_screen);
        }
        render_page();
    } else if (detail_open && ((page_index == 0 && selection == 1) || (page_index == 1 && selection == 1) || page_index == 3)) {
        render_page();
    }
}

static lv_obj_t *home_label(lv_obj_t *screen, const char *text, int top, uint32_t color)
{
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, 212);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 14, top);
    return label;
}

void app_main(void)
{
    ESP_LOGI("eye", "start reset=%d", esp_reset_reason());
    ESP_ERROR_CHECK(fall_detector_init(&fall_detector));
    detector_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(detector_lock ? ESP_OK : ESP_ERR_NO_MEM);
    esp_err_t store_result = care_store_init();
    care_store_ready = store_result == ESP_OK;
    if (!care_store_ready) ESP_LOGE("care", "Storage unavailable (not erased): %s", esp_err_to_name(store_result));
    esp_err_t audio_result = care_audio_init();
    if (audio_result != ESP_OK) ESP_LOGW("audio", "Microphone unavailable: %s", esp_err_to_name(audio_result));
    lcd_start();
    esp_err_t indicator_result = care_indicator_init();
    if (indicator_result != ESP_OK) ESP_LOGW("indicator", "Status LED unavailable: %s", esp_err_to_name(indicator_result));
    ESP_ERROR_CHECK(xTaskCreate(camera_task, "camera", 6144, NULL, 2, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    adc_oneshot_unit_init_cfg_t init = {.unit_id = ADC_UNIT_1};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init, &adc_handle));
    adc_oneshot_chan_cfg_t config = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, BUTTON_ADC_CHANNEL, &config));
    main_screen = lv_screen_active();
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(0x102333), 0);
    lv_obj_set_style_bg_opa(main_screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(main_screen, LV_OBJ_FLAG_SCROLLABLE);
    home_label(main_screen, "AI CARE TERMINAL", 8, 0xffffff);
    page_label = home_label(main_screen, "", 32, 0x42dfba);
    time_label = home_label(main_screen, "UP 00:00", 54, 0xffffff);
    status_label = home_label(main_screen, "", 82, 0xffffff);
    lv_obj_set_style_text_line_space(status_label, 8, 0);
    home_label(main_screen, "MENU: page  PLAY: enter", 162, 0xa8bed0);
    home_label(main_screen, "CP+: up  CW-: down", 181, 0xa8bed0);
    monitor_label = home_label(main_screen, "Monitoring paused / DEMO", 200, 0xffd166);
    key_label = home_label(main_screen, "Release keys to start", 219, 0x42dfba);
    if (care_store_ready) {
        esp_err_t network_result = care_portal_init();
        if (network_result == ESP_OK) {
            page_index = 3;
            selection = 0;
            detail_open = true;
        } else ESP_LOGE("care", "Portal unavailable: %s", esp_err_to_name(network_result));
    }
    esp_err_t ble_result = care_ble_init();
    if (ble_result != ESP_OK) ESP_LOGW("ble", "Bluetooth unavailable: %s", esp_err_to_name(ble_result));
    reminder_started_us = esp_timer_get_time();
    render_page();
    lv_timer_create(update_home, 1000, NULL);
    lv_timer_create(button_timer, 25, NULL);
    ESP_LOGI("care", "Firmware care-portal-1 ready; local screen alert test available in browser");
    while (1) {
        uint32_t delay_ms = lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(delay_ms > 20 ? 20 : delay_ms < 1 ? 1 : delay_ms));
    }
}
