#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "fh_venc_mpi.h"
#include "libdmc.h"
#include "libdmc_record_raw.h"

/*
 * 板端裸 H.264 录像订阅者（用户空间）
 *
 * main.c 只负责按键触发 dmc_record_start()；真正的数据来自取流线程调用的
 * dmc_input()。DMC 会同步调用 record_input()，因此本文件必须在回调返回前
 * 完成 fwrite，随后 main.c 才能安全执行 FH_VENC_ReleaseStream()。
 *
 * 状态转换：ready -> active/wait-key-frame -> recording -> finished。
 * RECORD 订阅始终存在，active 标志只控制“是否写文件”，不会暂停 PES/VLC。
 */
/*
 * ======================== 答辩重点：板端录像参数 ========================
 * 只保存主码流通道 0。编码器 PTS 单位为微秒，使用 PTS 差判断 60 秒，而不是
 * sleep(60)：sleep 只能测线程经过的墙上时间，会受启动等待和调度延迟影响；
 * PTS 直接代表视频时间轴，能保证录像时长偏差落在一帧左右。
 *
 * 1.75 Mbps 是根据开发板实测校准的 CBR 目标。理论值约 12.5 MiB/60s，但
 * I 帧、码率控制波动和编码头会增加文件大小，最终以板端 bytes 日志验收。
 */
#define HOMEWORK_RECORD_CHANNEL       0
#define HOMEWORK_RECORD_DURATION_US   (60ULL * 1000ULL * 1000ULL)
#define HOMEWORK_FLUSH_INTERVAL_FRAME 25
#define HOMEWORK_FILE_NAME            "project_device_1280x720_25fps_60s.h264"
#define HOMEWORK_CONFIGURED_BITRATE   1750000

struct record_file_info
{
    /* active=按键已触发；started=已收到可解码关键帧；finished=本轮已完成。 */
    unsigned int active;
    unsigned int started;
    unsigned int finished;
    unsigned int led_notified;
    unsigned int frames_since_flush;
    /* first_pts 是录像时间轴原点；bytes_written 用于最终文件大小统计。 */
    unsigned long long first_pts;
    unsigned long long bytes_written;
    FILE *fp;
    /* 持续缓存最近的 SPS/PPS，保证按键在任意 GOP 位置触发都能生成独立文件。 */
    unsigned char *sps;
    unsigned int sps_len;
    unsigned char *pps;
    unsigned int pps_len;
};

static struct record_file_info *g_record_files = NULL;
static unsigned int g_max_channel_count = 0;

/*
 * 从 Annex-B NAL 中读取 H.264 nal_unit_type。SDK 输出可能使用三字节或四字节
 * 起始码，因此只在数据开头的小范围内查找 00 00 01。返回 7/8/5 分别表示
 * SPS、PPS、IDR；无法识别时返回 -1，但仍可按普通 NAL 写入。
 */
static int h264_nal_type(const unsigned char *data, int len)
{
    int i;
    int search_len = len < 8 ? len : 8;

    for (i = 0; i + 3 < search_len; i++)
    {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            return data[i + 3] & 0x1f;
        if (i + 4 < search_len && data[i] == 0 && data[i + 1] == 0 &&
            data[i + 2] == 0 && data[i + 3] == 1)
            return data[i + 4] & 0x1f;
    }
    return -1;
}

/* 更新参数集缓存；先分配新缓冲，成功后才替换旧值，避免内存不足时丢失旧头。 */
static int cache_parameter_set(unsigned char **cache,
                               unsigned int *cache_len,
                               const unsigned char *data,
                               unsigned int len)
{
    unsigned char *new_cache;

    new_cache = malloc(len);
    if (new_cache == NULL)
        return -1;

    memcpy(new_cache, data, len);
    free(*cache);
    *cache = new_cache;
    *cache_len = len;
    return 0;
}

/* 所有录像写入都经此函数统计字节数，最终日志包含补写的 SPS/PPS。 */
static int write_record_bytes(struct record_file_info *record,
                              const unsigned char *data,
                              unsigned int len)
{
    size_t written = fwrite(data, 1, len, record->fp);

    if (written != len)
    {
        printf("[record] write failed: expected=%u, actual=%lu\n",
               len, (unsigned long)written);
        return -1;
    }
    record->bytes_written += written;
    return 0;
}

/*
 * 结束顺序：fflush 把 C 库缓冲交给内核，fsync 要求内核提交文件，fclose 关闭
 * 文件描述符。程序常从 NFS 目录运行，这一顺序可确保打印完成日志前数据已经
 * 提交给 Ubuntu NFS 服务端。最后通知 main.c 停止灯效并关闭全部 LED。
 */
static void finish_record(struct record_file_info *record,
                          unsigned long long elapsed_us)
{
    unsigned long long mib_whole;
    unsigned long long mib_decimal;

    if (record->fp != NULL)
    {
        fflush(record->fp);
        fsync(fileno(record->fp));
        fclose(record->fp);
        record->fp = NULL;
    }

    record->active = 0;
    record->finished = 1;

    mib_whole = record->bytes_written / (1024ULL * 1024ULL);
    mib_decimal = ((record->bytes_written % (1024ULL * 1024ULL)) * 100ULL) /
                  (1024ULL * 1024ULL);

    printf("[record] finished: %s, duration=%llu.%03llu s, "
           "bytes=%llu (%llu.%02llu MiB)\n",
           HOMEWORK_FILE_NAME,
           elapsed_us / 1000000ULL,
           (elapsed_us % 1000000ULL) / 1000ULL,
           record->bytes_written,
           mib_whole,
           mib_decimal);

    /* Recording ownership of the LEDs ends at the same PTS boundary. */
    project_recording_led_finished();
}

/*
 * DMC 每收到一个 H.264 NAL 单元就同步调用本函数。一帧可能含多个 NAL，
 * 它们共享同一 PTS，frame_end_flag=1 表示该帧最后一个 NAL。
 *
 * 本回调首先过滤通道：只写 media_chn=0 的 H.264，通道 1 子码流仍由 PES
 * 发送给 VLC，但不会写入板端录像。这样主、子码流职责清晰且互不影响。
 */
static int record_input(int media_chn,
                        int media_type,
                        int media_subtype,
                        unsigned long long frame_pts,
                        unsigned char *frame_data,
                        int frame_len,
                        int frame_end_flag)
{
    struct record_file_info *record;
    unsigned long long elapsed_us;
    int nal_type;

    if (media_chn != HOMEWORK_RECORD_CHANNEL ||
        media_type != DMC_MEDIA_TYPE_H264)
    {
        return 0;
    }

    if (media_chn < 0 || (unsigned int)media_chn >= g_max_channel_count ||
        g_record_files == NULL)
    {
        printf("[record] invalid encoder channel: %d\n", media_chn);
        return -1;
    }

    record = &g_record_files[media_chn];

    /*
     * RECORD 订阅在编码启动前已经注册，因此即使尚未按键，也能保存编码器最早
     * 输出的 SPS/PPS。后续强制 IDR 若不重复携带参数集，就从缓存补到文件头。
     */
    nal_type = h264_nal_type(frame_data, frame_len);
    if (nal_type == 7)
    {
        if (cache_parameter_set(&record->sps, &record->sps_len,
                                frame_data, (unsigned int)frame_len) != 0)
            printf("[record] warning: cannot cache H.264 SPS\n");
    }
    else if (nal_type == 8)
    {
        if (cache_parameter_set(&record->pps, &record->pps_len,
                                frame_data, (unsigned int)frame_len) != 0)
            printf("[record] warning: cannot cache H.264 PPS\n");
    }

    /* 未按 GPIO24 时直接返回，不开文件、不写数据，但 VLC 订阅者仍正常工作。 */
    if (!record->active || record->finished)
    {
        return 0;
    }

    /*
     * 按键可能落在 GOP 中间，P 帧依赖按键前的数据，不能作为独立文件开头。
     * 等待 FH_FRAME_I 对应的 subtype 后再开始；dmc_record_start() 还会主动
     * 请求 IDR，所以通常只需等待很短时间，失败时也会等到自然 GOP 的 I 帧。
     */
    if (!record->started && media_subtype != DMC_MEDIA_SUBTYPE_IFRAME)
        return 0;

    /*
     * 若关键帧不是以 SPS 开头，则必须已有完整参数集缓存；宁可继续等待，也不
     * 生成一个 VLC 无法解码的文件。正常流程中订阅早于 StartRecvPic，因此
     * 编码器启动时的首组 SPS/PPS 已经被缓存。
     */
    if (!record->started && nal_type != 7 &&
        (record->sps == NULL || record->pps == NULL))
    {
        printf("[record] waiting for H.264 SPS/PPS before starting file\n");
        return 0;
    }

    if (!record->started)
    {
        /* "wb" 表示二进制写入并覆盖旧文件，适合反复验收固定文件名。 */
        record->fp = fopen(HOMEWORK_FILE_NAME, "wb");
        if (record->fp == NULL)
        {
            printf("[record] cannot create %s\n", HOMEWORK_FILE_NAME);
            record->active = 0;
            record->finished = 1;
            return -1;
        }

        /* 以首个关键帧 PTS 为 0 点，不把按键后等待 IDR 的时间计入录像时长。 */
        record->started = 1;
        record->first_pts = frame_pts;

        /*
         * 若当前关键帧没有从 SPS/PPS 开始，则先补写缓存的参数集。VLC 必须先
         * 获得分辨率、Profile 等 SPS/PPS 信息，才能正确解码后面的 IDR/P 帧。
         */
        if (nal_type != 7 && record->sps != NULL &&
            write_record_bytes(record, record->sps, record->sps_len) != 0)
        {
            finish_record(record, 0);
            return -1;
        }
        if (nal_type != 7 && nal_type != 8 && record->pps != NULL &&
            write_record_bytes(record, record->pps, record->pps_len) != 0)
        {
            finish_record(record, 0);
            return -1;
        }

        printf("[record] started: %s, target=60 s, 1280x720@25fps, "
               "H.264 CBR %d bps, first_nal_type=%d, SPS=%u, PPS=%u\n",
               HOMEWORK_FILE_NAME, HOMEWORK_CONFIGURED_BITRATE, nal_type,
               record->sps_len, record->pps_len);
    }

    elapsed_us = frame_pts >= record->first_pts
                   ? frame_pts - record->first_pts
                   : 0;

    /*
     * 在“时间达到 60 秒的那一帧”的第一个 NAL 写入前停止。这样文件最后保留
     * 的一定是上一完整帧，不会出现只写入半帧的损坏数据。25 fps 时相邻帧约
     * 40 ms，远小于作业允许的 5 秒误差。
     */
    if (elapsed_us >= HOMEWORK_RECORD_DURATION_US)
    {
        finish_record(record, elapsed_us);
        return 0;
    }

    /* frame_data 属于编码器缓冲区，必须在取流线程 ReleaseStream 前完成写入。 */
    if (write_record_bytes(record, frame_data,
                           (unsigned int)frame_len) != 0)
    {
        finish_record(record, elapsed_us);
        return -1;
    }

    /*
     * 只有第一段数据 fwrite 成功后才启动流水灯，确保灯所表达的“开始录像”
     * 与磁盘中真正出现有效 H.264 数据一致，而不是仅与按键时刻一致。
     */
    if (!record->led_notified)
    {
        record->led_notified = 1;
        project_recording_led_notify();
    }

    /* 每 25 个完整帧 fflush 一次，约等于 25 fps 下每秒刷新一次。 */
    if (frame_end_flag)
    {
        record->frames_since_flush++;
        if (record->frames_since_flush >= HOMEWORK_FLUSH_INTERVAL_FRAME)
        {
            fflush(record->fp);
            record->frames_since_flush = 0;
        }
    }

    return 0;
}

int dmc_record_subscribe(int max_channel)
{
    /* 按编码通道分配状态，便于未来扩展；当前作业只写通道 0。 */
    if (max_channel <= HOMEWORK_RECORD_CHANNEL)
    {
        printf("[record] channel count %d does not include channel 0\n",
               max_channel);
        return -1;
    }

    if (g_record_files != NULL)
    {
        dmc_record_unsubscribe();
    }

    g_record_files = malloc(sizeof(*g_record_files) * (size_t)max_channel);
    if (g_record_files == NULL)
    {
        printf("[record] insufficient memory\n");
        return -1;
    }

    memset(g_record_files, 0,
           sizeof(*g_record_files) * (size_t)max_channel);
    g_max_channel_count = (unsigned int)max_channel;

    /*
     * RECORD 回调在程序启动时一次注册并始终保留，按键仅改变 active 状态。
     * 因此无需停止编码器或重启 VLC，录像与实时预览可并行进行。
     */
    if (dmc_subscribe("RECORD", DMC_MEDIA_TYPE_H264, record_input) != 0)
        return -1;

    printf("[record] ready: press GPIO24 to record one 60-second file\n");
    return 0;
}

int dmc_record_start(void)
{
    struct record_file_info *record;
    int ret;

    if (g_record_files == NULL ||
        g_max_channel_count <= HOMEWORK_RECORD_CHANNEL)
    {
        printf("[record] start rejected: recorder is not initialized\n");
        return -1;
    }

    record = &g_record_files[HOMEWORK_RECORD_CHANNEL];
    if (record->active)
    {
        printf("[record] GPIO24 ignored: recording is already active\n");
        return 1;
    }

    /*
     * 再次按键可以开始新一轮测试。只有确认旧文件已关闭后才清零整个状态结构；
     * 下一个关键帧到来时重新 fopen("wb")。若正在录像，则忽略按键，防止两个流程
     * 同时写同一 FILE*。
     */
    if (record->fp != NULL)
    {
        printf("[record] start rejected: previous file is still open\n");
        return -1;
    }

    /* 只复位单次录像状态，保留程序启动后持续收集到的 SPS/PPS 缓存。 */
    record->started = 0;
    record->finished = 0;
    record->led_notified = 0;
    record->frames_since_flush = 0;
    record->first_pts = 0;
    record->bytes_written = 0;
    record->active = 1;

    /* 强制通道 0 尽快输出 IDR；若 SDK 拒绝，仍等待两秒 GOP 中的自然 I 帧。 */
    ret = FH_VENC_RequestIDR(HOMEWORK_RECORD_CHANNEL);
    if (ret != 0)
        printf("[record] warning: request IDR failed with 0x%x; waiting for natural I-frame\n",
               (unsigned int)ret);
    else
        printf("[record] GPIO24 pressed: IDR requested, waiting for a decodable key frame\n");
    return 0;
}

int dmc_record_is_active(void)
{
    if (g_record_files == NULL ||
        g_max_channel_count <= HOMEWORK_RECORD_CHANNEL)
        return 0;

    return g_record_files[HOMEWORK_RECORD_CHANNEL].active ? 1 : 0;
}

int dmc_record_unsubscribe(void)
{
    unsigned int i;

    /* 先移除回调，保证释放状态数组后不会再有新码流进入 record_input()。 */
    dmc_unsubscribe("RECORD", DMC_MEDIA_TYPE_H264);

    if (g_record_files != NULL)
    {
        for (i = 0; i < g_max_channel_count; i++)
        {
            if (g_record_files[i].fp != NULL)
            {
                /* A manual Ctrl+C may stop a test before the 60-second mark. */
                finish_record(&g_record_files[i], 0);
            }
            free(g_record_files[i].sps);
            free(g_record_files[i].pps);
            g_record_files[i].sps = NULL;
            g_record_files[i].pps = NULL;
        }

        free(g_record_files);
        g_record_files = NULL;
    }

    g_max_channel_count = 0;
    return 0;
}
