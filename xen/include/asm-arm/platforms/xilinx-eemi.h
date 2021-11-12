#ifndef __ASM_ARM_PLATFORMS_XILINX_EEMI_H
#define __ASM_ARM_PLATFORMS_XILINX_EEMI_H

#include <asm/platforms/xilinx-versal-eemi.h>
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
    /* PLL control API functions */
    PM_PLL_SET_PARAMETER = 48,
    PM_PLL_GET_PARAMETER,
    PM_PLL_SET_MODE,
    PM_PLL_GET_MODE,
    /* PM Feature Check */
    PM_FEATURE_CHECK = 63,
    PM_GET_TRUSTZONE_VERSION = 2563,
    PM_API_MAX
};

enum pm_ioctl_id {
    IOCTL_GET_PLL_FRAC_MODE = 9,
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
    uint32_t addr;
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
