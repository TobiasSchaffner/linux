/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __ASM_GENERIC_VDSO_PRIVATE_H
#define __ASM_GENERIC_VDSO_PRIVATE_H

#ifdef CONFIG_VDSO_PRIVATE_DATA
#define __VDSO_PRIV_PAGES	1
#else
#define __VDSO_PRIV_PAGES	0
#endif

#endif /* __ASM_GENERIC_VDSO_PRIVATE_H */
