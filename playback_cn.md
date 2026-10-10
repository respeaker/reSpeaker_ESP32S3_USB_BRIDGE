# USB 播放与 I2S 音频

[English](PLAYBACK.md) | **中文**

本固件同时提供立体声 USB 录音、播放和 CDC 控制功能。
USB 与 I2S 使用相同的标称采样率，在编译时通过 `bridge_config.h` 选择。
录音和播放均使用按左、右声道顺序排列的有符号 32 位 PCM 数据。
模式选择和编译说明见 [readme_cn.md](readme_cn.md)。

## 接线与音频模式

下表中的所有引脚方向均以 ESP32-S3 为参照。

| ESP32-S3 引脚 | 连接信号 | 方向 |
| --- | --- | --- |
| GPIO8 | BCLK | reSpeaker XVF3800 为主设备时是输入；ESP32-S3 为主设备时是输出 |
| GPIO7 | WS / LRCLK | reSpeaker XVF3800 为主设备时是输入；ESP32-S3 为主设备时是输出 |
| GPIO43 | 音频 DOUT | 输入：从 reSpeaker XVF3800 接收音频，用于 USB 录音 |
| GPIO44 | reSpeaker XVF3800 音频 DIN 和 codec 音频 DIN | 输出：向两个接收端发送同一条 USB 播放音频流 |
| GND | GND | 共地 |

I2C 使用 GPIO5 作为 SDA、GPIO6 作为 SCL，详见 [control_cn.md](control_cn.md)。
信号电平为 3.3 V。原生 USB D−/D+ 分别使用 GPIO19/20，请通过开发板的原生 USB 接口连接，而非外接 USB 转串口适配器。
ESP32-S3 桥接固件不连接或生成 MCLK；如果 codec 需要 MCLK，必须由电路板的时钟方案提供。

音频格式为标准 Philips I2S：每帧包含两个 32 位时隙，WS 为低电平时对应左声道，高电平时对应右声道，最高有效位（MSB）在 WS 跳变后延迟一个 BCLK 周期传输。

| 采样率 | BCLK | WS | 每个 1 ms 数据块的帧数 | 播放预填充 |
| --- | --- | --- | --- | --- |
| 48 kHz | 3.072 MHz | 48 kHz | 48 | 576 帧 / 12 ms |
| 16 kHz | 1.024 MHz | 16 kHz | 16 | 192 帧 / 12 ms |

在 reSpeaker XVF3800 为主设备的模式下，默认配置将 ESP32-S3 的 I2S0 TX 和 I2S1 RX 设置为从设备，共享外部 BCLK 和 WS。
在 reSpeaker XVF3800 为从设备的模式下，ESP32-S3 使用配对的 I2S0 TX/RX 通道，作为时钟主设备。
使用前需将 reSpeaker XVF3800 配置为匹配的角色和采样率；只能由一个设备驱动 BCLK 和 WS。

所有模式下，GPIO43 和 GPIO44 分别保持为录音输入和播放输出。
如使用不同硬件，请修改 `reSpeaker_ESP32S3_USB_BRIDGE.ino` 中的引脚常量。不要将两个音频输出端连接在一起。

## 播放路径、codec 配置与主机设置

主机播放音频通过 USB 到达 ESP32-S3，再通过其 I2S 输出（GPIO44）发送。
这条数据线同时连接 reSpeaker XVF3800 和 codec 的音频输入端，两者接收相同的播放音频流。
reSpeaker XVF3800 将该音频流用作远端参考信号，codec 则将其转换为播放输出。

codec 的初始化和配置由 reSpeaker XVF3800 固件完成。
ESP32-S3 桥接固件负责提供 I2S 音频数据；codec 的播放数据直接来自这条共享数据线，不经过 reSpeaker XVF3800 转发。

ESP32-S3 输出正确的 I2S 信号，并不代表扬声器一定能正常出声。
如果播放无声，请检查两个接收端的 GPIO44 信号、codec 所需的时钟，以及 reSpeaker XVF3800 固件中的 codec 初始化、输出配置、静音和音量设置。
reSpeaker XVF3800 的参考输入配置需单独检查。

烧录或切换模式后，请重新连接 USB，并重新选择录音设备、播放设备和 CDC 端口。
USB 产品名称随所选模式变化：

| 模式 | USB 产品名称 |
| --- | --- |
| reSpeaker XVF3800 主设备，48 kHz | `BRIDGE I2S Master 48k` |
| reSpeaker XVF3800 主设备，16 kHz | `BRIDGE I2S Master 16k` |
| reSpeaker XVF3800 从设备，16 kHz | `BRIDGE I2S Slave 16k` |

每种模式都有独立的 USB 序列号字符串。某些主机的音频菜单会显示 `TinyUSB UAC1`，而非 USB 产品名称。
请选择与编译采样率一致的立体声 32 位音频格式。总音量、各声道音量和静音仅作用于播放端。

## 播放缓冲与时钟匹配

专用 USB 读取任务是 TinyUSB 音频 OUT FIFO 的唯一读取方。
它在 `USB.begin()` 之后启动，等待 USB 配置完成和音频类挂载，再进行有次数或长度限制的读取，将 PCM 数据提交给播放缓冲区。
固件不调用 `gUac.begin()`，因为指定版本的核心会因此创建另一个 FIFO 读取任务。
USBAudioCard 仍负责音频接口注册、录音数据写入，以及音量和静音处理。

播放处理流程如下：

1. 将收到的字节组装为完整的立体声帧，放入容量为 4096 帧（32 KiB）的应用层 FIFO。
2. TX 任务根据 I2S DMA 的数据消耗，以 1 ms 为单位提供数据块。累计 12 ms 数据后开始播放。
3. 对 FIFO 占用量进行滤波后送入 PI 控制器，在 ±5000 ppm 范围内调整分数采样步长。两个声道使用相同的插值相位，采用 24 位小数精度和 64 位中间运算。
4. 重采样后应用 USB 总音量、各声道音量和静音设置。

USB 播放使用自适应 OUT 端点，录音使用异步 IN 端点。播放端不使用显式反馈端点。
软件速率校正用于处理相同标称采样率下的微小时钟差异，不提供任意采样率转换。

**播放不保证逐位无损（bit-perfect）。** 线性插值会改变采样值，并影响高频响应。
它用于校正时钟漂移，并非带限异步采样率转换器。

缓冲引入的延迟包括约 12 ms 的应用层预填充、最多约 8 ms 的 TX DMA 缓冲，以及主机和 USB 缓冲。
这些数值不是实测的端到端延迟，也不包含 reSpeaker XVF3800 和 codec 的处理时间。

录音端保留输入的 32 位采样值，不进行播放音量处理或软件重采样。应用层环形缓冲区可容纳 2048 帧。
录音未启用时收到的数据会被丢弃；USB 背压另行处理。输入时钟缺失时，固件不会自行生成录音采样数据。

## 停止、欠载与恢复

- 播放未启用、USB 挂起或断开连接时，TX 输出静音数据。
- 播放会话发生变化时，先禁用 TX，将 TX DMA 填零，再重新启动 TX；RX 保持运行。已发送的采样数据无法撤回。
- 当任务无法提供新数据时，DMA 自动清零可避免旧数据块被无限重复播放。
- FIFO 欠载时，清空剩余数据并重新预缓冲；溢出时，丢弃完整的新帧并增加计数。
- I2S 写入超时为 20 ms。写入失败时，清空播放缓冲并重置 TX DMA；恢复传输后重新进行播放预缓冲。
- CDC/I2C 操作在独立任务中执行，不在音频回调中处理。

调试器可查看的统计项包括 `gPlayback.stats.received`、`rendered`、`dropped`、`underruns`、`highWater` 和 `correctionPpm`。
`gPlayback.available()` 返回队列中的帧数。`gStats.txTimeouts` 和 `txErrors` 记录 I2S 写入失败次数。

## 音频状态

在 `host` 目录中执行以下命令，并将端口替换为实际设备端口：

```sh
python3 bridge_host.py --list-ports
python3 bridge_host.py --port /dev/cu.usbmodemXXXX --audio-status
python3 bridge_host.py --port /dev/cu.usbmodemXXXX --usb-status
```

这两种状态查询只读取 ESP32 状态，不访问 I2C，也不改变音量。
播放非静音音频时，可对比间隔约一秒获取的状态快照。

`S seq` 音频响应按传输顺序包含下列字段。每个字段占一个小端 32 位字；只有 `master_volume_db` 为有符号数。

| 字段 | 含义 |
| --- | --- |
| `flags` | 位 0：播放已启用；位 1：使用独立的 I2S 控制器。正常播放时，默认的 reSpeaker XVF3800 主设备配置通常为 3，从设备模式为 1。 |
| `usb_callbacks` | 专用 USB 读取任务向播放缓冲区提交 PCM 的次数 |
| `usb_bytes` | 提交到播放缓冲区的字节数 |
| `usb_peak` | 最近一次提交的 USB 数据块的采样峰值 |
| `capture_frames` | 从 I2S RX 读取的帧数 |
| `rendered_frames` | 重采样器生成的播放帧数 |
| `tx_bytes` | 已交给 I2S 驱动的字节数，包括静音数据；不代表已确认在线路上输出 |
| `tx_peak` | 最近生成的 TX 数据块在音量处理后的峰值 |
| `tx_dma_completions` | TX DMA 完成回调次数 |
| `tx_timeouts` | I2S TX 超时次数 |
| `tx_errors` | 其他 I2S TX 错误次数 |
| `fifo_frames` | 当前播放 FIFO 中的帧数 |
| `dropped_frames` | 因播放 FIFO 溢出而丢弃的帧数 |
| `underruns` | 播放 FIFO 欠载次数 |
| `mute_mask` | 位 0：总静音；位 1：左声道静音；位 2：右声道静音 |
| `master_volume_db` | 总音量，单位为 dB；0 表示无衰减。不包含各声道音量。 |

计数器可能在达到 32 位上限后回绕。峰值描述的是最近的数据块，静音片段中可能为零。
不同模块的状态快照使用独立的锁，因此整个音频链路的快照并非原子操作。
软件峰值非零且 DMA 正在运行，并不能证明外部设备已正确接收 I2S 数据；排查路由或接线问题时，应检查物理信号。

## USB OUT 状态

`U seq` 响应按传输顺序包含下列无符号、小端 32 位字段。
当前固件返回全部 16 个字段；主机工具也兼容只包含前八个字段的旧版响应。

| 字段 | 含义 |
| --- | --- |
| `reader_loops` | 专用 USB 读取任务的循环次数 |
| `fifo_high_water` | 观测到的 TinyUSB OUT FIFO 最大占用量，单位为字节 |
| `out_packets` | FIFO 接收并重新准备传输后触发的音频接收回调次数 |
| `out_packet_bytes` | 这些接收回调报告的字节数 |
| `out_endpoint` | 最近一次接收回调的端点号；播放时预期为 1 |
| `read_bytes` | 从 TinyUSB OUT FIFO 读取的总字节数 |
| `usb_flags` | 位 0：USB 已配置；位 1：音频类已挂载；位 2：已挂起。正常运行时通常为 3。 |
| `reader_stack_min` | 读取任务的最小剩余栈空间，单位与 ESP-IDF FreeRTOS API 一致 |
| `doepctl1` | OUT 端点 1 控制寄存器 |
| `doeptsiz1` | OUT 端点 1 传输大小寄存器 |
| `doepint1` | OUT 端点 1 中断寄存器 |
| `gintsts` | 全局中断状态寄存器 |
| `gintmsk` | 全局中断屏蔽寄存器 |
| `dsts` | 设备状态寄存器 |
| `dctl` | 设备控制寄存器 |
| `grxfsiz` | 硬件接收 FIFO 大小，以 32 位字为单位 |

读取这些寄存器不会弹出 FIFO 数据或改变硬件状态，但多个寄存器的读取不是原子操作。
数据包计数来自软件回调，并非 USB 分析仪的测量结果。在首次回调之前端点值为 0，并不表示端点不存在。

如果读取任务的循环次数增加，但数据包计数和 FIFO 占用量一直为零，请检查主机播放、端点设置和 USB 协议栈。
如果数据包已到达，但 `read_bytes` 不增长，请检查 FIFO 读取流程。
如果读取量增长，请结合音频状态检查预缓冲、数据生成、静音、音量和 TX 进度。

## USB 硬件 FIFO 容量分配

ESP32-S3 USB 控制器具有 256 个字的 FIFO RAM。
项目内的 TinyUSB 0.21.0 DWC2 驱动为一个最大接收包及 setup/status 预留空间。
CDC 通知包为 16 字节，CDC 批量传输包为 32 字节，从而为双向立体声音频留出空间。

| 采样率 | 最大音频包 | RX FIFO | RX + IN 总分配量 |
| --- | --- | --- | --- |
| 48 kHz | 392 字节 | 127 个字 | 253 / 256 个字 |
| 16 kHz | 136 字节 | 63 个字 | 125 / 256 个字 |

这些硬件 FIFO 与应用层音频缓冲区相互独立。增加 USB 设备类或数据包大小前，必须重新计算空间分配。
单包 RX 缓冲对中断延迟的容忍度低于双缓冲，因此需要在 CDC 并发通信时验证双向音频。

描述符测试与固件使用相同的配置定义。
更换核心或工具链版本时，应确认链接的是项目内的 DWC2 目标文件，而非 SDK 预编译驱动。
源码来源和替换机制见 [src/tinyusb/dwc2/README.md](src/tinyusb/dwc2/README.md)。

## 验证

项目记录表明，48 kHz 录音、播放和 CDC 控制已在硬件上正常工作。
16 kHz 模式仍需硬件验证；在源码中选择某种模式，不代表该模式已经通过测试。

播放仿真覆盖帧组装、插值、停止时静音、溢出和欠载恢复。
同时覆盖 0、±100 和 ±1000 ppm 时钟漂移，每种情况下模拟三分钟，USB 数据以 2 ms 突发方式到达。

在仓库根目录使用支持未定义行为检测器（UndefinedBehaviorSanitizer）的 C++17 编译器运行测试。
以下命令显式选择 48 kHz 测试模式：

```sh
c++ -std=c++17 -O2 -fsanitize=undefined -Wall -Wextra -Werror \
  -DBRIDGE_MODE=0 tests/playback_test.cpp -o /tmp/xvf-playback-test
/tmp/xvf-playback-test
```

将参数分别替换为 `-DBRIDGE_MODE=1` 和 `-DBRIDGE_MODE=2`，即可测试两种 16 kHz 播放配置。
这些是软件仿真，不是对物理 I2S 主从角色的测试。
控制和 Python 测试命令见 [control_cn.md](control_cn.md#测试与硬件检查)。

硬件验收步骤：

1. 确认 USB 音频输入和输出均以编译时指定的采样率完成枚举。
2. 左、右声道分别播放不同的测试音。使用逻辑分析仪解码 GPIO44，检查 reSpeaker XVF3800 和 codec 输入端的 Philips I2S 帧格式、时隙宽度、声道顺序和电平。检查 reSpeaker XVF3800 的参考信号接收和 codec/扬声器输出，包括 reSpeaker XVF3800 固件对 codec 的配置。
3. 使用已知的立体声输入验证录音，并检查播放音量和静音功能。
4. 同时录音和播放至少 30 分钟，期间通过 CDC 读取受支持的 reSpeaker XVF3800 参数。观察 FIFO 丢帧、欠载、I2S 错误及声道连续性。
5. 测试仅播放、仅录音、音频流关闭与重开、USB 重新连接、主机挂起与恢复，以及 reSpeaker XVF3800 主设备模式下的外部时钟丢失与恢复。确认控制功能仍然可用，且旧音频不会重复播放。

Linux 下可使用 `aplay -l` 和 `arecord -l` 列出设备。48 kHz 固件的示例命令如下：

```sh
aplay -D hw:CARD,DEV stereo_48k_s32.wav
arecord -D hw:CARD,DEV -t wav -f S32_LE -r 48000 -c 2 -d 10 capture.wav
```

将 `CARD,DEV` 替换为实际设备选择。播放文件应为与编译采样率一致的立体声、有符号 32 位 WAV 文件；
对于 16 kHz 固件，请使用 16 kHz 文件，并将录音参数改为 `-r 16000`。
