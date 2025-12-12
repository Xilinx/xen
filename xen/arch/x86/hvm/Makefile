obj-$(CONFIG_AMD_SVM) += svm/
obj-$(CONFIG_INTEL_VMX) += vmx/
obj-$(CONFIG_VIRIDIAN) += viridian/

obj-y += asid.o
obj-y += dm.o
obj-bin-y += dom0_build.init.o
obj-bin-y += dom_build.init.o
obj-y += domain.o
obj-y += emulate.o
obj-$(CONFIG_GRANT_TABLE) += grant_table.o
obj-$(CONFIG_VHPET) += hpet.o
obj-y += hvm.o
obj-y += hypercall.o
obj-y += intercept.o
obj-y += io.o
obj-y += ioreq.o
obj-y += irq.o
obj-y += mmio.o
obj-y += monitor.o
obj-y += mtrr.o
obj-y += nestedhvm.o
obj-y += pmtimer.o
obj-y += quirks.o
obj-$(CONFIG_VRTC) += rtc.o
obj-y += save.o
obj-$(CONFIG_VVGA) += stdvga.o
obj-y += vioapic.o
obj-y += vlapic.o
obj-y += vm_event.o
obj-y += vmsi.o
obj-$(CONFIG_VPIC) += vpic.o
obj-y += vpt.o

# Build ACPI DSDT for PVH guests
ACPI_C = $(obj)/dsdt_pvh.c
ACPI_OBJS = $(patsubst $(obj)/%.c,%.init.o,$(ACPI_C))
obj-bin-y += $(ACPI_OBJS)
$(obj)/%.c: $(obj)/%.asl
	iasl -vs -p $(obj)/$* -tc $<
	sed -e 's/AmlCode/$*/g' -e 's/_aml_code//g' -e 's/$*/__initdata $*/g' $(obj)/$*.hex > $(obj)/$*.tmp
	echo "int __initdata $*_len = sizeof($*);" >> $(obj)/$*.tmp
	mv -f $(obj)/$*.tmp $(obj)/$*.c
	rm -f $(addprefix $(obj)/, $*.aml $*.hex)
$(obj)/$(ACPI_OBJS): $(obj)/%.init.o: $(obj)/%.c
clean::
	rm -f $(patsubst %.c,%.hex,$(ACPI_C)) $(patsubst %.c,%.aml,$(ACPI_OBJS))
	rm -f $(patsubst %.c,%.tmp,$(ACPI_C)) $(ACPI_C)
