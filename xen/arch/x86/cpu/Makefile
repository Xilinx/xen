obj-y += mcheck/
obj-y += microcode/
obj-y += mtrr/

obj-$(CONFIG_AMD) += amd.o
obj-$(CONFIG_AMD_SVM) += amd.o
obj-$(CONFIG_CENTAUR) += centaur.o
obj-y += common.o
obj-$(CONFIG_HYGON) += hygon.o
obj-$(CONFIG_INTEL) += intel.o
obj-$(CONFIG_INTEL) += intel_cacheinfo.o
obj-$(CONFIG_INTEL) += mwait-idle.o
obj-$(CONFIG_SHANGHAI) += shanghai.o
ifeq ($(CONFIG_VPMU), y)
obj-y += vpmu.o
obj-$(CONFIG_AMD) += vpmu_amd.o
obj-$(CONFIG_INTEL) += vpmu_intel.o
endif
