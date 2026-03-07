obj-y += vpci.o header.o rebar.o
obj-$(CONFIG_HAS_PCI_MSI) += msi.o
obj-$(CONFIG_HAS_PCI_MSI) += msix_common.o
ifeq ($(CONFIG_X86),y)
obj-$(CONFIG_HAS_PCI_MSI) += msix.o
endif
