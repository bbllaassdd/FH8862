# FH8862 实践大作业功能与运行文档

## 1. 工程说明

- 开发板：FH8862
- 小组名称：四大三队
- 最终程序：`project_fh8862`
- 作业图片备份：[docs/assignment_reference.jpg](docs/assignment_reference.jpg)
- 主源码：[src/main.c](src/main.c)
- 板端录像模块：[src/libdmc_record_raw.c](src/libdmc_record_raw.c)
- LED 驱动：[led_driver/stream_led.c](led_driver/stream_led.c)
- PC 录像工具：[tools/record_pc_60s.bat](tools/record_pc_60s.bat)、[tools/record_pc_60s.sh](tools/record_pc_60s.sh)

## 2. 已实现功能

| 项目 | 实现结果 | 关键位置 |
| --- | --- | --- |
| 图像旋转 180 度 | `mirror + flip`，ISP 方向值为 3 | `HOMEWORK_ISP_ORIENTATION` |
| 红色中文队名 | 左上角显示“四大三队” | `PROJECT_TEAM_NAME_GB2312`、`project_set_osd()` |
| 删除原学校 OSD | 最终 OSD 不再调用原学校名称 | `project_set_osd()` |
| OSD 自动反色 | 时间和码率按字符反色；红色队名固定不反色 | `FH_OSD_INVERT_BY_CHAR` |
| 标准日期和时间 | 从 NFS 服务器自动校时，OSD 直接读取系统时间；无 NFS 时有两级兜底 | `clock_tags`、`project_sync_system_time()` |
| 实时码率 OSD | 每秒按实际编码字节更新 `bitrate: xxx bps` | `homework_update_bitrate_osd()` |
| 设备端 1 分钟录像 | GPIO24 按下才开始；1280x720、25 fps、PTS 精确截止、约 15 MB | `libdmc_record_raw.c` |
| 主码流 VLC | 1280x720、25 fps、H.264 CBR 1.75 Mbps | UDP 端口 1234 |
| 子码流 VLC | 640x360、15 fps、H.264 CBR 512 Kbps | UDP 端口 1235 |
| 按键录像与灯效 | GPIO24 按下后请求 IDR；可解码关键帧落盘时触发四灯流水 3 秒；满 60 秒全灭 | `project_record_button_poll()`、`dmc_record_start()` |
| PC 端 1 分钟录像 | Windows VLC 和 Ubuntu FFmpeg 脚本 | `tools/record_pc_60s.*` |
| 两块隐私马赛克 | 左上、右下各一块，使用 VPU 硬件处理 | `FH_VPSS_SetMask()` |
| 基础画质控制 | 自动曝光、自动白平衡、饱和度、亮度、基础降噪 | `isp_set_param()` |

## 3. 关键代码说明

### 3.1 中文队名不乱码

FH8862 的 OSD 字库使用 GB2312，而 Windows 编辑器通常保存 UTF-8。直接在 C 字符串中写中文可能在 Ubuntu 编译后乱码，因此代码保存明确的 GB2312 字节：

```c
static const FH_CHAR PROJECT_TEAM_NAME_GB2312[] = {
    (FH_CHAR)0xcb, (FH_CHAR)0xc4, /* 四 */
    (FH_CHAR)0xb4, (FH_CHAR)0xf3, /* 大 */
    (FH_CHAR)0xc8, (FH_CHAR)0xfd, /* 三 */
    (FH_CHAR)0xb6, (FH_CHAR)0xd3, /* 队 */
    0
};
```

队名使用独立 OSD 层，正常颜色设置为红色：

```c
layer[1].normalColor.fRed = 255;
layer[1].normalColor.fGreen = 0;
layer[1].normalColor.fBlue = 0;
```

### 3.2 自动反色

信息层启用按字符亮度反色，红色队名层固定不反色。SDK 检测时间和码率字符所在区域的画面亮度；亮背景使用黑色，暗背景使用白色。

```c
layer[0].osdInvertEnable = FH_OSD_INVERT_BY_CHAR;
layer[0].osdInvertThreshold.high_level = 220;
layer[0].osdInvertThreshold.low_level = 200;
```

阈值设置了 20 的回差，画面处于临界亮度时不容易来回闪色。验收时可把白纸和黑色物体分别移动到文字后面，直观看到切换效果。

### 3.3 设备端录像时长与大小

录像回调只保存主编码通道，并使用编码帧 PTS 计算时间。到达 60 秒前先结束上一完整帧，避免 `sleep()`、启动耗时或调度误差影响录像时长。

```c
#define HOMEWORK_RECORD_DURATION_US (60ULL * 1000ULL * 1000ULL)

if (elapsed_us >= HOMEWORK_RECORD_DURATION_US) {
    finish_record(record, elapsed_us);
    return 0;
}
```

主码流 CBR 为 1,750,000 bps。该值根据前一次板端实测结果校准，用来让 60 秒裸 H.264 文件落在 15 MB 的 ±10% 区间。最终仍应以本块板的 `bytes=` 完成日志为准。

### 3.4 实时码率

码率不是显示配置常量，而是累加编码器实际产生的数据长度。每经过约一秒，用 `字节数 × 8 ÷ PTS 时间` 计算 bps，然后更新 OSD：

```c
bitrate_bps = meter->window_bytes * 8ULL * 1000000ULL / elapsed_us;
snprintf(g_bitrate_osd_text, sizeof(g_bitrate_osd_text),
         "bitrate: %llu bps", bitrate_bps);
```

### 3.5 发流指示灯

程序启动后只进行主、子码流预览，四个 LED 保持熄灭，也不会自动创建录像。GPIO24 稳定按下约 60 ms 后，录像模块调用 `FH_VENC_RequestIDR(0)` 并等待主码流关键帧。回调会把缓存的 SPS/PPS 补到文件头，因此按键无论落在 GOP 的哪个位置，生成的裸 H.264 都能独立解码。关键帧首段成功写入时，GPIO43、GPIO44、GPIO52、GPIO53 按顺序单独点亮；每步 187.5 ms，共 16 步（四个完整循环），总计 3 秒。流水结束后 GPIO43 常亮，表示一分钟录像仍在进行；录像满 60 秒并关闭文件后，四个 LED 全部熄灭。

GPIO24 按键和四路流水灯都要求加载最新版 `/dev/stream_led` 驱动。旧 `/dev/helloworld` 仅支持 GPIO43，既不能读取按键，也不能完成四路流水；程序检测到旧驱动时会打印明确警告，但 VLC 预览仍继续运行。不要同时加载两个 LED 驱动，否则 GPIO 会因资源占用而导致新驱动加载失败。

## 4. Ubuntu 22.04 编译

先把 Windows 中整个 `Project` 文件夹复制到虚拟机，例如放到：

```text
/home/fh8862/resource/Project
```

确认交叉编译器已经在 PATH 中：

```sh
arm-fullhanv3-linux-uclibcgnueabi-gcc --version
```

然后编译主程序：

```sh
cd ~/resource/Project
make clean
make -j4
file project_fh8862
```

`file` 应显示 ARM 32 位可执行文件，不能显示 x86-64。

复制到 NFS 共享目录：

```sh
mkdir -p /mnt/nfs_share/Project
cp -f project_fh8862 /mnt/nfs_share/Project/
cp -a driver /mnt/nfs_share/Project/
```

### 编译流水灯驱动

三秒流水灯需要新版 `stream_led.ko`，它按照 `Led.c` 和管脚复用表中已验证的配置控制 GPIO43、44、52、53。不要同时加载旧 `Led.ko`，两个驱动会争用同一组 GPIO。

执行项目根目录的 `./make.sh` 时会同时编译应用、LED 驱动和四灯测试工具，并复制到 NFS 的 `/mnt/nfs_share/Project/`。也可以只编译 LED 部分：

```sh
cd ~/resource/Project/led_driver
make clean
make -j4
file stream_led.ko
cp -f stream_led.ko /mnt/nfs_share/Project/
cp -f stream_led_test /mnt/nfs_share/Project/
```

若内核源码不在默认位置，显式指定：

```sh
make KDIR=~/resource/FH8862_IPC_V1.0.0_20221111/board_support/kernel/linux-4.9
```

## 5. FH8862 板端运行

先加载本项目 `driver` 目录中第四节课已经验证可用的 FH8862 媒体模块。模块、传感器配置和程序必须来自同一套第四节课资源，不能混用旧版 `media_support`。脚本使用相对路径查找 `.ko`，因此必须先进入 `driver` 目录再执行。

```sh
mount -t nfs 192.168.1.1:/mnt/nfs_share /mnt -o nolock
cd /mnt/Project/driver
chmod +x load_modules_FH8862.sh
./load_modules_FH8862.sh
lsmod

cd /mnt/Project
chmod +x project_fh8862
```

加载新版 LED 驱动：

```sh
chmod +x project_fh8862 stream_led_test

# 若以前加载过旧 Led.ko，必须先停止应用并卸载它。
rmmod Led 2>/dev/null
rmmod stream_led 2>/dev/null
insmod stream_led.ko
ls -l /dev/stream_led

# 先检查四个灯，再根据提示按 GPIO24；工具应打印按键已检测。
./stream_led_test
```

如果 `insmod stream_led.ko` 显示 `Device or resource busy`，说明旧 LED 驱动仍占用 GPIO。最稳妥的处理是重启开发板，只加载媒体模块和 `stream_led.ko`，不要再加载旧 `Led.ko`。如果 `/dev/stream_led` 不存在，项目程序会退回 `/dev/helloworld`，终端将打印 `WARNING`，此时只能点亮 GPIO43，无法实现四灯流水。

运行程序。`192.168.1.2` 是接收视频的 Windows 有线网卡地址，`1234` 是主码流端口。程序不再接收手工时间参数：从 `/mnt/Project` 运行时，会通过 NFS 临时时间探针自动取得 Ubuntu 服务器时间并校准开发板系统时钟，OSD 随后直接读取系统时间。

```sh
./project_fh8862 192.168.1.2 1234
```

NFS 自动校时成功时打印 `[time] board clock synchronized automatically from the NFS server`。若程序不在 NFS 目录运行，则使用已有的有效系统时间；系统时间仍为 1970 时，最后退回本次编译时间。
下一行会打印最终时间，例如 `[time] source=NFS, system time=2026-07-22 16:30:00 CST`，可据此直接核对 OSD，不必猜测板卡 RTC 是否准确。

正常日志应包括：

```text
[time] board clock synchronized automatically from the NFS server
[led] /dev/stream_led ready: GPIO24 key and GPIO43/44/52/53 LEDs enabled
[record] ready: press GPIO24 to record one 60-second file
[led] preview active; LEDs are off, press GPIO24 to record
[record] GPIO24 pressed: waiting for the next main-stream frame
[record] started: project_device_1280x720_25fps_60s.h264, target=60 s
[led] recording started: GPIO43->GPIO44->GPIO52->GPIO53 for 3 seconds
[led] recording chase finished
PES: send stream to 192.168.1.2:1234 through UDP
[record] finished: ..., duration=60.xxx s, bytes=...
[led] recording finished: all LEDs are off
```

录像文件保存在程序当前目录。这里程序从 NFS 的 `/mnt/Project` 运行，所以文件会直接出现在 Ubuntu 的 `/mnt/nfs_share/Project` 中。

## 6. VLC 同时预览主、子码流

Windows 有线网卡保持 `192.168.1.2/24`。打开两个 VLC 窗口，分别选择“媒体 -> 打开网络串流”：

```text
udp://@:1234
udp://@:1235
```

- 1234：主码流，1280x720@25 fps
- 1235：子码流，640x360@15 fps

两个窗口必须能够同时播放。不要选择虚拟机的 VMnet1/VMnet8 地址，因为板子实际把 UDP 包发往 Windows 物理有线网卡。

## 7. PC 端录制 1 分钟

### Windows + VLC

在 Windows 命令提示符进入 `Project` 目录并执行：

```bat
tools\record_pc_60s.bat
```

默认录主码流 1234；录子码流可执行：

```bat
tools\record_pc_60s.bat 1235
```

输出文件为 `project_pc_port1234_60s.ts` 或 `project_pc_port1235_60s.ts`。

### Ubuntu + FFmpeg

```sh
cd ~/resource/Project
chmod +x tools/record_pc_60s.sh
./tools/record_pc_60s.sh 1234
```

注意：只有当 UDP 视频确实发往 Ubuntu 虚拟机 IP 时，该脚本才能在 Ubuntu 收到流。当前发往 Windows `192.168.1.2` 时，应使用 Windows 脚本。

## 8. 录像验收

在 Ubuntu 中检查板端文件大小：

```sh
cd /mnt/nfs_share/Project
ls -lh project_device_1280x720_25fps_60s.h264
stat -c 'bytes=%s' project_device_1280x720_25fps_60s.h264
```

15 MB 按十进制计算的合格范围为 13,500,000 到 16,500,000 字节。若老师按 MiB 计算，15 MiB 的 ±10% 范围为 14,155,776 到 17,301,504 字节。

用 FFmpeg 封装成便于拖动播放的 MP4，不重新编码：

```sh
ffmpeg -y -fflags +genpts -r 25 \
  -i project_device_1280x720_25fps_60s.h264 \
  -c:v copy project_device_60s.mp4
```

检查视频参数：

```sh
ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name,width,height,r_frame_rate,duration \
  -of default=noprint_wrappers=1 project_device_60s.mp4
```

## 9. 演示顺序建议

1. 同时打开 VLC 1234 和 1235，证明主、子码流互不影响。
2. 展示画面已经旋转 18  展示发流 LED 闪烁后常亮。
6. 等待板端日志打印 60 秒录像完成，检查文件大小。
7. 运行 PC 端 60 秒录像脚本并播放结果。
8. 展示两块马赛克和基础画质控制，作为额外工程工作量。

## 10. 后续可增加的高分功能

以下功能尚未标记为已实现，可作为时间允许时的第二阶段：

- 按键实时切换红、绿、蓝三种 OSD 主题，并在画面显示当前模式。
- 增加 JPEG 抓拍按键，文件名自动带标准时间。
- 输出录像旁车信息文件，记录起止时间、实际字节数和平均码率。
- 增加编码/网络异常看门狗，码流长时间无数据时自动重启编码通道。
- 若板端具备网络授时条件，增加 NTP 自动校时，减少手工输入时间。
- 按官方媒体链路顺序重新接入高级 ISP，再启用坏点校正、YNR、3DNR 和锐化。当前第四节课固件若在 VPU/编码器启动前调用 `FHAdv_Isp_Init()`，会在打印 `ADV_ISP version` 后段错误，因此默认关闭该可选功能。

当前版本已覆盖图片中全部基础项和四个加分方向，并额外保留双码流、实时码率、双马赛克及基础画质控制功能。
