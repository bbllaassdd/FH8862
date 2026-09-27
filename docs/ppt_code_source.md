# FH8862 Project 代码答辩素材

## 汇报目标

- 面向课程答辩，15 分钟左右。
- 先解释程序主链路，再按功能落到具体代码层、函数调用和底层 API。
- 最终工程以 `D:/Linux/hello_world/Project` 为主，`D:/Linux/hello_world/demo/demo` 仅用于说明颜色切换和基础 LED 方案。
- 所有结论都来自当前代码，不把队友 demo 与最终 Project 混写。

## 程序主链路

入口：`Project/src/main.c::main()`。

1. `project_sync_system_time()`：媒体初始化前校准 Linux 系统时间。
2. `sample_common_media_driver_config()`、`FH_SYS_Init()`：驱动与媒体系统初始化。
3. `start_isp()`：启动 Sensor/ISP。
4. `FH_VPSS_CreateGrp()`、`FH_VPSS_CreateChn()`、`FH_VPSS_SetChnAttr()`：创建 VPU 主、子通道；主通道输出 1280x720。
5. `sample_set_venc_cfg()`：创建 VENC 主、子码流；主流用于录像与 VLC，子流仅用于 VLC。
6. `FH_SYS_Bind()`：建立 `Sensor -> VICAP -> ISP -> VPU -> VENC` 媒体链。
7. `sample_dmc_init()`：注册 PES/VLC 订阅；`dmc_record_subscribe()`：追加 RECORD 订阅。
8. `project_set_osd()`：加载字库并配置队名、时间、实时码率 OSD。
9. `FH_VENC_StartRecvPic()`：启动两路编码。
10. `sample_common_get_stream_proc()`：独立线程阻塞取流，并通过 `dmc_input()` 同步分发。
11. 主循环每 20 ms 调用 `project_record_button_poll()` 检测 GPIO24。
12. 退出时停止取流、停止 LED、关闭录像文件并注销订阅。

运行时数据流：

`FH_VENC_GetStream_Block()` -> 遍历 H.264 NAL -> `dmc_input()` -> PES/VLC + RECORD -> `FH_VENC_ReleaseStream()`。

## 180 度旋转

- 层级：板端 ISP 参数层，早于 VPU、VENC、DMC。
- `main.c`：`isp_set_param(ISP_MF, HOMEWORK_ISP_ORIENTATION)`，其中取值 3。
- `main.c::isp_set_param()`：`case ISP_MF` 调用 `isp_set_mirrorflip(param)`。
- `isp.c::isp_set_mirrorflip()`：3 映射为 `mirror=1, flip=1`。
- 最底层 API：`API_ISP_SetMirrorAndflip(0, mirror, flip)`。
- 总结：旋转在板端 ISP 输出阶段完成，后续预览、推流与录像得到的都是同一份方向已校正画面。

## OSD 颜色

参考 demo：

- `TEAM_OSD_COLOR_MODE` 选择红、绿、蓝。
- `set_team_osd_color()` 把模式映射为 RGB。
- `sample_set_osd()` 把结果写入 OSD layer 的 `normalColor`。

最终 Project：

- 队名使用独立 `layer[1]`。
- `normalColor = (255,0,0)`，`FH_OSD_INVERT_DISABLE`，始终保持红色。
- 通过 `FHAdv_Osd_Ex_SetText()` 提交 layer 配置。
- 总结：颜色在 OSD 图层配置阶段指定，不是编码后再修改图片；参考 demo 可宏切换，最终工程固定红色以保证验收一致。

## OSD 队名

- 层级：高级 OSD 文本层。
- 每一行由 `FHT_OSD_TextLine_t` 描述。
- `textInfo` 指向实际显示字符串缓冲区。
- 加载 `FHEN_FONT_TYPE_CHINESE` GB2312 字库。
- `PROJECT_TEAM_NAME_GB2312` 保存“四大三队”的 GB2312 字节，避免 Windows/Ubuntu 源码编码差异。
- 复制到第 0 行 `line[0].textInfo`。
- `FHAdv_Osd_Ex_SetTextLine()` 把第 0 行绑定到红色队名层。
- 总结：队名作为独立文本行写入 `textInfo`，中文字节、图层颜色、位置与尺寸均可独立配置。

## OSD 自动反色

- 层级：SDK OSD layer，不修改原始视频像素。
- 信息层：白色 `normalColor`，黑色 `invertColor`。
- `osdInvertEnable = FH_OSD_INVERT_BY_CHAR`。
- `high_level` 与 `low_level` 构成亮度迟滞阈值，避免临界背景频繁闪烁。
- 按字符判断：同一行长日期跨越亮、暗区域时，每个字符可独立切换。
- 队名层关闭反色，始终红色。
- 总结：SDK 在 OSD 合成阶段按字符背景亮度选择白字或黑字，既保留画面，也提升复杂背景下的可读性。

## 标准时间

时间格式：

- 最终 Project 使用 SDK 内嵌时钟标签组成 `YYYY-MM-DD HH:MM:SS`。
- `timeOsdEnable=0`，避免在时钟标签后再次追加 SDK 模板，解决重复时间。
- 时钟标签由 SDK 每秒刷新。

时间来源：

- OSD 读取开发板 Linux 系统时间，不在代码里写死日期字符串。
- `project_sync_system_time()` 启动时优先用 NFS 文件 mtime 自动校时，再进入媒体初始化。
- `date -s` 修改 Linux 系统时间，所以 OSD 立即跟随。
- `hwclock -w` 把当前系统时间写入 RTC，重启后可由 RTC 恢复。
- 总结：显示格式由 SDK 时钟标签定义，时间值来自系统时钟；最终工程增加 NFS 自动校时，手动系统时间与 RTC 路径仍然兼容。

## 设备端 1 分钟录像

- 层级：DMC 码流分发层的一个订阅者，不另起编码链。
- `sample_dmc_init()` 先注册 PES；随后 `dmc_record_subscribe()` 调用 `dmc_subscribe("RECORD", DMC_MEDIA_TYPE_H264, record_input)`。
- `record_input()` 只接受 `media_chn==0` 且 `media_type==H264`，子码流继续预览但不落盘。
- 按键未触发时 `active==0`，回调直接返回，不影响 VLC。
- 按键触发后，下一帧到来时 `fopen(...,"wb")` 创建 `project_device_1280x720_25fps_60s.h264`。
- 每个 NAL 在 `FH_VENC_ReleaseStream()` 前同步 `fwrite()`。
- 第一帧 PTS 为时间原点；`elapsed_us = frame_pts - first_pts`。
- 达到 60,000,000 us 时，在阈值帧首个 NAL 写入前调用 `finish_record()`，保留上一完整帧。
- `finish_record()` 顺序为 `fflush -> fsync -> fclose`，并打印时长和字节数。
- 输出是相对路径，文件生成在程序当前工作目录；从 `/mnt/Project` 运行时 Ubuntu NFS 端可直接看到。
- 总结：录像复用现有 DMC 分发，主流预览和录像并行；用视频 PTS 而不是 wall-clock 控时，时长误差约一帧。

## LED 与按键

基础 demo：

- 层级：用户态 demo 通过字符设备 `/dev/helloworld` 与 LED 驱动交互。
- `set_led_state()`：`open -> ioctl(SET_LED_ON/OFF) -> close`。
- DMC、取流线程和 OSD 准备完毕后，若目标 IP 非空则亮灯，表示推流路径已经配置并开始工作。
- 退出清理前 `set_led_state(0)`，表示发流结束。

最终 Project 增强：

- 设备节点改为 `/dev/stream_led`；GPIO24 作为按键，GPIO43/44/52/53 作为四路 LED。
- 主循环每 20 ms 读取按键，连续 3 次稳定低电平判定按下，约 60 ms 软件消抖。
- 按键只调用 `dmc_record_start()` 改变录像状态，VLC 两路预览始终不停止。
- 第一段 H.264 数据成功写入后才启动灯效，保证“灯亮=真正开始落盘”。
- 4 灯循环约 3 秒；随后 GPIO43 保持亮，表示录像进行中。
- `finish_record()` 关闭文件后调用 `project_recording_led_finished()`，停止灯效并全部熄灭。
- 总结：基础方案把 LED 与推流状态联动；最终方案进一步把按键、录像状态和四灯提示形成可观察的状态机。

## 实测证据

- VLC 画面含红色“四大三队”、标准时间和实时码率。
- 示例记录：`duration=60.022 s, bytes=15747648 (15.01 MiB)`。
- 输出分辨率 1280x720，主码流 25 fps；子码流使用不同参数并可在另一 VLC 端口同步预览。

## 建议页序

1. 封面：从 Sensor 到文件落盘。
2. 主程序初始化链路。
3. 运行时取流与 DMC 分发链。
4. 180 度旋转。
5. OSD 分层模型。
6. OSD 颜色：参考 demo 到最终 Project。
7. OSD 队名与中文字库。
8. OSD 按字符自动反色。
9. 标准时间：格式与来源。
10. 录像复用 DMC 订阅。
11. 主通道过滤、写盘与文件位置。
12. PTS 控制 60 秒与完整帧边界。
13. 基础 LED：推流状态联动。
14. 增强 LED：GPIO24 按键与四灯录像状态机。
15. 成品展示与实测结果。
16. 总结：一条媒体链、多个同步能力。
