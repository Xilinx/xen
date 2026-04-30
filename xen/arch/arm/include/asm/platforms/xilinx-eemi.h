#ifndef __ASM_ARM_PLATFORMS_XILINX_EEMI_H
#define __ASM_ARM_PLATFORMS_XILINX_EEMI_H

#include <asm/platforms/xilinx-versal-eemi.h>
#include <asm/platforms/xilinx-versal-net-eemi.h>
#include <asm/platforms/amd-versal2-eemi.h>
#include <asm/platforms/xilinx-zynqmp-eemi.h>

/**
 * Get EEMI PM Function ID
 */
#define EEMI_PM_FID(fid) ((fid) & 0xFFFF)

#define EEMI_FID(fid) ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, \
                                         ARM_SMCCC_CONV_64,   \
                                         ARM_SMCCC_OWNER_SIP, \
                                         fid)

/* IPI SMC function numbers enum definition and fids */
#define IPI_MAILBOX_FID(fid) ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, \
                                                ARM_SMCCC_CONV_32,   \
                                                ARM_SMCCC_OWNER_SIP, \
                                                fid)

/*
 * Following calls are used for interface between
 * xen and TF-A. We don't consider these as EEMI calls.
 * They follow SMC standard but, PLM firmware is unaware
 * of these calls.
 */
#define TF_A_PM_FEATURE_CHECK 0xa00
#define PM_GET_CALLBACK_DATA 0xa01
#define PM_GET_TRUSTZONE_VERSION 0xa03
/* From linux kernel to set SGI in TF-A */
#define TF_A_PM_REGISTER_SGI 0xa04

/* Fix ID to pass PLM specific APIs through TF-A to firmware */
#define PASS_THROUGH_SMC_ID	EEMI_FID(0xFFFU)

/* API ID mask */
#define XPM_API_ID_MASK	(0xFFU)

/* Module ID mask */
#define MODULE_ID_MASK	(0xFF00U)

/* Extract Module ID */
#define MODULE_ID(x)	((x & MODULE_ID_MASK) >> 8)

/* XilPM module ID */
#define XPM_MODULE_ID (0x2U)

enum ipi_api_id {
    IPI_MAILBOX_OPEN = 0x1000,
    IPI_MAILBOX_RELEASE,
    IPI_MAILBOX_STATUS_ENQUIRY,
    IPI_MAILBOX_NOTIFY,
    IPI_MAILBOX_ACK,
    IPI_MAILBOX_ENABLE_IRQ,
    IPI_MAILBOX_DISABLE_IRQ,
};

enum pm_api_id {
    /* Miscellaneous API functions: */
    PM_GET_API_VERSION = 1, /* Do not change or move */
    PM_SET_CONFIGURATION,
    PM_GET_NODE_STATUS,
    PM_GET_OP_CHARACTERISTIC,
    PM_REGISTER_NOTIFIER,
    /* API for suspending of PUs: */
    PM_REQ_SUSPEND,
    PM_SELF_SUSPEND,
    PM_FORCE_POWERDOWN,
    PM_ABORT_SUSPEND,
    PM_REQ_WAKEUP,
    PM_SET_WAKEUP_SOURCE,
    PM_SYSTEM_SHUTDOWN,
    /* API for managing PM slaves: */
    PM_REQ_NODE,
    PM_RELEASE_NODE,
    PM_SET_REQUIREMENT,
    PM_SET_MAX_LATENCY,
    /* Direct control API functions: */
    PM_RESET_ASSERT,
    PM_RESET_GET_STATUS,
    PM_MMIO_WRITE,
    PM_MMIO_READ,
    PM_INIT,
    PM_FPGA_LOAD,
    PM_FPGA_GET_STATUS,
    PM_GET_CHIPID,
    /* ID 25 is been used by U-boot to process secure boot images */
    /* Secure library generic API functions */
    PM_SECURE_SHA = 26,
    PM_SECURE_RSA,
    /* Pin control API functions */
    PM_PINCTRL_REQUEST,
    PM_PINCTRL_RELEASE,
    PM_PINCTRL_GET_FUNCTION,
    PM_PINCTRL_SET_FUNCTION,
    PM_PINCTRL_CONFIG_PARAM_GET,
    PM_PINCTRL_CONFIG_PARAM_SET,
    /* PM IOCTL API */
    PM_IOCTL,
    /* API to query information from firmware */
    PM_QUERY_DATA,
    /* Clock control API functions */
    PM_CLOCK_ENABLE,
    PM_CLOCK_DISABLE,
    PM_CLOCK_GETSTATE,
    PM_CLOCK_SETDIVIDER,
    PM_CLOCK_GETDIVIDER,
    PM_CLOCK_SETRATE,
    PM_CLOCK_GETRATE,
    PM_CLOCK_SETPARENT,
    PM_CLOCK_GETPARENT,
    PM_FPGA_READ = 46,
    PM_SECURE_AES,
    /* PLL control API functions */
    PM_PLL_SET_PARAMETER = 48,
    PM_PLL_GET_PARAMETER,
    PM_PLL_SET_MODE,
    PM_PLL_GET_MODE,
    /* PM Feature Check */
    PM_FEATURE_CHECK = 63,
    PM_FPGA_GET_VERSION = 72,
    PM_FPGA_GET_FEATURE_LIST,
    PM_API_MAX
};

enum pm_ioctl_id {
    IOCTL_SD_DLL_RESET = 6,
    IOCTL_SET_SD_TAPDELAY = 7,
    IOCTL_GET_PLL_FRAC_MODE = 9,
    IOCTL_REGISTER_SGI = 25,
};

/*
 * Module feature check API ID. Each module's feature check is issued as
 * EEMI_FID((module_id << 8) | PM_API_FEATURES).
 * See Linux: include/linux/firmware/xlnx-zynqmp.h
 */
#define PM_API_FEATURES 0

/*
 * XilSECURE API IDs (module 0x5).
 * See Linux: include/linux/firmware/xlnx-zynqmp-crypto.h
 */
enum xsecure_id {
    XSECURE_API_FEATURES = 0x500,
    XSECURE_API_RSA_SIGN_VERIFY = 0x501,
    XSECURE_API_RSA_PUBLIC_ENCRYPT,
    XSECURE_API_RSA_PRIVATE_DECRYPT,
    XSECURE_API_SHA3_UPDATE,
    XSECURE_API_ELLIPTIC_VALIDATE_KEY = 0x507,
    XSECURE_API_ELLIPTIC_VERIFY_SIGN,
    XSECURE_API_AES_INIT,
    XSECURE_API_AES_OP_INIT,
    XSECURE_API_AES_UPDATE_AAD,
    XSECURE_API_AES_ENCRYPT_UPDATE,
    XSECURE_API_AES_ENCRYPT_FINAL,
    XSECURE_API_AES_DECRYPT_UPDATE,
    XSECURE_API_AES_DECRYPT_FINAL,
    XSECURE_API_AES_KEY_ZERO,
    XSECURE_API_AES_WRITE_KEY = 0x511,
};

/*
 * XilPUF API IDs (module 0xC).
 * See Linux: include/linux/firmware/xlnx-zynqmp-crypto.h
 */
enum xpuf_id {
    XPUF_API_FEATURES = 0xc00,
    XPUF_API_PUF_REGISTRATION = 0xc01,
    XPUF_API_PUF_REGENERATION,
    XPUF_API_PUF_CLEAR_PUF_ID = 0xc03,
};

/*
 * XilNVM BBRAM and eFuse API IDs (module 0xB).
 * See Linux: include/linux/firmware/xlnx-zynqmp-nvm.h
 *            drivers/nvmem/xlnx_secure_config.c (eFuse write APIs)
 */
enum xilnvm_id {
    XILNVM_API_FEATURES = 0xB00,
    PM_BBRAM_WRITE_KEY = 0xB01,
    PM_BBRAM_ZEROIZE,
    PM_BBRAM_WRITE_USERDATA,
    PM_BBRAM_READ_USERDATA,
    PM_BBRAM_LOCK_USERDATA = 0xB05,
};

enum efuse_id {
    PM_EFUSE_READ_VERSAL = 0xB17,
    PM_EFUSE_WRITE_IV_ACCESS_VERSAL = 0xB18,
    PM_EFUSE_WRITE_MISC1_ACCESS_VERSAL,
    PM_EFUSE_WRITE_PUF_ACCESS_VERSAL,
    PM_EFUSE_WRITE_OFFCHIP_ACCESS_VERSAL,
    PM_EFUSE_WRITE_USER_ACCESS_VERSAL,
    PM_EFUSE_WRITE_REVOCATIONID_ACCESS_VERSAL,
    PM_EFUSE_WRITE_PPK_ACCESS_VERSAL,
    PM_EFUSE_WRITE_ANLG_TRIM_ACCESS_VERSAL,
    PM_EFUSE_WRITE_BOOT_ENV_CTRL_ACCESS_VERSAL,
    PM_EFUSE_WRITE_MISC_CTRL_ACCESS_VERSAL,
    PM_EFUSE_WRITE_SECURITY_CTRL_ACCESS_VERSAL,
    PM_EFUSE_WRITE_SECURITY_MISC0_ACCESS_VERSAL,
    PM_EFUSE_WRITE_AES_KEYS_ACCESS_VERSAL = 0xB24,
};

/**
 * @XST_PM_SUCCESS:		Success
 * @XST_PM_ARGS:		illegal arguments provided (deprecated)
 * @XST_PM_NOTSUPPORTED:	feature not supported  (deprecated)
 * @XST_PM_INVALID_PARAM:	invalid argument
 * @XST_PM_INTERNAL:	Unexpected error
 * @XST_PM_CONFLICT:	Conflicting requirements
 * @XST_PM_NO_ACCESS:	Access rights violation
 * @XST_PM_INVALID_NODE:	Does not apply to node passed as argument
 * @XST_PM_DOUBLE_REQ:	Duplicate request
 * @XST_PM_ABORT_SUSPEND:	Target has aborted suspend
 * @XST_PM_TIMEOUT:		timeout in communication with PMU
 * @XST_PM_NODE_USED:		node is already in use
 */
enum pm_ret_status {
    XST_PM_SUCCESS = 0,
    XST_PM_ARGS = 1,
    XST_PM_NOTSUPPORTED = 4,
    XST_PM_INVALID_PARAM = 15,
    XST_PM_INTERNAL = 2000,
    XST_PM_CONFLICT,
    XST_PM_NO_ACCESS,
    XST_PM_INVALID_NODE,
    XST_PM_DOUBLE_REQ,
    XST_PM_ABORT_SUSPEND,
    XST_PM_TIMEOUT,
    XST_PM_NODE_USED
};

/*
 * Selected set of memory mapped definitions of device nodes.
 */
struct pm_access
{
    uint64_t addr;
    bool hwdom_access;    /* HW domain gets access regardless. */
};

struct pm_clk2node
{
    uint32_t clk_idx;
    uint32_t dev_idx;
};

bool xilinx_eemi(struct cpu_user_regs *regs, const uint32_t fid,
                 uint32_t nodeid,
                 uint32_t pm_fn,
                 const struct pm_access *pm_node_access,
                 const uint32_t pm_node_access_size,
                 const struct pm_access *pm_rst_access,
                 const uint32_t pm_rst_access_size,
                 const struct pm_clk2node *pm_clock_node_map,
                 const uint32_t pm_clock_node_map_size,
                 const uint32_t clk_end);

#define PM_CLK2NODE(clk, dev)   { .clk_idx = clk, .dev_idx = dev }

bool pm_check_access(const struct pm_access *acl, struct domain *d, u32 idx);

/* Check if a domain has access to a node.  */
bool domain_has_node_access(struct domain *d, const u32 node,
                            const struct pm_access *pm_node_access,
                            const uint32_t table_size);

/* Check if a clock id is valid */
bool clock_id_is_valid(u32 clk_id, u32 clk_end);

/*
 * Check if a domain has access to a clock control. * Note: domain has access to clock control if it has access to all the nodes
 * the are driven by the target clock.
 */
bool domain_has_clock_access(struct domain *d, u32 clk_id,
                 const struct pm_access *pm_node_access,
                 const uint32_t pm_node_access_size,
                 const struct pm_clk2node *pm_clk_node_map,
                 const uint32_t table_size);

#endif /* __ASM_ARM_PLATFORMS_XILINX_EEMI_H */
