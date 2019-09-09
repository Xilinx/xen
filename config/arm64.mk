CONFIG_ARM := y
CONFIG_ARM_64 := y
CONFIG_ARM_$(XEN_OS) := y

CONFIG_XEN_INSTALL_SUFFIX :=

ifeq ($(armds),y)
# VE needed
CFLAGS += --target=aarch64-arm-none-eabi -march=armv8.1-a+nofp+nosimd
else
CFLAGS += #-marm -march= -mcpu= etc
# Use only if calling $(LD) directly.
LDFLAGS_DIRECT += -EL
endif

IOEMU_CPU_ARCH ?= aarch64

EFI_DIR ?= /usr/lib64/efi
