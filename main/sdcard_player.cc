#include "sdcard_player.h"
#include "application.h"
#include "board.h"
#include "display/display.h"
#include "audio/audio_codec.h"

#include <esp_log.h>
#include <esp_mp3_dec.h>

#include <cmath>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

#define TAG "SdCardPlayer"

SdCardPlayer::SdCardPlayer()
    : task_handle_(nullptr)
    , running_(false)
    , switch_requested_(false)
    , switch_direction_(0)
    , current_index_(0) {
}

SdCardPlayer::~SdCardPlayer() {
    Stop();
}

void SdCardPlayer::Start() {
    if (running_) return;

    running_ = true;
    switch_requested_ = false;
    switch_direction_ = 0;
    current_index_ = 0;

    auto ret = xTaskCreate([](void* arg) {
        auto* player = static_cast<SdCardPlayer*>(arg);
        player->TaskLoop();
        player->running_ = false;
        player->task_handle_ = nullptr;
        Application::GetInstance().Schedule([]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateSdCardMp3) {
                app.SetDeviceState(kDeviceStateIdle);
            }
        });
        vTaskDelete(nullptr);
    }, "sdcard_player", 8 * 1024, this, 5, &task_handle_);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create SD card player task");
        running_ = false;
        task_handle_ = nullptr;
    }
}

void SdCardPlayer::Stop() {
    if (!running_) return;
    running_ = false;
    while (task_handle_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void SdCardPlayer::Next() {
    if (!running_) return;
    switch_direction_ = 1;
    switch_requested_ = true;
}

void SdCardPlayer::Previous() {
    if (!running_) return;
    switch_direction_ = -1;
    switch_requested_ = true;
}

const char* SdCardPlayer::GetCurrentFileName() const {
    if (file_list_.empty() || current_index_ >= (int)file_list_.size()) return "";
    return file_list_[current_index_].c_str();
}

void SdCardPlayer::ShowFileInfo() {
    auto display = Board::GetInstance().GetDisplay();
    if (display && !file_list_.empty() && current_index_ < (int)file_list_.size()) {
        const char* path = file_list_[current_index_].c_str();
        const char* basename = strrchr(path, '/');
        basename = basename ? basename + 1 : path;
        display->ShowNotification(basename);
    }
}

void SdCardPlayer::ScanFiles() {
    file_list_.clear();
    std::vector<std::string> dirs;
    dirs.push_back("/sdcard");

    while (!dirs.empty()) {
        std::string dir = dirs.back();
        dirs.pop_back();

        DIR* d = opendir(dir.c_str());
        if (!d) continue;

        struct dirent* entry;
        while ((entry = readdir(d)) != nullptr) {
            if (entry->d_name[0] == '.') continue;

            std::string full_path = dir + "/" + entry->d_name;

            if (entry->d_type == DT_DIR) {
                dirs.push_back(full_path);
            } else if (entry->d_type == DT_REG) {
                const char* ext = strrchr(entry->d_name, '.');
                if (ext && strcasecmp(ext, ".mp3") == 0) {
                    file_list_.push_back(full_path);
                }
            }
        }
        closedir(d);
    }

    std::sort(file_list_.begin(), file_list_.end());
    ESP_LOGI(TAG, "Found %d MP3 files on SD card", file_list_.size());
}

// --- Simple resampler (linear interpolation) ---

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
            double s0 = in[idx0];
            double s1 = in[idx1];
            out[i * channels + ch] = (int16_t)(s0 + (s1 - s0) * frac);
        }
    }
}

// --- Stereo to mono ---

static void stereo_to_mono(const std::vector<int16_t>& stereo, std::vector<int16_t>& mono) {
    mono.resize(stereo.size() / 2);
    for (size_t i = 0; i < mono.size(); i++) {
        int32_t mixed = (int32_t)stereo[i * 2] + stereo[i * 2 + 1];
        mono[i] = (int16_t)(mixed / 2);
    }
}

void SdCardPlayer::TaskLoop() {
    auto codec = Board::GetInstance().GetAudioCodec();
    codec->EnableOutput(true);

    int output_rate = codec->output_sample_rate();

    // Play a short beep
    {
        float duration = 0.3f;
        float freq = 660.0f;
        int total_samples = (int)(output_rate * duration);
        std::vector<int16_t> beep(total_samples);
        for (int i = 0; i < total_samples; i++) {
            beep[i] = (int16_t)(sinf(2.0f * 3.14159265f * freq * i / output_rate) * 8192);
        }
        codec->OutputData(beep);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ScanFiles();

    if (file_list_.empty()) {
        ESP_LOGW(TAG, "No MP3 files found on SD card");
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            display->ShowNotification("SD卡无MP3文件");
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
        codec->EnableOutput(false);
        return;
    }

    while (running_) {
        if (switch_requested_) {
            int dir = switch_direction_.load();
            current_index_ = (dir == 1)
                ? (current_index_ + 1) % file_list_.size()
                : (current_index_ == 0) ? (int)file_list_.size() - 1 : current_index_ - 1;
            switch_requested_ = false;
            switch_direction_ = 0;
            ShowFileInfo();
        }

        const char* file_path = file_list_[current_index_].c_str();
        ESP_LOGI(TAG, "Playing: %s", file_path);
        ShowFileInfo();

        FILE* fp = fopen(file_path, "rb");
        if (!fp) {
            ESP_LOGE(TAG, "Failed to open file: %s", file_path);
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (running_ && !switch_requested_) {
                current_index_ = (current_index_ + 1) % file_list_.size();
            }
            continue;
        }

        // Skip ID3v2 tag if present
        {
            uint8_t id3_header[10];
            if (fread(id3_header, 1, 10, fp) == 10 &&
                id3_header[0] == 'I' && id3_header[1] == 'D' && id3_header[2] == '3') {
                // Synchsafe integer: 4 bytes, each byte uses only lower 7 bits
                uint32_t tag_size = ((uint32_t)id3_header[6] << 21) |
                                    ((uint32_t)id3_header[7] << 14) |
                                    ((uint32_t)id3_header[8] << 7) |
                                    (uint32_t)id3_header[9];
                long skip = 10 + tag_size;
                // ID3v2.4+ may have a 10-byte footer
                if (id3_header[5] & 0x10) skip += 10;
                ESP_LOGI(TAG, "Skipping ID3v2 tag: %ld bytes", skip);
                fseek(fp, skip, SEEK_SET);
            } else {
                // No ID3 tag, rewind to start
                fseek(fp, 0, SEEK_SET);
            }
        }

        // Open MP3 decoder
        void* mp3_handle = nullptr;
        esp_audio_err_t mp3_ret = esp_mp3_dec_open(nullptr, 0, &mp3_handle);
        if (mp3_ret != ESP_AUDIO_ERR_OK || !mp3_handle) {
            ESP_LOGE(TAG, "Failed to open MP3 decoder: %d", mp3_ret);
            fclose(fp);
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (running_ && !switch_requested_) {
                current_index_ = (current_index_ + 1) % file_list_.size();
            }
            continue;
        }

        int src_rate = 0;
        int src_ch = 0;
        bool info_obtained = false;

        std::vector<uint8_t> acc_buf;
        std::vector<uint8_t> pcm_buf(8192);
        size_t offset = 0;
        bool file_done = false;
        int decode_count = 0, error_count = 0;

        while (running_ && !switch_requested_ && !file_done) {
            // Read more data if buffer running low
            if (acc_buf.size() - offset < 8192) {
                if (offset > 0) {
                    acc_buf.erase(acc_buf.begin(), acc_buf.begin() + offset);
                    offset = 0;
                }
                uint8_t read_buf[4096];
                size_t bytes_read = fread(read_buf, 1, sizeof(read_buf), fp);
                if (bytes_read == 0) {
                    file_done = true;
                    if (acc_buf.size() == 0) break;
                } else {
                    acc_buf.insert(acc_buf.end(), read_buf, read_buf + bytes_read);
                }
            }

            esp_audio_dec_in_raw_t raw = {};
            raw.buffer = acc_buf.data() + offset;
            raw.len = acc_buf.size() - offset;

            while (raw.len > 0 && running_ && !switch_requested_) {
                esp_audio_dec_out_frame_t frame = {};
                esp_audio_dec_info_t dec_info = {};
                frame.buffer = pcm_buf.data();
                frame.len = pcm_buf.size();

                mp3_ret = esp_mp3_dec_decode(mp3_handle, &raw, &frame, &dec_info);
                if (mp3_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    pcm_buf.resize(frame.needed_size);
                    continue;
                }
                if (mp3_ret == ESP_AUDIO_ERR_DATA_LACK || mp3_ret == ESP_AUDIO_ERR_CONTINUE) {
                    offset = raw.buffer - acc_buf.data();
                    break;
                }
                if (mp3_ret != ESP_AUDIO_ERR_OK) {
                    error_count++;
                    size_t skip = raw.consumed > 0 ? raw.consumed : 1;
                    ESP_LOGW(TAG, "MP3 decode err=%d consumed=%lu offset=%zu skip=%zu",
                             mp3_ret, raw.consumed, offset, skip);
                    raw.buffer += skip;
                    raw.len -= skip;
                    offset += skip;
                    continue;
                }
                decode_count++;

                size_t consumed = raw.consumed;
                raw.buffer += consumed;
                raw.len -= consumed;
                offset += consumed;

                if (frame.decoded_size == 0) continue;

                if (!info_obtained && dec_info.sample_rate > 0) {
                    src_rate = dec_info.sample_rate;
                    src_ch = dec_info.channel;
                    info_obtained = true;
                    ESP_LOGI(TAG, "MP3 info: %d Hz, %d ch", src_rate, src_ch);
                }

                int samples = frame.decoded_size / sizeof(int16_t);
                auto* pcm = reinterpret_cast<int16_t*>(frame.buffer);

                std::vector<int16_t> resampled;
                const int16_t* src_pcm = pcm;
                int src_samples = samples;
                if (src_rate > 0 && src_rate != output_rate) {
                    resample_pcm(pcm, samples, src_rate, resampled, output_rate, src_ch);
                    src_pcm = resampled.data();
                    src_samples = resampled.size();
                }

                std::vector<int16_t> mono;
                const int16_t* out_pcm = src_pcm;
                int out_samples = src_samples;
                if (src_ch == 2) {
                    std::vector<int16_t> src_vec(src_pcm, src_pcm + src_samples);
                    stereo_to_mono(src_vec, mono);
                    out_pcm = mono.data();
                    out_samples = mono.size();
                }

                std::vector<int16_t> output(out_pcm, out_pcm + out_samples);
                codec->OutputData(output);
            }

            // Periodic trim to prevent unbounded growth
            if (offset > 65536) {
                acc_buf.erase(acc_buf.begin(), acc_buf.begin() + offset);
                offset = 0;
            }

            vTaskDelay(1);
        }

        esp_mp3_dec_close(mp3_handle);
        fclose(fp);

        ESP_LOGI(TAG, "File done: decodes=%d errors=%d", decode_count, error_count);

        // Advance to next file (unless switched by user)
        if (running_ && !switch_requested_) {
            current_index_ = (current_index_ + 1) % file_list_.size();
            ESP_LOGI(TAG, "Next file: %s", file_list_[current_index_].c_str());
        }
    }

    UBaseType_t stack_watermark = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(TAG, "Player exit: running=%d switch=%d stack_free=%lu",
             running_ ? 1 : 0, switch_requested_ ? 1 : 0, stack_watermark);
    codec->EnableOutput(false);
}
