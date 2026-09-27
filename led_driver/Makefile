obj-m := stream_led.o

# Override KDIR when the SDK is installed elsewhere:
# make KDIR=/absolute/path/to/linux-4.9
KDIR ?= /home/fh8862/resource/FH8862_IPC_V1.0.0_20221111/board_support/kernel/linux-4.9
CROSS_COMPILE ?= arm-fullhanv3-linux-uclibcgnueabi-
ARCH ?= arm
TEST_APP := stream_led_test

all: module $(TEST_APP)

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

$(TEST_APP): stream_led_test.c
	$(CROSS_COMPILE)gcc -Wall -O2 -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean
	rm -f $(TEST_APP)

.PHONY: all module clean
