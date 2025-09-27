/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _EVL_ARM64_ASM_FPTEST_H
#define _EVL_ARM64_ASM_FPTEST_H

#include <linux/cpufeature.h>
#include <asm/neon.h>
#include <uapi/asm/evl/fptest.h>

static inline bool evl_begin_fpu(void)
{
	kernel_neon_begin();

	return true;
}

static inline void evl_end_fpu(void)
{
	kernel_neon_end();
}

static inline u32 evl_detect_fpu(void)
{
	u32 features = 0;

	if (system_supports_fpsimd())
		return features |= evl_arm64_fpsimd;

	if (system_supports_sve())
		return features |= evl_arm64_sve;

	return features;
}

#endif /* _EVL_ARM64_ASM_FPTEST_H */
