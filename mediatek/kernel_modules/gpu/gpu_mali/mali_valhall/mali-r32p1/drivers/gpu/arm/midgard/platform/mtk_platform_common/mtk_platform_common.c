// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <mali_kbase.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <platform/mtk_platform_common.h>
#if IS_ENABLED(CONFIG_MALI_MIDGARD_DVFS) && IS_ENABLED(CONFIG_MALI_MTK_DVFS_POLICY)
#include "mtk_gpu_dvfs.h"
#endif
#include <mtk_gpufreq.h>
#if IS_ENABLED(CONFIG_MTK_GPU_SWPM_SUPPORT)
#include <mtk_gpu_power_sspm_ipi.h>
#include <platform/mtk_mfg_counter.h>
#endif
#include <ged_dvfs.h>
#if IS_ENABLED(CONFIG_PROC_FS)
#include <linux/proc_fs.h>
#if IS_ENABLED(CONFIG_MALI_MTK_MEM_TRACK)
#include <device/mali_kbase_device.h>
#include <linux/delay.h>
#endif
#endif
#if IS_ENABLED(CONFIG_MALI_MTK_DEVFREQ)
#include "mtk_gpu_devfreq_governor.h"
#endif
#if IS_ENABLED(CONFIG_MALI_MTK_DEBUG)
#include "mtk_platform_debug.h"
#endif
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_MM_OSVELTE)
#include <linux/proc_fs.h>
#include "sys-memstat.h"
#include "common.h"
#include "memstat.h"
#endif /* CONFIG_OPLUS_FEATURE_MM_OSVELTE */
#if IS_ENABLED(CONFIG_MALI_MTK_MEM_TRACK)
extern unsigned int (*mtk_get_gpu_memory_usage_fp)(void);
#endif

static bool mfg_powered;
static DEFINE_MUTEX(mfg_pm_lock);
static struct kbase_device *mali_kbdev;
#if IS_ENABLED(CONFIG_PROC_FS)
static struct proc_dir_entry *mtk_mali_root;
#endif
#if IS_ENABLED(CONFIG_MALI_MTK_MEM_TRACK)
static DEFINE_MUTEX(memtrack_lock);
#endif

struct kbase_device *mtk_common_get_kbdev(void)
{
	return mali_kbdev;
}

bool mtk_common_pm_is_mfg_active(void)
{
	return mfg_powered;
}

void mtk_common_pm_mfg_active(void)
{
	mutex_lock(&mfg_pm_lock);
	mfg_powered = true;
	mutex_unlock(&mfg_pm_lock);
}

void mtk_common_pm_mfg_idle(void)
{
	mutex_lock(&mfg_pm_lock);
	mfg_powered = false;
	mutex_unlock(&mfg_pm_lock);
}

void mtk_common_debug_dump(void)
{
#if IS_ENABLED(CONFIG_MALI_MTK_DEBUG)
	mtk_common_debug_dump_status();
#endif
}

int mtk_common_gpufreq_bringup(void)
{
	static int bringup = -1;

	if (bringup == -1) {
#if defined(CONFIG_MTK_GPUFREQ_V2)
		bringup = gpufreq_bringup();
#else
		bringup = mt_gpufreq_bringup();
#endif
	}

	return bringup;
}

int mtk_common_gpufreq_commit(int opp_idx)
{
	int ret = -1;

	mutex_lock(&mfg_pm_lock);
	if (opp_idx >= 0 && mtk_common_pm_is_mfg_active()) {
#if defined(CONFIG_MTK_GPUFREQ_V2)
		ret = mtk_common_gpufreq_bringup() ?
			-1 : gpufreq_commit(TARGET_DEFAULT, opp_idx);
#else
		ret = mtk_common_gpufreq_bringup() ?
			-1 : mt_gpufreq_target(opp_idx, KIR_POLICY);
#endif /* CONFIG_MTK_GPUFREQ_V2 */
	}
	mutex_unlock(&mfg_pm_lock);

	return ret;
}

int mtk_common_ged_dvfs_get_last_commit_idx(void)
{
#if IS_ENABLED(CONFIG_MALI_MIDGARD_DVFS) && IS_ENABLED(CONFIG_MALI_MTK_DVFS_POLICY)
	return (int)ged_dvfs_get_last_commit_idx();
#else
	return -1;
#endif
}

#if IS_ENABLED(CONFIG_MALI_MTK_MEM_TRACK)
static unsigned int mtk_common_gpu_memory_usage(void)
{
	unsigned int used_pages = atomic_read(&(mali_kbdev->memdev.used_pages));
	return used_pages * 4096;
}
#endif

#if IS_ENABLED(CONFIG_PROC_FS)
static int mtk_common_gpu_utilization_show(struct seq_file *m, void *v)
{
#if IS_ENABLED(CONFIG_MALI_MIDGARD_DVFS) && IS_ENABLED(CONFIG_MALI_MTK_DVFS_POLICY)
	unsigned int util_active, util_3d, util_ta, util_compute, cur_opp_idx;

	mtk_common_update_gpu_utilization();

#if defined(CONFIG_MTK_GPUFREQ_V2)
	cur_opp_idx = mtk_common_gpufreq_bringup() ?
		0 : gpufreq_get_cur_oppidx(TARGET_DEFAULT);
#else
	cur_opp_idx = mtk_common_gpufreq_bringup() ?
		0 : mt_gpufreq_get_cur_freq_index();
#endif /* CONFIG_MTK_GPUFREQ_V2 */

	util_active = mtk_common_get_util_active();
	util_3d = mtk_common_get_util_3d();
	util_ta = mtk_common_get_util_ta();
	util_compute = mtk_common_get_util_compute();

	seq_printf(m, "ACTIVE=%u 3D/TA/COMPUTE=%u/%u/%u OPP_IDX=%u MFG_PWR=%d\n",
	           util_active, util_3d, util_ta, util_compute, cur_opp_idx, mfg_powered);
#else
	seq_puts(m, "GPU DVFS doesn't be enabled\n");
#endif

	return 0;
}
DEFINE_PROC_SHOW_ATTRIBUTE(mtk_common_gpu_utilization);

static int mtk_common_gpu_memory_show(struct seq_file *m, void *v)
{
#if IS_ENABLED(CONFIG_MALI_MTK_MEM_TRACK)
	struct kbase_device *kbdev = (struct kbase_device *)mtk_common_get_kbdev();
	struct kbase_context *kctx;
	unsigned int trylock_count = 0;

	if (IS_ERR_OR_NULL(kbdev))
		return -1;

	lockdep_off();

	mutex_lock(&memtrack_lock);
	while (!mutex_trylock(&kbdev->kctx_list_lock)) {
		if (trylock_count > 3) {
			pr_info("[%s] lock held, bypass memory usage query", __func__);
			seq_printf(m, "<INVALID>");
			goto out_lock_held;
		}
		trylock_count ++;
		udelay(10);
	}

	/* output the total memory usage */
	seq_printf(m, "%-16s  %10u\n",
	           kbdev->devname,
	           atomic_read(&(kbdev->memdev.used_pages)));
	list_for_each_entry(kctx, &kbdev->kctx_list, kctx_list_link) {
		/* output the memory usage and cap for each kctx */
		seq_printf(m, "  %s-0x%p %10u %10u\n",
		           "kctx",
		           kctx,
		           atomic_read(&(kctx->used_pages)),
		           kctx->tgid);
	}
	mutex_unlock(&kbdev->kctx_list_lock);

out_lock_held:
	mutex_unlock(&memtrack_lock);

	lockdep_on();
#else
	seq_puts(m, "GPU mem_profile doesn't be enabled\n");
#endif

	return 0;
}
DEFINE_PROC_SHOW_ATTRIBUTE(mtk_common_gpu_memory);

#if IS_ENABLED(CONFIG_MALI_MTK_MEM_TRACK)
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_MM_OSVELTE)
static int mali_procinfo_show(struct seq_file *s, void *unused)
{
	return mtk_common_gpu_memory_show(s, unused);
}
DEFINE_PROC_SHOW_ATTRIBUTE(mali_procinfo);

static int mali_procdetail_max_show(struct seq_file *sfile, void *unused)
{
#if IS_ENABLED(CONFIG_DEBUG_FS)
	unsigned int sz = 0;
	unsigned int sz_max = 0;
	struct kbase_context *kctx = NULL;
	struct kbase_context *kctx_max = NULL;
	struct kbase_device *kbdev = (struct kbase_device *)mtk_common_get_kbdev();

	if (IS_ERR_OR_NULL(kbdev))
		return -ENODEV;


	mutex_lock(&memtrack_lock);

	mutex_lock(&kbdev->kctx_list_lock);
	list_for_each_entry(kctx, &kbdev->kctx_list, kctx_list_link) {
		sz = atomic_read(&(kctx->used_pages));
		if(sz > sz_max) {
			sz_max = sz;
			kctx_max = kctx;
		}
	}
	if (kctx_max) {
		atomic_inc(&kctx_max->refcount);
	}
	mutex_unlock(&kbdev->kctx_list_lock);

	if (kctx_max) {
		mutex_lock(&kctx_max->mem_profile_lock);
		seq_write(sfile, kctx_max->mem_profile_data, kctx_max->mem_profile_size);
		mutex_unlock(&kctx_max->mem_profile_lock);
		atomic_dec(&kctx_max->refcount);
	}

	seq_putc(sfile, '\n');
	mutex_unlock(&memtrack_lock);

#endif
	return 0;
}
DEFINE_PROC_SHOW_ATTRIBUTE(mali_procdetail_max);

long read_mali_mem_usage(enum mtrack_subtype type)
{
	struct kbase_device *kbdev = (struct kbase_device *)mtk_common_get_kbdev();
	unsigned long sz = 0;

	if (IS_ERR_OR_NULL(kbdev))
		return 0;

	if (type == MTRACK_GPU_TOTAL)
		sz = atomic_read(&(kbdev->memdev.used_pages));

	return sz;
}

void dump_mali_usage_stat(bool verbose)
{
	unsigned int sz = 0;
	struct kbase_device *kbdev = (struct kbase_device *)mtk_common_get_kbdev();
	struct kbase_context *kctx;
#if IS_ENABLED(CONFIG_DEBUG_FS)
	unsigned int sz_max = 0;
	struct kbase_context *kctx_max = NULL;
#endif

	if (IS_ERR_OR_NULL(kbdev))
		return;

	osvelte_info("======= %s\n", __func__);
	osvelte_info("%16s %5s %s\n", "comm", "pid", "size");

	mutex_lock(&kbdev->kctx_list_lock);
	list_for_each_entry(kctx, &kbdev->kctx_list, kctx_list_link) {
		struct task_struct *task;
		rcu_read_lock();
		sz = atomic_read(&(kctx->used_pages));
#if IS_ENABLED(CONFIG_DEBUG_FS)
		if(sz > sz_max) {
			sz_max = sz;
			kctx_max = kctx;
		}
#endif
		task = find_task_by_vpid(kctx->tgid);
		osvelte_info("%16s %5u %u\n",
			     task ? task->comm : "[null task]",
			     kctx->tgid,
			     K(sz));
		rcu_read_unlock();
	}
	mutex_unlock(&kbdev->kctx_list_lock);

#if IS_ENABLED(CONFIG_DEBUG_FS)
	if(sz_max >= (SZ_1G >> PAGE_SHIFT)) {
		char *buf = NULL;
		size_t size = 0;
		if(kctx_max) {
			mutex_lock(&kctx_max->mem_profile_lock);
			if(kctx_max->mem_profile_data && kctx_max->mem_profile_size > 0) {
				buf = kmalloc(kctx_max->mem_profile_size, GFP_KERNEL);
				size = kctx_max->mem_profile_size;
				if(buf) {
					memcpy(buf, kctx_max->mem_profile_data, size);
				}
			} else {
				pr_err("failed to copy mem_profile");
			}
			mutex_unlock(&kctx_max->mem_profile_lock);
		}

		if(buf) {
			char result[100] = {0};
			int lastLine = size - 1;
			int enterTime = 0;
			while(lastLine < size) {
				if(buf[lastLine] == '\n') {
					enterTime++;
				}
				if(enterTime == 2) {
					int resultPos = 0;
					while(lastLine < size - 1) {
						result[resultPos] = buf[lastLine+1];
						lastLine++;
						resultPos++;
					}
					result[resultPos] = '\0';
					break;
				}
				lastLine--;
			}
			pr_err("%s", result);
			kfree(buf);
		}
	}
#endif
}

long read_pid_mali_mem_usage(enum mtrack_subtype mtype, pid_t pid)
{
	struct kbase_device *kbdev = (struct kbase_device *)mtk_common_get_kbdev();
	struct kbase_context *kctx;
	unsigned long sz = 0;

	if (IS_ERR_OR_NULL(kbdev))
		return 0;

	mutex_lock(&kbdev->kctx_list_lock);
	list_for_each_entry(kctx, &kbdev->kctx_list, kctx_list_link) {
		if (kctx->tgid == pid) {
			sz = atomic_read(&(kctx->used_pages));
			break;
		}
	}
	mutex_unlock(&kbdev->kctx_list_lock);

	return sz;
}

static struct mtrack_debugger mali_mtrack_debugger = {
	.mem_usage = read_mali_mem_usage,
	.pid_mem_usage = read_pid_mali_mem_usage,
	.dump_usage_stat = dump_mali_usage_stat,
};
#endif /* CONFIG_OPLUS_FEATURE_MM_OSVELTE */
#endif /* CONFIG_MALI_MTK_MEM_TRACK */

void mtk_common_procfs_init(void)
{
	struct kbase_device *kbdev = (struct kbase_device *)mtk_common_get_kbdev();

	if (IS_ERR_OR_NULL(kbdev))
		return;

  	mtk_mali_root = proc_mkdir("mtk_mali", NULL);
  	if (!mtk_mali_root) {
  		pr_info("cannot create /proc/%s\n", "mtk_mali");
  		return;
  	}
	proc_create("utilization", 0444, mtk_mali_root, &mtk_common_gpu_utilization_proc_ops);
	proc_create("gpu_memory", 0444, mtk_mali_root, &mtk_common_gpu_memory_proc_ops);

#if IS_ENABLED(CONFIG_MALI_MTK_MEM_TRACK)
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_MM_OSVELTE)
	register_mtrack_debugger(MTRACK_GPU, &mali_mtrack_debugger);
	register_mtrack_procfs(MTRACK_GPU, "procinfo", 0444,
			       &mali_procinfo_proc_ops, NULL);
	register_mtrack_procfs(MTRACK_GPU, "procdetail_max", 0444,
			       &mali_procdetail_max_proc_ops, NULL);
#endif /* CONFIG_OPLUS_FEATURE_MM_OSVELTE */
#endif /* CONFIG_MALI_MTK_MEM_TRACK */
}

void mtk_common_procfs_exit(void)
{
	struct kbase_device *kbdev = (struct kbase_device *)mtk_common_get_kbdev();

	if (IS_ERR_OR_NULL(kbdev))
		return;

	mtk_mali_root = NULL;
	remove_proc_entry("utilization", mtk_mali_root);
	remove_proc_entry("gpu_memory", mtk_mali_root);

#if IS_ENABLED(CONFIG_MALI_MTK_MEM_TRACK)
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_MM_OSVELTE)
	unregister_mtrack_debugger(MTRACK_GPU, &mali_mtrack_debugger);
	unregister_mtrack_procfs(MTRACK_GPU, "procinfo");
#endif /* CONFIG_OPLUS_FEATURE_MM_OSVELTE */
#endif /* CONFIG_MALI_MTK_MEM_TRACK */

	remove_proc_entry("mtk_mali", NULL);
}
#endif


int mtk_common_device_init(struct kbase_device *kbdev)
{
	if (!kbdev) {
		pr_info("@%s: kbdev is NULL\n", __func__);
		return -1;
	}

	mali_kbdev = kbdev;

#if IS_ENABLED(CONFIG_MTK_IOMMU_V2)
	if (g_ion_device) {
		kbdev->client = ion_client_create(g_ion_device, "mali_kbase");
	}

	if (kbdev->client == NULL) {
		pr_info("@%s: create ion client failed!\n", __func__);
	}
#endif

#if IS_ENABLED(CONFIG_MALI_MIDGARD_DVFS) && IS_ENABLED(CONFIG_MALI_MTK_DVFS_POLICY)
#if IS_ENABLED(CONFIG_MALI_MTK_DVFS_LOADING_MODE)
	ged_dvfs_cal_gpu_utilization_ex_fp = mtk_common_cal_gpu_utilization_ex;
	mtk_notify_gpu_freq_change_fp = MTKGPUFreq_change_notify;
#else
	ged_dvfs_cal_gpu_utilization_fp = mtk_common_cal_gpu_utilization;
#endif
	ged_dvfs_gpu_freq_commit_fp = mtk_common_ged_dvfs_commit;
	ged_dvfs_set_gpu_core_mask_fp = mtk_set_core_mask;
#endif
#if IS_ENABLED(CONFIG_MALI_MTK_MEM_TRACK)
	mtk_get_gpu_memory_usage_fp = mtk_common_gpu_memory_usage;
#endif
#if IS_ENABLED(CONFIG_MALI_MTK_DEBUG)
	mtk_gpu_fence_debug_dump_fp = mtk_common_gpu_fence_debug_dump;
	mtk_common_debug_init();
#endif
#if IS_ENABLED(CONFIG_MALI_MTK_DEVFREQ)
	mtk_common_devfreq_init();
#endif
#if IS_ENABLED(CONFIG_MTK_GPU_SWPM_SUPPORT)
	MTKGPUPower_model_init();
	mtk_mfg_counter_init();
#endif

	return 0;
}

void mtk_common_device_term(struct kbase_device *kbdev)
{
	if (!kbdev) {
		pr_info("@%s: kbdev is NULL\n", __func__);
		return;
	}

#if IS_ENABLED(CONFIG_MTK_IOMMU_V2)
	if (kbdev->client != NULL) {
		ion_client_destroy(kbdev->client);
	}
#endif

#if IS_ENABLED(CONFIG_MALI_MIDGARD_DVFS) && IS_ENABLED(CONFIG_MALI_MTK_DVFS_POLICY)
#if IS_ENABLED(CONFIG_MALI_MTK_DVFS_LOADING_MODE)
	ged_dvfs_cal_gpu_utilization_ex_fp = NULL;
	mtk_notify_gpu_freq_change_fp = NULL;
#else
	ged_dvfs_cal_gpu_utilization_fp = NULL;
#endif
	ged_dvfs_gpu_freq_commit_fp = NULL;
#endif
#if IS_ENABLED(CONFIG_MALI_MTK_MEM_TRACK)
	mtk_get_gpu_memory_usage_fp = NULL;
#endif
#if IS_ENABLED(CONFIG_MALI_MTK_DEBUG)
	mtk_gpu_fence_debug_dump_fp = NULL;
	mtk_common_debug_term();
#endif
#if IS_ENABLED(CONFIG_MALI_MTK_DEVFREQ)
	mtk_common_devfreq_term();
#endif
#if IS_ENABLED(CONFIG_MTK_GPU_SWPM_SUPPORT)
	MTKGPUPower_model_destroy();
#endif
}
