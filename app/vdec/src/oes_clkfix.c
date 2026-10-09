// SPDX-License-Identifier: GPL-2.0
/*
 * oes_clkfix: keep the VDEC_1 clock off hifi_pll.
 *
 * media_clock asks vdec_1 for 667 MHz; with CLK_SET_RATE_PARENT the
 * framework retunes hifi_pll to 2001 MHz and reparents vdec_1_sel onto it
 * while the gate is live, leaving hifi_pll's enable/prepare counts
 * unbalanced ("hifi_pll already disabled" WARN on every decoder close).
 *
 * Fix: park vdec_1_sel on fclk_div3 (666.67 MHz), move hifi_pll to a rate
 * that cannot give ~667 MHz, and hold it rate-exclusive so later
 * clk_set_rate() calls cannot retune it.  vdec then stays on fclk_div3
 * and no reparent happens while the clock is enabled.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/string.h>

static char *dev_name_p = "ff620000.vcodec_dos_dev";
module_param_named(dev, dev_name_p, charp, 0444);
static char *con = "clk_vdec_mux";
module_param(con, charp, 0444);
static unsigned long hifi_rate = 1806336000UL;
module_param(hifi_rate, ulong, 0444);
static bool dry;
static unsigned long gp0_rate = 846000000UL;
module_param(gp0_rate, ulong, 0444);
module_param(dry, bool, 0444);

static struct clk *c_vdec, *c_hifi, *c_div3, *c_sel;
static bool excl;
#define MAXP 8
static struct clk *c_lock[MAXP];
static int nlock;

static struct clk_hw *find_parent(struct clk_hw *hw, const char *name)
{
	unsigned int i, n = clk_hw_get_num_parents(hw);

	for (i = 0; i < n; i++) {
		struct clk_hw *p = clk_hw_get_parent_by_index(hw, i);

		if (p && !strcmp(clk_hw_get_name(p), name))
			return p;
	}
	return NULL;
}

static void cleanup(void)
{
	while (nlock > 0) {
		nlock--;
		clk_rate_exclusive_put(c_lock[nlock]);
		clk_put(c_lock[nlock]);
	}
	if (excl)
		clk_rate_exclusive_put(c_hifi);
	excl = false;
	if (!IS_ERR_OR_NULL(c_hifi))
		clk_put(c_hifi);
	if (!IS_ERR_OR_NULL(c_div3))
		clk_put(c_div3);
	if (!IS_ERR_OR_NULL(c_sel))
		clk_put(c_sel);
	if (!IS_ERR_OR_NULL(c_vdec))
		clk_put(c_vdec);
	c_hifi = c_div3 = c_vdec = c_sel = NULL;
}

static int __init clkfix_init(void)
{
	struct device *dev;
	struct clk_hw *hw, *sel = NULL, *hifi, *div3;
	int ret, depth;

	dev = bus_find_device_by_name(&platform_bus_type, NULL, dev_name_p);
	if (!dev) {
		pr_err("oes_clkfix: device %s not found\n", dev_name_p);
		return -ENODEV;
	}
	c_vdec = clk_get(dev, con);
	put_device(dev);
	if (IS_ERR(c_vdec)) {
		ret = PTR_ERR(c_vdec);
		c_vdec = NULL;
		pr_err("oes_clkfix: clk_get %s failed %d\n", con, ret);
		return ret;
	}

	/* walk up from the gate to the mux that offers hifi_pll */
	hw = __clk_get_hw(c_vdec);
	for (depth = 0; hw && depth < 4; depth++) {
		pr_info("oes_clkfix: chain[%d] %s rate=%lu\n", depth,
			clk_hw_get_name(hw), clk_hw_get_rate(hw));
		if (find_parent(hw, "hifi_pll")) {
			sel = hw;
			break;
		}
		hw = clk_hw_get_parent(hw);
	}
	if (!sel) {
		pr_err("oes_clkfix: no mux with hifi_pll parent, nothing to do\n");
		cleanup();
		return -ENODEV;
	}
	hifi = find_parent(sel, "hifi_pll");
	div3 = find_parent(sel, "fclk_div3");
	if (!div3) {
		pr_err("oes_clkfix: %s has no fclk_div3 parent\n",
		       clk_hw_get_name(sel));
		cleanup();
		return -ENODEV;
	}
	pr_info("oes_clkfix: mux %s parent=%s; vdec on=%d\n",
		clk_hw_get_name(sel),
		clk_hw_get_name(clk_hw_get_parent(sel)),
		__clk_is_enabled(c_vdec));

	if (__clk_is_enabled(c_vdec)) {
		pr_err("oes_clkfix: vdec clock is ON (decoder busy?), refusing\n");
		cleanup();
		return -EBUSY;
	}
	if (dry) {
		pr_info("oes_clkfix: dry run, not changing anything\n");
		cleanup();
		return -EAGAIN;
	}

	c_hifi = clk_hw_get_clk(hifi, "oes_clkfix");
	c_div3 = clk_hw_get_clk(div3, "oes_clkfix");
	c_sel = clk_hw_get_clk(sel, "oes_clkfix");
	if (IS_ERR(c_hifi) || IS_ERR(c_div3) || IS_ERR(c_sel)) {
		pr_err("oes_clkfix: clk_hw_get_clk failed\n");
		cleanup();
		return -EINVAL;
	}

	/* 1. park the mux on fclk_div3 while the gate is off */
	ret = clk_set_parent(c_sel, c_div3);
	if (ret) {
		pr_err("oes_clkfix: set_parent fclk_div3 failed %d\n", ret);
		cleanup();
		return ret;
	}
	/* 2. retune hifi_pll away from 3*667 MHz (it is off, nobody uses it) */
	ret = clk_set_rate(c_hifi, hifi_rate);
	if (ret)
		pr_warn("oes_clkfix: hifi_pll set_rate %lu failed %d (continuing)\n",
			hifi_rate, ret);
	/* 3. freeze the rate of every other parent (PLLs) so the mux can only
	 *    reach 667 MHz through fclk_div3 */
	{
		unsigned int i, n = clk_hw_get_num_parents(sel);

		for (i = 0; i < n && nlock < MAXP; i++) {
			struct clk_hw *p = clk_hw_get_parent_by_index(sel, i);
			struct clk *c;

			if (!p || p == div3)
				continue;
			c = clk_hw_get_clk(p, "oes_clkfix");
			if (IS_ERR(c))
				continue;
			if (!strcmp(clk_hw_get_name(p), "gp0_pll") && gp0_rate &&
			    clk_get_rate(c) != gp0_rate) {
				ret = clk_set_rate(c, gp0_rate);
				pr_info("oes_clkfix: restore gp0_pll -> %lu (%d) now %lu\n",
					gp0_rate, ret, clk_get_rate(c));
			}
			if (clk_rate_exclusive_get(c)) {
				clk_put(c);
				continue;
			}
			pr_info("oes_clkfix: locked %s @ %lu\n",
				clk_hw_get_name(p), clk_get_rate(c));
			c_lock[nlock++] = c;
		}
	}

	/* 4. replay the vendor request to verify the framework's choice */
	ret = clk_set_rate(c_vdec, 667000000);
	pr_info("oes_clkfix: test set_rate(vdec,667M)=%d -> %s rate=%lu, hifi_pll=%lu\n",
		ret, clk_hw_get_name(clk_hw_get_parent(sel)),
		clk_get_rate(c_vdec), clk_get_rate(c_hifi));
	if (strcmp(clk_hw_get_name(clk_hw_get_parent(sel)), "fclk_div3")) {
		pr_err("oes_clkfix: framework still picks %s, giving up\n",
		       clk_hw_get_name(clk_hw_get_parent(sel)));
		cleanup();
		return -EINVAL;
	}
	pr_info("oes_clkfix: CLKFIX OK (vdec on fclk_div3, hifi_pll locked)\n");
	return 0;
}

static void __exit clkfix_exit(void)
{
	cleanup();
	pr_info("oes_clkfix: unloaded (hifi_pll released)\n");
}

module_init(clkfix_init);
module_exit(clkfix_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Keep amlogic vdec_1 clock on fclk_div3 instead of hifi_pll");
