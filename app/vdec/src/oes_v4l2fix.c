// SPDX-License-Identifier: GPL-2.0
/*
 * oes_v4l2fix: steer V4L2 clients to the 2-plane capture formats.
 *
 * In MPLANE mode amvdec_ports reports single-plane NV12/NV21/YU12 with
 * plane[0].sizeimage = Y size only (update_ctx_dimension sets
 * sizeimage[0]=y, sizeimage[1]=c regardless of num_planes).  Clients that
 * pick those formats (ffmpeg h264_v4l2m2m always tries YU12/NV21 first)
 * get buffers too small for the chroma -> segfault / memory corruption.
 * Only NM12/NM21 (2 planes) are sized correctly.
 *
 * This module kprobes the driver's TRY_FMT / S_FMT handlers and rejects
 * single-plane YU12/NV12/NV21 on the capture queue with -EINVAL, so ffmpeg
 * falls back (via ENUM_FMT) to NM21/NM12.
 *
 * ops= address of aml_vdec_ioctl_ops (from /proc/kallsyms).
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/ptrace.h>
#include <linux/videodev2.h>

static unsigned long ops;
module_param(ops, ulong, 0444);
static int verbose = 8;
module_param(verbose, int, 0644);
static unsigned long rejected;
module_param(rejected, ulong, 0444);

#define IDX_S_FMT_CAP_MPLANE	30	/* vidioc_s_fmt_vid_cap_mplane */
#define IDX_S_FMT_OUT_MPLANE	31
#define IDX_TRY_FMT_CAP_MPLANE	44	/* vidioc_try_fmt_vid_cap_mplane */
#define IDX_TRY_FMT_OUT_MPLANE	45
#define IDX_QUERYCAP		0

static bool bad_fmt(struct pt_regs *r, const char *who)
{
	u32 *f = (u32 *)r->regs[2];	/* struct v4l2_format * */
	u32 type, pf;

	if (!f)
		return false;
	type = f[0];
	pf = f[4];			/* fmt @+8, pix_mp.pixelformat @+8 */
	if (type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
	    type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return false;
	/* only the 2-plane formats are sized correctly by the driver */
	if (pf == V4L2_PIX_FMT_NV21M || pf == V4L2_PIX_FMT_NV12M)
		return false;
	rejected++;
	if (verbose > 0) {
		verbose--;
		pr_info("oes_v4l2fix: %s reject %.4s on capture (use NM21/NM12)\n",
			who, (char *)&pf);
	}
	/* return -EINVAL to the caller without running the function */
	r->regs[0] = (u64)(s64)-EINVAL;
	instruction_pointer_set(r, r->regs[30]);
	return true;
}

static int pre_try(struct kprobe *p, struct pt_regs *r)
{
	return bad_fmt(r, "TRY_FMT") ? 1 : 0;
}

static int pre_s(struct kprobe *p, struct pt_regs *r)
{
	return bad_fmt(r, "S_FMT") ? 1 : 0;
}

static struct kprobe kp_try = { .pre_handler = pre_try };
static struct kprobe kp_s = { .pre_handler = pre_s };
static bool r_try, r_s;

/* skip the two patchable-entry nops; probe lands on paciasp (steppable) */
static kprobe_opcode_t *entry(unsigned long fn)
{
	u32 *i = (u32 *)fn;

	if (i[0] == 0xd503201f && i[1] == 0xd503201f)
		return (kprobe_opcode_t *)(fn + 8);
	return (kprobe_opcode_t *)fn;
}

static int __init fix_init(void)
{
	unsigned long *t = (unsigned long *)ops;
	unsigned long f_try, f_s;
	int ret;

	if (!ops) {
		pr_err("oes_v4l2fix: need ops=<aml_vdec_ioctl_ops>\n");
		return -EINVAL;
	}
	f_try = t[IDX_TRY_FMT_CAP_MPLANE];
	f_s = t[IDX_S_FMT_CAP_MPLANE];
	pr_info("oes_v4l2fix: querycap=%px try=%px/%px s=%px/%px\n",
		(void *)t[IDX_QUERYCAP], (void *)f_try,
		(void *)t[IDX_TRY_FMT_OUT_MPLANE], (void *)f_s,
		(void *)t[IDX_S_FMT_OUT_MPLANE]);
	/* kernel VA sanity; register_kprobe() validates text itself */
	if (f_try < 0xffff000000000000UL || f_s < 0xffff000000000000UL || !f_try || !f_s || f_try == f_s ||
	    f_try != t[IDX_TRY_FMT_OUT_MPLANE] ||
	    f_s != t[IDX_S_FMT_OUT_MPLANE]) {
		pr_err("oes_v4l2fix: ops table layout mismatch, not hooking\n");
		return -EINVAL;
	}

	kp_try.addr = entry(f_try);
	ret = register_kprobe(&kp_try);
	if (ret) {
		pr_err("oes_v4l2fix: kprobe try failed %d\n", ret);
		return ret;
	}
	r_try = true;
	kp_s.addr = entry(f_s);
	ret = register_kprobe(&kp_s);
	if (ret) {
		pr_err("oes_v4l2fix: kprobe s_fmt failed %d\n", ret);
		unregister_kprobe(&kp_try);
		return ret;
	}
	r_s = true;
	pr_info("oes_v4l2fix: V4L2 FIX HOOKED v2 (capture: only NM21/NM12 allowed)\n");
	return 0;
}

static void __exit fix_exit(void)
{
	if (r_s)
		unregister_kprobe(&kp_s);
	if (r_try)
		unregister_kprobe(&kp_try);
	pr_info("oes_v4l2fix: unloaded, rejected %lu\n", rejected);
}

module_init(fix_init);
module_exit(fix_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Reject mis-sized single-plane capture formats of amvdec_ports");
