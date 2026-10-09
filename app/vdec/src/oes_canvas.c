// SPDX-License-Identifier: GPL-2.0
/*
 * oes_canvas: make the vendor canvas API actually program the hardware.
 *
 * On fnOS the vendor symbols canvas_config_ex / canvas_config /
 * canvas_config_config are provided by amlogic_media_compat_provider.ko,
 * which only records the values in a software table and never writes the
 * DMC canvas LUT.  The video decoder then DMAs decoded frames to whatever
 * stale addresses the LUT holds -> random memory corruption (ffmpeg segv,
 * systemd ABRT, hard freezes).
 *
 * This module kprobes those three functions and writes the same canvas to
 * the DMC canvas LUT (G12B: 0xff638048, same layout as mainline
 * drivers/soc/amlogic/meson-canvas.c).
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/io.h>
#include <linux/spinlock.h>

#define LUT_BASE		0xff638048
#define DMC_CAV_LUT_DATAL	0x00
#define CANVAS_WIDTH_LBIT	29
#define CANVAS_WIDTH_LWID	3
#define DMC_CAV_LUT_DATAH	0x04
#define CANVAS_WIDTH_HBIT	0
#define CANVAS_HEIGHT_BIT	9
#define CANVAS_WRAP_BIT		22
#define CANVAS_BLKMODE_BIT	24
#define CANVAS_ENDIAN_BIT	26
#define DMC_CAV_LUT_ADDR	0x08
#define CANVAS_LUT_WR_EN	BIT(9)

static unsigned long lut_base = LUT_BASE;
module_param(lut_base, ulong, 0444);
static int dry;			/* 1 = only log, never write HW */
module_param(dry, int, 0644);
static int verbose = 24;	/* log the first N canvas configs */
module_param(verbose, int, 0644);
static unsigned long count;
module_param(count, ulong, 0444);

static void __iomem *reg;
static DEFINE_SPINLOCK(lock);

static void hw_config(u32 idx, unsigned long addr, u32 width, u32 height,
		      u32 wrap, u32 blk, u32 endian, const char *who)
{
	unsigned long flags;
	u32 l, h;

	if (idx > 0xff)
		return;
	l = (u32)((addr + 7) >> 3) |
	    (((width + 7) >> 3) << CANVAS_WIDTH_LBIT);
	h = ((((width + 7) >> 3) >> CANVAS_WIDTH_LWID) << CANVAS_WIDTH_HBIT) |
	    ((height & 0x1fff) << CANVAS_HEIGHT_BIT) |
	    ((wrap & 3) << CANVAS_WRAP_BIT) |
	    ((blk & 3) << CANVAS_BLKMODE_BIT) |
	    ((endian & 0xf) << CANVAS_ENDIAN_BIT);

	count++;
	if (verbose > 0) {
		verbose--;
		pr_info("oes_canvas: %s idx=%u addr=0x%lx w=%u h=%u wrap=%u blk=%u endian=%u%s\n",
			who, idx, addr, width, height, wrap, blk, endian,
			dry ? " (dry)" : "");
	}
	if (dry || !reg)
		return;

	spin_lock_irqsave(&lock, flags);
	writel_relaxed(l, reg + DMC_CAV_LUT_DATAL);
	writel_relaxed(h, reg + DMC_CAV_LUT_DATAH);
	writel_relaxed(CANVAS_LUT_WR_EN | idx, reg + DMC_CAV_LUT_ADDR);
	readl_relaxed(reg + DMC_CAV_LUT_DATAH);	/* flush */
	spin_unlock_irqrestore(&lock, flags);
}

/* void canvas_config_ex(u32 index, ulong addr, u32 width, u32 height,
 *                       u32 wrap, u32 blkmode, u32 endian) */
static int pre_ex(struct kprobe *p, struct pt_regs *r)
{
	hw_config(r->regs[0], r->regs[1], r->regs[2], r->regs[3],
		  r->regs[4], r->regs[5], r->regs[6], "ex");
	return 0;
}

/* void canvas_config(u32 index, ulong addr, u32 width, u32 height,
 *                    u32 wrap, u32 blkmode) */
static int pre_cfg(struct kprobe *p, struct pt_regs *r)
{
	hw_config(r->regs[0], r->regs[1], r->regs[2], r->regs[3],
		  r->regs[4], r->regs[5], 0, "cfg");
	return 0;
}

/* void canvas_config_config(u32 index, struct canvas_config_s *cfg)
 * layout (from the compat disassembly): ulong phy_addr @0, u32 width @8,
 * u32 height @0xc, u32 block_mode @0x10, u32 endian @0x14 */
struct canvas_cfg_s {
	unsigned long phy_addr;
	u32 width, height, block_mode, endian;
};
static int pre_cc(struct kprobe *p, struct pt_regs *r)
{
	struct canvas_cfg_s *c = (struct canvas_cfg_s *)r->regs[1];

	if (c)
		hw_config(r->regs[0], c->phy_addr, c->width, c->height, 0,
			  c->block_mode, c->endian, "cc");
	return 0;
}

/* +8 skips the two patchable-entry nops (ftrace site); args untouched */
static struct kprobe kps[] = {
	{ .symbol_name = "canvas_config_ex",     .offset = 8, .pre_handler = pre_ex  },
	{ .symbol_name = "canvas_config",        .offset = 8, .pre_handler = pre_cfg },
	{ .symbol_name = "canvas_config_config", .offset = 8, .pre_handler = pre_cc  },
};
static bool reged[ARRAY_SIZE(kps)];

static int __init oes_canvas_init(void)
{
	int i, ret, ok = 0;

	reg = ioremap(lut_base, 0x10);
	if (!reg) {
		pr_err("oes_canvas: ioremap 0x%lx failed\n", lut_base);
		return -ENOMEM;
	}
	for (i = 0; i < ARRAY_SIZE(kps); i++) {
		ret = register_kprobe(&kps[i]);
		if (ret) {
			pr_err("oes_canvas: kprobe %s failed %d\n",
			       kps[i].symbol_name, ret);
			continue;
		}
		reged[i] = true;
		ok++;
		pr_info("oes_canvas: hooked %s at %px\n", kps[i].symbol_name,
			kps[i].addr);
	}
	if (!ok) {
		iounmap(reg);
		return -ENOENT;
	}
	pr_info("oes_canvas: CANVAS HOOKED (%d/3), lut 0x%lx dry=%d\n", ok,
		lut_base, dry);
	return 0;
}

static void __exit oes_canvas_exit(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(kps); i++)
		if (reged[i])
			unregister_kprobe(&kps[i]);
	iounmap(reg);
	pr_info("oes_canvas: unloaded, %lu configs written\n", count);
}

module_init(oes_canvas_init);
module_exit(oes_canvas_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Write vendor canvas configs to the G12B DMC canvas LUT");
