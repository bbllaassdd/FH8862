#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>

#include "fh_system_mpi.h"
#include "fh_vpu_mpi.h"
#include "fh_venc_mpi.h"
#include "FHAdv_OSD_mpi.h"
/* FHAdv_Isp_mpi_v3.h exposes an API taking struct isp_sensor_if *. */
#include "isp_sensor_if.h"
#include "FHAdv_Isp_mpi_v3.h"
#include "fh_vb_mpipara.h"
#include "fh_vb_mpi.h"

#include "libdmc.h"
#include "libdmc_pes.h"
#include "libdmc_record_raw.h"
#include "font_array.h"
#include "sensor.h"
#include "project_stream_config.h"

#include "vicap/fh_vicap_mpi.h"

/*
 * Project 主控程序（用户空间）
 *
 * 本文件只负责“编排”，不重新实现编码器、PES 或 GPIO 驱动：
 *   1. 初始化 Sensor -> VICAP -> ISP -> VPU -> VENC 媒体链路；
 *   2. 建立主码流 ch0 与子码流 ch1，并把编码结果交给 DMC 分发；
 *   3. 配置队名、系统时间和实时码率 OSD；
 *   4. 轮询 GPIO24 按键，触发 60 秒录像和三秒流水灯。
 *
 * 线程关系：主线程负责初始化、按键轮询和退出清理；取流线程负责从 VENC
 * 取出码流并同步调用 DMC 订阅者；灯效线程只负责低实时性的 LED 动画。
 * 编码器返回的 frame_data 在 FH_VENC_ReleaseStream() 后失效，因此 PES 打包
 * 与文件写入必须在 ReleaseStream() 之前完成。
 */

#define CHECK_RET(state, error_code)																	\
	if (state)																						\
	{																								\
		printf("[%s]:%d line [%s] return 0x%x ERROR\n",__FILE__,__LINE__ , __func__, error_code);	\
		return error_code;																			\
	}

#define ALIGN_UP(addr, edge)   ((addr + edge - 1) & ~(edge - 1)) /* 数据结构对齐定义 */
#define ALIGN_BACK(addr, edge) ((edge) * (((addr) / (edge))))
#define ISP_PROC "/proc/driver/isp"
#define VPU_PROC "/proc/driver/vpu"
#define BGM_PROC "/proc/driver/bgm"
#define ENC_PROC "/proc/driver/enc"
#define JPEG_PROC "/proc/driver/jpeg"
#define TRACE_PROC "/proc/driver/trace"

/*
 * ======================== 答辩重点：按键与流水灯接口 ========================
 * 用户程序不能直接随意操作物理寄存器，而是通过 open + ioctl 访问字符设备。
 * /dev/stream_led 由 led_driver/stream_led.c 创建，属于内核空间；本文件运行
 * 在用户空间。两侧必须使用完全相同的 ioctl 命令号，这就是用户态与驱动的
 * 接口约定（ABI）。
 *
 * SET_MASK 的低 4 位与 LED 一一对应：
 * bit0=GPIO43，bit1=GPIO44，bit2=GPIO52，bit3=GPIO53。
 * 例如 0x04 只点亮 GPIO52，0x00 关闭全部 LED。
 */
#define PROJECT_STREAM_LED_DEVICE "/dev/stream_led"
#define PROJECT_LEGACY_LED_DEVICE "/dev/helloworld"
#define PROJECT_LED_ON       _IO('L', 1)
#define PROJECT_LED_OFF      _IO('L', 2)
#define PROJECT_LED_SET_MASK _IO('L', 3)
#define PROJECT_LED_GET_KEY  _IOR('L', 4, int)
/* Driver mask bits 0..3 map to GPIO43, GPIO44, GPIO52 and GPIO53. */
#define PROJECT_LED_STREAM_MASK 0x01UL
/* Four complete rounds: 16 x 187.5 ms = exactly 3 seconds. */
#define PROJECT_LED_CHASE_COUNT 16
#define PROJECT_LED_CHASE_STEP_US 187500
#define PROJECT_KEY_DEBOUNCE_SAMPLES 3

#define WR_PROC_DEV(device,cmd)  do{ \
    int _tmp_fd; \
    _tmp_fd = open(device, O_RDWR, 0); \
    if(_tmp_fd >= 0) { \
        write(_tmp_fd, cmd, sizeof(cmd)); \
        close(_tmp_fd); \
    } \
}while(0)


/* 收到 Ctrl+C 后只修改标志，由主循环统一释放资源，避免在信号函数中做复杂操作。 */
static int g_sig_stop = 0;
static int g_stream_led_fd = -1;
/* 旧驱动只能控制 GPIO43；这两个能力标志用于决定能否使用四灯和 GPIO24。 */
static int g_stream_led_has_mask = 0;
static int g_stream_led_has_key = 0;
/* volatile 表示变量会被灯效线程异步修改，主线程每次都应从内存重新读取。 */
static volatile int g_record_led_running = 0;
static volatile int g_record_led_stop = 0;
static void sample_vlcview_handle_sig(int signo)
{
    (void)signo;
    g_sig_stop = 1;
}

/* 板端时间、NFS 时间都无效时，使用编译时间兜底，避免 OSD 显示 1970 年。 */
static int project_set_time_from_build(void)
{
    static const char *month_names[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    char month_name[4];
    int day, year, hour, minute, second;
    int month;
    struct tm build_time;
    struct timeval tv;
    time_t epoch;

    if (sscanf(__DATE__, "%3s %d %d", month_name, &day, &year) != 3 ||
        sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3)
        return -1;

    for (month = 0; month < 12; month++)
    {
        if (strcmp(month_name, month_names[month]) == 0)
            break;
    }
    if (month == 12)
        return -1;

    memset(&build_time, 0, sizeof(build_time));
    build_time.tm_year = year - 1900;
    build_time.tm_mon = month;
    build_time.tm_mday = day;
    build_time.tm_hour = hour;
    build_time.tm_min = minute;
    build_time.tm_sec = second;
    build_time.tm_isdst = -1;
    epoch = mktime(&build_time);
    if (epoch == (time_t)-1)
        return -1;

    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    if (settimeofday(&tv, NULL) != 0)
    {
        perror("[time] set build time");
        return -1;
    }

    printf("[time] invalid board clock replaced with build time: %s %s\n",
           __DATE__, __TIME__);
    return 0;
}

static void project_log_system_time(const char *source)
{
    char text[48];
    time_t now;
    struct tm *local_now;

    now = time(NULL);
    local_now = localtime(&now);
    if (local_now != NULL &&
        strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S %Z", local_now) > 0)
    {
        printf("[time] source=%s, system time=%s\n", source, text);
    }
}

/*
 * 自动时间来源按优先级选择：NFS 文件 mtime -> 已有效的板端时钟 -> 编译时间。
 * 程序在 /mnt/Project 运行时，新建文件的 mtime 由 Ubuntu NFS 服务器写入，
 * 因此无需命令行传时间，也无需开发板具备 NTP 客户端。settimeofday() 更新
 * Linux 系统时钟后，SDK 时间标签会直接读取该时钟并每秒刷新。
 */
static int project_sync_system_time(void)
{
    char cwd[256];
    char probe_path[320];
    struct stat probe_stat;
    struct timeval tv;
    struct tm *current;
    time_t now;
    int fd;

    /* Force OSD localtime to China Standard Time on minimal board images. */
    setenv("TZ", "CST-8", 1);
    tzset();

    if (getcwd(cwd, sizeof(cwd)) != NULL)
    {
        snprintf(probe_path, sizeof(probe_path),
                 "%s/.fh8862_time_probe_%ld", cwd, (long)getpid());
        fd = open(probe_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0)
        {
            if (write(fd, "T", 1) == 1)
                fsync(fd);
            close(fd);

            if (stat(probe_path, &probe_stat) == 0)
            {
                current = localtime(&probe_stat.st_mtime);
                if (current != NULL && current->tm_year + 1900 >= 2020)
                {
                    tv.tv_sec = probe_stat.st_mtime;
                    tv.tv_usec = 0;
                    if (settimeofday(&tv, NULL) == 0)
                    {
                        unlink(probe_path);
                        printf("[time] board clock synchronized automatically from the NFS server\n");
                        project_log_system_time("NFS");
                        return 0;
                    }
                    perror("[time] settimeofday from NFS");
                }
            }
            unlink(probe_path);
        }
    }

    /* A board with a working RTC or an earlier NTP sync needs no adjustment. */
    now = time(NULL);
    current = localtime(&now);
    if (current != NULL && current->tm_year + 1900 >= 2020)
    {
        printf("[time] using the existing board system clock\n");
        project_log_system_time("board");
        return 0;
    }

    printf("[time] NFS time unavailable and board clock is invalid; using build time\n");
    if (project_set_time_from_build() != 0)
        return -1;
    project_log_system_time("build-fallback");
    return 0;
}

/*
 * 打开 LED/按键设备。先探测新驱动的 SET_MASK、GET_KEY 能力，再尝试旧驱动。
 * 这里的“能力探测”比只判断文件存在更可靠：如果开发板复制了旧版 ko，设备
 * 节点可能存在，但 ioctl 会返回错误，程序便能明确提示重新加载驱动。
 */
static void project_stream_led_prepare(void)
{
    int key_pressed;

    if (g_stream_led_fd >= 0)
        return;

    g_stream_led_fd = open(PROJECT_STREAM_LED_DEVICE, O_RDWR);
    if (g_stream_led_fd >= 0)
    {
        /*
         * Probe SET_MASK once. This catches an obsolete or wrongly copied
         * stream_led.ko before recording starts, instead of silently showing
         * only GPIO43 and making the chase appear broken.
         */
        if (ioctl(g_stream_led_fd, PROJECT_LED_SET_MASK, 0UL) == 0)
        {
            g_stream_led_has_mask = 1;
            if (ioctl(g_stream_led_fd, PROJECT_LED_GET_KEY,
                      &key_pressed) == 0)
            {
                g_stream_led_has_key = 1;
                printf("[led] /dev/stream_led ready: GPIO24 key and GPIO43/44/52/53 LEDs enabled\n");
            }
            else
            {
                printf("[led] WARNING: reload the newest stream_led.ko; GPIO24 GET_KEY is missing\n");
            }
            return;
        }

        printf("[led] /dev/stream_led has no SET_MASK support; reload the new stream_led.ko\n");
        close(g_stream_led_fd);
        g_stream_led_fd = -1;
    }

    g_stream_led_fd = open(PROJECT_LEGACY_LED_DEVICE, O_RDWR);
    if (g_stream_led_fd < 0)
    {
        printf("[led] no LED driver loaded; video continues without LED indication\n");
        return;
    }

    printf("[led] WARNING: using legacy /dev/helloworld; it only controls GPIO43\n");
    printf("[led] load the newest stream_led.ko for GPIO24 recording control\n");
}

/* 只预览时四灯全灭；录像只能由 GPIO24 的稳定按下事件启动。 */
static void project_stream_led_start(void)
{
    project_stream_led_prepare();
    if (g_stream_led_fd < 0)
        return;

    ioctl(g_stream_led_fd, PROJECT_LED_OFF);
    if (g_stream_led_has_key)
        printf("[led] preview active; LEDs are off, press GPIO24 to record\n");
}

/*
 * ======================== 答辩重点：三秒流水灯线程 ========================
 * 此函数在线程中运行，不能直接放进 H.264 录像回调。原因是这里每步会休眠
 * 187.5 ms；若阻塞编码回调，会导致取流不及时、编码缓冲积压，进而影响 VLC
 * 和录像完整性。把“实时性较弱的灯效”与“实时性较强的码流处理”分开，是
 * 嵌入式多线程设计中常见的职责分离。
 *
 * 每一步只置一个 bit，顺序为 GPIO43 -> 44 -> 52 -> 53；16 步正好四轮，
 * 16 * 187.5 ms = 3000 ms。流水结束且录像仍在进行时，GPIO43 常亮作为
 * “正在录像”指示；若录像已经结束，则 mask=0，所有灯保持熄灭。
 */
static void *project_recording_led_thread(void *argument)
{
    int step;
    unsigned long mask;

    (void)argument;
    printf("[led] recording started: GPIO43->GPIO44->GPIO52->GPIO53 for 3 seconds\n");
    for (step = 0;
         step < PROJECT_LED_CHASE_COUNT && !g_record_led_stop;
         step++)
    {
        /*
         * step%4 得到 0、1、2、3；左移后依次得到 0x01、0x02、0x04、0x08。
         * 驱动把这四个 bit 分别转换为四个 gpio_set_value() 输出。
         */
        mask = 1UL << (step % 4);
        if (ioctl(g_stream_led_fd, PROJECT_LED_SET_MASK, mask) != 0)
        {
            printf("[led] chase requires the new /dev/stream_led driver\n");
            break;
        }
        usleep(PROJECT_LED_CHASE_STEP_US);
    }

    if (g_stream_led_fd >= 0)
    {
        /* stop=0 表示仍在录像，保留 GPIO43；stop=1 表示录像已结束，四灯全灭。 */
        ioctl(g_stream_led_fd, PROJECT_LED_SET_MASK,
              g_record_led_stop ? 0UL : PROJECT_LED_STREAM_MASK);
    }
    g_record_led_running = 0;
    printf("[led] recording chase finished\n");
    return NULL;
}

/*
 * 录像模块成功写入第一段 H.264 数据后调用本函数。创建 detached（分离）
 * 线程后无需 pthread_join；线程结束时系统自动回收线程控制资源。
 */
void project_recording_led_notify(void)
{
    pthread_t thread;
    pthread_attr_t attr;
    int ret;

    if (g_stream_led_fd < 0 || g_record_led_running)
        return;

    if (!g_stream_led_has_mask)
    {
        printf("[led] four-LED chase skipped: /dev/stream_led is not loaded\n");
        return;
    }

    g_record_led_stop = 0;
    g_record_led_running = 1;
    /* 显式设置线程属性，避免使用开发板 libc 默认的较大线程栈。 */
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 16 * 1024);
    ret = pthread_create(&thread, &attr,
                         project_recording_led_thread, NULL);
    pthread_attr_destroy(&attr);
    if (ret != 0)
    {
        g_record_led_running = 0;
        printf("[led] cannot create recording chase thread: %d\n", ret);
    }
}

/*
 * 录像文件关闭后立即调用：先通知灯效线程退出，再等待其结束，最后执行
 * LED_OFF。这样即使 60 秒结束恰好发生在一次灯效 ioctl 附近，也不会出现
 * “主线程刚灭灯、灯效线程又把某盏灯点亮”的竞态现象。
 */
void project_recording_led_finished(void)
{
    /* 先发停止请求，再等待灯线程离开 ioctl/usleep 循环。 */
    g_record_led_stop = 1;
    while (g_record_led_running)
        usleep(20 * 1000);

    if (g_stream_led_fd >= 0)
        ioctl(g_stream_led_fd, PROJECT_LED_OFF);
    printf("[led] recording finished: all LEDs are off\n");
}

/*
 * ======================== 答辩重点：GPIO24 软件消抖 ========================
 * 机械按键闭合/断开时会在数毫秒内反复跳变。主循环每 20 ms 调用一次本函数，
 * 连续 3 次读到相同电平（约 60 ms）才更新 stable_state，从而过滤抖动。
 *
 * raw_state：驱动本次返回的原始状态；stable_state：已确认的稳定状态；
 * same_sample_count：连续相同采样次数。只有 stable_state 从 0 变成 1 的
 * “按下沿”才调用 dmc_record_start()，所以一直按住不会重复启动录像；松开并
 * 再次按下，才会产生下一次有效事件。
 */
static void project_record_button_poll(void)
{
    static int raw_state = -1;
    static int stable_state = 0;
    static int same_sample_count = 0;
    int pressed;

    if (!g_stream_led_has_key || g_stream_led_fd < 0)
        return;

    if (ioctl(g_stream_led_fd, PROJECT_LED_GET_KEY, &pressed) != 0)
        return;
    pressed = pressed ? 1 : 0;

    /* 原始电平发生变化：重新开始累计连续相同样本。 */
    if (pressed != raw_state)
    {
        raw_state = pressed;
        same_sample_count = 1;
        return;
    }

    /* 只有累计到阈值并且稳定状态确实变化，才产生一次按键状态事件。 */
    if (same_sample_count < PROJECT_KEY_DEBOUNCE_SAMPLES)
        same_sample_count++;
    if (same_sample_count < PROJECT_KEY_DEBOUNCE_SAMPLES ||
        stable_state == raw_state)
        return;

    stable_state = raw_state;
    /* 只处理“按下”，松开事件仅用于允许下一次按下再次触发。 */
    if (stable_state)
        dmc_record_start();
}

static void project_stream_led_stop(void)
{
    /* 退出路径必须先停灯效线程，再关设备，避免线程对已关闭 fd 发 ioctl。 */
    if (g_stream_led_fd >= 0)
    {
        g_record_led_stop = 1;
        while (g_record_led_running)
            usleep(20 * 1000);
        ioctl(g_stream_led_fd, PROJECT_LED_OFF);
        close(g_stream_led_fd);
        g_stream_led_fd = -1;
        g_stream_led_has_mask = 0;
        g_stream_led_has_key = 0;
        printf("[led] stream stopped: all LEDs are off\n");
    }
}


struct isp_sensor_if sensor_func;

#define ISP_W0	3864
#define ISP_H0	2192
#define ISP_W	3840
#define ISP_H	2160

/*
 * Homework image tuning values.
 *
 * SATURATION uses the ISP range [1, 255]. Smaller values reduce color
 * intensity. Use 85 to reduce the remaining warm/yellow color cast without
 * making the picture look monochrome.
 *
 * BRIGHTNESS uses the public range [0, 255]. isp_set_bright() converts it to
 * a signed offset by subtracting 128, so 135 means a +7 brightness offset.
 *
 * ORIENTATION mode: 0=normal, 1=mirror, 2=flip, 3=mirror plus flip.
 */
#define HOMEWORK_ISP_SATURATION 85  //饱和度
#define HOMEWORK_ISP_BRIGHTNESS 135  //亮度
#define HOMEWORK_ISP_ORIENTATION 3  //镜像+翻转

/*
 * The lesson-4 firmware crashes inside FHAdv_Isp_Init() when that optional
 * library is initialized before the complete VPU/encoder pipeline. Keep the
 * advanced tuning code available for later SDK work, but use the proven base
 * ISP controls by default so video, OSD and recording start reliably.
 */
#define PROJECT_ENABLE_ADVANCED_ISP_TUNING 0

/*
 * Automatic white-balance compensation is stored in U.6 format, where a
 * change of 4 is a small 1/16 gain step. Red -4 and blue +4 cool the image
 * slightly while AWB remains enabled and continues following scene changes.
 * Change both values in steps of 2; large offsets can produce a blue cast.
 */
#define HOMEWORK_AWB_RED_DELTA (-4)
#define HOMEWORK_AWB_BLUE_DELTA 4

/*
 * Noise reduction and sharpness controls.
 *
 * DPC_ENABLE: 0=disable defective-pixel correction, 1=enable it.
 * DPC_CTRL_MODE: 0=correct isolated points, 1=also correct clustered points.
 * DPC_POINT_MODE: 1=white points, 2=black points, 3=both. This stage runs
 * before normal NR and is especially useful for bright/dark snow-like dots.
 * If real tiny highlights disappear, try CTRL_MODE=0 before disabling DPC.
 *
 * YNR_STRONG_MODE: 0=weak spatial luma NR, 1=strong spatial luma NR.
 * Strong mode removes bright/dark snow but can soften fine texture.
 *
 * NR3D_COEFF: range [0, 31]. 0 relies mainly on the current frame; increasing
 * it uses more temporal iteration and removes more moving snow. High values
 * can leave trails behind moving objects, so start at the moderate value 4.
 *
 * SHARPNESS_MODE: 0=manual, 1=gain mapping. Gain mapping is preferred because
 * it can reduce sharpening automatically when sensor gain/noise rises.
 *
 * SHARPNESS_VALUE: range [0, 255], 128 means the ISP-profile default.
 * 144 is a moderate +16 increase; too high creates white/black halos and makes
 * residual sensor noise more visible.
 */
#define HOMEWORK_ISP_DPC_ENABLE 1
#define HOMEWORK_ISP_DPC_CTRL_MODE 1
#define HOMEWORK_ISP_DPC_POINT_MODE 3
#define HOMEWORK_ISP_YNR_STRONG_MODE 1
#define HOMEWORK_ISP_NR3D_COEFF 4
#define HOMEWORK_ISP_SHARPNESS_MODE 1
#define HOMEWORK_ISP_SHARPNESS_VALUE 144

/*
 * The OSD font uses half-width ASCII glyphs: a 64-pixel font is about
 * 32 pixels wide per English character. sizeof(text)-1 excludes the string
 * terminator, allowing each literal to be centered at compile time.
 */
#define HOMEWORK_OSD_FRAME_WIDTH 1280U
#define HOMEWORK_OSD_FONT_SIZE 48U
#define HOMEWORK_OSD_ASCII_WIDTH (HOMEWORK_OSD_FONT_SIZE / 2U)
#define PROJECT_OSD_INVERT_HIGH 220U
#define PROJECT_OSD_INVERT_LOW 200U
#define HOMEWORK_OSD_CENTER_X(text) \
	((HOMEWORK_OSD_FRAME_WIDTH - \
	  ((FH_UINT32)(sizeof(text) - 1U) * HOMEWORK_OSD_ASCII_WIDTH)) / 2U)
#define ISP_F	30	/* Frame rate in fps */


/*
 * ======================== 答辩重点：主码流与子码流 ========================
 * 通道 0 是主码流：1280x720、25 fps、1.75 Mbps，用于录像和高清预览。
 * 通道 1 是子码流：640x360、15 fps、512 Kbps，仅用于低带宽 VLC 预览。
 * 两路共用同一个 ISP 采集源，但各自拥有 VPU 输出、编码器配置和 UDP 端口，
 * 因而子码流不会修改主码流的分辨率、帧率或录像数据。
 */
#define HOMEWORK_STREAM_COUNT 2
#define HOMEWORK_MAIN_CHANNEL 0
#define HOMEWORK_SUB_CHANNEL 1
#define HOMEWORK_BITRATE_INTERVAL_US 1000000ULL
#define HOMEWORK_BITRATE_OSD_LINE_ID 4

/* 厂商 OSD 字库按 GB2312 查字形，因此源码虽为 UTF-8，字节数据必须显式写成 GB2312。 */
static const FH_CHAR PROJECT_TEAM_NAME_GB2312[] = {
    (FH_CHAR)0xcb, (FH_CHAR)0xc4, /* 四 */
    (FH_CHAR)0xb4, (FH_CHAR)0xf3, /* 大 */
    (FH_CHAR)0xc8, (FH_CHAR)0xfd, /* 三 */
    (FH_CHAR)0xb6, (FH_CHAR)0xd3, /* 队 */
    0
};

static int g_get_stream_stop = 0;
static int g_get_stream_running = 0;

/*
 * 码率统计器只由取流线程写，不需要互斥锁。它累计主码流实际产生的 NAL 字节，
 * 因此 OSD 展示的是“实测码率”，而不是直接显示配置的 CBR 目标值。
 */
struct homework_bitrate_meter
{
	unsigned int initialized;
	unsigned long long window_start_pts;
	unsigned long long window_bytes;
};

static struct homework_bitrate_meter g_main_bitrate_meter;
static FHT_OSD_TextLine_t g_bitrate_osd_line;
static FH_CHAR g_bitrate_osd_text[64];
static int g_bitrate_osd_ready;

#define GROUP_ID 0 


/*
 * 初始化 Sensor、VICAP 与 ISP，但暂不启动 ISP 工作线程。
 * ISP 输出必须先在 main() 中绑定到 VPU，再调用 isp_server_run()；否则首帧
 * 阶段可能出现 SDK not-ready。成功返回 0，任一 SDK 调用失败返回其错误码。
 */
int start_isp()
{
	int ret;
	int vimod = 0;//0:online 1:offline
	int vomod = 1;//1:to vpu 2:to ddr

	ISP_MEM_INIT stMemInit = {0};
	ISP_VI_ATTR_S vi_attr = {0};

	ISP_PARAM_CONFIG stIsp_para_cfg;
	unsigned int param_addr, param_size;
	char *isp_param_buff;
	Sensor_Init_t initConf = {0};
	
	FH_VICAP_DEV_ATTR_S stViDev = {0};
	FH_VICAP_VI_ATTR_S stViAttr = {0};

	FILE *param_file;

		
	/* Reset the sensor before bringing up the ISP pipeline. */
	isp_sensor_reset();

	/* Initialize ISP memory for the selected work mode and output format. */
	stMemInit.enOfflineWorkMode   = vimod;
	stMemInit.enIspOutMode		  = vomod;
	stMemInit.enIspOutFmt		  = 1; /* 422 8-bit */
	stMemInit.stPicConf.u32Width  = ISP_W;
	stMemInit.stPicConf.u32Height = ISP_H;
	ret = API_ISP_MemInit(0, &stMemInit);
	CHECK_RET(ret != 0, ret);

	/* Configure the VI attributes used by the sensor input path. */
	vi_attr.u16InputHeight = ISP_H;
	vi_attr.u16InputWidth  = ISP_W;
	vi_attr.u16PicHeight   = ISP_H;
	vi_attr.u16PicWidth    = ISP_W;
	vi_attr.u16FrameRate = 30;
	vi_attr.enBayerType	= BAYER_GBRG;
	ret = API_ISP_SetViAttr(0, &vi_attr);
	CHECK_RET(ret != 0, ret);

	/* Register the sensor callback handlers for control, AE and format updates. */
	sensor_func.init					= sensor_init_imx415;
	sensor_func.set_sns_fmt 			= sensor_set_fmt_imx415;
	sensor_func.set_sns_reg 			= sensor_write_reg;
	sensor_func.get_sns_reg 			= sensor_read_reg;
	sensor_func.set_exposure_ratio		= sensor_set_exposure_ratio_imx415;
	sensor_func.get_exposure_ratio		= sensor_get_exposure_ratio_imx415;
	sensor_func.get_sensor_attribute	= sensor_get_attribute_imx415;
	sensor_func.set_flipmirror			= sensor_set_mirror_flip_imx415;
	sensor_func.get_sns_ae_default		= GetAEDefault;
	sensor_func.get_sns_ae_info 		= GetAEInfo;
	sensor_func.set_sns_gain			= SetGain;
	sensor_func.set_sns_intt			= SetIntt;
	ret = API_ISP_SensorRegCb(0, 0, &sensor_func);
	CHECK_RET(ret != 0, ret);
	
	/* Initialize the sensor after the callback table is registered. */
	ret = API_ISP_SensorInit(0, &initConf);
	CHECK_RET(ret != 0, ret);
	
	/* Initialize the ISP core. */
	ret = API_ISP_Init(0);
	CHECK_RET(ret != 0, ret);
	
	/* Initialize the VICAP device used by the sensor pipeline. */
	stViDev.enWorkMode = vimod;
	stViDev.stSize.u16Width  = ISP_W;
	stViDev.stSize.u16Height = ISP_H;
	ret = FH_VICAP_InitViDev(0, &stViDev);
	CHECK_RET(ret != 0, ret);
	
	/* Configure the input crop and size for the VI path. */
	stViAttr.enWorkMode = vimod;
	stViAttr.stInSize.u16Width		= ISP_W0;
	stViAttr.stInSize.u16Height 		= ISP_H0;
	stViAttr.stCropSize.bCutEnable		= 1;
	stViAttr.stCropSize.stRect.u16Width 	= ISP_W;
	stViAttr.stCropSize.stRect.u16Height	= ISP_H;
	ret = FH_VICAP_SetViAttr(0, &stViAttr);
	CHECK_RET(ret != 0, ret);

	if(vimod == 1)
	{
		FH_BIND_INFO src,dst;
		src.obj_id = FH_OBJ_VICAP;
		src.dev_id = 0;
		src.chn_id = 0;
		dst.obj_id = FH_OBJ_ISP;
		dst.dev_id = 0;
		dst.chn_id = 0;
		FH_SYS_Bind(src,dst);
	}
	
	/* Retrieve the ISP parameter binary address and size. */
	ret = API_ISP_GetBinAddr(0, &stIsp_para_cfg);
	param_size = stIsp_para_cfg.u32BinSize;
	CHECK_RET(ret != 0, ret);
	
	/* Load the ISP parameter file into memory and apply it to the ISP core. */
	isp_param_buff = (char*)malloc(param_size);
	param_file = fopen(SENSOR_PARAM, "rb");
	if(NULL == param_file)
	{
		free(isp_param_buff);
		
		printf("open file failed!\n");
		return -1;
	}
	
	if(param_size != fread(isp_param_buff, 1, param_size, param_file))
	{
		free(isp_param_buff);
		fclose(param_file);
		
		printf("open file failed!\n");
		return -1;
	}
	ret = API_ISP_LoadIspParam(0, isp_param_buff);
	CHECK_RET(ret != 0, ret);
	free(isp_param_buff);
	fclose(param_file);

	/*
	 * The ISP worker is started later in main(), after ISP output is bound
	 * to VPU input. Starting it here can cause a one-off not-ready error.
	 */
	return 0;
}

/*
 * ISP 参数统一入口：key 选择 AE/AWB/色彩/亮度/降噪/镜像翻转。
 * 旋转 180 度的实际调用链为：
 * main -> isp_set_param(ISP_MF, 3) -> isp_set_mirrorflip(3)
 *      -> API_ISP_SetMirrorAndflip(0, mirror=1, flip=1)。
 * 该设置位于 ISP 层，VPU、编码、录像和 VLC 得到的都是已旋转画面。
 */
int isp_set_param(int key, int param)
{
	int ret;

	printf("isp set key %d, val %d\n", key, param);

	switch(key)
	{
		case ISP_AE: /* AE enable flag, range [0, 1] */
			ret = isp_set_ae(param);
			break;	
		case ISP_AWB: /* AWB enable flag, range [0, 1] */
			ret = isp_set_awb(param);
			break;
		case ISP_COLOR: /* Color saturation, range [1, 255] */
			ret = isp_set_saturation(param);
			break;
		case ISP_BRIGHT: /* Brightness, range [0, 255] */
			ret = isp_set_bright(param);
			break;
		case ISP_NR: /* Noise reduction, range [0, 1] */
			ret = isp_set_nr(param);
			break;
		case ISP_MF: /* Mirror/flip mode, range [0, 3] */
			ret = isp_set_mirrorflip(param);
			break;
		default:
			printf("Error: not support the key %d\n", key);
			break;
	}
	CHECK_RET(ret != 0, ret);

	return ret;
}

/*
 * Apply only the requested NR/sharpness overrides on top of the calibrated
 * IMX415 ISP profile. Reading each structure first preserves every field that
 * is not explicitly documented and changed below.
 */
static int homework_tune_noise_and_sharpness(void)
{
	FH_SINT32 ret;
	ISP_DPC_CFG dpc_cfg;
	ISP_YNR_CFG ynr_cfg;
	ISP_NR3D_CFG nr3d_cfg;

	/*
	 * DPC removes isolated or clustered white/black point noise first. Read the
	 * sensor profile before changing only the documented switches and modes;
	 * all sensor-specific threshold values therefore remain calibrated.
	 */
	ret = API_ISP_GetDpcCfg(GROUP_ID, &dpc_cfg);
	CHECK_RET(ret != 0, ret);
	dpc_cfg.bDpcEn = HOMEWORK_ISP_DPC_ENABLE;
	dpc_cfg.bCtrlMode = HOMEWORK_ISP_DPC_CTRL_MODE;
	dpc_cfg.u08Enable = HOMEWORK_ISP_DPC_POINT_MODE;
	ret = API_ISP_SetDpcCfg(GROUP_ID, &dpc_cfg);
	CHECK_RET(ret != 0, ret);

	/*
	 * YNR is spatial luminance noise reduction. Strong mode is useful for the
	 * isolated bright/dark "snow" seen in a single frame, but may blur texture.
	 */
	ret = API_ISP_GetYnrCfg(GROUP_ID, &ynr_cfg);
	CHECK_RET(ret != 0, ret);
	ynr_cfg.bYnrEn = 1;
	ynr_cfg.bCtrlMode = HOMEWORK_ISP_YNR_STRONG_MODE;
	ret = API_ISP_SetYnrCfg(GROUP_ID, &ynr_cfg);
	CHECK_RET(ret != 0, ret);

	/*
	 * NR3D compares neighboring frames. Manual coefficient 4 gives moderate
	 * temporal denoising. Raise it one step at a time if snow remains; lower it
	 * if moving objects develop visible trails or ghost images.
	 */
	ret = API_ISP_GetNr3dCfg(GROUP_ID, &nr3d_cfg);
	CHECK_RET(ret != 0, ret);
	nr3d_cfg.bNr3dEn = 1;
	nr3d_cfg.bNr3dMode = 0;
	nr3d_cfg.u08Nr3dCoeffSel = HOMEWORK_ISP_NR3D_COEFF;
	ret = API_ISP_SetNr3dCfg(GROUP_ID, &nr3d_cfg);
	CHECK_RET(ret != 0, ret);

	/*
	 * Apply sharpening after NR so edges are restored after noise is reduced.
	 * Value 128 is the loaded profile default; 144 is a moderate adjustment.
	 */
	ret = FHAdv_Isp_SetSharpeness(GROUP_ID,
	                              HOMEWORK_ISP_SHARPNESS_MODE,
	                              HOMEWORK_ISP_SHARPNESS_VALUE);
	CHECK_RET(ret != 0, ret);

	printf("ISP tuning: saturation=%d brightness=%d dpc=%d/%d/%d ynr_strong=%d "
	       "nr3d_coeff=%d sharpness=%d\n",
	       HOMEWORK_ISP_SATURATION,
	       HOMEWORK_ISP_BRIGHTNESS,
	       HOMEWORK_ISP_DPC_ENABLE,
	       HOMEWORK_ISP_DPC_CTRL_MODE,
	       HOMEWORK_ISP_DPC_POINT_MODE,
	       HOMEWORK_ISP_YNR_STRONG_MODE,
	       HOMEWORK_ISP_NR3D_COEFF,
	       HOMEWORK_ISP_SHARPNESS_VALUE);
	return 0;
}


/* Clamp an AWB U.6 compensation value to its documented 8-bit range. */
static FH_UINT8 homework_clamp_u8(int value)
{
	if (value < 0)
	{
		return 0;
	}
	if (value > 255)
	{
		return 255;
	}
	return (FH_UINT8)value;
}

/*
 * Keep automatic white balance enabled, but bias its calibrated compensation
 * very slightly toward blue. Reading the complete profile first preserves all
 * color-temperature boxes, thresholds, speed, and scene-detection settings.
 */
static int homework_tune_white_balance(void)
{
	FH_SINT32 ret;
	AWB_DEFAULT_CFG awb_cfg;
	FH_UINT8 old_red;
	FH_UINT8 old_blue;

	ret = API_ISP_GetAwbDefaultCfg(GROUP_ID, &awb_cfg);
	CHECK_RET(ret != 0, ret);

	old_red = awb_cfg.stAwbCompCfg.u08REnhance;
	old_blue = awb_cfg.stAwbCompCfg.u08BEnhance;
	awb_cfg.stAwbCompCfg.u08REnhance =
		homework_clamp_u8((int)old_red + HOMEWORK_AWB_RED_DELTA);
	awb_cfg.stAwbCompCfg.u08BEnhance =
		homework_clamp_u8((int)old_blue + HOMEWORK_AWB_BLUE_DELTA);

	ret = API_ISP_SetAwbDefaultCfg(GROUP_ID, &awb_cfg);
	CHECK_RET(ret != 0, ret);

	printf("ISP AWB compensation: red=%u->%u blue=%u->%u\n",
	       (unsigned int)old_red,
	       (unsigned int)awb_cfg.stAwbCompCfg.u08REnhance,
	       (unsigned int)old_blue,
	       (unsigned int)awb_cfg.stAwbCompCfg.u08BEnhance);
	return 0;
}

/*
 * 每经过一个 PTS 秒更新一次主码流码率：bps = 字节数 * 8 * 1000000 / PTS差。
 * PTS 单位为微秒，所以要乘 1000000；中间量使用 64 位，避免 32 位整数溢出。
 */
static void homework_update_bitrate_osd(unsigned long long frame_pts,
	                                    unsigned int frame_bytes)
{
	unsigned long long elapsed_us;
	unsigned long long bitrate_bps;
	FH_SINT32 ret;

	if (!g_bitrate_osd_ready)
	{
		return;
	}

	if (!g_main_bitrate_meter.initialized)
	{
		g_main_bitrate_meter.initialized = 1;
		g_main_bitrate_meter.window_start_pts = frame_pts;
		g_main_bitrate_meter.window_bytes = frame_bytes;
		return;
	}

	g_main_bitrate_meter.window_bytes += frame_bytes;
	elapsed_us = frame_pts >= g_main_bitrate_meter.window_start_pts
		             ? frame_pts - g_main_bitrate_meter.window_start_pts
		             : 0;
	if (elapsed_us < HOMEWORK_BITRATE_INTERVAL_US)
	{
		return;
	}

	bitrate_bps = (g_main_bitrate_meter.window_bytes * 8ULL * 1000000ULL) /
	              elapsed_us;
	snprintf(g_bitrate_osd_text, sizeof(g_bitrate_osd_text),
	         "bitrate: %llu bps", bitrate_bps);
	ret = FHAdv_Osd_SetTextLine(GROUP_ID, HOMEWORK_MAIN_CHANNEL, 0,
	                            &g_bitrate_osd_line);
	if (ret != FH_SUCCESS)
	{
		printf("[bitrate] OSD update failed with %d\n", ret);
	}

	g_main_bitrate_meter.window_start_pts = frame_pts;
	g_main_bitrate_meter.window_bytes = 0;
}

int sample_dmc_init(FH_CHAR *dst_ip, FH_UINT32 port ,FH_SINT32 max_channel_no)
{
    FH_SINT32 ret;

    /* dmc_init() 建立发布/订阅表；随后 PES 订阅编码数据并配置各路 UDP。 */
    ret = dmc_init();
    if (ret != 0)
    {
        return ret;
    }

    if (dst_ip != NULL && *dst_ip != 0)
    {
        ret = dmc_pes_subscribe(max_channel_no, dst_ip, port);
        if (ret != 0)
        {
            return ret;
        }
        printf("VLC main=udp://@:%u, sub=udp://@:%u\n",
               (unsigned int)port, (unsigned int)(port + 1));
    }

    return 0;
}

FH_VOID *sample_common_get_stream_proc(FH_VOID *arg)
{
    FH_SINT32 ret, i;
    FH_SINT32 end_flag;
    FH_SINT32 subtype;
    FH_VENC_STREAM stream;
    FH_SINT32 *stop = (FH_SINT32 *)arg;
	FH_UINT32 startup_retry = 0;
	FH_UINT32 frame_bytes;

    /*
     * 这是整个应用的数据“总入口”线程：阻塞等待编码器输出一帧，然后把该帧
     * 拆成 NAL 单元送入 DMC。DMC 再同步分发给 PES/VLC 和 RECORD 两个订阅者。
     * FH_VENC_ReleaseStream 必须在每帧处理完后调用，否则编码器内部缓冲会耗尽。
     */
    while (*stop == 0)
    {
        WR_PROC_DEV(TRACE_PROC, "timing_GetStream_START");

        /* 阻塞获取任意编码通道输出的 H.264/H.265 帧。 */
        /*
         * FH_STREAM_ALL 去掉 JPEG 后同时等待通道 0/1 的视频码流。返回结构中的
         * stream.chan 用于识别来源通道，所以一个取流线程即可服务两路编码器。
         */
        ret = FH_VENC_GetStream_Block(FH_STREAM_ALL & (~(FH_STREAM_JPEG)), &stream);
        WR_PROC_DEV(TRACE_PROC, "timing_EncBlkFinish_xxx");

        if (ret != 0)
        {
			/* The encoder may need up to one second to produce its first I-frame. */
			if (startup_retry < 25)
			{
				startup_retry++;
				usleep(40 * 1000);
				continue;
			}
            printf("Error(%d - %x): FH_VENC_GetStream_Block(FH_STREAM_ALL & (~(FH_STREAM_JPEG))) failed!\n", ret, ret);
			usleep(10 * 1000);
            continue;
        }
		startup_retry = 25;
		
		/* H.264 一帧可含多个 NAL，逐个发布，最后一个 NAL 携带 end_flag。 */
        if (stream.stmtype == FH_STREAM_H264)
        {
			frame_bytes = 0;
            subtype = stream.h264_stream.frame_type == FH_FRAME_I ? DMC_MEDIA_SUBTYPE_IFRAME : DMC_MEDIA_SUBTYPE_PFRAME;
            for (i = 0; i < stream.h264_stream.nalu_cnt; i++)
            {
				frame_bytes += stream.h264_stream.nalu[i].length;
            	end_flag = (i == (stream.h264_stream.nalu_cnt - 1)) ? 1 : 0;
                /*
                 * 不复制 NAL 内容，只把 SDK 缓冲区地址传给 DMC。所有订阅回调
                 * 必须在 ReleaseStream 前同步使用完该地址，之后指针不再有效。
                 */
                dmc_input(stream.chan,
			    		  DMC_MEDIA_TYPE_H264,
			    		  subtype,
			    		  stream.h264_stream.time_stamp,
		    		      stream.h264_stream.nalu[i].start,
		    		      stream.h264_stream.nalu[i].length,
				      end_flag);
            }

			/* The assignment defines realtime bitrate from main recorded bytes. */
			if (stream.chan == HOMEWORK_MAIN_CHANNEL)
			{
				homework_update_bitrate_osd(stream.h264_stream.time_stamp,
				                            frame_bytes);
			}
        }

		/* H.265 与 H.264 使用相同的逐 NAL 分发策略。 */
        else if (stream.stmtype == FH_STREAM_H265)
        {
            subtype = stream.h265_stream.frame_type == FH_FRAME_I ? DMC_MEDIA_SUBTYPE_IFRAME : DMC_MEDIA_SUBTYPE_PFRAME;
            for (i = 0; i < stream.h265_stream.nalu_cnt; i++)
			{
            	end_flag = (i == (stream.h265_stream.nalu_cnt - 1)) ? 1 : 0;
                dmc_input(stream.chan,
			    		  DMC_MEDIA_TYPE_H265,
			    		  subtype,
			    		  stream.h265_stream.time_stamp,
		    		      stream.h265_stream.nalu[i].start,
		    		      stream.h265_stream.nalu[i].length,
		    		      end_flag);
			}
        }

        /* MJPEG 没有此处的 NAL 数组，整张 JPEG 作为一个完整帧发布。 */
        else if (stream.stmtype == FH_STREAM_MJPEG)
        {
            dmc_input(stream.chan,
                      DMC_MEDIA_TYPE_MJPEG,
                      0,
                      0,
                      stream.mjpeg_stream.start,
                      stream.mjpeg_stream.length,
                      1);
        }

		/* 当前帧的所有同步订阅者返回后，才能归还编码器缓冲区。 */
        /* 成功或失败处理完当前帧后都归还 SDK 缓冲，否则长时间运行会无流可取。 */
        ret = FH_VENC_ReleaseStream(&stream);
        if(ret)
        {
            printf("Error(%d - %x): FH_VENC_ReleaseStream failed for chan(%d)!\n", ret, ret, stream.chan);
        }
        WR_PROC_DEV(TRACE_PROC, "timing_GetStream_END");
    }

    *stop = 0;
    return NULL;
}

/*
 * 创建独立 VPU 输出通道。子码流通道关闭 BGM/SAD 等分析功能，以降低资源占用；
 * width/height 决定 VPU 缩放后的输出尺寸，之后再绑定到同编号编码器通道。
 */
static int sample_create_vpu_channel(int chan, int width, int height,
	                                 int analytics_enable)
{
	FH_SINT32 ret;
	FH_VPU_CHN_INFO chn_info = {0};
	FH_VPU_CHN_CONFIG chn_attr = {0};

	chn_info.bgm_enable = analytics_enable;
	chn_info.cpy_enable = analytics_enable;
	chn_info.sad_enable = analytics_enable;
	chn_info.bgm_ds = analytics_enable ? 8 : 0;
	chn_info.chn_max_size.u32Width = width;
	chn_info.chn_max_size.u32Height = height;
	chn_info.out_mode = VPU_VOMODE_SCAN;
	chn_info.support_mode = 1 << chn_info.out_mode;
	chn_info.bufnum = 3;
	chn_info.max_stride = 0;

	ret = FH_VPSS_CreateChn(GROUP_ID, chan, &chn_info);
	CHECK_RET(ret != 0, ret);

	chn_attr.vpu_chn_size.u32Width = width;
	chn_attr.vpu_chn_size.u32Height = height;
	chn_attr.crop_area.crop_en = 0;
	chn_attr.offset = 0;
	chn_attr.depth = 1;
	chn_attr.stride = 0;
	ret = FH_VPSS_SetChnAttr(GROUP_ID, chan, &chn_attr);
	CHECK_RET(ret != 0, ret);

	ret = FH_VPSS_SetVOMode(GROUP_ID, chan, VPU_VOMODE_SCAN);
	CHECK_RET(ret != 0, ret);

	return FH_VPSS_OpenChn(GROUP_ID, chan);
}

static int sample_set_venc_cfg(int chan, int enc_w, int enc_h,
	                           int frame_rate, int bitrate, int gop_size)
{
	FH_VENC_CHN_CAP cfg_vencmem = {0};

    cfg_vencmem.support_type       = FH_NORMAL_H264|FH_NORMAL_H265;
    cfg_vencmem.max_size.u32Width  = enc_w;
    cfg_vencmem.max_size.u32Height = enc_h;
	
    int ret = FH_VENC_CreateChn(chan, &cfg_vencmem);
	if (ret != 0)
	{
	   return ret;
	}
	
	/*
	 * 两路均采用 H.264 Main Profile + CBR。frame_count/frame_time 表示帧率；
	 * bitrate 是目标平均码率；GOP 是相邻 I 帧间隔。主码流 GOP=50，即约 2 秒
	 * 一个 I 帧；子码流 GOP=30，在 15 fps 下同样约 2 秒一个 I 帧。
	 */
	FH_VENC_CHN_CONFIG cfg_param = {0};

	cfg_param.chn_attr.enc_type 					 = FH_NORMAL_H264;
	cfg_param.chn_attr.h264_attr.profile			 = H264_PROFILE_MAIN;
	cfg_param.chn_attr.h264_attr.i_frame_intterval	 = gop_size;
	cfg_param.chn_attr.h264_attr.size.u32Width		 = enc_w;
	cfg_param.chn_attr.h264_attr.size.u32Height 	 = enc_h;

	cfg_param.rc_attr.rc_type						  = FH_RC_H264_CBR;
	cfg_param.rc_attr.h264_cbr.bitrate				  = bitrate;
	cfg_param.rc_attr.h264_cbr.init_qp				  = 35;
	cfg_param.rc_attr.h264_cbr.FrameRate.frame_count   = frame_rate;
	cfg_param.rc_attr.h264_cbr.FrameRate.frame_time	  = 1;
	cfg_param.rc_attr.h264_cbr.maxrate_percent		  = 200;
	cfg_param.rc_attr.h264_cbr.IFrmMaxBits			  = 0;
	cfg_param.rc_attr.h264_cbr.IP_QPDelta			  = 3;
	cfg_param.rc_attr.h264_cbr.I_BitProp 			  = 5;
	cfg_param.rc_attr.h264_cbr.P_BitProp 			  = 1;
	cfg_param.rc_attr.h264_cbr.fluctuate_level		  = 0;

	return FH_VENC_SetChnAttr(chan, &cfg_param);
}

static int sampe_set_jpeg_cfg(int chan, int enc_w, int enc_h, int quality)
{
	sleep(1);
	
	static int jpeg_cnt = 0;
	static int jpeg_init = 0;
	int ret = 0;

	if(jpeg_init == 0)
	{
		FH_VENC_CHN_CAP cfg_vencmem;

		cfg_vencmem.support_type       = FH_JPEG;
		cfg_vencmem.max_size.u32Width  = ISP_W;
		cfg_vencmem.max_size.u32Height = ISP_H;

		ret = FH_VENC_CreateChn(chan, &cfg_vencmem);
		if (ret != 0)
		{
			return ret;
		}
		
		jpeg_init = 1;
	}
	FH_VENC_CHN_CONFIG cfg_param = {0};

	cfg_param.chn_attr.enc_type = FH_JPEG;
	cfg_param.chn_attr.jpeg_attr.encode_speed = 4;
	cfg_param.chn_attr.jpeg_attr.qp = quality;

	ret = FH_VENC_SetChnAttr(chan, &cfg_param);
	CHECK_RET(ret != 0, ret);

	FH_BIND_INFO src,dst;
	src.obj_id = FH_OBJ_VPU_VO;
    src.dev_id = 0;
    src.chn_id = 0;

    dst.obj_id = FH_OBJ_JPEG;
    dst.dev_id = 0;
    dst.chn_id = chan;
	
    ret = FH_SYS_Bind(src, dst);
	CHECK_RET(ret != 0, ret);

	FH_VENC_STREAM jpeg_stream;

	while(1)
	{
		ret = FH_VENC_GetStream_Timeout(FH_STREAM_JPEG, &jpeg_stream,1000);
		if(ret == 0)
		{
			break;
		}
	}
	
	if (jpeg_stream.stmtype == FH_STREAM_JPEG)
	{
		char jpeg_path[50]  = {0};
		snprintf(jpeg_path, sizeof(jpeg_path), "/home/jpeg_%d.jpg",jpeg_cnt);
		FILE *jpeg_file = fopen(jpeg_path,"w+");
		if(jpeg_file)
		{
			fwrite(jpeg_stream.jpeg_stream.start, sizeof(char), jpeg_stream.jpeg_stream.length, jpeg_file);
			fclose(jpeg_file);
			printf("get jpeg file %d\n",jpeg_stream.jpeg_stream.length);
		}
		jpeg_cnt++;
	}
	ret = FH_VENC_ReleaseStream(&jpeg_stream);
	CHECK_RET(ret != 0, ret);

	ret = FH_SYS_UnBindbyDst(dst);
	CHECK_RET(ret != 0, ret);

	return 0;
}

/*
 * 厂商 OSD 示例保留作 API 对照，不在 main() 中调用。
 * 项目实际 OSD 入口是下方 project_set_osd()，答辩与修改均应以它为准。
 */
static int sample_set_osd_vendor_reference(void)
{
	int ret;
    int graph_ctrl = 0;
	
    graph_ctrl |= FHT_OSD_GRAPH_CTRL_TOSD_AFTER_VP;

    /* 初始化 OSD 引擎，并指定 OSD 合成在视频处理之后。 */
    ret = FHAdv_Osd_Init(0,FHT_OSD_DEBUG_LEVEL_ERROR, graph_ctrl, 0, 0);
    if (ret != FH_SUCCESS)
    {
        printf("FHAdv_Osd_Init failed with %x\n", ret);
        return ret;
    }

	/* 加载 ASCII 字库，供英文、数字和时间标签使用。 */
    FHT_OSD_FontLib_t font_lib;

	font_lib.pLibData = asc16;
	font_lib.libSize  = sizeof(asc16);
	ret = FHAdv_Osd_LoadFontLib(FHEN_FONT_TYPE_ASC, &font_lib);
	if (ret != 0)
	{
	    printf("Error: Load ASC font lib failed, ret=%d\n", ret);
	    return ret;
	}

    /* 加载 GB2312 字库，供中文文字使用。 */
    font_lib.pLibData = gb2312;
	font_lib.libSize  = sizeof(gb2312);
	ret = FHAdv_Osd_LoadFontLib(FHEN_FONT_TYPE_CHINESE, &font_lib);
	if (ret != 0)
	{
	    printf("Error: Load CHINESE font lib failed, ret=%d\n", ret);
	    return ret;
	}

	FHT_OSD_CONFIG_t osd_cfg;
    FHT_OSD_Layer_Config_t  pOsdLayerInfo[4];
    FHT_OSD_TextLine_t text_line_cfg[4];
    FH_CHAR text_data[4][128]; /*it should be enough*/
    FH_SINT32 user_defined_time = 0;

	memset(&osd_cfg, 0, sizeof(osd_cfg));
    memset(&pOsdLayerInfo[0], 0, 4 * sizeof(FHT_OSD_Layer_Config_t));
    memset(&text_line_cfg[0], 0, 4 * sizeof(FHT_OSD_TextLine_t));
    memset(&text_data, 0, sizeof(text_data));

	/* OSD 不旋转；画面方向已经在 ISP 层统一处理。 */
    osd_cfg.osdRotate        = 0;
    osd_cfg.pOsdLayerInfo = &pOsdLayerInfo[0];
    /* 每个 layer 可以采用不同颜色与反色策略。 */
    /* Layer 0 is white/black information; layer 1 is the red team name. */
    osd_cfg.nOsdLayerNum     = 2;

    pOsdLayerInfo[0].layerStartX = 0;
    pOsdLayerInfo[0].layerStartY = 0;
    /* pOsdLayerInfo[0].layerMaxWidth = 640; */
    /* pOsdLayerInfo[0].layerMaxHeight = 480; */
    /* 字号由项目配置统一控制。 */
    pOsdLayerInfo[0].osdSize     = HOMEWORK_OSD_FONT_SIZE;

    /* 普通颜色为不透明白色。 */
    pOsdLayerInfo[0].normalColor.fAlpha = 255;
    pOsdLayerInfo[0].normalColor.fRed   = 255;
    pOsdLayerInfo[0].normalColor.fGreen = 255;
    pOsdLayerInfo[0].normalColor.fBlue  = 255;

    /* 亮背景下的反色为不透明黑色。 */
    pOsdLayerInfo[0].invertColor.fAlpha = 255;
    pOsdLayerInfo[0].invertColor.fRed   = 0;
    pOsdLayerInfo[0].invertColor.fGreen = 0;
    pOsdLayerInfo[0].invertColor.fBlue  = 0;

    /* 边缘颜色保留为黑色；edgePixel=0 时不绘制描边。 */
    pOsdLayerInfo[0].edgeColor.fAlpha = 255;
    pOsdLayerInfo[0].edgeColor.fRed   = 0;
    pOsdLayerInfo[0].edgeColor.fGreen = 0;
    pOsdLayerInfo[0].edgeColor.fBlue  = 0;

    /* 背景 alpha=0，文字后方不绘制实色矩形。 */
    pOsdLayerInfo[0].bkgColor.fAlpha = 0;

    /* 自动反色取代固定描边。 */
    /* Automatic inversion replaces the original fixed outline. */
    pOsdLayerInfo[0].edgePixel        = 0;

    /* 按字符判断背景亮度，同一行文字可同时出现黑字与白字。 */
    pOsdLayerInfo[0].osdInvertEnable  = FH_OSD_INVERT_BY_CHAR;
    pOsdLayerInfo[0].osdInvertThreshold.high_level = 180;
    pOsdLayerInfo[0].osdInvertThreshold.low_level  = 160;
    pOsdLayerInfo[0].layerFlag = FH_OSD_LAYER_USE_TWO_BUF;
    pOsdLayerInfo[0].layerId = 0;

    /*
     * Team-name layer: normal text is red. On a bright background the SDK
     * switches individual glyphs to black using the luminance thresholds.
     */
    pOsdLayerInfo[1] = pOsdLayerInfo[0];
    pOsdLayerInfo[1].normalColor.fAlpha = 255;
    pOsdLayerInfo[1].normalColor.fRed = 255;
    pOsdLayerInfo[1].normalColor.fGreen = 0;
    pOsdLayerInfo[1].normalColor.fBlue = 0;
    pOsdLayerInfo[1].invertColor.fAlpha = 255;
    pOsdLayerInfo[1].invertColor.fRed = 0;
    pOsdLayerInfo[1].invertColor.fGreen = 0;
    pOsdLayerInfo[1].invertColor.fBlue = 0;
    pOsdLayerInfo[1].layerId = 1;
	
	ret = FHAdv_Osd_Ex_SetText(0, 0, &osd_cfg);
    if (ret != FH_SUCCESS)
    {
		printf("FHAdv_Osd_Ex_SetText failed with %d\n", ret);
        return ret;
    }
	/* Four independent line IDs share one OSD layer. */
	text_line_cfg[0].textInfo = text_data[0];
	text_line_cfg[1].textInfo = text_data[1];
	text_line_cfg[2].textInfo = text_data[2];
	text_line_cfg[3].textInfo = text_data[3];
	FH_CHAR user_tag_data[] = {
        0xe4, 0x0d+0, /* FHT_OSD_USER1 用户自定义标签。 */
        0x0a,         /* 换行控制字节。 */
        0xe4, 0x01, /* FHT_OSD_YEAR4 四位年份。 */
        '-',
        0xe4, 0x03, /* FHT_OSD_MONTH2 两位月份。 */
        '-',
        0xe4, 0x04, /* FHT_OSD_DAY 两位日期。 */
        0x20,       /* 空格。 */
        0xe4, 0x07, /* FHT_OSD_HOUR24 二十四小时制小时。 */
        ':',
        0xe4, 0x09, /* FHT_OSD_MINUTE 两位分钟。 */
        ':',
        0xe4, 0x0a, /* FHT_OSD_SECOND 两位秒数。 */
        0,          /*null terminated string*/
    };
#if 1
 	sprintf(text_line_cfg[0].textInfo, "Camera Channel - %d", 0);
	text_line_cfg[0].textEnable    = 1;                          /* 启用普通文本。 */
    text_line_cfg[0].timeOsdEnable = 0;                          /* 不追加 SDK 默认时间。 */
    text_line_cfg[0].textLineWidth = (64/2) * 36;                /* 预留 36 个 ASCII 字符宽度。 */
    text_line_cfg[0].linePositionX = 320;                         /* 行起点 X 坐标。 */
    text_line_cfg[0].linePositionY = 240;                         /* 行起点 Y 坐标。 */

    text_line_cfg[0].lineId = 0;
    text_line_cfg[0].enable = 1;

	ret = FHAdv_Osd_SetTextLine(0, 0, pOsdLayerInfo[0].layerId, &text_line_cfg[0]);
	if (ret != FH_SUCCESS)
	{
		printf("FHAdv_Osd_Ex_SetText failed with %d\n", ret);
		return ret;
	}
#endif	
#if 1
	strcat(text_line_cfg[1].textInfo, user_tag_data);
	text_line_cfg[1].textEnable    = 1;                          /* 启用标签组合文本。 */
    text_line_cfg[1].timeOsdEnable = 0;                          /* 标签自身已经包含时间字段。 */
    text_line_cfg[1].textLineWidth = (64/2) * 36;                /* 预留 36 个 ASCII 字符宽度。 */
    text_line_cfg[1].linePositionX = 640;                         /* 行起点 X 坐标。 */
    text_line_cfg[1].linePositionY = 480;                         /* 行起点 Y 坐标。 */

    text_line_cfg[1].lineId = 1;
    text_line_cfg[1].enable = 1;

	ret = FHAdv_Osd_SetTextLine(0, 0, pOsdLayerInfo[0].layerId, &text_line_cfg[1]);
	if (ret != FH_SUCCESS)
	{
		printf("FHAdv_Osd_Ex_SetText failed with %d\n", ret);
		return ret;
	}
#endif

	/*
	 * Homework OSD line 3. At 64-pixel font size an ASCII glyph is about
	 * 32 pixels wide, so a 36-character line width fits inside 1280 pixels.
	 * Y=320 sits between the original Y=240 and Y=480 blocks without overlap.
	 */
	snprintf(text_line_cfg[2].textInfo, sizeof(text_data[2]),
	         "Legacy OSD disabled");
	text_line_cfg[2].textEnable = 1;
	text_line_cfg[2].timeOsdEnable = 0;
	text_line_cfg[2].textLineWidth = (64 / 2) * 36;
	/* 26 ASCII characters: (1280 - 26 * 32) / 2 = 224. */
	text_line_cfg[2].linePositionX =
		HOMEWORK_OSD_CENTER_X("Legacy OSD disabled");
	text_line_cfg[2].linePositionY = 320;
	text_line_cfg[2].lineId = 2;
	text_line_cfg[2].enable = 1;

	ret = FHAdv_Osd_SetTextLine(0, 0, pOsdLayerInfo[0].layerId,
	                            &text_line_cfg[2]);
	if (ret != FH_SUCCESS)
	{
		printf("FHAdv_Osd_SetTextLine line 2 failed with %d\n", ret);
		return ret;
	}

	/* Homework OSD line 4, separated from line 3 by 80 pixels. */
	snprintf(text_line_cfg[3].textInfo, sizeof(text_data[3]), "Hikvision");
	text_line_cfg[3].textEnable = 1;
	text_line_cfg[3].timeOsdEnable = 0;
	text_line_cfg[3].textLineWidth = (64 / 2) * 36;
	/* 9 ASCII characters: (1280 - 9 * 32) / 2 = 496. */
	text_line_cfg[3].linePositionX = HOMEWORK_OSD_CENTER_X("Hikvision");
	text_line_cfg[3].linePositionY = 400;
	text_line_cfg[3].lineId = 3;
	text_line_cfg[3].enable = 1;

	ret = FHAdv_Osd_SetTextLine(0, 0, pOsdLayerInfo[0].layerId,
	                            &text_line_cfg[3]);
	if (ret != FH_SUCCESS)
	{
		printf("FHAdv_Osd_SetTextLine line 3 failed with %d\n", ret);
		return ret;
	}

	/*
	 * Fifth OSD line required by the final assignment. Keep this structure and
	 * its text buffer global because the stream thread rewrites the text once
	 * per second with the measured channel-0 bitrate.
	 */
	memset(&g_bitrate_osd_line, 0, sizeof(g_bitrate_osd_line));
	memset(&g_main_bitrate_meter, 0, sizeof(g_main_bitrate_meter));
	snprintf(g_bitrate_osd_text, sizeof(g_bitrate_osd_text),
	         "bitrate: 0 bps");
	g_bitrate_osd_line.textInfo = g_bitrate_osd_text;
	g_bitrate_osd_line.textEnable = 1;
	g_bitrate_osd_line.timeOsdEnable = 0;
	g_bitrate_osd_line.textLineWidth = (64 / 2) * 24;
	g_bitrate_osd_line.linePositionX = 32;
	g_bitrate_osd_line.linePositionY = 32;
	g_bitrate_osd_line.lineId = HOMEWORK_BITRATE_OSD_LINE_ID;
	g_bitrate_osd_line.enable = 1;

	ret = FHAdv_Osd_SetTextLine(GROUP_ID, HOMEWORK_MAIN_CHANNEL,
	                            pOsdLayerInfo[0].layerId,
	                            &g_bitrate_osd_line);
	if (ret != FH_SUCCESS)
	{
		printf("FHAdv_Osd_SetTextLine bitrate failed with %d\n", ret);
		return ret;
	}
	g_bitrate_osd_ready = 1;
	return 0;
}

/*
 * Configure the final-project OSD without modifying the vendor sample above.
 * Keeping the project layout in one function makes each scoring item easy to
 * locate during the presentation and keeps the vendor API call order intact.
 */
static int project_set_osd(void)
{
    int ret;
    int graph_ctrl = FHT_OSD_GRAPH_CTRL_TOSD_AFTER_VP;
    FHT_OSD_FontLib_t font_lib;
    FHT_OSD_CONFIG_t osd_cfg;
    FHT_OSD_Layer_Config_t layer[2];
    FHT_OSD_TextLine_t line[2];
    FHT_OSD_Ex_TextLine_t text_config;
    FH_CHAR text[2][128];
    static const FH_CHAR clock_tags[] = {
        0xe4, 0x01, /* Four-digit year. */
        '-',
        0xe4, 0x03, /* Two-digit month. */
        '-',
        0xe4, 0x04, /* Two-digit day. */
        ' ',
        0xe4, 0x07, /* 24-hour clock. */
        ':',
        0xe4, 0x09, /* Minute. */
        ':',
        0xe4, 0x0a, /* Second. */
        0
    };

    ret = FHAdv_Osd_Init(0, FHT_OSD_DEBUG_LEVEL_ERROR,
                         graph_ctrl, 0, 0);
    if (ret != FH_SUCCESS)
    {
        printf("FHAdv_Osd_Init failed with %x\n", ret);
        return ret;
    }

    /* ASCII is used by the clock, bitrate and stream-profile lines. */
    font_lib.pLibData = asc16;
    font_lib.libSize = sizeof(asc16);
    ret = FHAdv_Osd_LoadFontLib(FHEN_FONT_TYPE_ASC, &font_lib);
    if (ret != FH_SUCCESS)
    {
        printf("Load ASC font library failed, ret=%d\n", ret);
        return ret;
    }

    /* GB2312 is used by the Chinese team-name line. */
    font_lib.pLibData = gb2312;
    font_lib.libSize = sizeof(gb2312);
    ret = FHAdv_Osd_LoadFontLib(FHEN_FONT_TYPE_CHINESE, &font_lib);
    if (ret != FH_SUCCESS)
    {
        printf("Load GB2312 font library failed, ret=%d\n", ret);
        return ret;
    }

    memset(&osd_cfg, 0, sizeof(osd_cfg));
    memset(layer, 0, sizeof(layer));
    memset(line, 0, sizeof(line));
    memset(&text_config, 0, sizeof(text_config));
    memset(text, 0, sizeof(text));

    osd_cfg.osdRotate = 0;
    osd_cfg.pOsdLayerInfo = layer;
    osd_cfg.nOsdLayerNum = 2;

    /*
     * Information layer: evaluate each glyph independently so one long date
     * can cross both white and colored regions. The higher thresholds prevent
     * ordinary light colors from being classified as a white background.
     */
    layer[0].layerStartX = 0;
    layer[0].layerStartY = 0;
    layer[0].osdSize = HOMEWORK_OSD_FONT_SIZE;
    layer[0].normalColor.fAlpha = 255;
    layer[0].normalColor.fRed = 255;
    layer[0].normalColor.fGreen = 255;
    layer[0].normalColor.fBlue = 255;
    layer[0].invertColor.fAlpha = 255;
    layer[0].invertColor.fRed = 0;
    layer[0].invertColor.fGreen = 0;
    layer[0].invertColor.fBlue = 0;
    layer[0].edgePixel = 0;
    layer[0].bkgColor.fAlpha = 0;
    layer[0].osdInvertEnable = FH_OSD_INVERT_BY_CHAR;
    layer[0].osdInvertThreshold.high_level = PROJECT_OSD_INVERT_HIGH;
    layer[0].osdInvertThreshold.low_level = PROJECT_OSD_INVERT_LOW;
    layer[0].layerFlag = FH_OSD_LAYER_USE_TWO_BUF;
    layer[0].layerId = 0;

    /*
     * Team layer: red is the required normal color. The black inverse color
     * keeps the Chinese name readable when it crosses a white background.
     */
    layer[1] = layer[0];
    layer[1].normalColor.fRed = 255;
    layer[1].normalColor.fGreen = 0;
    layer[1].normalColor.fBlue = 0;
    layer[1].invertColor.fRed = 0;
    layer[1].invertColor.fGreen = 0;
    layer[1].invertColor.fBlue = 0;
    /* The team name must stay solid red regardless of scene brightness. */
    layer[1].osdInvertEnable = FH_OSD_INVERT_DISABLE;
    layer[1].layerId = 1;

    ret = FHAdv_Osd_Ex_SetText(GROUP_ID, HOMEWORK_MAIN_CHANNEL, &osd_cfg);
    if (ret != FH_SUCCESS)
    {
        printf("FHAdv_Osd_Ex_SetText failed with %d\n", ret);
        return ret;
    }

    line[0].textInfo = text[0];
    line[1].textInfo = text[1];

    /* Explicit GB2312 bytes survive Windows/Ubuntu source-code conversion. */
    memcpy(line[0].textInfo, PROJECT_TEAM_NAME_GB2312,
           sizeof(PROJECT_TEAM_NAME_GB2312));
    line[0].textEnable = 1;
    line[0].timeOsdEnable = 0;
    line[0].textLineWidth = HOMEWORK_OSD_FONT_SIZE * 6;
    line[0].linePositionX = 32;
    line[0].linePositionY = 24;
    line[0].lineId = 0;
    line[0].enable = 1;
    /* Reconfig=1 removes text retained by an earlier process invocation. */
    text_config.LineNum = 1;
    text_config.Reconfig = 1;
    text_config.pLineCfg = &line[0];
    ret = FHAdv_Osd_Ex_SetTextLine(GROUP_ID, HOMEWORK_MAIN_CHANNEL,
                                   layer[1].layerId, &text_config);
    if (ret != FH_SUCCESS)
    {
        printf("Set team-name OSD failed with %d\n", ret);
        return ret;
    }

    /*
     * The embedded date/time tags refresh themselves once per second.
     * timeOsdEnable must stay zero here; setting it to one would append the
     * SDK's formatted clock after these tags and display the date twice.
     */
    memcpy(line[1].textInfo, clock_tags, sizeof(clock_tags));
    line[1].textEnable = 1;
    line[1].timeOsdEnable = 0;
    line[1].textLineWidth = HOMEWORK_OSD_ASCII_WIDTH * 24;
    line[1].linePositionX = 32;
    line[1].linePositionY = 96;
    line[1].lineId = 1;
    line[1].enable = 1;
    /* Clear the information layer too, guaranteeing exactly one clock line. */
    text_config.LineNum = 1;
    text_config.Reconfig = 1;
    text_config.pLineCfg = &line[1];
    ret = FHAdv_Osd_Ex_SetTextLine(GROUP_ID, HOMEWORK_MAIN_CHANNEL,
                                   layer[0].layerId, &text_config);
    if (ret != FH_SUCCESS)
    {
        printf("Set clock OSD failed with %d\n", ret);
        return ret;
    }

    /*
     * The stream worker updates this global buffer once per second using the
     * bytes actually produced by channel 0, not merely the configured CBR.
     */
    memset(&g_bitrate_osd_line, 0, sizeof(g_bitrate_osd_line));
    memset(&g_main_bitrate_meter, 0, sizeof(g_main_bitrate_meter));
    snprintf(g_bitrate_osd_text, sizeof(g_bitrate_osd_text),
             "bitrate: 0 bps");
    g_bitrate_osd_line.textInfo = g_bitrate_osd_text;
    g_bitrate_osd_line.textEnable = 1;
    g_bitrate_osd_line.timeOsdEnable = 0;
    g_bitrate_osd_line.textLineWidth = HOMEWORK_OSD_ASCII_WIDTH * 24;
    g_bitrate_osd_line.linePositionX = 32;
    g_bitrate_osd_line.linePositionY = 168;
    g_bitrate_osd_line.lineId = HOMEWORK_BITRATE_OSD_LINE_ID;
    g_bitrate_osd_line.enable = 1;
    ret = FHAdv_Osd_SetTextLine(GROUP_ID, HOMEWORK_MAIN_CHANNEL,
                                layer[0].layerId,
                                &g_bitrate_osd_line);
    if (ret != FH_SUCCESS)
    {
        printf("Set bitrate OSD failed with %d\n", ret);
        return ret;
    }

    g_bitrate_osd_ready = 1;
    return 0;
}

FH_VOID sample_common_media_driver_config(FH_VOID)
{
	VB_CONF_S stVbConf;
    FH_SINT32 ret;

    FH_VB_Exit();

    memset(&stVbConf, 0, sizeof(VB_CONF_S));
    stVbConf.u32MaxPoolCnt = 4;
    stVbConf.astCommPool[0].u32BlkSize = 3840 * 2160 * 3;
    stVbConf.astCommPool[0].u32BlkCnt = 4;
    stVbConf.astCommPool[1].u32BlkSize = 1920 * 1080 * 3;
    stVbConf.astCommPool[1].u32BlkCnt = 4;
    stVbConf.astCommPool[2].u32BlkSize = 1280 * 720 * 3;
    stVbConf.astCommPool[2].u32BlkCnt = 4;
    stVbConf.astCommPool[3].u32BlkSize = 768 * 448 * 3;
    stVbConf.astCommPool[3].u32BlkCnt = 4;

    ret = FH_VB_SetConf(&stVbConf);
    if (ret)
    {
        printf("[FH_VB_SetConf] failed with:%x\n", ret);
    }

    ret = FH_VB_Init();
    if (ret)
    {
        printf("[FH_VB_Init] failed with:%x\n", ret);
    }
	
	WR_PROC_DEV(ENC_PROC, "allchnstm_0_20000000_40");
    WR_PROC_DEV(ENC_PROC, "stm_20000000_40");
    WR_PROC_DEV(JPEG_PROC, "frmsize_1_3000000_3000000");
    WR_PROC_DEV(JPEG_PROC, "jpgstm_12000000_2");
    WR_PROC_DEV(JPEG_PROC, "mjpgstm_12000000_2");
	
}

int main(int argc, char *argv[])
{
    int  ret;
    char *dst_ip;
    unsigned int port;

    /* Flush every diagnostic immediately, even when stdout is redirected. */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* 信号处理函数只置退出标志，资源释放由主循环按正常顺序完成。 */
    signal(SIGINT,  sample_vlcview_handle_sig);
    signal(SIGQUIT, sample_vlcview_handle_sig);
    signal(SIGKILL, sample_vlcview_handle_sig);
    signal(SIGTERM, sample_vlcview_handle_sig);

    /* argv[1] 是 VLC 所在 PC 的 IP；argv[2] 是主码流基准端口，默认 1234。 */
    dst_ip = argc > 1 ? argv[1] : NULL;
    port   = argc > 2 ? strtol(argv[2], NULL, 0) : 1234;

    /* OSD 读取系统时间；从 NFS 运行时优先利用服务器文件时间自动校时。 */
    ret = project_sync_system_time();
    if (ret != 0)
    {
        printf("[time] WARNING: automatic clock synchronization failed\n");
    }

	printf("demo_main driver_config\n");

	/*
	 * 用户态媒体初始化顺序不能随意交换：
	 * VB/SYS -> Sensor/ISP -> VPU -> VENC -> Bind -> DMC/OSD -> StartRecvPic。
	 * 前一级为后一级提供缓冲区或数据源，顺序错误常表现为 SDK 返回 not-ready。
	 */
	sample_common_media_driver_config();

	ret = FH_SYS_Init(); 
	CHECK_RET(ret != 0, ret);

	printf("start_isp\n");
	
	ret = start_isp();
	CHECK_RET(ret != 0, ret);
	printf("start_isp success\n");

#if PROJECT_ENABLE_ADVANCED_ISP_TUNING
	/*
	 * Only enable this after validating the advanced ISP initialization order
	 * against the exact board firmware and library build in use.
	 */
	ret = FHAdv_Isp_SensorInit(GROUP_ID, &sensor_func);
	CHECK_RET(ret != 0, ret);
	ret = FHAdv_Isp_Init(GROUP_ID);
	CHECK_RET(ret != 0, ret);
#else
	printf("[isp] optional advanced tuning disabled for firmware stability\n");
#endif
	
	FH_VPU_SET_GRP_INFO grp_info = {0};
	grp_info.vi_max_size.u32Width = ISP_W;
	grp_info.vi_max_size.u32Height = ISP_H;
	grp_info.ycmean_en = 1;
	grp_info.ycmean_ds = 16;

	printf("[vpu] FH_VPSS_CreateGrp begin\n");
    ret = FH_VPSS_CreateGrp(GROUP_ID, &grp_info);
	CHECK_RET(ret != 0, ret);
	printf("[vpu] FH_VPSS_CreateGrp success\n");
	
	FH_VPU_SIZE vi_pic = {0};
	vi_pic.vi_size.u32Width  = ISP_W;
	vi_pic.vi_size.u32Height = ISP_H;
	vi_pic.crop_area.crop_en = 0;
	vi_pic.crop_area.vpu_crop_area.u32X = 0;
	vi_pic.crop_area.vpu_crop_area.u32Y = 0;
	vi_pic.crop_area.vpu_crop_area.u32Width = 0;
	vi_pic.crop_area.vpu_crop_area.u32Height = 0;

	printf("[vpu] FH_VPSS_SetViAttr begin\n");
	ret = FH_VPSS_SetViAttr(GROUP_ID,&vi_pic);
	CHECK_RET(ret != 0, ret);
	printf("[vpu] FH_VPSS_SetViAttr success\n");

	printf("[vpu] FH_VPSS_Enable begin\n");
	ret = FH_VPSS_Enable(GROUP_ID, VPU_MODE_ISP);
	CHECK_RET(ret != 0, ret)
	printf("[vpu] FH_VPSS_Enable success\n");

	FH_VPU_CHN_INFO chn_info = {0};
	chn_info.bgm_enable = 1;
	chn_info.cpy_enable = 1;
	chn_info.sad_enable = 1;
	chn_info.bgm_ds = 8;
	chn_info.chn_max_size.u32Width = ISP_W;
    chn_info.chn_max_size.u32Height = ISP_H;
	chn_info.out_mode = VPU_VOMODE_SCAN;
	chn_info.support_mode = 1<<chn_info.out_mode;
	chn_info.bufnum = 3;
	chn_info.max_stride = 0;
	printf("[vpu] create main channel begin\n");
	ret = FH_VPSS_CreateChn(GROUP_ID, 0, &chn_info);
	CHECK_RET(ret != 0, ret);
	printf("[vpu] create main channel success\n");

    FH_VPU_CHN_CONFIG chn_attr = {0};
	/* Scale the 4K IMX415 input to the exact homework recording size. */
    chn_attr.vpu_chn_size.u32Width  = 1280;
    chn_attr.vpu_chn_size.u32Height = 720;
	chn_attr.crop_area.crop_en = 0;
    chn_attr.crop_area.vpu_crop_area.u32X = 0;
    chn_attr.crop_area.vpu_crop_area.u32Y = 0;
    chn_attr.crop_area.vpu_crop_area.u32Width = 0;
    chn_attr.crop_area.vpu_crop_area.u32Height = 0;
    chn_attr.offset = 0;
    chn_attr.depth = 1;
    chn_attr.stride = 0;
    printf("[vpu] configure main channel begin\n");
    ret = FH_VPSS_SetChnAttr(GROUP_ID, 0, &chn_attr);  /* VPU 通道 0 输出缩放为 1280x720。 */
    CHECK_RET(ret != 0, ret);

    ret = FH_VPSS_SetVOMode(GROUP_ID, 0, VPU_VOMODE_SCAN);
    CHECK_RET(ret != 0, ret);

	ret = FH_VPSS_OpenChn(GROUP_ID, 0);
	CHECK_RET(ret != 0, ret);
	printf("[vpu] main channel ready\n");

	/* 创建主编码通道 0，录像模块只接受该通道。 */
	ret = sample_set_venc_cfg(HOMEWORK_MAIN_CHANNEL,
	                          HOMEWORK_MAIN_WIDTH, HOMEWORK_MAIN_HEIGHT,
	                          HOMEWORK_MAIN_FPS, HOMEWORK_MAIN_BITRATE, 50);
	CHECK_RET(ret != 0, ret);

	/* 子码流使用独立 VPU/VENC 通道，降低分辨率、帧率和码率，不改动通道 0。 */
	ret = sample_create_vpu_channel(HOMEWORK_SUB_CHANNEL,
	                                HOMEWORK_SUB_WIDTH,
	                                HOMEWORK_SUB_HEIGHT, 0);
	CHECK_RET(ret != 0, ret);
	ret = sample_set_venc_cfg(HOMEWORK_SUB_CHANNEL,
	                          HOMEWORK_SUB_WIDTH, HOMEWORK_SUB_HEIGHT,
	                          HOMEWORK_SUB_FPS, HOMEWORK_SUB_BITRATE, 30);
	CHECK_RET(ret != 0, ret);

	FH_BIND_INFO src = {0};
	FH_BIND_INFO dst = {0};
	src.obj_id = FH_OBJ_ISP;
    src.dev_id = 0;
    src.chn_id = 0;
    dst.obj_id = FH_OBJ_VPU_VI;
    dst.dev_id = 0;
    dst.chn_id = 0;
    ret = FH_SYS_Bind(src, dst);
	CHECK_RET(ret != 0, ret);

	/* Start API_ISP_Run only after ISP has a valid VPU destination. */
	ret = isp_server_run();
	CHECK_RET(ret != 0, ret);
	
    src.obj_id = FH_OBJ_VPU_VO;
    src.dev_id = GROUP_ID;
    src.chn_id = 0;

    dst.obj_id = FH_OBJ_ENC;
    dst.dev_id = 0;
    dst.chn_id = 0;
	
    ret = FH_SYS_Bind(src, dst);
	CHECK_RET(ret != 0, ret);

	/*
	 * 媒体链路绑定关系（答辩可画成方框图）：
	 * Sensor -> VICAP -> ISP -> VPU group
	 *                         |-> VPU ch0 -> VENC ch0 -> DMC -> PES:1234 + RECORD
	 *                         `-> VPU ch1 -> VENC ch1 -> DMC -> PES:1235
	 */
	src.obj_id = FH_OBJ_VPU_VO;
	src.dev_id = GROUP_ID;
	src.chn_id = HOMEWORK_SUB_CHANNEL;
	dst.obj_id = FH_OBJ_ENC;
	dst.dev_id = 0;
	dst.chn_id = HOMEWORK_SUB_CHANNEL;
	ret = FH_SYS_Bind(src, dst);
	CHECK_RET(ret != 0, ret);

	/*
	 * sample_dmc_init() 会先清空 DMC 订阅表并注册 PES，故 RECORD 必须随后注册。
	 * 两个订阅者会同时收到主码流；子码流被 RECORD 回调主动过滤，只用于预览。
	 */
	ret = sample_dmc_init(dst_ip, port, HOMEWORK_STREAM_COUNT);
	CHECK_RET(ret != 0, ret);
	ret = dmc_record_subscribe(HOMEWORK_STREAM_COUNT);
	CHECK_RET(ret != 0, ret);

#if 1
	ret = project_set_osd();
	CHECK_RET(ret != 0, ret);
#endif

    /* Open GPIO24 and LED control before entering preview mode. */
	project_stream_led_prepare();

	/* Start encoding only after bind, VLC, recorder, and OSD are ready. */
	/* 两路绑定和订阅都完成后再启动编码，防止启动阶段丢失首批码流。 */
	ret = FH_VENC_StartRecvPic(0);
	CHECK_RET(ret != 0, ret);
	ret = FH_VENC_StartRecvPic(HOMEWORK_SUB_CHANNEL);
	CHECK_RET(ret != 0, ret);

	usleep(100 * 1000);
	
	pthread_attr_t attr;
	pthread_t thread_stream;
	
	if(!g_get_stream_running)
	{
		g_get_stream_running = 1;
		g_get_stream_stop = 0;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		/* 取流线程同时处理两路码流和 OSD；64 KiB 栈用于局部结构和 SDK 调用。 */
		pthread_attr_setstacksize(&attr, 64 * 1024);
		ret = pthread_create(&thread_stream, &attr,
		                     sample_common_get_stream_proc, &g_get_stream_stop);
		pthread_attr_destroy(&attr);
		CHECK_RET(ret != 0, ret);
		/* 预览已就绪，但灯保持熄灭，等待 GPIO24 真正触发录像。 */
		project_stream_led_start();
	}

	isp_set_param(ISP_AE, 1);  /* 开启自动曝光。 */

	isp_set_param(ISP_AWB, 1); /* 开启自动白平衡。 */

#if PROJECT_ENABLE_ADVANCED_ISP_TUNING
	/* Apply a small cool bias while automatic white balance remains active. */
	ret = homework_tune_white_balance();
	CHECK_RET(ret != 0, ret);
#endif

	/* Keep saturation low enough to reduce the remaining warm color cast. */
	isp_set_param(ISP_COLOR, HOMEWORK_ISP_SATURATION);

	/* Apply only a small positive brightness offset: 135 - 128 = +7. */
	isp_set_param(ISP_BRIGHT, HOMEWORK_ISP_BRIGHTNESS);

	isp_set_param(ISP_NR, 1); /* 开启基础降噪。 */

#if PROJECT_ENABLE_ADVANCED_ISP_TUNING
	/* Refine basic NR only when the optional advanced ISP is initialized. */
	ret = homework_tune_noise_and_sharpness();
	CHECK_RET(ret != 0, ret);
#endif

	/* Camera installation requires both mirror and flip for upright output. */
	isp_set_param(ISP_MF, HOMEWORK_ISP_ORIENTATION);
#if 1
	FH_VPU_MASK stVpumaskinfo;
	memset(&stVpumaskinfo, 0, sizeof(stVpumaskinfo));

	/*
	 * Convert the original solid-color privacy mask to mosaic mode.
	 * The vendor field is spelled "masaic" in the FH8862 SDK.
	 * masaic_size: 0=16x16, 1=32x32, 2=64x64 mosaic blocks.
	 */
	stVpumaskinfo.masaic.masaic_enable = 1;
	stVpumaskinfo.masaic.masaic_size = 1;

	/* Mosaic area 0: upper-left, all values aligned to 16 pixels. */
	stVpumaskinfo.mask_enable[0] = 1;
	stVpumaskinfo.area_value[0].u32X = 64;
	stVpumaskinfo.area_value[0].u32Y = 64;
	stVpumaskinfo.area_value[0].u32Width = 320;
	stVpumaskinfo.area_value[0].u32Height = 192;

	/* Mosaic area 1: lower-right and completely separate from area 0. */
	stVpumaskinfo.mask_enable[1] = 1;
	stVpumaskinfo.area_value[1].u32X = 800;
	stVpumaskinfo.area_value[1].u32Y = 432;
	stVpumaskinfo.area_value[1].u32Width = 320;
	stVpumaskinfo.area_value[1].u32Height = 192;
	
	ret = FH_VPSS_SetMask(GROUP_ID, &stVpumaskinfo);
	CHECK_RET(ret != 0, ret);
#endif
#if 0
	sleep(2);
	ret = sampe_set_jpeg_cfg(1, 3840, 2160, 40);  /* 抓取 JPEG（当前编译关闭）。 */
	CHECK_RET(ret != 0, ret);

	ret = sampe_set_jpeg_cfg(1, 3840, 2160, 80);
	CHECK_RET(ret != 0, ret);
#endif
    while (!g_sig_stop)
    {
        /* 20 ms 轮询一次 GPIO24；录像是否启动不影响两路 VLC 持续预览。 */
        project_record_button_poll();
        usleep(20000);
    }

    /* Leave visible hardware and the recording file in a clean state. */
    /*
     * Ctrl+C 清理路径：通知取流线程停止、停止灯效并全灭、关闭未完成录像。
     * 这体现“谁申请谁释放”；否则反复运行可能残留亮灯或未刷新的录像文件。
     */
    g_get_stream_stop = 1;
    project_stream_led_stop();
    dmc_record_unsubscribe();

    return 0;
}

