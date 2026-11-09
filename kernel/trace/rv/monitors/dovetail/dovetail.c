// SPDX-License-Identifier: GPL-2.0
/*
 * dovetail: container for the Dovetail monitors.
 *
 * Copyright (C) 2026 Siemens AG
 * Author: Tobias Schaffner <tobias.schaffner@siemens.com>
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/rv.h>

#define MODULE_NAME "dovetail"

#include "dovetail.h"

struct rv_monitor rv_dovetail = {
	.name = "dovetail",
	.description = "container for the Dovetail monitors",
	.enable = NULL,
	.disable = NULL,
	.reset = NULL,
	.enabled = 0,
};

static int __init register_dovetail(void)
{
	return rv_register_monitor(&rv_dovetail, NULL);
}

static void __exit unregister_dovetail(void)
{
	rv_unregister_monitor(&rv_dovetail);
}

module_init(register_dovetail);
module_exit(unregister_dovetail);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tobias Schaffner <tobias.schaffner@siemens.com>");
MODULE_DESCRIPTION("dovetail: container for the Dovetail monitors");
