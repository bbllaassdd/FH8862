#include <stdio.h>
#include <string.h>
#include "libdmc.h"
#include "libdmc_pes.h"
#include "libpes.h"

/*
 * DMC 到 VLC 的 PES/UDP 适配层（用户空间）
 *
 * 上游以 NAL 为单位调用 _pes_input_fn()；本文件按 media_chn 分别收集同一帧
 * 的多个 NAL，在 frame_end_flag 到达时交给 libpes_stream_pack()。libpes
 * 负责 PES 封装与 UDP 发送，通道 i 使用 base_port+i，因此主、子码流可以
 * 同时在 VLC 中打开，且不会共享帧组装状态。
 */
#define MAX_PES_CHANNEL_COUNT 8

struct dmc_pes_info
{
    char tar_ip[16];
    unsigned int port[MAX_PES_CHANNEL_COUNT];
    unsigned int printed[MAX_PES_CHANNEL_COUNT];
};

/*
 * ======================== 答辩重点：双路网络预览 ========================
 * 每个编码通道必须拥有独立的 NAL 计数和帧组装器，否则通道 0/1 交错到达时
 * 会把两路 NAL 拼进同一帧。数组下标就是 media_chn，实现状态完全隔离。
 */
static int g_frame_length[MAX_PES_CHANNEL_COUNT];
static int g_nalu_count[MAX_PES_CHANNEL_COUNT];
static struct vlcview_enc_stream_element
    g_stream_element[MAX_PES_CHANNEL_COUNT];
static struct dmc_pes_info g_print_info;

static int _pes_input_fn(int media_chn, int media_type, int media_subtype, \
        unsigned long long frame_pts, unsigned char *frame_data, \
        int frame_len, int frame_end_flag )
{
    int ret;
    int *frame_length;
    int *nalu_count;
    struct vlcview_enc_stream_element *stream_element;

    if (media_chn < 0 || media_chn >= MAX_PES_CHANNEL_COUNT ||
        !g_print_info.port[media_chn])
    {
        printf("Error: invalid PES channel %d\n", media_chn);
        return -1;
    }

    /* 取出本通道的状态槽，避免 ch0/ch1 交错 NAL 相互覆盖。 */
    frame_length = &g_frame_length[media_chn];
    nalu_count = &g_nalu_count[media_chn];
    stream_element = &g_stream_element[media_chn];

    /* 一帧 H.264/H.265 可能由多个 NAL 组成，先保存每段地址和长度。 */
    if (frame_len > 0)
    {
        if (*nalu_count >= MAX_NALU_COUNT)
        {
            printf("Error: libpes, max supported NALU is %d!\n", MAX_NALU_COUNT);
            return 0;
        }

        if (media_type == DMC_MEDIA_TYPE_H264)
            stream_element->enc_type = VLCVIEW_ENC_H264;
        else /*if (media_type == DMC_MEDIA_TYPE_H265)*/
            stream_element->enc_type = VLCVIEW_ENC_H265;

        if (media_subtype == DMC_MEDIA_SUBTYPE_IFRAME)
            stream_element->frame_type = VLCVIEW_ENC_I_FRAME;
        else /*if (media_subtype == DMC_MEDIA_SUBTYPE_PFRAME)*/
            stream_element->frame_type = VLCVIEW_ENC_P_FRAME;

        stream_element->time_stamp = frame_pts;
        stream_element->nalu[*nalu_count].start = frame_data;
        stream_element->nalu[*nalu_count].len = frame_len;

        *frame_length += frame_len;
        (*nalu_count)++;
    }

    /* 最后一个 NAL 到达后才把完整帧交给 PES 打包，并清零本通道计数。 */
    if (frame_end_flag)
    {
        stream_element->nalu_count = *nalu_count;
        stream_element->frame_len  = *frame_length;

        ret = libpes_stream_pack(media_chn, *stream_element);
        *frame_length = 0;
        *nalu_count = 0;
        if(ret)
        {
            printf("Error: libpes_stream_pack failed with %d\n", ret);
            return -1;
        }
    }

    if (!g_print_info.printed[media_chn])
    {
        printf("PES: send stream to %s:%d through UDP\n", g_print_info.tar_ip, g_print_info.port[media_chn]);
        g_print_info.printed[media_chn] = 1;
    }

    return 0;
}

int dmc_pes_subscribe(int max_channel, char* ip, int port)
{
    int i;
    int ret;

    if (ip == NULL)
    {
        printf("Error: NULL ip address, please run \"vlcview -h\"\n");
        return -1;
    }

    if (max_channel > MAX_PES_CHANNEL_COUNT)
    {
        printf("Error: channel num is larger than %d\n", MAX_PES_CHANNEL_COUNT);
        return -1;
    }

    memset(g_frame_length, 0, sizeof(g_frame_length));
    memset(g_nalu_count, 0, sizeof(g_nalu_count));
    memset(g_stream_element, 0, sizeof(g_stream_element));
    memset(&g_print_info, 0, sizeof(g_print_info));

    /* 先初始化发送库和每路目标，再向 DMC 注册回调，避免回调提前到达。 */
    ret = libpes_init();
    if (ret != 0)
    {
        return -1;
    }

    strncpy(g_print_info.tar_ip, ip, sizeof(g_print_info.tar_ip));

    /* 通道 i 使用 base_port+i：主码流 1234，子码流 1235。 */
    for(i = 0; i < max_channel; i++)
    {
        ret = libpes_send_to_vlc(i, ip, port + i);
        if (ret != 0)
        {
            printf("Error: configure PES channel %d port %d failed\n",
                   i, port + i);
            libpes_uninit();
            return -1;
        }
        g_print_info.port[i] = port + i;
        g_print_info.printed[i] = 0;
    }

    ret = dmc_subscribe("PES", DMC_MEDIA_TYPE_H264 | DMC_MEDIA_TYPE_H265,
                        _pes_input_fn);
    if (ret != 0)
    {
        libpes_uninit();
    }
    return ret;
}

int dmc_pes_unsubscribe(void)
{
    /* 先断开上游回调，再释放 PES 发送资源。 */
    dmc_unsubscribe("PES", DMC_MEDIA_TYPE_H264 | DMC_MEDIA_TYPE_H265);
    libpes_uninit();
    return 0;
}
