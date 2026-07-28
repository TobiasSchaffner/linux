// SPDX-License-Identifier: GPL-2.0
/*
 * Raw console device for RISC-V using SBI debug console (DBCN).
 *
 * Copyright (C) 2026 Siemens AG
 * Author:       Tobias Schaffner <tobias.schaffner@siemens.com>.
 */

#include <linux/kernel.h>
#include <linux/console.h>
#include <linux/init.h>
#include <asm/sbi.h>

static void raw_console_write(struct console *co,
			      const char *s, unsigned int count)
{
	sbi_debug_console_write(s, count);
}

static struct console raw_console = {
	.name		= "rawcon",
	.write_raw	= raw_console_write,
	.flags		= CON_PRINTBUFFER | CON_ENABLED,
	.index		= -1,
};

static int __init raw_console_init(void)
{
	register_console(&raw_console);

	return 0;
}
console_initcall(raw_console_init);
