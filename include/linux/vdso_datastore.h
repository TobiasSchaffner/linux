/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_VDSO_DATASTORE_H
#define _LINUX_VDSO_DATASTORE_H

#include <linux/mm_types.h>

extern const struct vm_special_mapping vdso_vvar_mapping;
struct vm_area_struct *vdso_install_vvar_mapping(struct mm_struct *mm, unsigned long addr);

int vdso_install_private_mapping(unsigned long addr, unsigned long len);

#endif /* _LINUX_VDSO_DATASTORE_H */
