# 构建指南

## 当前板型

| 项 | 值 |
|---|---|
| Board Type | **无名科技星智1.54(WIFI)** (`BOARD_TYPE_XINGZHI_CUBE_1_54TFT_WIFI`) |
| 芯片 | ESP32-S3 |
| Flash | 16MB, QIO, 80MHz |
| 分区表 | `partitions/v2/16m.csv` |
| 屏幕 | 240x240 TFT (SPI) |
| 音频 | I2S, 16kHz 输入 / 24kHz 输出 |
| SD 卡 | SPI 模式 (SPI2_HOST) |
| 唤醒词 | AFE Wake Word |
| 字体 | Noto 20_4 + Font Awesome 20_4 + Noto Emoji 128 |
| ESP-IDF | ≥ 5.5.2 |

## 环境准备 (首次)

```bash
# 1. 安装 ESP-IDF 5.5.2+
git clone -b v5.5.2 --recursive https://github.com/espressif/esp-idf.git ~/esp-idf-v5.5.2
cd ~/esp-idf-v5.5.2
./install.sh esp32s3

# 2. 克隆项目
git clone https://github.com/yuhuan417/xiaozhi-esp32.git
cd xiaozhi-esp32

# 3. 加载 IDF 环境
source ~/esp-idf-v5.5.2/export.sh
```

## 首次构建

```bash
# 确保当前是 esp32s3 目标
idf.py set-target esp32s3

# 用 menuconfig 选择板型（或直接使用已有的 sdkconfig）
idf.py menuconfig
# 进入 "Xiaozhi Assistant" → "Board Type" → 选择 "无名科技星智1.54(WIFI)"

# 构建
idf.py build
```

## 从已有 sdkconfig 构建 (快速重建)

如果 `sdkconfig` 文件已存在且配置正确，跳过 menuconfig：

```bash
idf.py build
```

## 烧录

```bash
# USB 直接烧录
idf.py -p /dev/ttyACM0 flash

# 或指定端口
idf.py -p /dev/ttyUSB0 flash monitor
```

## 关键功能开关

| Kconfig 项 | 当前值 | 说明 |
|---|---|---|
| `CONFIG_USE_WECHAT_MESSAGE_STYLE` | n | 微信风格聊天气泡 |
| `CONFIG_USE_AFE_WAKE_WORD` | y | AFE 前端唤醒词 |
| `CONFIG_USE_AUDIO_PROCESSOR` | y | 音频处理器 (AEC/NS) |
| `CONFIG_USE_MULTILINE_CHAT_MESSAGE` | n | 多行聊天消息 |
| `CONFIG_USE_EMOTE_MESSAGE_STYLE` | n | Emote 表情动画风格 |

## OTA 更新

分区表为 16MB OTA 双区模式 (`ota_0` + `ota_1` 各 4MB)，支持 OTA 固件升级。

## 网络电台 / SD 卡 MP3

SD 卡使用 SPI 模式，挂载点 `/sdcard`。电台列表在 `config.h` 的 `RADIO_STATION_LIST` 宏中配置。
