// recording_app.h — 会议录音小程序
// PCM 16kHz/16bit/mono → UDP 推流到 ASR 服务器
#pragma once
#include "demo.h"
#include "bsp_button.h"

void demo_recording_enter(void);
void demo_recording_exit(void);
void demo_recording_key(bsp_btn_t btn, bsp_btn_ev_t ev);
esp_err_t demo_recording_start(void);
esp_err_t demo_recording_stop(void);
