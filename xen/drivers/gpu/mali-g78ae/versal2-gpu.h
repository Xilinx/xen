#ifdef CONFIG_MALI_G78AE_VERSAL2
#include <xen/vmap.h>

#define VERSAL2_MMI_SLRC_BASE       0xED720000
#define VERSAL2_MMI_SLRC_SIZE       0x2000

static inline int versal2_mali_gpu_init(void)
{
    void __iomem *mmi_slrc_base;

    mmi_slrc_base = ioremap_nocache(VERSAL2_MMI_SLRC_BASE,VERSAL2_MMI_SLRC_SIZE);
    printk(XENLOG_DEBUG "Current MMI_SLRC base address: %p\n", mmi_slrc_base);
    if ( !mmi_slrc_base )
    {
        printk(XENLOG_ERR "gpu: Failed to map MMI_SLRC base address\n");
        return -ENOMEM;
    }

    printk(XENLOG_DEBUG "gpu: Configuring MMI_SLRC registers\n");
    printk(XENLOG_DEBUG "Current value of MMI_SLRC register 0x43C: %x\n",
           readl(mmi_slrc_base + 0x43C));
    writel(CONFIG_MALI_G78AE_VERSAL_MMI_SLRC_REG_CFG_0, mmi_slrc_base + 0x43C);
    printk(XENLOG_DEBUG "New value of MMI_SLRC register 0x43C: %x\n",
           readl(mmi_slrc_base + 0x43C));
    printk(XENLOG_DEBUG "Current value of MMI_SLRC register 0x440: %x\n",
           readl(mmi_slrc_base + 0x440));
    writel(CONFIG_MALI_G78AE_VERSAL_MMI_SLRC_REG_CFG_1, mmi_slrc_base + 0x440);
    printk(XENLOG_DEBUG "New value of MMI_SLRC register 0x440: %x\n",
           readl(mmi_slrc_base + 0x440));

    printk(XENLOG_DEBUG "Current value of MMI_SLRC register 0x458: %x\n",
           readl(mmi_slrc_base + 0x458));
    writel(CONFIG_MALI_G78AE_VERSAL_MMI_SLRC_REG_CFG_0, mmi_slrc_base + 0x458);
    printk(XENLOG_DEBUG "New value of MMI_SLRC register 0x458: %x\n",
           readl(mmi_slrc_base + 0x458));
    printk(XENLOG_DEBUG "Current value of MMI_SLRC register 0x45C: %x\n",
              readl(mmi_slrc_base + 0x45C));
    writel(CONFIG_MALI_G78AE_VERSAL_MMI_SLRC_REG_CFG_1, mmi_slrc_base + 0x45C);
    printk(XENLOG_DEBUG "New value of MMI_SLRC register 0x45C: %x\n",
           readl(mmi_slrc_base + 0x45C));
    iounmap(mmi_slrc_base);
    return 0;
}
#else /* !CONFIG_MALI_G78AE_VERSAL2 */

static inline int versal2_mali_gpu_init(void) { return 0; }

#endif /* CONFIG_MALI_G78AE_VERSAL2 */