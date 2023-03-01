/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Pinctrl / GPIO driver for StarFive JH7110 SoC
 *
 * Copyright (C) 2022 StarFive Technology Co., Ltd.
 */

#ifndef __DRIVERS_PINCTRL_STARFIVE_H__
#define __DRIVERS_PINCTRL_STARFIVE_H__

#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinmux.h>

struct starfive_pinctrl {
	struct device *dev;
	struct gpio_chip gc;
	struct pinctrl_gpio_range gpios;
	raw_spinlock_t lock;
	void __iomem *base;
	struct pinctrl_dev *pctl;
	/* register read/write mutex */
	struct mutex mutex;
	const struct starfive_pinctrl_soc_info *info;
};

struct starfive_gpio_irq_reg {
	unsigned int is_reg_base;
	unsigned int ic_reg_base;
	unsigned int ibe_reg_base;
	unsigned int iev_reg_base;
	unsigned int ie_reg_base;
	unsigned int ris_reg_base;
	unsigned int mis_reg_base;
};

struct starfive_pinctrl_soc_info {
	const struct pinctrl_pin_desc *pins;
	unsigned int npins;
	unsigned int ngpios;
	unsigned int gc_base;
	unsigned int flags;

	/* gpio dout/doen/din/gpioinput register */
	unsigned int dout_reg_base;
	unsigned int dout_mask;
	unsigned int doen_reg_base;
	unsigned int doen_mask;
	unsigned int gpi_reg_base;
	unsigned int gpi_mask;
	unsigned int gpioin_reg_base;

	const struct starfive_gpio_irq_reg *irq_reg;

	/* generic pinmux */
	int (*starfive_set_one_pin_mux)(struct starfive_pinctrl *sfp,
					unsigned int pin,
					unsigned int din, u32 dout,
					u32 doen, u32 func);
	/* gpio chip */
	int (*starfive_get_padcfg_base)(struct starfive_pinctrl *sfp,
					unsigned int pin);
	void (*starfive_gpio_irq_handler)(struct irq_desc *desc);
	int (*starfive_gpio_init_hw)(struct gpio_chip *gc);
};

void starfive_set_gpiomux(struct starfive_pinctrl *sfp, unsigned int pin,
			  unsigned int din, u32 dout, u32 doen);
int starfive_pinctrl_probe(struct platform_device *pdev,
			   const struct starfive_pinctrl_soc_info *info);
struct starfive_pinctrl *starfive_from_irq_desc(struct irq_desc *desc);

#endif /* __DRIVERS_PINCTRL_STARFIVE_H__ */
