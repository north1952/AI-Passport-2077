// recording_app.c — 会议录音小程序
// 从 ES8311 采集 PCM 16kHz/16bit/mono，通过 UDP 推流到 ASR 服务器
// 交互：OK 开始/停止录音，长 OK 返回
#include "recording_app.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "badge_theme.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "rec_app";

// ASR 服务器地址（AgentServer）
#define REC_SERVER_IP   "10.19.102.102"
#define REC_SERVER_PORT 10096

// 音频参数
#define REC_SAMPLE_RATE  16000
#define REC_BITS         16
#define REC_CHANNELS     1
#define REC_CHUNK_MS     100  // 每 100ms 一个 UDP 包
#define REC_CHUNK_SAMPLES (REC_SAMPLE_RATE * REC_CHUNK_MS / 1000)  // 1600 samples
#define REC_CHUNK_BYTES   (REC_CHUNK_SAMPLES * 2)                   // 3200 bytes

static lv_obj_t *s_scr, *s_status, *s_timer, *s_dot;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_done;
static volatile bool s_recording;
static int s_sock = -1;

// 更新状态文字（从音频任务调用，需加 LVGL 锁）
static void ui_set_status(const char *text) {
    if (!bsp_lvgl_lock(200)) return;
    if (s_status) lv_label_set_text(s_status, text);
    bsp_lvgl_unlock();
}

static void ui_set_timer(int seconds) {
    if (!bsp_lvgl_lock(200)) return;
    if (s_timer) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d:%02d", seconds / 60, seconds % 60);
        lv_label_set_text(s_timer, buf);
    }
    bsp_lvgl_unlock();
}

// 录音任务：循环读 PCM → UDP 发送
static void recording_task(void *arg) {
    (void)arg;
    int16_t *buf = malloc(REC_CHUNK_BYTES);
    if (!buf) {
        ESP_LOGE(TAG, "Buffer alloc failed");
        if (s_done) xSemaphoreGive(s_done);
        vTaskDelete(NULL);
        return;
    }

    // 创建 UDP socket
    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "Socket create failed");
        free(buf);
        if (s_done) xSemaphoreGive(s_done);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(REC_SERVER_PORT);
    inet_aton(REC_SERVER_IP, &addr.sin_addr);

    if (connect(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "UDP connect to %s:%d failed", REC_SERVER_IP, REC_SERVER_PORT);
        close(s_sock);
        s_sock = -1;
        free(buf);
        if (s_done) xSemaphoreGive(s_done);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Recording → %s:%d", REC_SERVER_IP, REC_SERVER_PORT);
    uint32_t start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t last_tick = 0;

    while (s_recording) {
        // 读取 PCM
        esp_err_t err = bsp_audio_read(buf, REC_CHUNK_BYTES);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Audio read error: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // UDP 发送
        if (send(s_sock, buf, REC_CHUNK_BYTES, 0) < 0) {
            ESP_LOGW(TAG, "UDP send failed, retrying");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // 每秒更新计时器
        uint32_t elapsed = ((uint32_t)(esp_timer_get_time() / 1000) - start_ms) / 1000;
        if (elapsed != last_tick) {
            last_tick = elapsed;
            ui_set_timer((int)elapsed);
        }
    }

    close(s_sock);
    s_sock = -1;
    free(buf);
    ESP_LOGI(TAG, "Recording stopped, %u seconds", (unsigned)last_tick);
    if (s_done) xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

void demo_recording_enter(void) {
    const uint32_t *c = badge_theme_colors();

    s_scr = ui_pixel_screen_create("会议录音");
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 18, 58, 204, 200, UI_PAPER);

    // 录音指示点（红色圆点）
    s_dot = lv_obj_create(panel);
    lv_obj_set_size(s_dot, 20, 20);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(0xFF3333), 0);
    lv_obj_set_style_border_width(s_dot, 0, 0);
    lv_obj_align(s_dot, LV_ALIGN_TOP_MID, -60, 16);

    // 计时器（大字）
    s_timer = lv_label_create(panel);
    lv_obj_set_style_text_color(s_timer, lv_color_hex(c[BADGE_TEXT]), 0);
    lv_obj_set_style_text_font(s_timer, &font_badge_28, 0);
    lv_label_set_text(s_timer, "0:00");
    lv_obj_align(s_timer, LV_ALIGN_TOP_MID, 20, 12);

    // 状态文字
    s_status = lv_label_create(panel);
    lv_obj_set_style_text_color(s_status, lv_color_hex(c[BADGE_MUTED]), 0);
    lv_obj_set_style_text_font(s_status, &font_ui_14, 0);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_status, "按 OK 开始录音");
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -30);

    // 提示
    lv_obj_t *hint = lv_label_create(panel);
    lv_obj_set_style_text_color(hint, lv_color_hex(c[BADGE_MUTED]), 0);
    lv_obj_set_style_text_font(hint, &font_ui_14, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(hint, "OK 开始/停止  长OK 返回");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    lv_screen_load(s_scr);
}

void demo_recording_exit(void) {
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = s_status = s_timer = s_dot = NULL;
    }
}

void demo_recording_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        if (!s_recording) {
            demo_recording_start();
        } else {
            demo_recording_stop();
        }
    }
}

esp_err_t demo_recording_start(void) {
    if (s_recording) return ESP_OK;

    // 唤醒音频
    if (bsp_audio_is_sleeping()) {
        bsp_audio_wake();
    }
    if (bsp_audio_set_format(REC_SAMPLE_RATE, REC_BITS, REC_CHANNELS) != ESP_OK) {
        ui_set_status("音频初始化失败");
        return ESP_FAIL;
    }

    // 静音输出（避免回声）
    bsp_audio_set_volume(0);

    // 创建完成信号量
    if (s_done) vSemaphoreDelete(s_done);
    s_done = xSemaphoreCreateBinary();
    if (!s_done) return ESP_ERR_NO_MEM;

    s_recording = true;
    if (xTaskCreate(recording_task, "rec_audio", 4096, NULL, 5, &s_task) != pdPASS) {
        s_recording = false;
        vSemaphoreDelete(s_done);
        s_done = NULL;
        ui_set_status("任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    // 更新 UI
    if (bsp_lvgl_lock(200)) {
        if (s_dot) lv_obj_set_style_bg_color(s_dot, lv_color_hex(0xFF3333), 0);
        if (s_status) lv_label_set_text(s_status, "录音中... 按 OK 停止");
        bsp_lvgl_unlock();
    }

    ESP_LOGI(TAG, "Recording started");
    return ESP_OK;
}

esp_err_t demo_recording_stop(void) {
    if (!s_recording) return ESP_OK;

    s_recording = false;

    // 等待任务退出（最多 3 秒）
    if (s_done) {
        xSemaphoreTake(s_done, pdMS_TO_TICKS(3000));
        vSemaphoreDelete(s_done);
        s_done = NULL;
    }
    s_task = NULL;

    // 恢复音量
    bsp_audio_set_volume(40);

    // 更新 UI
    if (bsp_lvgl_lock(200)) {
        if (s_dot) lv_obj_set_style_bg_color(s_dot, lv_color_hex(0x666666), 0);
        if (s_status) lv_label_set_text(s_status, "录音结束  按 OK 再来");
        bsp_lvgl_unlock();
    }

    ESP_LOGI(TAG, "Recording stopped");
    return ESP_OK;
}
