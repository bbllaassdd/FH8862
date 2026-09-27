CROSS_COMPILE = arm-fullhanv3-linux-uclibcgnueabi-
CFLAGS = -Wall -Werror -fno-aggressive-loop-optimizations -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
CFLAGS += -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
CFLAGS += -lstdc++ -ldl -ffunction-sections -fdata-sections -ftree-vectorize -fPIC

CC = $(CROSS_COMPILE)gcc
STRIP = $(CROSS_COMPILE)strip

ROOT_DIR = .
LIBS_DIR = $(ROOT_DIR)/lib/static

INC_DIR = -I$(ROOT_DIR)/inc/
INC_DIR += -I$(ROOT_DIR)/include/
INC_DIR += -I$(ROOT_DIR)/include/types/
INC_DIR += -I$(ROOT_DIR)/include/dsp/
INC_DIR += -I$(ROOT_DIR)/include/dsp_ext/
INC_DIR += -I$(ROOT_DIR)/include/isp/
INC_DIR += -I$(ROOT_DIR)/include/isp_ext/
INC_DIR += -I$(ROOT_DIR)/include/mpp/
INC_DIR += -I$(ROOT_DIR)/include/vicap/

CFLAGS += $(INC_DIR)

TARGET = project_fh8862

SOURCES = $(wildcard ./src/*.c)
OBJECTS = $(SOURCES:%.c=%.o)
OBJECTSEX =  $(wildcard ./obj/*.o)
OBJ_DEP = $(SOURCES:%.c=%.d)

FH_MPP_LIBS = \
	$(LIBS_DIR)/libdsp.a \
	$(LIBS_DIR)/libdbi.a \
	$(LIBS_DIR)/libvb_mpi.a \
	$(LIBS_DIR)/libvmm.a \
	$(LIBS_DIR)/libmipi.a \
	$(LIBS_DIR)/libimx415_mipi.a \
	$(LIBS_DIR)/libadvapi_isp.a \
	$(LIBS_DIR)/libisp.a \
	$(LIBS_DIR)/libispcore.a \
	$(LIBS_DIR)/libadvapi_osd.a \
	$(LIBS_DIR)/libadvapi.a

all: $(TARGET) led-driver

$(TARGET) : $(OBJECTS)
	@$(CC) $(CFLAGS) -o $@ $(OBJECTS) $(OBJECTSEX) $(FH_MPP_LIBS) -lpthread -lm -lrt -ldl -rdynamic
	
	rm -rf $(ROOT_DIR)/src/*.o

# Keep the board application and its required four-LED driver in one build.
led-driver:
	$(MAKE) -C led_driver

clean:
	rm -rf $(ROOT_DIR)/src/*.o 
	rm -rf $(TARGET)
	$(MAKE) -C led_driver clean

.PHONY: all led-driver clean
