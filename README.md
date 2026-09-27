# FH8862 实践大作业：四大三队

本目录是基于第四节课可运行 `demo` 独立出的最终工程。功能、关键代码、编译、板端运行和验收步骤统一记录在 [PROJECT_FEATURES.md](PROJECT_FEATURES.md)。

最终程序名：`project_fh8862`

最常用的板端启动命令：

```sh
./project_fh8862 192.168.1.2 1234
```

- 主码流：VLC 打开 `udp://@:1234`
- 子码流：VLC 打开 `udp://@:1235`
- GPIO24：按下后开始板端一分钟录像
- 板端录像：`project_device_1280x720_25fps_60s.h264`

详细实现和验收步骤以 `PROJECT_FEATURES.md` 为准。

答辩前请重点阅读 `DEFENSE_GUIDE.md`。其中包含 Project 目录逐项说明、
必须精读的 .c 文件、按键/录像/流水灯/主子码流总流程、课程知识点映射和
高频答辩问答。源码中的新增中文注释可搜索“答辩重点”快速定位。

## Windows 答辩控制台

在 Windows 中双击 `start_dashboard.bat`，浏览器会打开：

```text
http://localhost:8862/
```

控制台可以打开主/子码流 VLC、分别录制 60 秒 TS、显示录像倒计时、
导入从虚拟机复制到 Windows 的 H.264 文件并调用 VLC 播放。录像统一保存到
`Project\recordings\`。

页面修改编码参数后会更新 `src/project_stream_config.h`。参数不会直接进入
正在运行的开发板，必须把 Project 复制到 Ubuntu、重新 make，再部署新程序。
