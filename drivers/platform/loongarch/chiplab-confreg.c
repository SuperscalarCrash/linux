// SPDX-License-Identifier: GPL-2.0-only
/*
 * Loongson Chiplab CONFREG board peripheral driver
 *
 * Copyright (C) 2026 SuperscalarCrash
 */

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define CHIPLAB_LED_REG			0x00
#define CHIPLAB_BICOLOR_LED0_REG	0x04
#define CHIPLAB_BICOLOR_LED1_REG	0x08
#define CHIPLAB_SEVEN_SEGMENT_REG	0x10
#define CHIPLAB_SWITCH_REG		0x20

#define CHIPLAB_MONO_LED_COUNT		16
#define CHIPLAB_BICOLOR_LED_COUNT	2
#define CHIPLAB_BICOLOR_CHANNEL_COUNT	2
#define CHIPLAB_LED_COUNT		(CHIPLAB_MONO_LED_COUNT +	\
					 CHIPLAB_BICOLOR_LED_COUNT *	\
					 CHIPLAB_BICOLOR_CHANNEL_COUNT)
#define CHIPLAB_SWITCH_COUNT		8

struct chiplab_confreg;

struct chiplab_confreg_led {
	struct led_classdev cdev;
	struct chiplab_confreg *confreg;
	u32 reg;
	u8 bit;
};

struct chiplab_confreg {
	void __iomem *base;
	/* Protect read-modify-write accesses to shared output registers. */
	spinlock_t lock;
	struct gpio_chip switch_gpio;
	struct chiplab_confreg_led leds[CHIPLAB_LED_COUNT];
};

static u32 chiplab_confreg_read(struct chiplab_confreg *confreg, u32 reg)
{
	unsigned long flags;
	u32 value;

	spin_lock_irqsave(&confreg->lock, flags);
	value = readl(confreg->base + reg);
	spin_unlock_irqrestore(&confreg->lock, flags);

	return value;
}

static void chiplab_confreg_write(struct chiplab_confreg *confreg, u32 reg,
				  u32 value)
{
	unsigned long flags;

	spin_lock_irqsave(&confreg->lock, flags);
	writel(value, confreg->base + reg);
	spin_unlock_irqrestore(&confreg->lock, flags);
}

static void chiplab_confreg_update_bits(struct chiplab_confreg *confreg,
					u32 reg, u32 mask, u32 value)
{
	unsigned long flags;
	u32 old_value;

	spin_lock_irqsave(&confreg->lock, flags);
	old_value = readl(confreg->base + reg);
	writel((old_value & ~mask) | (value & mask), confreg->base + reg);
	spin_unlock_irqrestore(&confreg->lock, flags);
}

static void chiplab_led_set(struct led_classdev *cdev,
			    enum led_brightness brightness)
{
	struct chiplab_confreg_led *led =
		container_of(cdev, struct chiplab_confreg_led, cdev);

	chiplab_confreg_update_bits(led->confreg, led->reg, BIT(led->bit),
				    brightness ? BIT(led->bit) : 0);
}

static enum led_brightness chiplab_led_get(struct led_classdev *cdev)
{
	struct chiplab_confreg_led *led =
		container_of(cdev, struct chiplab_confreg_led, cdev);

	return chiplab_confreg_read(led->confreg, led->reg) & BIT(led->bit) ?
	       LED_ON : LED_OFF;
}

static int chiplab_register_led(struct device *dev,
				struct chiplab_confreg *confreg,
				unsigned int index, const char *name,
				unsigned int color, u32 reg, u8 bit)
{
	struct chiplab_confreg_led *led = &confreg->leds[index];

	led->confreg = confreg;
	led->reg = reg;
	led->bit = bit;
	led->cdev.name = name;
	led->cdev.color = color;
	led->cdev.max_brightness = LED_ON;
	led->cdev.brightness_set = chiplab_led_set;
	led->cdev.brightness_get = chiplab_led_get;
	led->cdev.brightness = chiplab_led_get(&led->cdev);

	return devm_led_classdev_register(dev, &led->cdev);
}

static int chiplab_register_leds(struct device *dev,
				 struct chiplab_confreg *confreg)
{
	/* Existing Chiplab functional tests encode green as 1 and red as 2. */
	static const unsigned int bicolor_colors[] = {
		LED_COLOR_ID_GREEN,
		LED_COLOR_ID_RED,
	};
	static const char * const bicolor_color_names[] = {
		"green",
		"red",
	};
	unsigned int led_index = 0;
	unsigned int channel;
	unsigned int i;
	const char *name;
	int ret;

	for (i = 0; i < CHIPLAB_MONO_LED_COUNT; i++, led_index++) {
		name = devm_kasprintf(dev, GFP_KERNEL, "chiplab:red:mono-%u", i);
		if (!name)
			return -ENOMEM;

		ret = chiplab_register_led(dev, confreg, led_index, name,
					   LED_COLOR_ID_RED, CHIPLAB_LED_REG, i);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to register LED %u\n", i);
	}

	for (i = 0; i < CHIPLAB_BICOLOR_LED_COUNT; i++) {
		for (channel = 0;
		     channel < CHIPLAB_BICOLOR_CHANNEL_COUNT;
		     channel++, led_index++) {
			name = devm_kasprintf(dev, GFP_KERNEL,
					      "chiplab:%s:bicolor-%u",
					     bicolor_color_names[channel], i);
			if (!name)
				return -ENOMEM;

			ret = chiplab_register_led(dev, confreg, led_index,
						   name,
						   bicolor_colors[channel],
						   i ? CHIPLAB_BICOLOR_LED1_REG :
						       CHIPLAB_BICOLOR_LED0_REG,
						   channel);
			if (ret)
				return dev_err_probe(dev, ret,
					"failed to register bicolor LED %u channel %u\n",
					i, channel);
		}
	}

	return 0;
}

static int chiplab_switch_get_direction(struct gpio_chip *chip,
					unsigned int offset)
{
	return GPIO_LINE_DIRECTION_IN;
}

static int chiplab_switch_direction_input(struct gpio_chip *chip,
					  unsigned int offset)
{
	return 0;
}

static int chiplab_switch_get(struct gpio_chip *chip, unsigned int offset)
{
	struct chiplab_confreg *confreg = gpiochip_get_data(chip);

	return !!(chiplab_confreg_read(confreg, CHIPLAB_SWITCH_REG) &
		  BIT(offset));
}

static int chiplab_register_switches(struct device *dev,
				     struct chiplab_confreg *confreg)
{
	struct gpio_chip *chip = &confreg->switch_gpio;

	chip->label = "chiplab-switches";
	chip->parent = dev;
	chip->owner = THIS_MODULE;
	chip->base = -1;
	chip->ngpio = CHIPLAB_SWITCH_COUNT;
	chip->can_sleep = false;
	chip->get_direction = chiplab_switch_get_direction;
	chip->direction_input = chiplab_switch_direction_input;
	chip->get = chiplab_switch_get;

	return devm_gpiochip_add_data(dev, chip, confreg);
}

static ssize_t leds_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct chiplab_confreg *confreg = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%04x\n",
			  (u32)(chiplab_confreg_read(confreg,
						    CHIPLAB_LED_REG) &
				GENMASK(15, 0)));
}

static ssize_t leds_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct chiplab_confreg *confreg = dev_get_drvdata(dev);
	u32 value;
	int ret;

	ret = kstrtou32(buf, 16, &value);
	if (ret)
		return ret;
	if (value & ~GENMASK(15, 0))
		return -ERANGE;

	chiplab_confreg_update_bits(confreg, CHIPLAB_LED_REG, GENMASK(15, 0),
				    value);

	return count;
}
static DEVICE_ATTR_RW(leds);

static ssize_t chiplab_bicolor_show(struct device *dev, char *buf, u32 reg)
{
	struct chiplab_confreg *confreg = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%x\n",
			  (u32)(chiplab_confreg_read(confreg, reg) &
				GENMASK(1, 0)));
}

static ssize_t chiplab_bicolor_store(struct device *dev, const char *buf,
				     size_t count, u32 reg)
{
	struct chiplab_confreg *confreg = dev_get_drvdata(dev);
	u32 value;
	int ret;

	ret = kstrtou32(buf, 16, &value);
	if (ret)
		return ret;
	if (value & ~GENMASK(1, 0))
		return -ERANGE;

	chiplab_confreg_update_bits(confreg, reg, GENMASK(1, 0), value);

	return count;
}

static ssize_t bicolor_led0_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return chiplab_bicolor_show(dev, buf, CHIPLAB_BICOLOR_LED0_REG);
}

static ssize_t bicolor_led0_store(struct device *dev,
				  struct device_attribute *attr,
				   const char *buf, size_t count)
{
	return chiplab_bicolor_store(dev, buf, count,
				      CHIPLAB_BICOLOR_LED0_REG);
}
static DEVICE_ATTR_RW(bicolor_led0);

static ssize_t bicolor_led1_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return chiplab_bicolor_show(dev, buf, CHIPLAB_BICOLOR_LED1_REG);
}

static ssize_t bicolor_led1_store(struct device *dev,
				  struct device_attribute *attr,
				   const char *buf, size_t count)
{
	return chiplab_bicolor_store(dev, buf, count,
				      CHIPLAB_BICOLOR_LED1_REG);
}
static DEVICE_ATTR_RW(bicolor_led1);

static ssize_t seven_segment_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct chiplab_confreg *confreg = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%08x\n",
			  chiplab_confreg_read(confreg,
					       CHIPLAB_SEVEN_SEGMENT_REG));
}

static ssize_t seven_segment_store(struct device *dev,
				   struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct chiplab_confreg *confreg = dev_get_drvdata(dev);
	u32 value;
	int ret;

	ret = kstrtou32(buf, 16, &value);
	if (ret)
		return ret;

	chiplab_confreg_write(confreg, CHIPLAB_SEVEN_SEGMENT_REG, value);

	return count;
}
static DEVICE_ATTR_RW(seven_segment);

static ssize_t switches_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct chiplab_confreg *confreg = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%02x\n",
			  (u32)(chiplab_confreg_read(confreg,
						    CHIPLAB_SWITCH_REG) &
				GENMASK(7, 0)));
}
static DEVICE_ATTR_RO(switches);

static struct attribute *chiplab_confreg_attrs[] = {
	&dev_attr_leds.attr,
	&dev_attr_bicolor_led0.attr,
	&dev_attr_bicolor_led1.attr,
	&dev_attr_seven_segment.attr,
	&dev_attr_switches.attr,
	NULL,
};

static const struct attribute_group chiplab_confreg_group = {
	.name = "registers",
	.attrs = chiplab_confreg_attrs,
};

static int chiplab_confreg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct chiplab_confreg *confreg;
	int ret;

	confreg = devm_kzalloc(dev, sizeof(*confreg), GFP_KERNEL);
	if (!confreg)
		return -ENOMEM;

	confreg->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(confreg->base))
		return PTR_ERR(confreg->base);

	spin_lock_init(&confreg->lock);
	platform_set_drvdata(pdev, confreg);

	ret = chiplab_register_leds(dev, confreg);
	if (ret)
		return ret;

	ret = chiplab_register_switches(dev, confreg);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register switch GPIOs\n");

	ret = devm_device_add_group(dev, &chiplab_confreg_group);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to create sysfs attributes\n");

	dev_info(dev,
		 "registered 16 LEDs, 2 bicolor LEDs, 8-digit display and 8 switches\n");

	return 0;
}

static const struct of_device_id chiplab_confreg_of_match[] = {
	{ .compatible = "loongson,chiplab-confreg" },
	{ }
};
MODULE_DEVICE_TABLE(of, chiplab_confreg_of_match);

static struct platform_driver chiplab_confreg_driver = {
	.probe = chiplab_confreg_probe,
	.driver = {
		.name = "chiplab-confreg",
		.of_match_table = chiplab_confreg_of_match,
	},
};
module_platform_driver(chiplab_confreg_driver);

MODULE_DESCRIPTION("Loongson Chiplab CONFREG peripheral driver");
MODULE_LICENSE("GPL");
