#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "power_save_timer.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "power_manager.h"
#include "mcp_server.h"
#include "network_radio.h"
#include "sdcard_player.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>

#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_vfs_fat.h>
#include <driver/sdspi_host.h>

#define TAG "XINGZHI_CUBE_1_54TFT_WIFI"

static const NetworkRadio::Station RADIO_STATIONS[] = {
    RADIO_STATION_LIST
};

class XINGZHI_CUBE_1_54TFT_WIFI : public WifiBoard {
private:
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    SpiLcdDisplay* display_;
    PowerSaveTimer* power_save_timer_;
    PowerManager* power_manager_;
    NetworkRadio network_radio_;
    SdCardPlayer sdcard_player_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    bool is_sdcard_found_ = false;

    void InitializePowerManager() {
        power_manager_ = new PowerManager(GPIO_NUM_38);
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
            } else {
                power_save_timer_->SetEnabled(true);
            }
        });
    }

    void InitializePowerSaveTimer() {
        rtc_gpio_init(GPIO_NUM_21);
        rtc_gpio_set_direction(GPIO_NUM_21, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(GPIO_NUM_21, 1);

        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            ESP_LOGI(TAG, "Shutting down");
            rtc_gpio_set_level(GPIO_NUM_21, 0);
            // 启用保持功能，确保睡眠期间电平不变
            rtc_gpio_hold_en(GPIO_NUM_21);
            esp_lcd_panel_disp_on_off(panel_, false); //关闭显示
            esp_deep_sleep_start();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SDA;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SCL;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void EnterRadioMode() {
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();

        if (state != kDeviceStateIdle &&
            state != kDeviceStateSpeaking &&
            state != kDeviceStateListening &&
            state != kDeviceStateSdCardMp3) {
            return;
        }

        app.Schedule([this]() {
            auto& app = Application::GetInstance();
            app.SetDeviceState(kDeviceStateIdle);
            app.SetDeviceState(kDeviceStateNetworkRadio);
            power_save_timer_->SetEnabled(false);
            network_radio_.Start(0);
            GetDisplay()->ShowNotification("📻 网络收音机");
        });
    }

    void ExitRadioMode() {
        network_radio_.Stop();
        power_save_timer_->SetEnabled(true);
        auto& app = Application::GetInstance();
        app.GetAudioService().ResetDecoder();
        app.CloseAudioChannel();
        app.SetDeviceState(kDeviceStateIdle);
        GetDisplay()->ShowNotification("对话模式");
    }

    void EnterSdCardMp3Mode() {
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();

        if (!is_sdcard_found_) {
            GetDisplay()->ShowNotification("未检测到SD卡");
            return;
        }

        // Enter from idle, speaking, listening, network radio, or sd card mp3 (re-entry)
        if (state != kDeviceStateIdle &&
            state != kDeviceStateSpeaking &&
            state != kDeviceStateListening &&
            state != kDeviceStateNetworkRadio &&
            state != kDeviceStateSdCardMp3) {
            return;
        }

        app.Schedule([this]() {
            auto& app = Application::GetInstance();

            if (network_radio_.IsRunning()) {
                network_radio_.Stop();
            }
            if (sdcard_player_.IsRunning()) {
                sdcard_player_.Stop();
            }

            app.SetDeviceState(kDeviceStateIdle);
            app.SetDeviceState(kDeviceStateSdCardMp3);
            power_save_timer_->SetEnabled(false);
            sdcard_player_.Start();
            GetDisplay()->ShowNotification("🎵 SD卡MP3");
        });
    }

    void ExitSdCardMp3Mode() {
        sdcard_player_.Stop();
        power_save_timer_->SetEnabled(true);
        auto& app = Application::GetInstance();
        app.GetAudioService().ResetDecoder();
        app.CloseAudioChannel();
        app.SetDeviceState(kDeviceStateIdle);
        GetDisplay()->ShowNotification("对话模式");
    }

    void InitializeButtons() {
        network_radio_.SetStations(RADIO_STATIONS, sizeof(RADIO_STATIONS) / sizeof(RADIO_STATIONS[0]));

        boot_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            if (network_radio_.IsRunning()) {
                network_radio_.NextStation();
                return;
            }
            if (sdcard_player_.IsRunning()) {
                sdcard_player_.Next();
                return;
            }
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        boot_button_.OnDoubleClick([this]() {
            power_save_timer_->WakeUp();
            if (network_radio_.IsRunning()) {
                network_radio_.PreviousStation();
                return;
            }
            if (sdcard_player_.IsRunning()) {
                sdcard_player_.Previous();
            }
        });

        boot_button_.OnLongPress([this]() {
            power_save_timer_->WakeUp();
            if (network_radio_.IsRunning()) {
                ExitRadioMode();
            } else if (sdcard_player_.IsRunning()) {
                ExitSdCardMp3Mode();
            }
        });

        volume_up_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            power_save_timer_->WakeUp();
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            power_save_timer_->WakeUp();
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    void InitializeSt7789Display() {
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS;
        io_config.dc_gpio_num = DISPLAY_DC;
        io_config.spi_mode = 3;
        io_config.pclk_hz = 80 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io_));

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RES;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, true));

        display_ = new SpiLcdDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeSDcardSpi() {
        spi_bus_config_t bus_cnf = {
            .mosi_io_num = SD_CMD,
            .miso_io_num = SD_DATA0,
            .sclk_io_num = SD_CLK,
            .quadwp_io_num = GPIO_NUM_NC,
            .quadhd_io_num = GPIO_NUM_NC,
            .max_transfer_sz = 400000,
        };

        esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &bus_cnf, SPI_DMA_CH_AUTO);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SD SPI bus init failed: %s", esp_err_to_name(err));
            return;
        }

        sdspi_device_config_t slot_cnf = {
            .host_id = SD_SPI_HOST,
            .gpio_cs = SD_CS,
            .gpio_cd = SDSPI_SLOT_NO_CD,
            .gpio_wp = GPIO_NUM_NC,
            .gpio_int = GPIO_NUM_NC,
        };

        esp_vfs_fat_sdmmc_mount_config_t mount_cnf = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 16 * 1024,
        };

        sdmmc_card_t* card = nullptr;
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cnf, &mount_cnf, &card);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SD card mount failed: %s", esp_err_to_name(err));
            is_sdcard_found_ = false;
            return;
        }
        ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);
        is_sdcard_found_ = true;
    }

    void DoEnterRadioMode(int station_index) {
        auto& app = Application::GetInstance();

        // Close any active audio channel and abort conversation
        app.AbortSpeaking(kAbortReasonNone);
        app.CloseAudioChannel();
        app.GetAudioService().ResetDecoder();

        if (network_radio_.IsRunning()) {
            network_radio_.Stop();
        }
        if (sdcard_player_.IsRunning()) {
            sdcard_player_.Stop();
        }

        app.SetDeviceState(kDeviceStateIdle);
        app.SetDeviceState(kDeviceStateNetworkRadio);
        power_save_timer_->SetEnabled(false);
        network_radio_.Start(station_index);
        GetDisplay()->ShowNotification("📻 网络收音机");
    }

    void DoEnterSdCardMp3Mode() {
        if (!is_sdcard_found_) {
            GetDisplay()->ShowNotification("未检测到SD卡");
            return;
        }

        auto& app = Application::GetInstance();

        // Close any active audio channel and abort conversation
        app.AbortSpeaking(kAbortReasonNone);
        app.CloseAudioChannel();
        app.GetAudioService().ResetDecoder();

        if (network_radio_.IsRunning()) {
            network_radio_.Stop();
        }
        if (sdcard_player_.IsRunning()) {
            sdcard_player_.Stop();
        }
        app.SetDeviceState(kDeviceStateIdle);
        app.SetDeviceState(kDeviceStateSdCardMp3);
        power_save_timer_->SetEnabled(false);
        sdcard_player_.Start();
        GetDisplay()->ShowNotification("🎵 SD卡MP3");
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool("self.audio_player.start_radio",
            "Start playing network radio. station_index selects a preset station (0-based).",
            PropertyList({
                Property("station_index", kPropertyTypeInteger, 0)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int station_index = properties["station_index"].value<int>();
                auto& app = Application::GetInstance();
                app.Schedule([this, station_index]() {
                    this->DoEnterRadioMode(station_index);
                });
                return true;
            });

        mcp_server.AddTool("self.audio_player.start_mp3",
            "Start playing MP3 files from the SD card.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                auto& app = Application::GetInstance();
                app.Schedule([this]() {
                    this->DoEnterSdCardMp3Mode();
                });
                return true;
            });

        mcp_server.AddTool("self.audio_player.stop",
            "Stop radio or MP3 playback and return to voice chat mode.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                auto& app = Application::GetInstance();
                app.Schedule([this]() {
                    this->StopPlayback();
                });
                return true;
            });
    }

    void StopPlayback() {
        if (network_radio_.IsRunning()) {
            ExitRadioMode();
        } else if (sdcard_player_.IsRunning()) {
            ExitSdCardMp3Mode();
        }
    }

public:
    XINGZHI_CUBE_1_54TFT_WIFI() :
        boot_button_(BOOT_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeSpi();
        InitializeButtons();
        InitializeSt7789Display();
        InitializeSDcardSpi();
        GetBacklight()->RestoreBrightness();
        InitializeTools();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging && !network_radio_.IsRunning() && !sdcard_player_.IsRunning()) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    void StopAudioPlayer() override { StopPlayback(); }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(XINGZHI_CUBE_1_54TFT_WIFI);
