/*
 *  Copyright (C) 2015, Shannon Zhao <shannon.zhao@linaro.org>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; If not, see <http://www.gnu.org/licenses/>.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#ifndef _ASM_ARM_ACPI_H
#define _ASM_ARM_ACPI_H

#include <asm/setup.h>

#define COMPILER_DEPENDENT_INT64   long long
#define COMPILER_DEPENDENT_UINT64  unsigned long long
#define ACPI_MAP_MEM_ATTR          PAGE_HYPERVISOR

/* Tables marked as reserved in efi table */
typedef enum {
    TBL_FADT,
    TBL_MADT,
    TBL_STAO,
    TBL_XSDT,
    TBL_RSDP,
    TBL_EFIT,
    TBL_MMAP,
    TBL_MMAX,
} EFI_MEM_RES;

bool acpi_psci_present(void);
bool acpi_psci_hvc_present(void);
void acpi_smp_init_cpus(void);

/*
 * This function returns the offset of a given ACPI/EFI table in the allocated
 * memory region. Currently, the tables should be created in the same order as
 * their associated 'index' in the enum EFI_MEM_RES. This means the function
 * won't return the correct offset until all the tables before a given 'index'
 * are created.
 */
paddr_t acpi_get_table_offset(struct membank tbl_add[], EFI_MEM_RES index);

/* Macros for consistency checks of the GICC subtable of MADT */
#define ACPI_MADT_GICC_LENGTH	\
    (acpi_gbl_FADT.header.revision < 6 ? 76 : 80)

#define BAD_MADT_GICC_ENTRY(entry, end)						\
    (!(entry) || (unsigned long)(entry) + sizeof(*(entry)) > (end) ||	\
     (entry)->header.length != ACPI_MADT_GICC_LENGTH)

#ifdef CONFIG_ACPI
extern bool acpi_disabled;
/* Basic configuration for ACPI */
static inline void disable_acpi(void)
{
    acpi_disabled = true;
}

static inline void enable_acpi(void)
{
    acpi_disabled = false;
}
#else
#define acpi_disabled (true)
#define disable_acpi()
#define enable_acpi()
#endif

#endif /*_ASM_ARM_ACPI_H*/
