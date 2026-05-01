#ifndef _NETWORK_RADIO_H_
#define _NETWORK_RADIO_H_

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <vector>
#include <string>

#include "audio_element.h"
#include "audio_pipeline.h"
#include "http_stream.h"
#include "esp_audio_simple_dec.h"
#include "esp_ae_rate_cvt.h"

class NetworkRadio {
public:
    struct Station {
        const char* name;
        const char* url;
    };

    NetworkRadio();
    ~NetworkRadio();

    void SetStations(const Station* stations, size_t count);
    void Start(int station_index = 0);
    void Stop();
    void NextStation();
    void PreviousStation();
    bool IsRunning() const { return running_; }
    int GetCurrentStationIndex() const { return current_station_; }
    const char* GetCurrentStationName() const;

private:
    TaskHandle_t task_handle_;
    std::atomic<bool> running_;
    std::atomic<bool> switch_requested_;
    std::atomic<int> switch_direction_;
    std::atomic<int> current_station_;
    const Station* stations_;
    size_t station_count_;

    void TaskLoop();
    void ShowStationInfo();
};

#endif
