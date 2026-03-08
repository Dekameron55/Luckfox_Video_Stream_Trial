#
#Copyright (c) 2026
#
#
#Disclaimer:
#
#This code is provided as an example and is not intended for production use
#without thorough testing and validation. The author and contributors are not
#liable for any damages or issues that may arise from its use.
#
#
# Makefile for the RV1106 JPEG Stream Trial Example
#

# Include the main build parameters from the root of the media component
ifeq ($(MEDIA_PARAM), )
    MEDIA_PARAM:=../../Makefile.param
    include $(MEDIA_PARAM)
endif

# Define compiler and target
BUILD_DIR   := build
TARGET      := rv1106_jpeg_stream_trial
CC          := $(RK_MEDIA_CROSS)-gcc
SOURCES     := $(TARGET).c

# Compiler Flags
CFLAGS      := -g -Wall
CFLAGS      += -I$(RK_MEDIA_OUTPUT)/include
CFLAGS      += -I$(RK_MEDIA_OUTPUT)/include/rkaiq
CFLAGS      += -I$(RK_MEDIA_OUTPUT)/include/rkaiq/uAPI2
CFLAGS      += -I$(RK_MEDIA_OUTPUT)/include/rkaiq/common
CFLAGS      += -I$(RK_MEDIA_OUTPUT)/include/rkaiq/xcore
CFLAGS      += -I$(RK_MEDIA_OUTPUT)/include/rkaiq/algos
CFLAGS      += -I$(RK_MEDIA_OUTPUT)/include/rkaiq/iq_parser
CFLAGS      += -I$(RK_MEDIA_OUTPUT)/include/rkaiq/iq_parser_v2
CFLAGS      += -I$(RK_MEDIA_OUTPUT)/include/rkaiq/smartIr
CFLAGS      += -DRKPLATFORM=ON -DARCH64=OFF -DUAPI2 -DRV1106_RV1103 -DRKAIQ
CFLAGS      += $(RK_MEDIA_CROSS_CFLAGS)

# Linker Flags
LDFLAGS     := -Wl,-rpath-link,${RK_MEDIA_OUTPUT}/lib
LDFLAGS     += -L$(RK_MEDIA_OUTPUT)/lib -L$(RK_MEDIA_OUTPUT)/root/usr/lib
LDFLAGS     += -lpthread -lm -lrockit -lrockchip_mpp -lrga -lstdc++ -lrkaiq
LDFLAGS     += -Wl,--gc-sections -Wl,--as-needed

# Build rules
all: $(BUILD_DIR)/$(TARGET)

$(BUILD_DIR)/$(TARGET): $(SOURCES)
	@echo "Creating build directory..."
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Build complete: $@"

clean:
	@echo "Cleaning up..."
	@rm -rf $(BUILD_DIR)
