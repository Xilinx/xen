include Makefile.libfdt

SECTIONS := text data $(SPECIAL_DATA_SECTIONS)

# For CONFIG_OVERLAY_DTB, libfdt functionalities will be needed during runtime.
ifneq ($(CONFIG_OVERLAY_DTB),y)
OBJCOPYFLAGS := $(foreach s,$(SECTIONS),--rename-section .$(s)=.init.$(s))
endif

obj-y += libfdt.o
nocov-y += libfdt.o

CFLAGS-y += -I$(BASEDIR)/include/xen/libfdt/

libfdt.o: libfdt-temp.o FORCE
	$(call if_changed,objcopy)

libfdt-temp.o: $(LIBFDT_OBJS) FORCE
	$(call if_changed,ld)

extra-y += libfdt-temp.o $(LIBFDT_OBJS)
