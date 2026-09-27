#ifndef __libdmc_record_h__
#define __libdmc_record_h__

/* 注册/注销录像订阅者；注册后仍需 dmc_record_start() 才真正写文件。 */
extern int dmc_record_subscribe(int max_channel);
extern int dmc_record_unsubscribe(void);
/* 由 GPIO24 按下沿调用：请求 IDR，并从携带 SPS/PPS 的关键帧开始计时。 */
extern int dmc_record_start(void);
extern int dmc_record_is_active(void);

/* 由 main.c 实现：启动非阻塞的三秒流水灯线程。 */
extern void project_recording_led_notify(void);
/* 由 main.c 实现：60 秒录像文件关闭后停止灯效并关闭全部 LED。 */
extern void project_recording_led_finished(void);

#endif /*__libdmc_record_h__*/
