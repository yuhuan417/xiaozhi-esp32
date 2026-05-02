#ifndef _SDCARD_PLAYER_H_
#define _SDCARD_PLAYER_H_

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <vector>
#include <string>

class SdCardPlayer {
public:
    SdCardPlayer();
    ~SdCardPlayer();

    void Start();
    void Stop();
    void Next();
    void Previous();
    bool IsRunning() const { return running_; }
    const char* GetCurrentFileName() const;
    int GetFileCount() const { return file_list_.size(); }

private:
    TaskHandle_t task_handle_;
    std::atomic<bool> running_;
    std::atomic<bool> switch_requested_;
    std::atomic<int> switch_direction_;
    std::atomic<int> current_index_;
    std::vector<std::string> file_list_;

    void TaskLoop();
    void ScanFiles();
    void ShowFileInfo();
};

#endif
