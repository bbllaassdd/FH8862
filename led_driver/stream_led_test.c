#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define STREAM_LED_DEVICE   "/dev/stream_led"
#define STREAM_LED_OFF      _IO('L', 2)
#define STREAM_LED_SET_MASK _IO('L', 3)
#define STREAM_LED_GET_KEY  _IOR('L', 4, int)

#define LED_COUNT 4
#define TEST_STEPS 16
#define STEP_US 187500

int main(void)
{
    static const int gpio_order[LED_COUNT] = { 43, 44, 52, 53 };
    unsigned long mask;
    int fd;
    int pressed;
    int step;

    /*
     * 该测试程序只验证驱动，不依赖摄像头、ISP 或编码器。答辩前先运行它，
     * 可以把“GPIO/驱动故障”和“媒体应用故障”分开定位。
     */
    fd = open(STREAM_LED_DEVICE, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n",
                STREAM_LED_DEVICE, strerror(errno));
        fprintf(stderr, "load the new stream_led.ko first\n");
        return 1;
    }

    printf("Testing GPIO43 -> GPIO44 -> GPIO52 -> GPIO53 for 3 seconds\n");
    for (step = 0; step < TEST_STEPS; step++) {
        /* Exactly one bit is set, so only one LED should be lit per step. */
        mask = 1UL << (step % LED_COUNT);
        printf("step %02d: GPIO%d on\n",
               step + 1, gpio_order[step % LED_COUNT]);
        if (ioctl(fd, STREAM_LED_SET_MASK, mask) != 0) {
            fprintf(stderr, "SET_MASK failed: %s\n", strerror(errno));
            ioctl(fd, STREAM_LED_OFF);
            close(fd);
            return 1;
        }
        usleep(STEP_US);
    }

    ioctl(fd, STREAM_LED_OFF);

    /* 20 ms 读取一次，共 500 次，即留给按键 10 秒测试窗口。 */
    printf("Now press GPIO24 within 10 seconds...\n");
    for (step = 0; step < 500; step++) {
        if (ioctl(fd, STREAM_LED_GET_KEY, &pressed) != 0) {
            fprintf(stderr, "GET_KEY failed: %s; reload the newest stream_led.ko\n",
                    strerror(errno));
            ioctl(fd, STREAM_LED_OFF);
            close(fd);
            return 1;
        }
        if (pressed) {
            printf("GPIO24 key press detected\n");
            break;
        }
        usleep(20000);
    }

    close(fd);
    if (step == 500)
        printf("GPIO24 was not pressed during the test window\n");
    printf("LED/key test finished; all LEDs are off\n");
    return 0;
}
