obj-bin-y := libelf.o
libelf-objs := libelf-tools.o libelf-loader.o libelf-dominfo.o

SECTIONS := text data $(SPECIAL_DATA_SECTIONS)
ifneq ($(CONFIG_DOMAIN_FULL_RESET),y)
ifneq ($(CONFIG_COVERAGE),y)
    OBJCOPYFLAGS := $(foreach s,$(SECTIONS),--rename-section .$(s)=.init.$(s))
endif
endif

CFLAGS-y += -Wno-pointer-sign

$(obj)/libelf.o: $(obj)/libelf-temp.o FORCE
	$(call if_changed,objcopy)

$(obj)/libelf-temp.o: $(addprefix $(obj)/,$(libelf-objs)) FORCE
	$(call if_changed,ld)

targets += libelf-temp.o $(libelf-objs)
