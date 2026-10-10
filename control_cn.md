# 通过 USB CDC 和 I2C 控制 reSpeaker XVF3800

[English](CONTROL.md) | **中文**

桥接固件在提供 USB 音频录音和播放的同时，还提供一个 USB CDC 串口接口。
控制请求从计算机经过 ESP32-S3，再通过 I2C 发送到 reSpeaker XVF3800。
无论选择哪种 I2S 音频模式，ESP32-S3 始终作为 I2C 主设备。

## 接线与配置

| 信号 | ESP32-S3 | reSpeaker XVF3800 |
| --- | --- | --- |
| SDA | GPIO5 / XIAO D4 | 控制接口 SDA |
| SCL | GPIO6 / XIAO D5 | 控制接口 SCL |
| 地 | GND | GND |

`respeaker_control.cpp` 中的默认配置为：**7 位地址 0x2C**、100 kHz 总线频率，以及 100 ms I2C 超时。
如有需要，可修改其中的 `kSdaPin`、`kSclPin` 或 `kRespeakerAddress`。不要对地址进行移位。

请连接 reSpeaker XVF3800 的 I2C **从设备控制接口**，而非 reSpeaker XVF3800 作为主设备控制 codec 的总线。
各设备应共地，SDA/SCL 应上拉至 3.3 V。板载上拉电阻可能已经足够；如果没有，可从 4.7 kΩ 开始选择。
避免在同一控制总线上连接另一个正在工作的主设备。

reSpeaker XVF3800 固件必须启用 I2C 控制，并实现所请求的命令。
桥接固件不会在启动时写入 reSpeaker XVF3800 参数。

编译设置见 [readme_cn.md](readme_cn.md)。请选择 USB-OTG (TinyUSB)，并关闭 CDC On Boot，因为固件会显式创建 `USBCDC gControlCdc`。
控制端口传输协议消息，不输出调试日志，也不使用 GPIO43/44 上的 UART0。

CDC 触发 bootloader 重启的功能已启用。Arduino 的 1200 波特率请求，或核心能够识别的 DTR/RTS 复位序列，会使开发板重启进入 ROM 下载模式。
正常控制通信使用 115200 波特率。上传固件会中断音频并使 USB 重新枚举；烧录完成并重启后，音频和控制 CDC 恢复。
USB 在 I2S 初始化之前启动，因此即使 I2S 初始化失败，仍可通过 CDC 请求进入 bootloader。

如果板上固件禁用了 CDC 复位，需要首次上传新固件，或应用无法处理 USB 请求，请按住 BOOT，按下并松开 RESET，再松开 BOOT，然后选择下载端口。
自动切换端口的行为取决于主机上传工具，需要在硬件上验证。

## Python 主机工具

从仓库根目录进入 `host`，并安装依赖：

```sh
cd host
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r requirements.txt
python3 bridge_host.py --list-ports
python3 bridge_host.py --list
```

在 Windows 命令提示符中，请使用 `.venv\Scripts\activate.bat` 激活环境，替代 `source` 命令。

以下示例均在 `host` 目录中运行。将 `/dev/cu.usbmodemXXXX` 替换为实际端口，例如 Linux 下的 `/dev/ttyACM0` 或 Windows 下的 `COM5`。
请关闭其他占用该端口的程序。

```sh
# 检查 ESP32 桥接固件，不访问 I2C。
python3 bridge_host.py --port /dev/cu.usbmodemXXXX --ping

# 读取 reSpeaker XVF3800 固件版本。
python3 bridge_host.py --port /dev/cu.usbmodemXXXX VERSION

# 读取和写入参数。
python3 bridge_host.py --port /dev/cu.usbmodemXXXX AUDIO_MGR_MIC_GAIN
python3 bridge_host.py --port /dev/cu.usbmodemXXXX AUDIO_MGR_MIC_GAIN --values 1.0
python3 bridge_host.py --port /dev/cu.usbmodemXXXX PP_AGCONOFF --values 1
python3 bridge_host.py --port /dev/cu.usbmodemXXXX AEC_FIXEDBEAMSAZIMUTH_VALUES --values 0.0 1.5707963

# 这些命令要求 reSpeaker XVF3800 固件提供相应实现。
python3 bridge_host.py --port /dev/cu.usbmodemXXXX DOA_VALUE
python3 bridge_host.py --port /dev/cu.usbmodemXXXX LED_COLOR --values 0x00FF00

# 读取 ESP32 诊断信息，不访问 I2C。
python3 bridge_host.py --port /dev/cu.usbmodemXXXX --audio-status
python3 bridge_host.py --port /dev/cu.usbmodemXXXX --usb-status
```

命令名称不区分大小写。`host/parameters.json` 包含从 `xvf_host.py` 提取的 120 个参数定义的快照。
参数目录中存在某个条目，并不保证所连接的 reSpeaker XVF3800 固件实现了该命令。

支持的类型包括 `uint8`、`uint16`、`uint32`、`int32`、`float`、`radians` 和 `char`，数值使用小端编码。
主机工具检查数值数量、读写权限和整数范围；设备负责检查各命令特有的语义限制。
角度以弧度传入。整数参数不接受小数值，也不会将其截断为整数。

如需使用其他参数目录：

```sh
python3 bridge_host.py --parameters-source /path/to/xvf_host.py \
  --port /dev/cu.usbmodemXXXX VERSION
```

工具会提取 `PARAMETERS` 字面量，不会执行或导入源脚本，也支持使用其他参数 JSON 文件。
无需安装 PyUSB 或 libusb。原脚本的 USB 厂商自定义传输无法访问此 CDC 桥接接口；请使用 `bridge_host.py` 并指定 `--port`。

## 串口协议 v1

端口的 CDC 线路编码使用 115200 波特率；该值不会设置 USB 音频采样率或 I2C 时钟。
打开端口后，请置位 DTR 并发送一个空行以同步；请求以 LF 或 CRLF 结尾。
同一时间只允许有一个等待响应的请求，且只能由一个程序使用端口。固件不会发送启动提示消息。

请求使用 ASCII 格式：

```text
P seq
R seq resid cmd data_length
W seq resid cmd hex_payload
S seq
U seq
```

| 字段或命令 | 含义 |
| --- | --- |
| `seq` | 十进制序列号，范围 1–65535 |
| `resid` | 十进制资源 ID，范围 0–255 |
| `cmd` | 十进制命令 ID，范围 0–127，不包含 I2C 读取标志 |
| `data_length` | 数据长度为 1–64 字节，不包括状态字节 |
| `hex_payload` | 最多表示 64 字节数据的连续十六进制数字，不带 `0x`；`-` 表示空写入 |
| `P` | 桥接 ping，不访问 I2C |
| `S` | 音频状态，不访问 I2C |
| `U` | USB OUT 状态，不访问 I2C |

响应格式如下：

```text
OK seq status hex_data
ERR seq bridge_error
```

对于 I2C 命令，`OK` 仅表示传输已完成；**只有 status 为 0 才表示命令执行成功**。
成功写入返回 `OK seq 0 -`，成功读取返回十六进制数据。reSpeaker XVF3800 状态码非零时，响应中的数据部分为 `-`。
即使 reSpeaker XVF3800 未连接，ping 仍返回 `OK seq 0 7265737065616b6572`，其中载荷对应 ASCII 字符串 `respeaker`。
无法恢复序列号的错误使用序列号 0。

主机工具与固件必须同时使用 `respeaker` 握手标识。使用新版主机工具前，请重新烧录旧版固件；旧握手标识不再被接受。

读取版本示例（示例响应表示版本 3.2.1）：

```text
R 1 48 0 3
OK 1 0 030201
```

启用 AGC 示例：

```text
W 2 17 10 01000000
OK 2 0 -
```

`S` 返回 64 字节，包含 16 个小端 32 位字段，最后一个字段为有符号数。
`U` 返回 64 字节，包含 16 个小端无符号 32 位字段。主机工具也兼容旧版的 32 字节 USB 状态响应。
字段定义和解释见 [playback_cn.md](playback_cn.md#音频状态)。

无效字符或行长度超限会产生 `BAD_LINE`；请求格式错误会产生 `BAD_REQUEST`。
如果一行尚未结束，且一秒内没有收到新字节，则产生 `RX_TIMEOUT`，之后丢弃输入，直到收到下一个换行符。
连接断开或 CDC RX 溢出后，也需要通过换行符重新同步。主机工具会自动执行同步和 ping。

序列号用于匹配请求和响应，不会对写入去重，也不保证跨连接断开场景下命令恰好执行一次。

## I2C 事务与重试

- **读取：** 写入 `[resid, cmd | 0x80, data_length + 1]`，发出重复 START，然后读取 `data_length + 1` 个字节。第一个字节为状态，后续为数据。
- **写入：** 写入 `[resid, cmd, payload_length, payload...]`，发出 STOP，然后单独读取一个状态字节。
- 固件直接转发 reSpeaker XVF3800 状态，不进行重试。Python 工具仅在收到明确的 `64 / RETRY` 状态时重试，尝试间隔为 10 ms，默认最多总计尝试 100 次。其他设备状态立即报错。
- `I2C_WRITE_2` 通常表示地址 NACK；`I2C_WRITE_5` 表示写入超时。`I2C_READ_n` 表示实际接收字节数与预期不符。重复 START 失败可能表现为 `I2C_READ_0`，因为 Wire API 无法区分这条路径上的所有错误原因。
- 传输失败和响应超时不会自动重新发送设备写入命令。响应丢失前，命令可能已经执行。手动重试前应先检查设备状态。本地 ping 握手没有设备副作用，因此可以重试。

专用控制任务运行在核心 0，优先级为 1，依次处理 I2C 和 CDC 操作。
音频回调不执行 I2C 请求。CDC 响应未一次写完时，会在最长 250 ms 的发送窗口内继续发送；会话变化后，不再发送旧 CDC 会话的响应。

## 测试与硬件检查

在仓库根目录使用 Python 3 和 C++17 编译器运行主机测试及固件控制仿真。
这些测试不需要开发板或 pyserial，也不会向硬件发送命令。

```sh
python3 -m unittest discover -s tests -p 'test_*.py' -v
c++ -std=c++17 -Wall -Wextra -Werror \
  -I tests/fakes tests/control_test.cpp -o /tmp/xvf-control-test
/tmp/xvf-control-test
```

测试覆盖参数编码、权限、重试、响应匹配、超时、诊断解码、解析器边界、I2C 事务顺序、读取字节不足、错误处理、CDC 分段写入和会话变化。

在硬件上，应验证音频与 CDC 枚举、ping、`VERSION` 读取，以及受支持参数的写入和回读。
同时录音和播放时进行控制通信测试，并检查 I2C 断开、USB 重新连接及主机挂起/恢复后的恢复情况。
音频验证状态记录在 [playback_cn.md](playback_cn.md#验证) 中。
