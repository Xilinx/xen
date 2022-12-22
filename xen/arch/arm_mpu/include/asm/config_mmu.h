/******************************************************************************
 * config_mmu.h
 *
 * A Linux-style configuration list, only can be included by config.h
 */

#ifndef __ARM_CONFIG_MMU_H__
#define __ARM_CONFIG_MMU_H__

/*
 * Common ARM32 and ARM64 layout:
 *   0  -   2M   Unmapped
 *   2M -   4M   Xen text, data, bss
 *   4M -   6M   Fixmap: special-purpose 4K mapping slots
 *   6M -  10M   Early boot mapping of FDT
 *   10M - 12M   Early relocation address (used when relocating Xen)
 *               and later for livepatch vmap (if compiled in)
 *
 * ARM32 layout:
 *   0  -  12M   <COMMON>
 *
 *  32M - 128M   Frametable: 24 bytes per page for 16GB of RAM
 * 256M -   1G   VMAP: ioremap and early_ioremap use this virtual address
 *                    space
 *
 *   1G -   2G   Xenheap: always-mapped memory
 *   2G -   4G   Domheap: on-demand-mapped
 *
 * ARM64 layout:
 * 0x0000000000000000 - 0x0000007fffffffff (512GB, L0 slot [0])
 *   0  -  12M   <COMMON>
 *
 *   1G -   2G   VMAP: ioremap and early_ioremap
 *
 *  32G -  64G   Frametable: 24 bytes per page for 5.3TB of RAM
 *
 * 0x0000008000000000 - 0x00007fffffffffff (127.5TB, L0 slots [1..255])
 *  Unused
 *
 * 0x0000800000000000 - 0x000084ffffffffff (5TB, L0 slots [256..265])
 *  1:1 mapping of RAM
 *
 * 0x0000850000000000 - 0x0000ffffffffffff (123TB, L0 slots [266..511])
 *  Unused
 */

#define XEN_VIRT_START         _AT(vaddr_t,0x00200000)
#define FIXMAP_ADDR(n)        (_AT(vaddr_t,0x00400000) + (n) * PAGE_SIZE)

#define BOOT_FDT_VIRT_START    _AT(vaddr_t,0x00600000)
#define BOOT_FDT_VIRT_SIZE     _AT(vaddr_t, MB(4))

#define BOOT_RELOC_VIRT_START  _AT(vaddr_t,0x00a00000)
#ifdef CONFIG_LIVEPATCH
#define LIVEPATCH_VMAP_START   _AT(vaddr_t,0x00a00000)
#define LIVEPATCH_VMAP_SIZE    _AT(vaddr_t, MB(2))
#endif

#define HYPERVISOR_VIRT_START  XEN_VIRT_START

#ifdef CONFIG_ARM_32

#define CONFIG_SEPARATE_XENHEAP 1

#define FRAMETABLE_VIRT_START  _AT(vaddr_t,0x02000000)
#define FRAMETABLE_SIZE        MB(128-32)
#define FRAMETABLE_NR          (FRAMETABLE_SIZE / sizeof(*frame_table))
#define FRAMETABLE_VIRT_END    (FRAMETABLE_VIRT_START + FRAMETABLE_SIZE - 1)

#define VMAP_VIRT_START        _AT(vaddr_t,0x10000000)
#define VMAP_VIRT_SIZE         _AT(vaddr_t, GB(1) - MB(256))

#define XENHEAP_VIRT_START     _AT(vaddr_t,0x40000000)
#define XENHEAP_VIRT_SIZE      _AT(vaddr_t, GB(1))

#define DOMHEAP_VIRT_START     _AT(vaddr_t,0x80000000)
#define DOMHEAP_VIRT_SIZE      _AT(vaddr_t, GB(2))

#define DOMHEAP_ENTRIES        1024  /* 1024 2MB mapping slots */

/* Number of domheap pagetable pages required at the second level (2MB mappings) */
#define DOMHEAP_SECOND_PAGES (DOMHEAP_VIRT_SIZE >> FIRST_SHIFT)

#else /* ARM_64 */

#define SLOT0_ENTRY_BITS  39
#define SLOT0(slot) (_AT(vaddr_t,slot) << SLOT0_ENTRY_BITS)
#define SLOT0_ENTRY_SIZE  SLOT0(1)

#define VMAP_VIRT_START  GB(1)
#define VMAP_VIRT_SIZE   GB(1)

#define FRAMETABLE_VIRT_START  GB(32)
#define FRAMETABLE_SIZE        GB(32)
#define FRAMETABLE_NR          (FRAMETABLE_SIZE / sizeof(*frame_table))

#define DIRECTMAP_VIRT_START   SLOT0(256)
#define DIRECTMAP_SIZE         (SLOT0_ENTRY_SIZE * (265-256))
#define DIRECTMAP_VIRT_END     (DIRECTMAP_VIRT_START + DIRECTMAP_SIZE - 1)

#define XENHEAP_VIRT_START     directmap_virt_start

#define HYPERVISOR_VIRT_END    DIRECTMAP_VIRT_END

#endif

/* Fixmap slots */
#define FIXMAP_CONSOLE  0  /* The primary UART */
#define FIXMAP_MISC     1  /* Ephemeral mappings of hardware */
#define FIXMAP_ACPI_BEGIN  2  /* Start mappings of ACPI tables */
#define FIXMAP_ACPI_END    (FIXMAP_ACPI_BEGIN + NUM_FIXMAP_ACPI_PAGES - 1)  /* End mappings of ACPI tables */

#endif /* __ARM_CONFIG_MMU_H__ */
/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
