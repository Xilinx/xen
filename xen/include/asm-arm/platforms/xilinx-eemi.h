#ifndef __ASM_ARM_PLATFORMS_XILINX_EEMI_H
#define __ASM_ARM_PLATFORMS_XILINX_EEMI_H

/* Generic PM defs:  */
/*
 * Version number is a 32bit value, like:
 * (PM_VERSION_MAJOR << 16) | PM_VERSION_MINOR
 */
#define PM_VERSION_MAJOR	1
#define PM_VERSION_MINOR	0

#define PM_VERSION	((PM_VERSION_MAJOR << 16) | PM_VERSION_MINOR)

#define PM_GET_TRUSTZONE_VERSION	0xa03

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
    PM_API_MAX
};

/**
 * @PM_RET_SUCCESS:             success
 * @PM_RET_ERROR_ARGS:          illegal arguments provided (deprecated)
 * @PM_RET_ERROR_NOTSUPPORTED:  feature not supported  (deprecated)
 * @PM_RET_INVALID_PARAM:       invalid argument
 * @PM_RET_ERROR_INTERNAL:      internal error
 * @PM_RET_ERROR_CONFLICT:      conflict
 * @PM_RET_ERROR_ACCESS:        access rights violation
 * @PM_RET_ERROR_INVALID_NODE:  invalid node
 * @PM_RET_ERROR_DOUBLE_REQ:    duplicate request for same node
 * @PM_RET_ERROR_ABORT_SUSPEND: suspend procedure has been aborted
 * @PM_RET_ERROR_TIMEOUT:       timeout in communication with PMU
 * @PM_RET_ERROR_NODE_USED:     node is already in use
 */
enum pm_ret_status {
    PM_RET_SUCCESS,
    PM_RET_ERROR_ARGS = 1,
    PM_RET_ERROR_NOTSUPPORTED = 4,
    PM_RET_INVALID_PARAM = 15,
    PM_RET_ERROR_INTERNAL = 2000,
    PM_RET_ERROR_CONFLICT = 2001,
    PM_RET_ERROR_ACCESS = 2002,
    PM_RET_ERROR_INVALID_NODE = 2003,
    PM_RET_ERROR_DOUBLE_REQ = 2004,
    PM_RET_ERROR_ABORT_SUSPEND = 2005,
    PM_RET_ERROR_TIMEOUT = 2006,
    PM_RET_ERROR_NODE_USED = 2007
};

#endif /* __ASM_ARM_PLATFORMS_XILINX_EEMI_H */
