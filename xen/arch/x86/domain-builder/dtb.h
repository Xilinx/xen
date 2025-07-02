/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef X86_DTB_PRIVATE_H
#define X86_DTB_PRIVATE_H

struct boot_info;

/*
 * Parse module 0 and, if determined to be a DTB, identify the kinds of all
 * modules from it.
 */
void fdt_identify_module_kinds(struct boot_info *bi);

/*
 * Extract all information about to-be-constructed domains from the DTB
 * in the first module.
 */
void dt_parse_domains(struct boot_info *bi);

#endif /* X86_DTB_PRIVATE_H */
