// SPDX-License-Identifier: GPL-2.0
/*
 * oes_vdec_alias: fnOS decoder_common asks for legacy driver names
 * ("amvdec_h264_v4l") while only multi-instance drivers are shipped
 * ("ammvdec_h264_v4l"). Register an alias platform driver under the
 * legacy name that forwards probe/remove to the real driver.
 * Load the real amvdec_*_v4l modules first.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

struct vdec_alias {
	struct platform_driver drv;
	struct platform_driver *real;
	char name[48];
	bool registered;
};

static const char *const pairs[][2] = {
	{ "amvdec_h264_v4l",   "ammvdec_h264_v4l" },
	{ "amvdec_h265_v4l",   "ammvdec_h265_v4l" },
	{ "amvdec_vp9_v4l",    "ammvdec_vp9_v4l" },
	{ "amvdec_mpeg12_v4l", "ammvdec_mpeg12_v4l" },
	{ "amvdec_mpeg4_v4l",  "ammvdec_mpeg4_v4l" },
	{ "amvdec_mjpeg_v4l",  "ammvdec_mjpeg_v4l" },
	{ "amvdec_av1_v4l",    "ammvdec_av1_v4l" },
	{ "amvdec_avs2_v4l",   "ammvdec_avs2_v4l" },
};
#define NPAIRS ARRAY_SIZE(pairs)

static struct vdec_alias *aliases[NPAIRS];

static struct vdec_alias *to_alias(struct platform_device *pdev)
{
	struct platform_driver *pd = to_platform_driver(pdev->dev.driver);

	return container_of(pd, struct vdec_alias, drv);
}

static int alias_probe(struct platform_device *pdev)
{
	struct vdec_alias *a = to_alias(pdev);

	pr_info("oes_vdec_alias: %s -> %s probe\n", a->name, a->real->driver.name);
	return a->real->probe ? a->real->probe(pdev) : -ENODEV;
}

static void alias_remove(struct platform_device *pdev)
{
	struct vdec_alias *a = to_alias(pdev);

	if (a->real->remove)
		a->real->remove(pdev);
}

static void alias_shutdown(struct platform_device *pdev)
{
	struct vdec_alias *a = to_alias(pdev);

	if (a->real->shutdown)
		a->real->shutdown(pdev);
}

static void cleanup(void)
{
	int i;

	for (i = 0; i < NPAIRS; i++) {
		struct vdec_alias *a = aliases[i];

		if (!a)
			continue;
		if (a->registered) {
			platform_driver_unregister(&a->drv);
			module_put(a->real->driver.owner);
		}
		kfree(a);
		aliases[i] = NULL;
	}
}

static int __init oes_vdec_alias_init(void)
{
	int i, n = 0;

	for (i = 0; i < NPAIRS; i++) {
		struct device_driver *real, *exist;
		struct vdec_alias *a;
		int r;

		exist = driver_find(pairs[i][0], &platform_bus_type);
		if (exist) {
			pr_info("oes_vdec_alias: %s already exists, skip\n", pairs[i][0]);
			continue;
		}
		real = driver_find(pairs[i][1], &platform_bus_type);
		if (!real)
			continue;
		if (!try_module_get(real->owner))
			continue;

		a = kzalloc(sizeof(*a), GFP_KERNEL);
		if (!a) {
			module_put(real->owner);
			cleanup();
			return -ENOMEM;
		}
		strscpy(a->name, pairs[i][0], sizeof(a->name));
		a->real = to_platform_driver(real);
		a->drv.probe = alias_probe;
		a->drv.remove = alias_remove;
		a->drv.shutdown = alias_shutdown;
		a->drv.driver.name = a->name;
		a->drv.driver.pm = real->pm;
		aliases[i] = a;

		r = platform_driver_register(&a->drv);
		if (r) {
			pr_err("oes_vdec_alias: register %s failed %d\n", a->name, r);
			module_put(real->owner);
			kfree(a);
			aliases[i] = NULL;
			continue;
		}
		a->registered = true;
		n++;
		pr_info("oes_vdec_alias: %s -> %s\n", a->name, real->name);
	}
	if (!n) {
		pr_err("oes_vdec_alias: nothing to alias (load amvdec_*_v4l first)\n");
		return -ENODEV;
	}
	return 0;
}

static void __exit oes_vdec_alias_exit(void)
{
	cleanup();
}

module_init(oes_vdec_alias_init);
module_exit(oes_vdec_alias_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OES: legacy vdec driver name aliases for fnOS");
