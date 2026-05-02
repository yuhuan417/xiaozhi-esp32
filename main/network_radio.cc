#include "network_radio.h"
#include "board.h"
#include "display/display.h"
#include "audio/audio_codec.h"

#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_aac_dec.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <queue>
#include <sstream>

#define TAG "NetworkRadio"
#define MAX_QUEUE_BYTES (560 * 1024)

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
    }, "radio_task", 8 * 1024, this, 5, &task_handle_);

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

// --- HTTP helpers ---

struct FetchResult {
    std::vector<uint8_t> data;
    int status = 0;
};

static esp_err_t fetch_event_handler(esp_http_client_event_t* evt) {
    auto* result = static_cast<FetchResult*>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        size_t old = result->data.size();
        result->data.resize(old + evt->data_len);
        memcpy(result->data.data() + old, evt->data, evt->data_len);
    }
    return ESP_OK;
}

static bool fetch_url_to_buffer(const char* url, FetchResult& result, int timeout_ms = 10000) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = fetch_event_handler;
    cfg.user_data = &result;
    cfg.timeout_ms = timeout_ms;
    cfg.buffer_size = 4096;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    result.status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP fetch failed: %s", esp_err_to_name(err));
        return false;
    }
    return result.status == 200;
}

// --- Thread-safe segment queue ---

class SegmentQueue {
    std::queue<std::vector<uint8_t>> queue_;
    size_t total_bytes_ = 0;
    SemaphoreHandle_t mutex_;
public:
    SegmentQueue() {
        mutex_ = xSemaphoreCreateMutex();
    }
    ~SegmentQueue() {
        if (mutex_) vSemaphoreDelete(mutex_);
    }

    bool push(std::vector<uint8_t>& data) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        if (total_bytes_ + data.size() > MAX_QUEUE_BYTES) {
            xSemaphoreGive(mutex_);
            return false;
        }
        total_bytes_ += data.size();
        queue_.push(std::move(data));
        xSemaphoreGive(mutex_);
        return true;
    }

    bool pop(std::vector<uint8_t>& out) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        if (queue_.empty()) {
            xSemaphoreGive(mutex_);
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop();
        total_bytes_ -= out.size();
        xSemaphoreGive(mutex_);
        return true;
    }

    bool empty() {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        bool e = queue_.empty();
        xSemaphoreGive(mutex_);
        return e;
    }

    size_t total_bytes() {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        size_t b = total_bytes_;
        xSemaphoreGive(mutex_);
        return b;
    }
};

// --- HLS download task ---

struct HlsDownloadCtx {
    SegmentQueue* queue;
    const std::atomic<bool>* running;
    std::string playlist_url;
};

static bool is_hls_playlist(const uint8_t* data, size_t len);
static std::vector<std::string> parse_hls_segments(const std::vector<uint8_t>& data);
static std::string resolve_url(const std::string& base, const std::string& relative);

static void hls_download_task(void* arg) {
    auto* ctx = static_cast<HlsDownloadCtx*>(arg);

    FetchResult playlist_result;
    std::vector<std::string> segments;
    size_t seg_idx = 0;

    while (ctx->running->load()) {
        if (seg_idx >= segments.size()) {
            if (!fetch_url_to_buffer(ctx->playlist_url.c_str(), playlist_result) ||
                !is_hls_playlist(playlist_result.data.data(), playlist_result.data.size())) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            segments = parse_hls_segments(playlist_result.data);
            seg_idx = 0;
            if (segments.empty()) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }

        std::string seg_url = resolve_url(ctx->playlist_url, segments[seg_idx]);
        seg_idx++;

        FetchResult seg_result;
        if (!fetch_url_to_buffer(seg_url.c_str(), seg_result, 15000))
            continue;

        ESP_LOGI(TAG, "DL: seg %zu size=%u queue_bytes=%zu",
                 seg_idx, seg_result.data.size(), ctx->queue->total_bytes());

        if (seg_result.data.size() >= 16) {
            auto& d = seg_result.data;
            ESP_LOGI(TAG, "DL: first16=%02x%02x %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x",
                     d[0],d[1], d[2],d[3], d[4],d[5], d[6],d[7],
                     d[8],d[9], d[10],d[11], d[12],d[13], d[14],d[15]);
        }

        // Block until space in queue
        while (ctx->running->load() && !ctx->queue->push(seg_result.data)) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    delete ctx;
    vTaskDelete(nullptr);
}

// --- HLS parsing ---

static bool is_hls_playlist(const uint8_t* data, size_t len) {
    std::string head((const char*)data, len < 7 ? len : 7);
    return head.find("#EXTM3U") != std::string::npos;
}

static std::vector<std::string> parse_hls_segments(const std::vector<uint8_t>& data) {
    std::vector<std::string> urls;
    std::string content((const char*)data.data(), data.size());
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        urls.push_back(line);
    }
    return urls;
}

static std::string resolve_url(const std::string& base, const std::string& relative) {
    if (relative.find("http://") == 0 || relative.find("https://") == 0)
        return relative;
    if (relative.find("//") == 0) {
        auto scheme_end = base.find("://");
        if (scheme_end != std::string::npos)
            return base.substr(0, scheme_end) + ":" + relative;
        return "http:" + relative;
    }
    if (!relative.empty() && relative[0] == '/') {
        auto scheme_end = base.find("://");
        if (scheme_end == std::string::npos) return relative;
        auto host_start = scheme_end + 3;
        auto host_end = base.find('/', host_start);
        if (host_end == std::string::npos)
            return base + relative;
        return base.substr(0, host_end) + relative;
    }
    auto pos = base.find_last_of('/');
    if (pos != std::string::npos && pos > base.find("://") + 2)
        return base.substr(0, pos + 1) + relative;
    return base + "/" + relative;
}

// --- Resampling ---

static void resample_pcm(const int16_t* in, size_t in_samples, int in_rate,
                          std::vector<int16_t>& out, int out_rate, int channels) {
    if (in_rate == out_rate || in_rate <= 0) {
        out.assign(in, in + in_samples);
        return;
    }
    double ratio = (double)in_rate / out_rate;
    size_t out_frames = (size_t)(in_samples / channels / ratio);
    out.resize(out_frames * channels);

    for (size_t i = 0; i < out_frames; i++) {
        double src_pos = i * ratio;
        size_t src_idx = (size_t)src_pos;
        double frac = src_pos - src_idx;

        for (int ch = 0; ch < channels; ch++) {
            size_t idx0 = src_idx * channels + ch;
            size_t idx1 = ((src_idx + 1) * channels + ch >= in_samples) ? idx0 : (src_idx + 1) * channels + ch;
            out[i * channels + ch] = (int16_t)(in[idx0] + (in[idx1] - in[idx0]) * frac);
        }
    }
}

static void stereo_to_mono(const std::vector<int16_t>& stereo, std::vector<int16_t>& mono) {
    mono.resize(stereo.size() / 2);
    for (size_t i = 0; i < mono.size(); i++) {
        int32_t mixed = (int32_t)stereo[i * 2] + stereo[i * 2 + 1];
        mono[i] = (int16_t)(mixed / 2);
    }
}

// --- Main task loop ---

void NetworkRadio::TaskLoop() {
    auto codec = Board::GetInstance().GetAudioCodec();
    codec->EnableOutput(true);

    int output_rate = codec->output_sample_rate();

    // Play a short beep
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

        // Fetch playlist
        FetchResult playlist_result;
        if (!fetch_url_to_buffer(playlist_url, playlist_result)) {
            ESP_LOGW(TAG, "Failed to fetch playlist, retrying...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (!is_hls_playlist(playlist_result.data.data(), playlist_result.data.size())) {
            // Direct stream URL (non-HLS) — download once and decode
            FetchResult stream_result;
            if (!fetch_url_to_buffer(playlist_url, stream_result)) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            ESP_LOGI(TAG, "Direct stream: %u bytes", stream_result.data.size());

            void* aac_handle = nullptr;
            esp_aac_dec_cfg_t aac_cfg;
            memset(&aac_cfg, 0, sizeof(aac_cfg));
            aac_cfg.aac_plus_enable = true;
            if (esp_aac_dec_open(&aac_cfg, sizeof(aac_cfg), &aac_handle) != ESP_AUDIO_ERR_OK) {
                continue;
            }

            int src_rate = 0, src_ch = 0;
            bool info_obtained = false;
            std::vector<uint8_t> pcm_buf(8192);
            std::vector<uint8_t>& data = stream_result.data;
            size_t offset = 0;

            while (offset < data.size() && running_ && !switch_requested_) {
                esp_audio_dec_in_raw_t raw = {};
                raw.buffer = data.data() + offset;
                raw.len = data.size() - offset;

                esp_audio_dec_out_frame_t frame = {};
                esp_audio_dec_info_t dec_info = {};
                frame.buffer = pcm_buf.data();
                frame.len = pcm_buf.size();

                auto ret = esp_aac_dec_decode(aac_handle, &raw, &frame, &dec_info);
                if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) { pcm_buf.resize(frame.needed_size); continue; }
                if (ret != ESP_AUDIO_ERR_OK) { offset++; continue; }

                offset += raw.consumed;
                if (frame.decoded_size == 0) continue;

                if (!info_obtained && dec_info.sample_rate > 0) {
                    src_rate = dec_info.sample_rate;
                    src_ch = dec_info.channel;
                    info_obtained = true;
                    ESP_LOGI(TAG, "Stream info: %d Hz, %d ch", src_rate, src_ch);
                }

                int samples = frame.decoded_size / sizeof(int16_t);
                auto* pcm = reinterpret_cast<int16_t*>(frame.buffer);

                std::vector<int16_t> resampled;
                const int16_t* sp = pcm;
                int ss = samples;
                if (src_rate > 0 && src_rate != output_rate) {
                    resample_pcm(pcm, samples, src_rate, resampled, output_rate, src_ch);
                    sp = resampled.data();
                    ss = resampled.size();
                }

                std::vector<int16_t> mono;
                if (src_ch == 2) {
                    std::vector<int16_t> src_vec(sp, sp + ss);
                    stereo_to_mono(src_vec, mono);
                    sp = mono.data();
                    ss = mono.size();
                }

                std::vector<int16_t> output(sp, sp + ss);
                codec->OutputData(output);
            }

            esp_aac_dec_close(aac_handle);
            continue;
        }

        // HLS stream: download task fills segment queue, main task decodes
        {
            SegmentQueue queue;
            std::atomic<bool> dl_running(true);
            HlsDownloadCtx* dl_ctx = new HlsDownloadCtx{&queue, &dl_running, playlist_url};

            TaskHandle_t dl_handle = nullptr;
            auto ret = xTaskCreate(hls_download_task, "hls_dl", 6 * 1024, dl_ctx, 5, &dl_handle);
            if (ret != pdPASS) {
                delete dl_ctx;
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }

            void* aac_handle = nullptr;
            esp_aac_dec_cfg_t aac_cfg;
            memset(&aac_cfg, 0, sizeof(aac_cfg));
            aac_cfg.aac_plus_enable = true;
            if (esp_aac_dec_open(&aac_cfg, sizeof(aac_cfg), &aac_handle) != ESP_AUDIO_ERR_OK) {
                dl_running = false;
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            int src_rate = 0, src_ch = 0;
            bool info_obtained = false;
            std::vector<uint8_t> pcm_buf(8192);

            while (running_ && !switch_requested_) {
                std::vector<uint8_t> segment;
                if (queue.pop(segment)) {
                    ESP_LOGI(TAG, "DEC: got segment size=%u queue_bytes=%zu",
                             segment.size(), queue.total_bytes());

                    size_t offset = 0;
                    while (offset < segment.size() && running_ && !switch_requested_) {
                        esp_audio_dec_in_raw_t raw = {};
                        raw.buffer = segment.data() + offset;
                        raw.len = segment.size() - offset;

                        esp_audio_dec_out_frame_t frame = {};
                        esp_audio_dec_info_t dec_info = {};
                        frame.buffer = pcm_buf.data();
                        frame.len = pcm_buf.size();

                        auto aac_ret = esp_aac_dec_decode(aac_handle, &raw, &frame, &dec_info);
                        if (aac_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) { pcm_buf.resize(frame.needed_size); continue; }
                        if (aac_ret == ESP_AUDIO_ERR_DATA_LACK || aac_ret == ESP_AUDIO_ERR_CONTINUE) break;
                        if (aac_ret != ESP_AUDIO_ERR_OK) { offset++; continue; }

                        offset += raw.consumed;
                        if (frame.decoded_size == 0) continue;

                        if (!info_obtained && dec_info.sample_rate > 0) {
                            src_rate = dec_info.sample_rate;
                            src_ch = dec_info.channel;
                            info_obtained = true;
                            ESP_LOGI(TAG, "Stream info: %d Hz, %d ch", src_rate, src_ch);
                        }

                        int samples = frame.decoded_size / sizeof(int16_t);
                        auto* pcm = reinterpret_cast<int16_t*>(frame.buffer);

                        std::vector<int16_t> resampled;
                        const int16_t* sp = pcm;
                        int ss = samples;
                        if (src_rate > 0 && src_rate != output_rate) {
                            resample_pcm(pcm, samples, src_rate, resampled, output_rate, src_ch);
                            sp = resampled.data();
                            ss = resampled.size();
                        }

                        std::vector<int16_t> mono;
                        if (src_ch == 2) {
                            std::vector<int16_t> src_vec(sp, sp + ss);
                            stereo_to_mono(src_vec, mono);
                            sp = mono.data();
                            ss = mono.size();
                        }

                        std::vector<int16_t> output(sp, sp + ss);
                        codec->OutputData(output);
                    }
                } else {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }

            dl_running = false;
            while (eTaskGetState(dl_handle) != eDeleted && eTaskGetState(dl_handle) != eInvalid) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            esp_aac_dec_close(aac_handle);
        }

        if (running_ && !switch_requested_) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    codec->EnableOutput(false);
}
