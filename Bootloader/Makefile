CROSS ?= arm-none-eabi-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE := $(CROSS)size

BUILD := build
MBEDTLS_LIB := ../Middlewares/Third_Party/mbedTLS/library

C_SOURCES := \
	bootloader.c \
	$(MBEDTLS_LIB)/bignum.c \
	$(MBEDTLS_LIB)/ecp.c \
	$(MBEDTLS_LIB)/ecp_curves.c \
	$(MBEDTLS_LIB)/ecdsa.c \
	$(MBEDTLS_LIB)/asn1parse.c \
	$(MBEDTLS_LIB)/asn1write.c \
	$(MBEDTLS_LIB)/sha256.c \
	$(MBEDTLS_LIB)/platform_util.c

C_OBJECTS := $(addprefix $(BUILD)/,$(notdir $(C_SOURCES:.c=.o)))
ASM_OBJECTS := $(BUILD)/boot_startup.o
OBJECTS := $(ASM_OBJECTS) $(C_OBJECTS)

INCLUDES := \
	-I. \
	-I../Core/Inc \
	-I../Drivers/CMSIS/Include \
	-I../Drivers/CMSIS/Device/ST/STM32H7xx/Include \
	-I../Drivers/STM32H7xx_HAL_Driver/Inc \
	-I../Middlewares/Third_Party/mbedTLS/include

CPU := -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard
DEFINES := -DSTM32H753xx '-DMBEDTLS_CONFIG_FILE=<boot_mbedtls_config.h>'
CFLAGS := $(CPU) $(DEFINES) $(INCLUDES) -std=c11 -Os \
	-ffunction-sections -fdata-sections -fno-common \
	-fno-unwind-tables -fno-asynchronous-unwind-tables \
	-Wall -Wextra -Werror -MMD -MP
ASFLAGS := $(CPU) -x assembler-with-cpp
LDFLAGS := $(CPU) -nostartfiles --specs=nano.specs --specs=nosys.specs \
	-Tbootloader.ld -Wl,--gc-sections -Wl,-Map=$(BUILD)/bootloader.map \
	-Wl,--start-group -lc -lgcc -Wl,--end-group

VPATH := .:$(MBEDTLS_LIB)
DEPS := $(C_OBJECTS:.o=.d)

ifeq ($(OS),Windows_NT)
REMOVE_BUILD := cmd /C "if exist $(BUILD) rmdir /S /Q $(BUILD)"
else
REMOVE_BUILD := rm -rf $(BUILD)
endif

.PHONY: all clean

all: $(BUILD)/bootloader.elf $(BUILD)/bootloader.bin

$(BUILD):
	mkdir $(BUILD)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/boot_startup.o: boot_startup.s | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/bootloader.elf: $(OBJECTS) bootloader.ld
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(BUILD)/bootloader.bin: $(BUILD)/bootloader.elf
	$(OBJCOPY) -O binary $< $@

clean:
	$(REMOVE_BUILD)

-include $(DEPS)
