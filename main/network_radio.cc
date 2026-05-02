#include "network_radio.h"
#include "board.h"
#include "display/display.h"
#include "audio/audio_codec.h"

#include <esp_log.h>
#include <esp_ae_types.h>
#include <esp_ae_rate_cvt.h>
#include <esp_audio_types.h>
#include <aac_decoder.h>
#include <cmath>
#include <cstring>

#define TAG "NetworkRadio"

NetworkRadio::NetworkRadio()
    : task_handle_(nullptr)
    , running_(false)
    , switch_requested_(false)
    , switch_direction_(0)
    , current_station_(0)
    , stations_(nullptr)
    , station_count_(0) {
}

NetworkRadio::~NetworkRadio() {
    Stop();
}

void NetworkRadio::SetStations(const Station* stations, size_t count) {
    stations_ = stations;
    station_count_ = count;
}

void NetworkRadio::Start(int station_index) {
    if (running_) return;

    current_station_ = station_index % station_count_;
    running_ = true;
    switch_requested_ = false;
    switch_direction_ = 0;

    auto ret = xTaskCreate([](void* arg) {
        auto* radio = static_cast<NetworkRadio*>(arg);
        radio->TaskLoop();
        radio->running_ = false;
        radio->task_handle_ = nullptr;
        vTaskDelete(nullptr);
    }, "radio_task", 6 * 1024, this, 5, &task_handle_);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create radio task");
        running_ = false;
        task_handle_ = nullptr;
    }
}

void NetworkRadio::Stop() {
    if (!running_) return;
    running_ = false;
    while (task_handle_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void NetworkRadio::NextStation() {
    if (!running_) return;
    switch_direction_ = 1;
    switch_requested_ = true;
}

void NetworkRadio::PreviousStation() {
    if (!running_) return;
    switch_direction_ = -1;
    switch_requested_ = true;
}

const char* NetworkRadio::GetCurrentStationName() const {
    if (!stations_ || station_count_ == 0) return "";
    return stations_[current_station_].name;
}

void NetworkRadio::ShowStationInfo() {
    auto display = Board::GetInstance().GetDisplay();
    if (display && stations_ && station_count_ > 0) {
        display->ShowNotification(stations_[current_station_].name);
    }
}

void NetworkRadio::TaskLoop() {
    auto codec = Board::GetInstance().GetAudioCodec();
    codec->EnableOutput(true);

    int output_rate = codec->output_sample_rate();

    // Play a short beep to verify audio output path
    {
        float duration = 0.3f;
        float freq = 880.0f;
        int total_samples = (int)(output_rate * duration);
        std::vector<int16_t> beep(total_samples);
        for (int i = 0; i < total_samples; i++) {
            beep[i] = (int16_t)(sinf(2.0f * 3.14159265f * freq * i / output_rate) * 8192);
        }
        codec->OutputData(beep);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    while (running_) {
        if (switch_requested_) {
            int dir = switch_direction_.load();
            current_station_ = (dir == 1)
                ? (current_station_ + 1) % station_count_
                : (current_station_ == 0) ? station_count_ - 1 : current_station_ - 1;
            switch_requested_ = false;
            switch_direction_ = 0;
            ShowStationInfo();
        }

        if (!stations_ || station_count_ == 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        const char* playlist_url = stations_[current_station_].url;
        ESP_LOGI(TAG, "Playing: %s [%s]", stations_[current_station_].name, playlist_url);

        ShowStationInfo();

        // Build pipeline: http_stream -> aac_decoder
        audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
        audio_pipeline_handle_t pipeline = audio_pipeline_init(&pipeline_cfg);
        if (!pipeline) {
            ESP_LOGE(TAG, "Failed to create pipeline");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
        http_cfg.enable_playlist_parser = true;
        http_cfg.type = AUDIO_STREAM_READER;
        http_cfg.out_rb_size = 20 * 1024;
        audio_element_handle_t http = http_stream_init(&http_cfg);
        if (!http) {
            ESP_LOGE(TAG, "Failed to create http_stream");
            audio_pipeline_deinit(pipeline);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        audio_element_set_uri(http, playlist_url);

        aac_decoder_cfg_t aac_cfg = DEFAULT_AAC_DECODER_CONFIG();
        aac_cfg.plus_enable = true;
        aac_cfg.out_rb_size = 32 * 1024;
        audio_element_handle_t aac = aac_decoder_init(&aac_cfg);
        if (!aac) {
            ESP_LOGE(TAG, "Failed to create aac_decoder");
            audio_element_deinit(http);
            audio_pipeline_deinit(pipeline);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        audio_pipeline_register(pipeline, http, "http");
        audio_pipeline_register(pipeline, aac, "aac");

        const char* link_tag[2] = {"http", "aac"};
        audio_pipeline_link(pipeline, link_tag, 2);

        // Create output ring buffer for the last element (pipeline doesn't create for last)
        ringbuf_handle_t aac_output_rb = rb_create(audio_element_get_output_ringbuf_size(aac), 1);
        if (aac_output_rb) {
            audio_element_set_output_ringbuf(aac, aac_output_rb);
        }

        // Read PCM directly from AAC decoder output ring buffer
        ringbuf_handle_t rb = audio_element_get_output_ringbuf(aac);

        // Set up resampler (esp_ae_rate_cvt, no FIR conflict with filter_resample)
        audio_element_info_t aac_info = {};
        audio_element_getinfo(aac, &aac_info);
        int src_rate = aac_info.sample_rates > 0 ? aac_info.sample_rates : 44100;
        int src_ch = aac_info.channels > 0 ? aac_info.channels : 2;

        ESP_LOGI(TAG, "AAC output: %d Hz, %d ch, resampling to %d Hz", src_rate, src_ch, output_rate);

        esp_ae_rate_cvt_handle_t resampler = nullptr;
        if (src_rate != output_rate) {
            esp_ae_rate_cvt_cfg_t rsp_cfg;
            memset(&rsp_cfg, 0, sizeof(rsp_cfg));
            rsp_cfg.src_rate = src_rate;
            rsp_cfg.dest_rate = output_rate;
            rsp_cfg.channel = src_ch;
            rsp_cfg.bits_per_sample = ESP_AUDIO_BIT16;
            rsp_cfg.complexity = 2;
            rsp_cfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
            auto ret = esp_ae_rate_cvt_open(&rsp_cfg, &resampler);
            if (ret != ESP_AE_ERR_OK || !resampler) {
                ESP_LOGE(TAG, "Failed to create resampler: %d", ret);
            }
        }

        std::vector<int16_t> pcm_buf(8192);

        // Keep restarting pipeline for each HLS segment until station switches
        while (running_ && !switch_requested_) {
            audio_element_set_uri(http, playlist_url);
            audio_pipeline_run(pipeline);

            while (running_ && !switch_requested_) {
                int bytes_read = rb_read(rb, (char*)pcm_buf.data(), pcm_buf.size() * sizeof(int16_t), pdMS_TO_TICKS(200));
                if (bytes_read <= 0) {
                    audio_element_state_t http_state = audio_element_get_state(http);
                    audio_element_state_t aac_state = audio_element_get_state(aac);
                    if (http_state == AEL_STATE_ERROR || http_state == AEL_STATE_FINISHED || http_state == AEL_STATE_STOPPED ||
                        aac_state == AEL_STATE_ERROR || aac_state == AEL_STATE_FINISHED || aac_state == AEL_STATE_STOPPED) {
                        break;
                    }
                    continue;
                }

                int samples = bytes_read / (int)sizeof(int16_t);

                // Resample if needed
                int16_t* src_data = pcm_buf.data();
                std::vector<int16_t> resampled;
                if (resampler) {
                    uint32_t in_samples = samples / src_ch;
                    uint32_t out_samples = 0;
                    esp_ae_rate_cvt_get_max_out_sample_num(resampler, in_samples, &out_samples);
                    resampled.resize(out_samples * src_ch);
                    uint32_t actual = out_samples;
                    esp_ae_rate_cvt_process(resampler, (esp_ae_sample_t)src_data, in_samples,
                                           (esp_ae_sample_t)resampled.data(), &actual);
                    resampled.resize(actual * src_ch);
                    src_data = resampled.data();
                    samples = actual * src_ch;
                }

                // Mix stereo to mono
                if (src_ch == 2) {
                    std::vector<int16_t> mono(samples / 2);
                    for (int i = 0; i < samples / 2; i++) {
                        int32_t mixed = (int32_t)src_data[i * 2] + src_data[i * 2 + 1];
                        mono[i] = (int16_t)(mixed / 2);
                    }
                    codec->OutputData(mono);
                } else {
                    std::vector<int16_t> out(src_data, src_data + samples);
                    codec->OutputData(out);
                }
            }

            // Stop and wait before restart (for next HLS segment or station switch)
            audio_pipeline_stop(pipeline);
            audio_pipeline_wait_for_stop(pipeline);
        }

        if (resampler) {
            esp_ae_rate_cvt_close(resampler);
        }
        audio_pipeline_deinit(pipeline);

        if (aac_output_rb) {
            rb_destroy(aac_output_rb);
        }
    }

    codec->EnableOutput(false);
}
