// SPDX-License-Identifier: GPL-2.0
/*
 * oes_reg_ops: fnOS registers.ko also never runs vdec_reg_ops_init()
 * (__init, uncalled), so codec_reg_read()/codec_reg_write() find an
 * empty ops table at registers.ko .bss+0x80 and silently read 0.
 * decoder_common then spins forever in dec_dmc_port_ctrl() waiting for
 * DMC_CHAN_STS bit21. This module rebuilds that table exactly as
 * vdec_reg_ops_init() would for cpu type > 0x23 (G12B = 0x29).
 *
 * Function addresses are derived from the exported codec_reg_read
 * (.text+0x14d8 in registers.ko) and verified by symbol name.
 */
#include <linux/module.h>
#include <linux/kallsyms.h>
#include <linux/slab.h>
#include <linux/string.h>

static unsigned long meson_dev;	/* kallsyms addr of codecio_meson_dev (.bss+0x40) */
module_param(meson_dev, ulong, 0444);
static bool dry_run;
module_param(dry_run, bool, 0444);

extern int codec_reg_read(u32 bus_type, unsigned int reg);
extern int get_cpu_type_from_media(void);

struct reg_ops {		/* 32 bytes, matches registers.ko */
	int bus_type;
	u32 offset;
	int (*read)(unsigned int reg);
	void (*write)(unsigned int reg, unsigned int val);
	u32 rd_cnt;
	u32 wr_cnt;
};

struct ent {
	int type;
	int offset;
	const char *rd;
	unsigned long rd_off;
	const char *wr;
	unsigned long wr_off;
};

#define TEXT_ANCHOR 0x14d8UL	/* codec_reg_read */
/* order = vdec_reg_ops_init: main table then ex table (ex type 2 overrides) */
static const struct ent ents[] = {
	{ 0, 0,      "codecio_read_dosbus",   0x0a68, "codecio_write_dosbus",   0x0b10 },
	{ 4, 0,      "codecio_read_vcbus",    0x0d18, "codecio_write_vcbus",    0x0db0 },
	{ 3, 0,      "codecio_read_cbus",     0x09a8, "codecio_write_cbus",     0x0a10 },
	{ 1, 0,      "codecio_read_aobus",    0x0c58, "codecio_write_aobus",    0x0cc0 },
	{ 13, 0,     "codecio_read_vcbus",    0x0d18, "codecio_write_vcbus",    0x0db0 },
	{ 6, 0xf00,  "codecio_read_parsbus",  0x0e38, "codecio_write_parsbus",  0x0ea0 },
	{ 8, -0x100, "codecio_read_aiubus",   0x0ef8, "codecio_write_aiubus",   0x0f60 },
	{ 5, 0x200,  "codecio_read_demuxbus", 0x0fb8, "codecio_write_demuxbus", 0x1020 },
	{ 15, -0xd00,"codecio_read_resetbus", 0x1078, "codecio_write_resetbus", 0x10e0 },
	{ 16, 0,     "codecio_read_efusebus", 0x1138, "codecio_write_efusebus", 0x11a0 },
	{ 17, 0,     "codecio_read_nocbus",   0x11f8, "codecio_write_nocbus",   0x1260 },
	{ 2, -0x1000,"codecio_read_hiubus",   0x0b98, "codecio_write_hiubus",   0x0c00 },
	{ 14, 0,     "codecio_read_dmcbus",   0x04b8, "codecio_write_dmcbus",   0x0268 },
};
#define NTYPES 18

static bool sym_is(unsigned long addr, const char *name)
{
	char buf[KSYM_SYMBOL_LEN];

	sprint_symbol_no_offset(buf, addr);
	/* buf looks like "codecio_read_dosbus [registers]" */
	return !strncmp(buf, name, strlen(name)) &&
	       (buf[strlen(name)] == ' ' || buf[strlen(name)] == 0);
}

static int __init oes_reg_ops_init(void)
{
	unsigned long text = (unsigned long)codec_reg_read - TEXT_ANCHOR;
	struct reg_ops **tbl;
	struct reg_ops *ops;
	char buf[KSYM_SYMBOL_LEN];
	int i, n = ARRAY_SIZE(ents);

	if (!meson_dev) {
		pr_err("oes_reg_ops: need meson_dev=\n");
		return -EINVAL;
	}
	if (!sym_is(text + TEXT_ANCHOR, "codec_reg_read")) {
		pr_err("oes_reg_ops: anchor check failed\n");
		return -EINVAL;
	}
	sprint_symbol_no_offset(buf, meson_dev);
	if (strncmp(buf, "codecio_meson_dev", 17)) {
		pr_err("oes_reg_ops: meson_dev is %s, abort\n", buf);
		return -EINVAL;
	}
	if (get_cpu_type_from_media() <= 0x23) {
		pr_err("oes_reg_ops: cpu type 0x%x not set, load oes_codec_io first\n",
		       get_cpu_type_from_media());
		return -EINVAL;
	}
	for (i = 0; i < n; i++) {
		if (!sym_is(text + ents[i].rd_off, ents[i].rd) ||
		    !sym_is(text + ents[i].wr_off, ents[i].wr)) {
			pr_err("oes_reg_ops: symbol check failed for %s/%s\n",
			       ents[i].rd, ents[i].wr);
			return -EINVAL;
		}
	}

	tbl = (struct reg_ops **)(meson_dev + 0x40);	/* .bss+0x80 */
	for (i = 0; i < NTYPES; i++) {
		if (tbl[i]) {
			pr_err("oes_reg_ops: table slot %d already set, abort\n", i);
			return -EBUSY;
		}
	}
	pr_info("oes_reg_ops: before: codec_reg_read(DMC,0x32)=0x%x\n",
		codec_reg_read(14, 0x32));
	if (dry_run) {
		pr_info("oes_reg_ops: dry run ok, %d entries verified\n", n);
		return -ECANCELED;
	}

	ops = kcalloc(n, sizeof(*ops), GFP_KERNEL);
	if (!ops)
		return -ENOMEM;
	for (i = 0; i < n; i++) {
		ops[i].bus_type = ents[i].type;
		ops[i].offset = (u32)ents[i].offset;
		ops[i].read = (void *)(text + ents[i].rd_off);
		ops[i].write = (void *)(text + ents[i].wr_off);
	}
	for (i = 0; i < n; i++)
		WRITE_ONCE(tbl[ops[i].bus_type], &ops[i]);

	pr_info("oes_reg_ops: installed %d bus ops, codec_reg_read(DMC,0x32)=0x%x\n",
		n, codec_reg_read(14, 0x32));
	return 0;
}
module_init(oes_reg_ops_init);
/* no exit: table stays in use */

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OES: install registers.ko bus ops that fnOS never initialises");
MODULE_SOFTDEP("pre: registers oes_codec_io");
