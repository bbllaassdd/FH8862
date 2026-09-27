#include <linux/device.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/uaccess.h>

/*
 * FH8862 按键/流水灯字符设备驱动（内核空间）
 *
 * 用户态通过 /dev/stream_led + ioctl 控制 GPIO43/44/52/53，并读取 GPIO24。
 * 驱动负责引脚复用、GPIO 所有权和电平转换；灯效时序与按键消抖留在 main.c，
 * 这样内核驱动保持短小、无阻塞，也便于单独替换用户态展示逻辑。
 */
#define DEVICE_NAME "stream_led"
#define CLASS_NAME "stream_led"

/*
 * ======================== 答辩重点：字符设备 ioctl 协议 ========================
 * 命令号必须与用户空间 src/main.c 完全一致。_IO 表示无需拷贝结构数据；
 * _IOR 表示数据方向为“内核读出到用户”，GET_KEY 因此使用 copy_to_user()。
 */
#define STREAM_LED_ON       _IO('L', 1)
#define STREAM_LED_OFF      _IO('L', 2)
#define STREAM_LED_SET_MASK _IO('L', 3)
#define STREAM_LED_GET_KEY  _IOR('L', 4, int)

#define STREAM_LED_GPIO_MODE 1
#define STREAM_LED_COUNT 4
#define STREAM_KEY_GPIO 24
#define STREAM_KEY_PINMUX 0x04020134

/*
 * GPIO 与 pinmux 地址来自课程 Led.c 和 FH8862 引脚复用表。
 * mask 的 bit0/1/2/3 分别控制 GPIO43/44/52/53。
 */
struct stream_led_pin {
    unsigned int gpio;
    unsigned long pinmux;
    const char *name;
};

static const struct stream_led_pin stream_led_pins[STREAM_LED_COUNT] = {
    { 43, 0x04020068, "stream_status" },
    { 44, 0x0402006c, "record_chase_1" },
    { 52, 0x04020098, "record_chase_2" },
    { 53, 0x0402009c, "record_chase_3" },
};

static int stream_led_major;
static struct class *stream_led_class;
static struct device *stream_led_device;
static unsigned int stream_led_requested;
static int stream_key_requested;
static unsigned long stream_led_mask;

/*
 * 芯片引脚可复用为 PWM、I2C、网口或 GPIO。ioremap 将物理寄存器临时映射到
 * 内核虚拟地址，只修改 [27:24] 功能选择位，其余位保持不变，随后 iounmap。
 */
static int stream_led_set_pinmux(unsigned long address)
{
    void __iomem *reg;
    unsigned int value;

    reg = ioremap(address, 4);
    if (reg == NULL)
        return -ENOMEM;

    value = readl(reg);
    value &= ~(0xfU << 24);
    value |= STREAM_LED_GPIO_MODE << 24;
    writel(value, reg);
    iounmap(reg);
    return 0;
}

static void stream_led_apply_mask(unsigned long mask)
{
    unsigned int i;

    /* 屏蔽高位，防止错误参数影响 4 个目标 GPIO 之外的逻辑。 */
    mask &= (1UL << STREAM_LED_COUNT) - 1UL;
    for (i = 0; i < STREAM_LED_COUNT; i++)
        gpio_set_value(stream_led_pins[i].gpio,
                       (mask & (1UL << i)) ? 1 : 0);
    stream_led_mask = mask;
}

static long stream_led_ioctl(struct file *file,
                             unsigned int command,
                             unsigned long argument)
{
    int key_pressed;

    (void)file;

    /*
     * ioctl 是用户态与内核态的控制入口。SET_MASK 的 argument 直接携带位图；
     * GET_KEY 则必须用 copy_to_user，不能在内核中直接解引用用户空间指针。
     */
    switch (command) {
    case STREAM_LED_ON:
        /* Preserve chase LEDs and turn on the GPIO43 stream indicator. */
        stream_led_apply_mask(stream_led_mask | 1UL);
        return 0;
    case STREAM_LED_OFF:
        stream_led_apply_mask(0);
        return 0;
    case STREAM_LED_SET_MASK:
        stream_led_apply_mask(argument);
        return 0;
    case STREAM_LED_GET_KEY:
        /*
         * The lesson-board key has an external pull-up. Its idle level is 1
         * and pressing it connects GPIO24 to ground, so userspace receives
         * the convenient logical value 1 only while the key is pressed.
         */
        /* 板上按键低电平有效：未按=1、按下=0；这里转换成用户易懂的按下=1。 */
        key_pressed = gpio_get_value(STREAM_KEY_GPIO) ? 0 : 1;
        if (copy_to_user((int __user *)argument,
                         &key_pressed, sizeof(key_pressed)) != 0)
            return -EFAULT;
        return 0;
    default:
        return -EINVAL;
    }
}

static const struct file_operations stream_led_operations = {
    /* owner 防止设备打开期间模块被卸载；unlocked_ioctl 提供控制命令入口。 */
    .owner = THIS_MODULE,
    .unlocked_ioctl = stream_led_ioctl,
};

static void stream_led_free_gpios(void)
{
    /* 逆序释放已成功申请的 GPIO；计数器同时让失败回滚与正常卸载复用此函数。 */
    while (stream_led_requested > 0) {
        stream_led_requested--;
        gpio_set_value(stream_led_pins[stream_led_requested].gpio, 0);
        gpio_free(stream_led_pins[stream_led_requested].gpio);
    }
}

static void stream_led_free_key(void)
{
    /* 只释放确实申请成功的 GPIO24，防止初始化中途失败时重复 gpio_free。 */
    if (stream_key_requested) {
        gpio_free(STREAM_KEY_GPIO);
        stream_key_requested = 0;
    }
}

static int __init stream_led_init(void)
{
    unsigned int i;
    int ret;

    /*
     * 驱动加载顺序：配置复用 -> 申请 GPIO 所有权 -> 设置输出且默认灭灯。
     * 任一步失败都跳到统一回滚路径，释放此前已申请的资源。
     */
    for (i = 0; i < STREAM_LED_COUNT; i++) {
        ret = stream_led_set_pinmux(stream_led_pins[i].pinmux);
        if (ret != 0)
            goto error_free_gpio;

        ret = gpio_request(stream_led_pins[i].gpio,
                           stream_led_pins[i].name);
        if (ret != 0) {
            pr_err("stream_led: GPIO%d request failed: %d\n",
                   stream_led_pins[i].gpio, ret);
            goto error_free_gpio;
        }
        stream_led_requested++;

        /* Keep every LED dark until the application selects a pattern. */
        ret = gpio_direction_output(stream_led_pins[i].gpio, 0);
        if (ret != 0)
            goto error_free_gpio;
    }

    /* GPIO24 is independent from the LEDs and is used as record-start key. */
    ret = stream_led_set_pinmux(STREAM_KEY_PINMUX);
    if (ret != 0)
        goto error_free_gpio;

    ret = gpio_request(STREAM_KEY_GPIO, "record_key_gpio24");
    if (ret != 0) {
        pr_err("stream_led: GPIO%d key request failed: %d\n",
               STREAM_KEY_GPIO, ret);
        goto error_free_gpio;
    }
    stream_key_requested = 1;

    ret = gpio_direction_input(STREAM_KEY_GPIO);
    if (ret != 0)
        goto error_free_gpio;

    /* 主设备号传 0 表示由内核动态分配。 */
    stream_led_major = register_chrdev(0, DEVICE_NAME,
                                       &stream_led_operations);
    if (stream_led_major < 0) {
        ret = stream_led_major;
        goto error_free_gpio;
    }

    stream_led_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(stream_led_class)) {
        ret = PTR_ERR(stream_led_class);
        goto error_unregister_chrdev;
    }

    /* 配合 class_create 自动生成 /dev/stream_led，供用户程序 open()。 */
    stream_led_device = device_create(stream_led_class, NULL,
                                      MKDEV(stream_led_major, 0),
                                      NULL, DEVICE_NAME);
    if (IS_ERR(stream_led_device)) {
        ret = PTR_ERR(stream_led_device);
        goto error_destroy_class;
    }

    pr_info("stream_led: LEDs GPIO43/44/52/53 and record key GPIO24 ready at /dev/%s\n",
            DEVICE_NAME);
    return 0;

error_destroy_class:
    class_destroy(stream_led_class);
error_unregister_chrdev:
    unregister_chrdev(stream_led_major, DEVICE_NAME);
error_free_gpio:
    stream_led_free_key();
    stream_led_free_gpios();
    return ret;
}

static void __exit stream_led_exit(void)
{
    /* 卸载模块前先灭灯，再按创建的逆序销毁设备、类别、字符设备和 GPIO。 */
    stream_led_apply_mask(0);
    device_destroy(stream_led_class, MKDEV(stream_led_major, 0));
    class_destroy(stream_led_class);
    unregister_chrdev(stream_led_major, DEVICE_NAME);
    stream_led_free_key();
    stream_led_free_gpios();
    pr_info("stream_led: removed\n");
}

module_init(stream_led_init);
module_exit(stream_led_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Team Four-Three");
MODULE_DESCRIPTION("FH8862 recording key and four-LED chase controller");
