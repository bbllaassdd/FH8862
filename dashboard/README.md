# Windows 答辩控制台

## 启动

回到 Project 根目录，双击 `start_dashboard.bat`。保持弹出的 PowerShell
窗口运行；关闭窗口或按 Ctrl+C 会停止网页服务。

默认地址：`http://localhost:8862/`

服务只监听 Windows 本机回环地址，不开放给局域网，也不要求管理员权限。

## 功能

- 打开 VLC 主码流：`udp://@:1234`
- 打开 VLC 子码流：`udp://@:1235`
- 使用 VLC 在 Windows 自动录制任一路 60 秒 TS
- 显示录像倒计时和进度
- 查看并用 VLC 播放 `Project\recordings` 中的文件
- 导入从虚拟机/NFS 复制到 Windows 的 H.264、TS、MP4、MKV
- 设置主子码流分辨率、帧率、码率并生成 C 配置头文件

## 参数生效方式

页面保存参数后会同时更新：

- `dashboard/config.json`：页面当前配置
- `src/project_stream_config.h`：FH8862 编译配置

之后将 Project 复制到 Ubuntu 并执行：

```sh
cd ~/resource/Project
make clean
make -j4
```

把新的 `project_fh8862` 复制到 NFS/开发板后，编码参数才会改变。

## 录像位置

PC 端录像和导入文件都位于：

```text
Project\recordings\
```

板端录像原本生成在开发板程序的当前目录。如果程序从 NFS 的
`/mnt/Project` 运行，Ubuntu 的 NFS 共享目录可直接看到它；把文件复制到
Windows 后，可通过页面“导入虚拟机录像”统一管理和播放。
