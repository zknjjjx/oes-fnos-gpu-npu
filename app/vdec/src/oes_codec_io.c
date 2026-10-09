// SPDX-License-Identifier: GPL-2.0
/*
 * oes_codec_io: fnOS ships Amlogic registers.ko without anything calling
 * codec_io_init(), so codec-io buses (cbus/dos/hiu/ao/vc/dmc/efuse) stay
 * unmapped and the decoder hangs. This helper fills registers.ko's
 * codecio_reg_map[] / codecio_reg_start[] / hiu_base / vpp_base from the
 * DT node and sets the media cpu type (G12B), like codec_io_probe() would.
 *
 * Layout of registers.ko .bss (from readelf on fnOS 6.18.18.c1151-trim):
 *   0x00 codecio_reg_map[8]   (static, 64 bytes)
 *   0x40 codecio_meson_dev    (4 bytes)
 *   0x48 codecio_reg_start[8] (32 bytes)
 *   0x68 hiu_base, 0x70 vpp_base
 * Pass meson_dev=<kallsyms addr of codecio_meson_dev> and
 * reg_start=<kallsyms addr of codecio_reg_start>; we cross-check them.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/io.h>

#define NBUS 8
#define CPU_G12B 0x29

static unsigned long meson_dev;
module_param(meson_dev, ulong, 0444);
static unsigned long reg_start;
module_param(reg_start, ulong, 0444);
static int cpu_id = CPU_G12B;
module_param(cpu_id, int, 0444);
static bool dry_run;
module_param(dry_run, bool, 0444);

extern void set_cpu_type_from_media(int cpu_id);
extern int get_cpu_type_from_media(void);

static const char *const names[NBUS] = {
	"cbus", "dos", "hiu", "ao", "vc", "dmc", "efuse", "noc" };

static int __init oes_codec_io_init(void)
{
	struct device_node *np;
	void __iomem **map;
	u32 *start;
	void __iomem **hiu, **vpp;
	u32 *dev;
	int i, n = 0;

	if (!meson_dev || !reg_start) {
		pr_err("oes_codec_io: need meson_dev= and reg_start=\n");
		return -EINVAL;
	}
	if (reg_start != meson_dev + 0x8) {
		pr_err("oes_codec_io: layout mismatch (reg_start-meson_dev=0x%lx), abort\n",
		       reg_start - meson_dev);
		return -EINVAL;
	}
	dev   = (u32 *)meson_dev;
	map   = (void __iomem **)(meson_dev - 0x40);
	start = (u32 *)reg_start;
	hiu   = (void __iomem **)(meson_dev + 0x28);
	vpp   = (void __iomem **)(meson_dev + 0x30);

	for (i = 0; i < NBUS; i++) {
		if (map[i] || start[i]) {
			pr_err("oes_codec_io: slot %d already set (%px/%x), abort\n",
			       i, map[i], start[i]);
			return -EBUSY;
		}
	}
	if (*dev || *hiu || *vpp) {
		pr_err("oes_codec_io: meson_dev/hiu/vpp not empty, abort\n");
		return -EBUSY;
	}

	np = of_find_compatible_node(NULL, NULL, "amlogic, meson-g12b, codec-io");
	if (!np) {
		pr_err("oes_codec_io: codec-io DT node not found\n");
		return -ENODEV;
	}

	pr_info("oes_codec_io: media cpu type before: 0x%x\n", get_cpu_type_from_media());

	for (i = 0; i < NBUS; i++) {
		struct resource res;
		void __iomem *p;

		if (of_address_to_resource(np, i, &res) || !res.start)
			continue;
		pr_info("oes_codec_io: %-5s %pa size 0x%llx\n", names[i],
			&res.start, (unsigned long long)resource_size(&res));
		if (dry_run)
			continue;
		p = ioremap(res.start, resource_size(&res));
		if (!p) {
			pr_err("oes_codec_io: ioremap %s failed\n", names[i]);
			continue;
		}
		start[i] = (u32)res.start;
		map[i] = p;
		n++;
	}
	of_node_put(np);

	if (dry_run) {
		pr_info("oes_codec_io: dry run, nothing changed\n");
		return -ECANCELED;
	}

	*hiu = map[2];
	*vpp = map[4];
	*dev = cpu_id;
	set_cpu_type_from_media(cpu_id);

	if (map[3])
		pr_info("oes_codec_io: AO_RTI_GEN_PWR_SLEEP0=0x%08x ISO0=0x%08x\n",
			readl(map[3] + 0xe8), readl(map[3] + 0xec));
	pr_info("oes_codec_io: mapped %d buses, media cpu type now 0x%x\n",
		n, get_cpu_type_from_media());
	return 0;
}
module_init(oes_codec_io_init);
/* no exit: mappings stay in use by registers.ko */

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OES: map Amlogic codec-io buses that fnOS never probes");
MODULE_SOFTDEP("pre: registers amlogic_cpuinfo");
