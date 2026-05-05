#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string>

class SimpleFtpServer {
public:
    SimpleFtpServer();
    ~SimpleFtpServer();

    bool Start(uint16_t port, const char* root_path);
    void Stop();
    bool IsRunning() const { return running_; }

private:
    void AcceptLoop();

    bool running_ = false;
    int listen_fd_ = -1;
    std::string root_path_;
    TaskHandle_t accept_task_ = nullptr;
};
