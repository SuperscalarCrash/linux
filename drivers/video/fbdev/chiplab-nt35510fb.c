// SPDX-License-Identifier: GPL-2.0-only
/*
 * Framebuffer driver for the NT35510 LCD on the Chiplab NSCSCC board.
 *
 * The panel initialization sequence is derived from the NT35510 character
 * driver published by Tsinghua University:
 * https://github.com/xht03/vmlinux/blob/b5a965108b672f8637f43600814314268090c294/drivers/char/nt35510.c
 *
 * Copyright (C) 2016-2019 Tsinghua University
 * Copyright (C) 2026 SuperscalarCrash contributors
 */

#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/vmalloc.h>

#define NT35510_REG_COMMAND	0x00
#define NT35510_REG_DATA	0x04
#define NT35510_REG_CONTROL	0x08
#define NT35510_REG_STATUS	0x0c

#define NT35510_CONTROL_RESET_N	BIT(0)
#define NT35510_CONTROL_BACKLIGHT BIT(1)
#define NT35510_STATUS_SIGNATURE	0x4c43

#define NT35510_DEFAULT_XRES	480
#define NT35510_DEFAULT_YRES	800
#define NT35510_BYTES_PER_PIXEL	2

struct nt35510_init_pair {
	u16 command;
	u16 data;
};

struct chiplab_nt35510 {
	struct fb_info *info;
	void __iomem *regs;
	struct mutex lock; /* protects panel commands and framebuffer updates */
	struct fb_deferred_io defio;
	u32 pseudo_palette[16];
	u32 width;
	u32 height;
	bool enabled;
};

static const struct nt35510_init_pair nt35510_power_init[] = {
	{ 0xf000, 0x55 }, { 0xf001, 0xaa }, { 0xf002, 0x52 },
	{ 0xf003, 0x08 }, { 0xf004, 0x01 },
	/* AVDD 5.2 V and ratio. */
	{ 0xb000, 0x0d }, { 0xb001, 0x0d }, { 0xb002, 0x0d },
	{ 0xb600, 0x34 }, { 0xb601, 0x34 }, { 0xb602, 0x34 },
	/* AVEE -5.2 V and ratio. */
	{ 0xb100, 0x0d }, { 0xb101, 0x0d }, { 0xb102, 0x0d },
	{ 0xb700, 0x34 }, { 0xb701, 0x34 }, { 0xb702, 0x34 },
	/* VCL -2.5 V and ratio. */
	{ 0xb200, 0x00 }, { 0xb201, 0x00 }, { 0xb202, 0x00 },
	{ 0xb800, 0x24 }, { 0xb801, 0x24 }, { 0xb802, 0x24 },
	/* VGH 15 V (free pump) and ratio. */
	{ 0xbf00, 0x01 },
	{ 0xb300, 0x0f }, { 0xb301, 0x0f }, { 0xb302, 0x0f },
	{ 0xb900, 0x34 }, { 0xb901, 0x34 }, { 0xb902, 0x34 },
	/* VGL -10 V and ratio. */
	{ 0xb500, 0x08 }, { 0xb501, 0x08 }, { 0xb502, 0x08 },
	{ 0xc200, 0x03 },
	{ 0xba00, 0x24 }, { 0xba01, 0x24 }, { 0xba02, 0x24 },
	/* Positive/negative gamma reference and VCOM. */
	{ 0xbc00, 0x00 }, { 0xbc01, 0x78 }, { 0xbc02, 0x00 },
	{ 0xbd00, 0x00 }, { 0xbd01, 0x78 }, { 0xbd02, 0x00 },
	{ 0xbe00, 0x00 }, { 0xbe01, 0x64 },
};

static const u8 nt35510_gamma[] = {
	0x00, 0x33, 0x00, 0x34, 0x00, 0x3a, 0x00, 0x4a,
	0x00, 0x5c, 0x00, 0x81, 0x00, 0xa6, 0x00, 0xe5,
	0x01, 0x13, 0x01, 0x54, 0x01, 0x82, 0x01, 0xca,
	0x02, 0x00, 0x02, 0x01, 0x02, 0x34, 0x02, 0x67,
	0x02, 0x84, 0x02, 0xa4, 0x02, 0xb7, 0x02, 0xcf,
	0x02, 0xde, 0x02, 0xf2, 0x02, 0xfe, 0x03, 0x10,
	0x03, 0x33, 0x03, 0x6d,
};

static const struct nt35510_init_pair nt35510_display_init[] = {
	/* Return to command page 0. */
	{ 0xf000, 0x55 }, { 0xf001, 0xaa }, { 0xf002, 0x52 },
	{ 0xf003, 0x08 }, { 0xf004, 0x00 },
	{ 0xb100, 0xcc }, { 0xb101, 0x00 },
	{ 0xb600, 0x05 },
	{ 0xb700, 0x70 }, { 0xb701, 0x70 },
	{ 0xb800, 0x01 }, { 0xb801, 0x03 },
	{ 0xb802, 0x03 }, { 0xb803, 0x03 },
	{ 0xbc00, 0x02 }, { 0xbc01, 0x00 }, { 0xbc02, 0x00 },
	{ 0xc900, 0xd0 }, { 0xc901, 0x02 }, { 0xc902, 0x50 },
	{ 0xc903, 0x50 }, { 0xc904, 0x50 },
	{ 0x3500, 0x00 },
	{ 0x3a00, 0x55 },	/* RGB565 */
};

static inline void nt35510_write_command(struct chiplab_nt35510 *lcd, u16 command)
{
	writel(command, lcd->regs + NT35510_REG_COMMAND);
}

static inline void nt35510_write_data(struct chiplab_nt35510 *lcd, u16 data)
{
	writel(data, lcd->regs + NT35510_REG_DATA);
}

static void nt35510_write_pairs(struct chiplab_nt35510 *lcd,
				const struct nt35510_init_pair *pairs,
				size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		nt35510_write_command(lcd, pairs[i].command);
		nt35510_write_data(lcd, pairs[i].data);
	}
}

static void nt35510_set_control(struct chiplab_nt35510 *lcd,
				bool reset_n, bool backlight)
{
	u32 value = reset_n ? NT35510_CONTROL_RESET_N : 0;

	if (backlight)
		value |= NT35510_CONTROL_BACKLIGHT;

	writel(value, lcd->regs + NT35510_REG_CONTROL);
	/* Flush the posted write. */
	readl(lcd->regs + NT35510_REG_STATUS);
}

static void nt35510_panel_init(struct chiplab_nt35510 *lcd)
{
	unsigned int bank;
	unsigned int i;

	nt35510_set_control(lcd, false, false);
	msleep(20);
	nt35510_set_control(lcd, true, false);
	msleep(120);

	nt35510_write_pairs(lcd, nt35510_power_init,
			    ARRAY_SIZE(nt35510_power_init));

	for (bank = 0; bank < 6; bank++) {
		u16 base = 0xd100 + bank * 0x100;

		for (i = 0; i < ARRAY_SIZE(nt35510_gamma); i++) {
			nt35510_write_command(lcd, base + i);
			nt35510_write_data(lcd, nt35510_gamma[i]);
		}
	}

	nt35510_write_pairs(lcd, nt35510_display_init,
			    ARRAY_SIZE(nt35510_display_init));

	nt35510_write_command(lcd, 0x1100);
	msleep(120);
	nt35510_write_command(lcd, 0x2900);
}

static void nt35510_set_window(struct chiplab_nt35510 *lcd,
			       u16 x, u16 y, u16 width, u16 height)
{
	u16 x_end = x + width - 1;
	u16 y_end = y + height - 1;

	nt35510_write_command(lcd, 0x2a00);
	nt35510_write_data(lcd, x >> 8);
	nt35510_write_command(lcd, 0x2a01);
	nt35510_write_data(lcd, x & 0xff);
	nt35510_write_command(lcd, 0x2a02);
	nt35510_write_data(lcd, x_end >> 8);
	nt35510_write_command(lcd, 0x2a03);
	nt35510_write_data(lcd, x_end & 0xff);

	nt35510_write_command(lcd, 0x2b00);
	nt35510_write_data(lcd, y >> 8);
	nt35510_write_command(lcd, 0x2b01);
	nt35510_write_data(lcd, y & 0xff);
	nt35510_write_command(lcd, 0x2b02);
	nt35510_write_data(lcd, y_end >> 8);
	nt35510_write_command(lcd, 0x2b03);
	nt35510_write_data(lcd, y_end & 0xff);

	nt35510_write_command(lcd, 0x2c00);
}

static void nt35510_update_rect_locked(struct chiplab_nt35510 *lcd,
				       u32 x, u32 y, u32 width, u32 height)
{
	const u16 *pixels = (const u16 *)lcd->info->screen_buffer;
	u32 row;
	u32 col;

	if (!width || !height || x >= lcd->width || y >= lcd->height)
		return;

	width = min(width, lcd->width - x);
	height = min(height, lcd->height - y);

	nt35510_set_window(lcd, x, y, width, height);
	for (row = y; row < y + height; row++) {
		const u16 *line = pixels + row * lcd->width + x;

		for (col = 0; col < width; col++)
			nt35510_write_data(lcd, line[col]);
	}
}

static void nt35510_update_rect(struct chiplab_nt35510 *lcd,
				u32 x, u32 y, u32 width, u32 height)
{
	mutex_lock(&lcd->lock);
	if (lcd->enabled)
		nt35510_update_rect_locked(lcd, x, y, width, height);
	mutex_unlock(&lcd->lock);
}

static void nt35510_damage_range(struct fb_info *info, off_t off, size_t len)
{
	struct chiplab_nt35510 *lcd = info->par;
	u32 first_row;
	u32 last_row;

	if (!len || off < 0 || off >= info->fix.smem_len)
		return;

	first_row = off / info->fix.line_length;
	last_row = DIV_ROUND_UP(min_t(size_t, off + len, info->fix.smem_len),
				info->fix.line_length);
	nt35510_update_rect(lcd, 0, first_row, lcd->width,
			    last_row - first_row);
}

static void nt35510_damage_area(struct fb_info *info, u32 x, u32 y,
				u32 width, u32 height)
{
	nt35510_update_rect(info->par, x, y, width, height);
}

FB_GEN_DEFAULT_DEFERRED_SYSMEM_OPS(nt35510,
				   nt35510_damage_range,
				   nt35510_damage_area)

static void nt35510_deferred_io(struct fb_info *info,
				struct list_head *pagereflist)
{
	struct chiplab_nt35510 *lcd = info->par;
	struct fb_deferred_io_pageref *pageref;
	unsigned long first = info->fix.smem_len;
	unsigned long last = 0;
	u32 first_row;
	u32 last_row;

	list_for_each_entry(pageref, pagereflist, list) {
		first = min(first, pageref->offset);
		last = max(last, min(pageref->offset + PAGE_SIZE,
				     info->fix.smem_len));
	}

	if (first >= last)
		return;

	first_row = first / info->fix.line_length;
	last_row = DIV_ROUND_UP(last, info->fix.line_length);
	nt35510_update_rect(lcd, 0, first_row, lcd->width,
			    last_row - first_row);
}

static int nt35510_setcolreg(unsigned int regno, unsigned int red,
			     unsigned int green, unsigned int blue,
			     unsigned int transp, struct fb_info *info)
{
	u32 value;

	if (regno >= 16)
		return -EINVAL;

	red >>= 16 - info->var.red.length;
	green >>= 16 - info->var.green.length;
	blue >>= 16 - info->var.blue.length;
	if (info->var.transp.length)
		transp >>= 16 - info->var.transp.length;
	else
		transp = 0;

	value = (red << info->var.red.offset) |
		(green << info->var.green.offset) |
		(blue << info->var.blue.offset) |
		(transp << info->var.transp.offset);
	((u32 *)info->pseudo_palette)[regno] = value;

	return 0;
}

static int nt35510_check_var(struct fb_var_screeninfo *var,
			     struct fb_info *info)
{
	struct chiplab_nt35510 *lcd = info->par;

	if (var->xres != lcd->width || var->yres != lcd->height ||
	    var->xres_virtual != lcd->width ||
	    var->yres_virtual != lcd->height ||
	    var->bits_per_pixel != 16)
		return -EINVAL;

	return 0;
}

static int nt35510_blank(int blank, struct fb_info *info)
{
	struct chiplab_nt35510 *lcd = info->par;

	mutex_lock(&lcd->lock);
	if (blank == FB_BLANK_UNBLANK) {
		nt35510_write_command(lcd, 0x2900);
		lcd->enabled = true;
		nt35510_update_rect_locked(lcd, 0, 0, lcd->width, lcd->height);
		nt35510_set_control(lcd, true, true);
	} else {
		lcd->enabled = false;
		nt35510_set_control(lcd, true, false);
		nt35510_write_command(lcd, 0x2800);
	}
	mutex_unlock(&lcd->lock);

	return 0;
}

static const struct fb_ops nt35510_fb_ops = {
	.owner		= THIS_MODULE,
	FB_DEFAULT_DEFERRED_OPS(nt35510),
	.fb_setcolreg	= nt35510_setcolreg,
	.fb_check_var	= nt35510_check_var,
	.fb_blank	= nt35510_blank,
};

static int chiplab_nt35510_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct chiplab_nt35510 *lcd;
	struct fb_info *info;
	size_t vmem_size;
	void *vmem;
	u32 status;
	int ret;

	info = framebuffer_alloc(sizeof(*lcd), dev);
	if (!info)
		return -ENOMEM;

	lcd = info->par;
	lcd->info = info;
	lcd->width = NT35510_DEFAULT_XRES;
	lcd->height = NT35510_DEFAULT_YRES;
	mutex_init(&lcd->lock);

	device_property_read_u32(dev, "width", &lcd->width);
	device_property_read_u32(dev, "height", &lcd->height);
	if (!lcd->width || !lcd->height ||
	    lcd->width > U16_MAX || lcd->height > U16_MAX) {
		ret = -EINVAL;
		goto err_release;
	}

	lcd->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(lcd->regs)) {
		ret = PTR_ERR(lcd->regs);
		goto err_release;
	}

	status = readl(lcd->regs + NT35510_REG_STATUS);
	if (status >> 16 != NT35510_STATUS_SIGNATURE) {
		dev_err(dev, "unexpected LCD controller signature %#08x\n",
			status);
		ret = -ENODEV;
		goto err_release;
	}

	vmem_size = lcd->width * lcd->height * NT35510_BYTES_PER_PIXEL;
	vmem = vzalloc(vmem_size);
	if (!vmem) {
		ret = -ENOMEM;
		goto err_release;
	}

	strscpy(info->fix.id, "chiplab-lcd", sizeof(info->fix.id));
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.line_length = lcd->width * NT35510_BYTES_PER_PIXEL;
	info->fix.smem_len = vmem_size;
	info->fix.accel = FB_ACCEL_NONE;

	info->var.xres = lcd->width;
	info->var.yres = lcd->height;
	info->var.xres_virtual = lcd->width;
	info->var.yres_virtual = lcd->height;
	info->var.bits_per_pixel = 16;
	info->var.red = (struct fb_bitfield) { 11, 5, 0 };
	info->var.green = (struct fb_bitfield) { 5, 6, 0 };
	info->var.blue = (struct fb_bitfield) { 0, 5, 0 };
	info->var.transp = (struct fb_bitfield) { 0, 0, 0 };
	info->var.activate = FB_ACTIVATE_NOW;
	info->var.height = -1;
	info->var.width = -1;
	info->var.vmode = FB_VMODE_NONINTERLACED;

	info->fbops = &nt35510_fb_ops;
	info->screen_buffer = vmem;
	info->screen_size = vmem_size;
	info->pseudo_palette = lcd->pseudo_palette;
	info->flags = FBINFO_VIRTFB;

	lcd->defio.delay = max_t(unsigned long, 1, HZ / 20);
	lcd->defio.deferred_io = nt35510_deferred_io;
	info->fbdefio = &lcd->defio;

	ret = fb_alloc_cmap(&info->cmap, 16, 0);
	if (ret)
		goto err_vfree;

	fb_deferred_io_init(info);
	platform_set_drvdata(pdev, info);

	mutex_lock(&lcd->lock);
	nt35510_panel_init(lcd);
	lcd->enabled = true;
	nt35510_update_rect_locked(lcd, 0, 0, lcd->width, lcd->height);
	nt35510_set_control(lcd, true, true);
	mutex_unlock(&lcd->lock);

	ret = register_framebuffer(info);
	if (ret)
		goto err_defio;

	dev_info(dev, "fb%d: NT35510 %ux%u RGB565 framebuffer registered\n",
		 info->node, lcd->width, lcd->height);
	return 0;

err_defio:
	mutex_lock(&lcd->lock);
	nt35510_set_control(lcd, false, false);
	mutex_unlock(&lcd->lock);
	fb_deferred_io_cleanup(info);
	fb_dealloc_cmap(&info->cmap);
err_vfree:
	vfree(vmem);
err_release:
	mutex_destroy(&lcd->lock);
	framebuffer_release(info);
	return ret;
}

static void chiplab_nt35510_remove(struct platform_device *pdev)
{
	struct fb_info *info = platform_get_drvdata(pdev);
	struct chiplab_nt35510 *lcd = info->par;

	unregister_framebuffer(info);
	fb_deferred_io_cleanup(info);

	mutex_lock(&lcd->lock);
	lcd->enabled = false;
	nt35510_write_command(lcd, 0x2800);
	nt35510_set_control(lcd, false, false);
	mutex_unlock(&lcd->lock);

	fb_dealloc_cmap(&info->cmap);
	vfree(info->screen_buffer);
	mutex_destroy(&lcd->lock);
	framebuffer_release(info);
}

static const struct of_device_id chiplab_nt35510_of_match[] = {
	{ .compatible = "superscalarcrash,chiplab-nt35510" },
	{ }
};
MODULE_DEVICE_TABLE(of, chiplab_nt35510_of_match);

static struct platform_driver chiplab_nt35510_driver = {
	.probe = chiplab_nt35510_probe,
	.remove = chiplab_nt35510_remove,
	.driver = {
		.name = "chiplab-nt35510fb",
		.of_match_table = chiplab_nt35510_of_match,
	},
};
module_platform_driver(chiplab_nt35510_driver);

MODULE_DESCRIPTION("Chiplab NT35510 framebuffer driver");
MODULE_AUTHOR("Zhang Yuxiang <zz593141477@gmail.com>");
MODULE_AUTHOR("Jiajie Chen <jiegec@qq.com>");
MODULE_AUTHOR("SuperscalarCrash contributors");
MODULE_LICENSE("GPL");
