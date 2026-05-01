#include "network_radio.h"
#include "board.h"
#include "display/display.h"
#include "audio/codecs/no_audio_codec.h"

#include <esp_log.h>
#include <esp_ae_rate_cvt.h>
#include <cmath>
#include <cstring>

#define TAG "NetworkRadio"

static const int PCM_BUFFER_SIZE = 4096;

// Context passed to the AAC decoder element via audio_element_setdata
struct AacDecoderCtx {
    esp_audio_simple_dec_handle_t decoder;
    esp_ae_rate_cvt_handle_t rate_cvt;
    int stream_rate;
    int stream_ch;
    int output_rate;
    std::vector<uint8_t> pcm_buf;
    std::vector<int16_t> resample_buf;
    bool first_frame;
};

// AAC decoder element callbacks
static esp_err_t aac_open(audio_element_handle_t self) {
    auto* ctx = (AacDecoderCtx*)audio_element_getdata(self);
    if (!ctx) return ESP_FAIL;

    ctx->first_frame = true;
    ctx->stream_rate = 0;
    ctx->stream_ch = 0;
    ctx->rate_cvt = nullptr;
    ctx->pcm_buf.resize(PCM_BUFFER_SIZE * sizeof(int16_t));
    ctx->resample_buf.resize(PCM_BUFFER_SIZE);

    esp_audio_simple_dec_cfg_t cfg = {};
    cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    cfg.dec_cfg = nullptr;
    cfg.cfg_size = 0;
    cfg.use_frame_dec = false;

    esp_audio_err_t ret = esp_audio_simple_dec_open(&cfg, &ctx->decoder);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open AAC decoder: %d", ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static audio_element_err_t aac_process(audio_element_handle_t self, char *el_buffer, int el_buf_len) {
    // Note: ADF convention — positive return = bytes produced, negative = audio_element_err_t
    auto* ctx = (AacDecoderCtx*)audio_element_getdata(self);
    if (!ctx || !ctx->decoder) return AEL_PROCESS_FAIL;

    esp_audio_simple_dec_raw_t raw = {};
    raw.buffer = (uint8_t*)el_buffer;
    raw.len = (uint32_t)el_buf_len;
    raw.eos = false;

    while (raw.consumed < raw.len) {
        esp_audio_simple_dec_out_t frame = {};
        frame.buffer = ctx->pcm_buf.data();
        frame.len = (uint32_t)ctx->pcm_buf.size();

        esp_audio_err_t ret = esp_audio_simple_dec_process(ctx->decoder, &raw, &frame);
        if (ret != ESP_AUDIO_ERR_OK && ret != ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            return AEL_PROCESS_FAIL;
        }

        if (frame.decoded_size == 0) continue;

        int total = (int)(frame.decoded_size / sizeof(int16_t));

        // Detect stream format from first frame
        if (ctx->first_frame) {
            esp_audio_simple_dec_info_t info;
            if (esp_audio_simple_dec_get_info(ctx->decoder, &info) == ESP_AUDIO_ERR_OK) {
                ctx->stream_rate = (int)info.sample_rate;
                ctx->stream_ch = (int)info.channel;
                ESP_LOGI(TAG, "Stream: %d Hz, %d ch", ctx->stream_rate, ctx->stream_ch);

                if (ctx->stream_rate != ctx->output_rate && ctx->stream_rate > 0) {
                    esp_ae_rate_cvt_cfg_t rcfg = {};
                    rcfg.src_rate = (uint32_t)ctx->stream_rate;
                    rcfg.dest_rate = (uint32_t)ctx->output_rate;
                    rcfg.channel = (uint8_t)ctx->stream_ch;
                    rcfg.bits_per_sample = 16;
                    rcfg.complexity = 1;
                    rcfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
                    esp_ae_rate_cvt_open(&rcfg, &ctx->rate_cvt);
                }
            }
            ctx->first_frame = false;
        }

        // Resample or pass-through
        int16_t* out_data;
        int out_samples;

        if (ctx->rate_cvt && ctx->stream_ch > 0) {
            uint32_t in_samples = (uint32_t)total / ctx->stream_ch;
            uint32_t out_s = (uint32_t)ctx->resample_buf.size();
            esp_ae_rate_cvt_process(ctx->rate_cvt,
                (esp_ae_sample_t)ctx->pcm_buf.data(), in_samples,
                (esp_ae_sample_t)ctx->resample_buf.data(), &out_s);
            out_data = ctx->resample_buf.data();
            out_samples = (int)out_s;
        } else {
            out_data = (int16_t*)ctx->pcm_buf.data();
            out_samples = total;
        }

        // Mix stereo to mono if needed
        if (ctx->stream_ch == 2) {
            int mono_samples = out_samples / 2;
            for (int i = 0; i < mono_samples; i++) {
                int32_t mixed = (int32_t)out_data[i * 2] + out_data[i * 2 + 1];
                out_data[i] = (int16_t)(mixed / 2);
            }
            out_samples = mono_samples;
        }

        int output_bytes = out_samples * sizeof(int16_t);
        if (output_bytes > 0) {
            memcpy(el_buffer, out_data, output_bytes);
            return (audio_element_err_t)output_bytes;
        }
    }

    return (audio_element_err_t)0;
}

static esp_err_t aac_close(audio_element_handle_t self) {
    auto* ctx = (AacDecoderCtx*)audio_element_getdata(self);
    if (ctx) {
        if (ctx->decoder) {
            esp_audio_simple_dec_close(ctx->decoder);
            ctx->decoder = nullptr;
        }
        if (ctx->rate_cvt) {
            esp_ae_rate_cvt_close(ctx->rate_cvt);
            ctx->rate_cvt = nullptr;
        }
    }
    return ESP_OK;
}

static esp_err_t aac_destroy(audio_element_handle_t self) {
    return ESP_OK;
}

// Write callback for the output element: routes PCM data to the audio codec
static audio_element_err_t codec_output_write(audio_element_handle_t self, char *buffer, int len, TickType_t ticks_to_wait, void *context) {
    if (!context || len <= 0) return AEL_IO_FAIL;

    auto* codec = (AudioCodec*)context;
    int samples = len / (int)sizeof(int16_t);
    std::vector<int16_t> pcm(samples);
    memcpy(pcm.data(), buffer, len);
    codec->OutputData(pcm);
    return (audio_element_err_t)len;
}

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
    }, "radio_task", 4096, this, 5, &task_handle_);

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

    // Play a short beep to verify audio output path
    {
        int output_rate = codec->output_sample_rate();
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

    int output_rate = codec->output_sample_rate();

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

        // Build ADF pipeline: http_stream -> aac_decoder -> codec_output
        audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
        audio_pipeline_handle_t pipeline = audio_pipeline_init(&pipeline_cfg);
        if (!pipeline) {
            ESP_LOGE(TAG, "Failed to create pipeline");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // HTTP/HLS stream element
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

        // AAC decoder element (custom)
        AacDecoderCtx aac_ctx = {};
        aac_ctx.output_rate = output_rate;

        audio_element_cfg_t aac_cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
        aac_cfg.open = aac_open;
        aac_cfg.process = aac_process;
        aac_cfg.close = aac_close;
        aac_cfg.destroy = aac_destroy;
        aac_cfg.buffer_len = PCM_BUFFER_SIZE * (int)sizeof(int16_t);
        aac_cfg.task_stack = 4096;
        aac_cfg.tag = "aac";
        audio_element_handle_t aac = audio_element_init(&aac_cfg);
        if (!aac) {
            ESP_LOGE(TAG, "Failed to create aac element");
            audio_element_deinit(http);
            audio_pipeline_deinit(pipeline);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        audio_element_setdata(aac, &aac_ctx);

        // Output element (custom write callback)
        audio_element_cfg_t out_cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
        out_cfg.buffer_len = PCM_BUFFER_SIZE * (int)sizeof(int16_t);
        out_cfg.task_stack = 4096;
        out_cfg.tag = "out";
        audio_element_handle_t out = audio_element_init(&out_cfg);
        if (!out) {
            ESP_LOGE(TAG, "Failed to create output element");
            audio_element_deinit(aac);
            audio_element_deinit(http);
            audio_pipeline_deinit(pipeline);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        audio_element_set_write_cb(out, codec_output_write, codec);

        // Register and link
        audio_pipeline_register(pipeline, http, "http");
        audio_pipeline_register(pipeline, aac, "aac");
        audio_pipeline_register(pipeline, out, "out");

        const char* link_tag[3] = {"http", "aac", "out"};
        audio_pipeline_link(pipeline, link_tag, 3);

        // Run the pipeline
        audio_pipeline_run(pipeline);

        // Wait for stop or switch request
        while (running_ && !switch_requested_) {
            audio_element_state_t state = audio_element_get_state(http);
            if (state == AEL_STATE_ERROR || state == AEL_STATE_FINISHED || state == AEL_STATE_STOPPED) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        // Tear down pipeline
        audio_pipeline_stop(pipeline);
        audio_pipeline_wait_for_stop(pipeline);
        audio_pipeline_deinit(pipeline);

        vTaskDelay(pdMS_TO_TICKS(500));
    }

    codec->EnableOutput(false);
}
