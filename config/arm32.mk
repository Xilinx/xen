CONFIG_ARM := y
CONFIG_ARM_32 := y
CONFIG_ARM_$(XEN_OS) := y

CONFIG_XEN_INSTALL_SUFFIX :=

# Explicitly specifiy 32-bit ARM ISA since toolchain default can be -mthumb:
ifeq ($(armds),y)
# VE needed
CFLAGS += --target=arm-arm-none-eabi -march=armv7-a
else
CFLAGS += -marm # -march= -mcpu=
# Use only if calling $(LD) directly.
LDFLAGS_DIRECT += -EL
endif

IOEMU_CPU_ARCH ?= arm
