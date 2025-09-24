/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mali G78AE GPU PTM Device Tree bindings helpers
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */
#ifndef DRIVERS__GPU_MALI_G78AE_DT_H
#define DRIVERS__GPU_MALI_G78AE_DT_H

#define MALI_GPU_PTM_DT_NAME "arm,mali-ptm"
#define MALI_GPU_AW_PTM_DT_NAME "arm,mali-gpu-aw-message"
#define MALI_GPU_KBASE_PTM_DT_NAME "arm,mali-midgard"
#define MALI_GPU_PT_CFG_PTM_DT_NAME "arm,mali-gpu-partition-config"
#define MALI_GPU_PT_CTRL_PTM_DT_NAME "arm,mali-gpu-partition-control"
#define MALI_GPU_RG_PTM_DT_NAME "arm,mali-gpu-resource-group"
#define MALI_GPU_ASSIGN_PTM_DT_NAME "arm,mali-gpu-assign"
#define MALI_GPU_SYSTEM_PTM_DT_NAME "arm,mali-gpu-system"
#define MALI_GPU_IF_PTM_DT_NAME "arm,mali-ptm-interface"


#define MALI_NODE_PTM_INTERFACE_SIZE        (0x300000)
#define MALI_NODE_PTM_CONFIG_SIZE           (0x10000)
#define MALI_NODE_PTM_CONTROL_SIZE          (0x10000)
#define MALI_NODE_PTM_RESOURCE_GROUP_SIZE   (0x10000)
#define MALI_NODE_PTM_AW_SIZE               (0x40)
#define MALI_NODE_PTM_GPU_SIZE              (0x1ffc0)
#define MALI_NODE_PTM_GPU_AW_SIZE \
    (MALI_NODE_PTM_AW_SIZE + MALI_NODE_PTM_GPU_SIZE)

#define MALI_NODE_PTM_RESOURCE_GROUP_START  (0xa0000)
#define MALI_NODE_PTM_CONFIG_START          (0x20000)

#define MALI_PARTITION_COUNT_MAX        (4)
#define MALI_RESOURCE_GROUP_COUNT_MAX        (4)

/* Control and config regions are interleaved */
#define MALI_NODE_PTM_CONTROL_START \
    (MALI_NODE_PTM_CONFIG_START + MALI_NODE_PTM_CONFIG_SIZE)

#define MALI_NODE_PTM_RESOURCE_GROUP_ADDR(ID)    \
    (MALI_NODE_PTM_RESOURCE_GROUP_START +       \
        ((ID) * MALI_NODE_PTM_RESOURCE_GROUP_SIZE))

#define MALI_NODE_PTM_RESOURCE_GROUP_ID_SAFE(ADDR)                   \
    ((((ADDR) < MALI_NODE_PTM_RESOURCE_GROUP_START ||                \
       (ADDR) >= (MALI_NODE_PTM_RESOURCE_GROUP_START +               \
                  (MALI_RESOURCE_GROUP_COUNT_MAX *                   \
                    MALI_NODE_PTM_RESOURCE_GROUP_SIZE))) ||          \
      (((ADDR) - MALI_NODE_PTM_RESOURCE_GROUP_START) %               \
       MALI_NODE_PTM_RESOURCE_GROUP_SIZE != 0))                      \
         ? -1                                                        \
         : (((ADDR) - MALI_NODE_PTM_RESOURCE_GROUP_START) /          \
            MALI_NODE_PTM_RESOURCE_GROUP_SIZE))


#define MALI_NODE_PTM_CONFIG_ADDR(ID)           \
    (MALI_NODE_PTM_CONFIG_START +               \
        ((ID) * (MALI_NODE_PTM_CONFIG_SIZE +    \
                 MALI_NODE_PTM_CONTROL_SIZE)))

#define MALI_NODE_PTM_CONFIG_ID_SAFE(ADDR)                             \
    ((((ADDR) < MALI_NODE_PTM_CONFIG_START ||                          \
       (ADDR) >= (MALI_NODE_PTM_CONFIG_START +                         \
                  (MALI_PARTITION_COUNT_MAX * (MALI_NODE_PTM_CONFIG_SIZE +\
                        MALI_NODE_PTM_CONTROL_SIZE)))) ||              \
      (((ADDR) - MALI_NODE_PTM_CONFIG_START) %                         \
       (MALI_NODE_PTM_CONFIG_SIZE + MALI_NODE_PTM_CONTROL_SIZE) != 0))  \
         ? -1                                                           \
         : (((ADDR) - MALI_NODE_PTM_CONFIG_START) /                    \
            (MALI_NODE_PTM_CONFIG_SIZE + MALI_NODE_PTM_CONTROL_SIZE)))

#define MALI_NODE_PTM_CONTROL_ADDR(ID)          \
    (MALI_NODE_PTM_CONTROL_START +              \
        ((ID) * (MALI_NODE_PTM_CONFIG_SIZE +    \
                 MALI_NODE_PTM_CONTROL_SIZE)))

#define MALI_NODE_PTM_CONTROL_ID_SAFE(ADDR)                             \
    ((((ADDR) < MALI_NODE_PTM_CONTROL_START ||                          \
       (ADDR) >= (MALI_NODE_PTM_CONTROL_START +                         \
                  (MALI_PARTITION_COUNT_MAX * (MALI_NODE_PTM_CONFIG_SIZE +\
                        MALI_NODE_PTM_CONTROL_SIZE)))) ||               \
      (((ADDR) - MALI_NODE_PTM_CONTROL_START) %                         \
       (MALI_NODE_PTM_CONFIG_SIZE + MALI_NODE_PTM_CONTROL_SIZE) != 0))  \
         ? -1                                                           \
         : (((ADDR) - MALI_NODE_PTM_CONTROL_START) /                    \
            (MALI_NODE_PTM_CONFIG_SIZE + MALI_NODE_PTM_CONTROL_SIZE)))

#define MALI_NODE_PTM_GPU_ADDR(ID)              \
    (MALI_NODE_PTM_GPU_START +                  \
        ((ID) * MALI_NODE_PTM_GPU_AW_SIZE))

#define MALI_NODE_PTM_AW_ADDR(ID)               \
    (MALI_NODE_PTM_AW_START +                   \
        ((ID) * MALI_NODE_PTM_GPU_AW_SIZE))

#endif /* DRIVERS__GPU_MALI_G78AE_DT_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */