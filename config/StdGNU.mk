AS         = $(CROSS_COMPILE)as
AR         = $(CROSS_COMPILE)ar
LD         = $(CROSS_COMPILE)ld
ifeq ($(clang),y)
ifeq ($(armds),y)
CC         = armclang
CXX        = armclang
LD_LTO     = armlink --verbose --no_scanlib
LD         = armlink --verbose --no_scanlib
AS         = armasm
AR         = armar
else
ifneq ($(CROSS_COMPILE),)
CC         = clang -target $(CROSS_COMPILE:-=)
CXX        = clang++ -target $(CROSS_COMPILE:-=)
else
CC         = clang
CXX        = clang++
endif
LD_LTO     = $(CROSS_COMPILE)llvm-ld
endif
else
CC         = $(CROSS_COMPILE)gcc
CXX        = $(CROSS_COMPILE)g++
LD_LTO     = $(CROSS_COMPILE)ld
endif
CPP        = $(CC) -E
RANLIB     = $(CROSS_COMPILE)ranlib
NM         = $(CROSS_COMPILE)nm
STRIP      = $(CROSS_COMPILE)strip
OBJCOPY    = $(CROSS_COMPILE)objcopy
OBJDUMP    = $(CROSS_COMPILE)objdump
SIZEUTIL   = $(CROSS_COMPILE)size

# Allow git to be wrappered in the environment
GIT        ?= git

INSTALL      = install
INSTALL_DIR  = $(INSTALL) -d -m0755 -p
INSTALL_DATA = $(INSTALL) -m0644 -p
INSTALL_PROG = $(INSTALL) -m0755 -p

BOOT_DIR ?= /boot
DEBUG_DIR ?= /usr/lib/debug

SOCKET_LIBS =
UTIL_LIBS = -lutil

SONAME_LDFLAG = -soname
SHLIB_LDFLAGS = -shared

