# reSpeaker ESP32-S3 USB Bridge

[English](README.md) | **中文**

本项目是用于连接 reSpeaker XVF3800 的 ESP32-S3 USB 音频与控制桥接固件，基于 Arduino 开发。
通过一个原生 USB 接口，即可同时提供立体声录音、立体声播放，以及通过 I2C 控制 reSpeaker XVF3800 的 CDC 串口。

## 固件功能

- 通过全速 USB 提供 USB Audio Class 1（UAC1）录音和播放功能。
- 使用立体声、有符号 32 位小端 PCM 数据，以及 32 位 Philips I2S 时隙。
- 编译时可选择 16 kHz 或 48 kHz 采样率，以及 I2S 时钟源。
- 支持 USB 同时录音和播放；USB 与 I2S 使用相同的标称采样率。
- 播放端提供缓冲和软件时钟漂移校正，支持 USB 主机控制总音量、各声道音量及静音。
- 录音端不进行播放音量处理或软件重采样。
- 将 USB CDC 命令转发到 reSpeaker XVF3800 的 I2C 控制接口，并提供 Python 主机工具及只读音频/USB 诊断功能。
- 使用项目内的 TinyUSB DWC2 驱动和 USB 描述符，使双向音频与 CDC 的 FIFO 分配满足 ESP32-S3 硬件容量限制。

### 音频模式

在 [bridge_config.h](bridge_config.h) 中设置 `BRIDGE_MODE`，然后重新编译整个工程。
**宏名称中的 Master 和 Slave 指的是 reSpeaker XVF3800 的 I2S 主从角色。** ESP32-S3 使用相反的角色。

| 模式 | 采样率 | reSpeaker 角色 | ESP32-S3 角色 | 时钟源 |
| --- | --- | --- | --- | --- |
| `BRIDGE_I2S_MASTER_48K` | 48 kHz | 主设备 | 从设备 | reSpeaker |
| `BRIDGE_I2S_MASTER_16K` | 16 kHz | 主设备 | 从设备 | reSpeaker |
| `BRIDGE_I2S_SLAVE_16K` | 16 kHz | 从设备 | 主设备 | ESP32-S3 |

当前源码选择的是 `BRIDGE_I2S_SLAVE_16K`：

```cpp
#ifndef BRIDGE_MODE
#define BRIDGE_MODE BRIDGE_I2S_SLAVE_16K
#endif
```

reSpeaker XVF3800 必须运行与所选采样率和 I2S 角色相匹配的固件。
本桥接固件不会配置或烧录 reSpeaker。切换模式需要重新编译；不支持运行时切换采样率，也不提供 16/48 kHz 之间的转换。

播放端使用线性插值校正微小的时钟差异，因此不保证逐位无损（bit-perfect）。
主机的播放音频经 USB 传输到 ESP32-S3，再由其 I2S 输出将同一条音频流同时发送给 reSpeaker XVF3800 和 codec。
reSpeaker XVF3800 接收播放参考信号，codec 则将音频转换为播放输出。
codec 的初始化和配置由 reSpeaker XVF3800 固件完成；ESP32-S3 桥接固件负责提供 I2S 音频流。

音频接线、缓冲和诊断说明见 [playback_cn.md](playback_cn.md)。
I2C 接线、Python 工具和 CDC 协议说明见 [control_cn.md](control_cn.md)。

## 编译

### 环境要求

- 支持原生 USB 的 ESP32-S3 开发板；项目提供的编译配置针对 Seeed Studio XIAO ESP32S3。
- Arduino IDE 或 Arduino CLI，并安装 Espressif 开发板支持包。
- Arduino-ESP32 **3.3.11**，版本已在 [sketch.yaml](sketch.yaml) 中固定。

无需单独安装 USB 音频库。请使用指定的核心版本，因为项目内用于替换默认实现的 USB 驱动和描述符依赖该版本的 TinyUSB 实现。

请将完整仓库放在名为 `reSpeaker_ESP32S3_USB_BRIDGE` 的文件夹中，与 `reSpeaker_ESP32S3_USB_BRIDGE.ino` 的名称保持一致。
如果下载的 ZIP 解压后在文件夹名称中带有分支后缀，请先重命名文件夹，再打开工程。

编译时必须保留整个工程目录，包括头文件、`.cpp` 文件、`sketch.yaml` 和 `src/tinyusb/dwc2`。
仅复制 `.ino` 文件无法完整构建本项目。

### Arduino CLI

在仓库根目录执行：

```sh
arduino-cli compile --profile xiao_esp32s3 .
```

该配置会选择开发板、核心版本和 USB 设置。若要切换音频模式，请在编译前修改 `BRIDGE_MODE`。

### Arduino IDE

打开 `reSpeaker_ESP32S3_USB_BRIDGE.ino`，安装并选择 Arduino-ESP32 3.3.11，然后使用以下设置：

| 设置项 | 值 |
| --- | --- |
| Board | Seeed Studio XIAO_ESP32S3 |
| USB Mode | USB-OTG (TinyUSB) |
| USB CDC On Boot | Disabled |
| USB Firmware MSC On Boot | Disabled |
| USB DFU On Boot | Disabled |
| CPU Frequency | 240 MHz |
| PSRAM | Disabled |

点击 **Verify（验证）** 进行编译。固件会自行创建 CDC 接口，因此必须保持 CDC On Boot 为 Disabled。
在项目固定的 CLI 配置中，`CDCOnBoot=cdc` 对应 Disabled。

控制 CDC 接口支持接收 Arduino 的重启进入 bootloader 请求，用于通过 USB 上传固件。
烧录期间音频会停止，重启后恢复。如果板上现有固件无法自动进入下载模式，请按住 BOOT，按下并松开 RESET，再松开 BOOT，然后选择下载端口。
重启行为和恢复方法详见 [control_cn.md](control_cn.md)。

使用其他 ESP32-S3 开发板时，请根据实际接线调整开发板选项和引脚常量。音频缓冲区和 DMA 使用片内 SRAM。
