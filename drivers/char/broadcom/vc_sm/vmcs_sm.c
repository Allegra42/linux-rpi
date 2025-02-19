/*
 ****************************************************************************
 * Copyright 2011-2012 Broadcom Corporation.  All rights reserved.
 *
 * Unless you and Broadcom execute a separate written software license
 * agreement governing use of this software, this software is licensed to you
 * under the terms of the GNU General Public License version 2, available at
 * http://www.broadcom.com/licenses/GPLv2.php (the "GPL").
 *
 * Notwithstanding the above, under no circumstances may you combine this
 * software in any way with any other Broadcom software provided under a
 * license other than the GPL, without Broadcom's express prior written
 * consent.
 ****************************************************************************
 */

/* ---- Include Files ----------------------------------------------------- */

#include <linux/cdev.h>
#include <linux/broadcom/vc_mem.h>
#include <linux/device.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/dma-buf.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/hugetlb.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pfn.h>
#include <linux/proc_fs.h>
#include <linux/pagemap.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/types.h>
#include <asm/cacheflush.h>

#include "vchiq_connected.h"
#include "vc_vchi_sm.h"

#include <linux/broadcom/vmcs_sm_ioctl.h>
#include "vc_sm_knl.h"

/* ---- Private Constants and Types --------------------------------------- */

#define DEVICE_NAME              "vcsm"
#define DRIVER_NAME		 "bcm2835-vcsm"
#define DEVICE_MINOR             0

#define VC_SM_DIR_ROOT_NAME       "vc-smem"
#define VC_SM_DIR_ALLOC_NAME      "alloc"
#define VC_SM_STATE               "state"
#define VC_SM_STATS               "statistics"
#define VC_SM_RESOURCES           "resources"
#define VC_SM_DEBUG               "debug"
#define VC_SM_WRITE_BUF_SIZE      128

/* Statistics tracked per resource and globally. */
enum sm_stats_t {
	/* Attempt. */
	ALLOC,
	FREE,
	LOCK,
	UNLOCK,
	MAP,
	FLUSH,
	INVALID,
	IMPORT,

	END_ATTEMPT,

	/* Failure. */
	ALLOC_FAIL,
	FREE_FAIL,
	LOCK_FAIL,
	UNLOCK_FAIL,
	MAP_FAIL,
	FLUSH_FAIL,
	INVALID_FAIL,
	IMPORT_FAIL,

	END_ALL,

};

static const char *const sm_stats_human_read[] = {
	"Alloc",
	"Free",
	"Lock",
	"Unlock",
	"Map",
	"Cache Flush",
	"Cache Invalidate",
	"Import",
};

typedef int (*VC_SM_SHOW) (struct seq_file *s, void *v);
struct sm_pde_t {
	VC_SM_SHOW show;          /* Debug fs function hookup. */
	struct dentry *dir_entry; /* Debug fs directory entry. */
	void *priv_data;          /* Private data */

};

/* Single resource allocation tracked for all devices. */
struct sm_mmap {
	struct list_head map_list;	/* Linked list of maps. */

	struct sm_resource_t *resource;	/* Pointer to the resource. */

	pid_t res_pid;			/* PID owning that resource. */
	unsigned int res_vc_hdl;	/* Resource handle (videocore). */
	unsigned int res_usr_hdl;	/* Resource handle (user). */

	unsigned long res_addr;	/* Mapped virtual address. */
	struct vm_area_struct *vma;	/* VM area for this mapping. */
	unsigned int ref_count;		/* Reference count to this vma. */

	/* Used to link maps associated with a resource. */
	struct list_head resource_map_list;
};

/* Single resource allocation tracked for each opened device. */
struct sm_resource_t {
	struct list_head resource_list;	/* List of resources. */
	struct list_head global_resource_list;	/* Global list of resources. */

	pid_t pid;		/* PID owning that resource. */
	uint32_t res_guid;	/* Unique identifier. */
	uint32_t lock_count;	/* Lock count for this resource. */
	uint32_t ref_count;	/* Ref count for this resource. */

	uint32_t res_handle;	/* Resource allocation handle. */
	void *res_base_mem;	/* Resource base memory address. */
	uint32_t res_size;	/* Resource size allocated. */
	enum vmcs_sm_cache_e res_cached;	/* Resource cache type. */
	struct sm_resource_t *res_shared;	/* Shared resource */

	enum sm_stats_t res_stats[END_ALL];	/* Resource statistics. */

	uint8_t map_count;	/* Counter of mappings for this resource. */
	struct list_head map_list;	/* Maps associated with a resource. */

	/* DMABUF related fields */
	struct dma_buf *dma_buf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	dma_addr_t dma_addr;

	struct sm_priv_data_t *private;
	bool map;		/* whether to map pages up front */
};

/* Private file data associated with each opened device. */
struct sm_priv_data_t {
	struct list_head resource_list; /* List of resources. */

	pid_t pid;                      /* PID of creator. */

	struct dentry *dir_pid;	   /* Debug fs entries root. */
	struct sm_pde_t dir_stats; /* Debug fs entries statistics sub-tree. */
	struct sm_pde_t dir_res;   /* Debug fs resource sub-tree. */

	int restart_sys;           /* Tracks restart on interrupt. */
	enum vc_sm_msg_type int_action; /* Interrupted action. */
	uint32_t int_trans_id;     /* Interrupted transaction. */

};

/* Global state information. */
struct sm_state_t {
	struct platform_device *pdev;
	struct sm_instance *sm_handle;	/* Handle for videocore service. */
	struct dentry *dir_root;   /* Debug fs entries root. */
	struct dentry *dir_alloc;  /* Debug fs entries allocations. */
	struct sm_pde_t dir_stats; /* Debug fs entries statistics sub-tree. */
	struct sm_pde_t dir_state; /* Debug fs entries state sub-tree. */
	struct dentry *debug;      /* Debug fs entries debug. */

	struct mutex map_lock;          /* Global map lock. */
	struct list_head map_list;      /* List of maps. */
	struct list_head resource_list;	/* List of resources. */

	enum sm_stats_t deceased[END_ALL];    /* Natural termination stats. */
	enum sm_stats_t terminated[END_ALL];  /* Forced termination stats. */
	uint32_t res_deceased_cnt;	      /* Natural termination counter. */
	uint32_t res_terminated_cnt;	      /* Forced termination counter. */

	struct cdev sm_cdev;	/* Device. */
	dev_t sm_devid;		/* Device identifier. */
	struct class *sm_class;	/* Class. */
	struct device *sm_dev;	/* Device. */

	struct sm_priv_data_t *data_knl;    /* Kernel internal data tracking. */

	struct mutex lock;	/* Global lock. */
	uint32_t guid;		/* GUID (next) tracker. */

};

/* ---- Private Variables ----------------------------------------------- */

static struct sm_state_t *sm_state;
static int sm_inited;

#if 0
static const char *const sm_cache_map_vector[] = {
	"(null)",
	"host",
	"videocore",
	"host+videocore",
};
#endif

/* ---- Private Function Prototypes -------------------------------------- */

/* ---- Private Functions ------------------------------------------------ */

static inline unsigned int vcaddr_to_pfn(unsigned long vc_addr)
{
	unsigned long pfn = vc_addr & 0x3FFFFFFF;

	pfn += mm_vc_mem_phys_addr;
	pfn >>= PAGE_SHIFT;
	return pfn;
}

/*
 * Carries over to the state statistics the statistics once owned by a deceased
 * resource.
 */
static void vc_sm_resource_deceased(struct sm_resource_t *p_res, int terminated)
{
	if (sm_state != NULL) {
		if (p_res != NULL) {
			int ix;

			if (terminated)
				sm_state->res_terminated_cnt++;
			else
				sm_state->res_deceased_cnt++;

			for (ix = 0; ix < END_ALL; ix++) {
				if (terminated)
					sm_state->terminated[ix] +=
					    p_res->res_stats[ix];
				else
					sm_state->deceased[ix] +=
					    p_res->res_stats[ix];
			}
		}
	}
}

/*
 * Fetch a videocore handle corresponding to a mapping of the pid+address
 * returns 0 (ie NULL) if no such handle exists in the global map.
 */
static unsigned int vmcs_sm_vc_handle_from_pid_and_address(unsigned int pid,
							   unsigned int addr)
{
	struct sm_mmap *map = NULL;
	unsigned int handle = 0;

	if (!sm_state || addr == 0)
		goto out;

	mutex_lock(&(sm_state->map_lock));

	/* Lookup the resource. */
	if (!list_empty(&sm_state->map_list)) {
		list_for_each_entry(map, &sm_state->map_list, map_list) {
			if (map->res_pid != pid)
				continue;
			if (addr < map->res_addr ||
						addr >= (map->res_addr + map->resource->res_size))
				continue;

			pr_debug("[%s]: global map %p (pid %u, addr %lx) -> vc-hdl %x (usr-hdl %x)\n",
				__func__, map, map->res_pid, map->res_addr,
				map->res_vc_hdl, map->res_usr_hdl);

			handle = map->res_vc_hdl;
			break;
		}
	}

	mutex_unlock(&(sm_state->map_lock));

out:
	/*
	 * Use a debug log here as it may be a valid situation that we query
	 * for something that is not mapped, we do not want a kernel log each
	 * time around.
	 *
	 * There are other error log that would pop up accordingly if someone
	 * subsequently tries to use something invalid after being told not to
	 * use it...
	 */
	if (handle == 0) {
		pr_debug("[%s]: not a valid map (pid %u, addr %x)\n",
			__func__, pid, addr);
	}

	return handle;
}

/*
 * Fetch a user handle corresponding to a mapping of the pid+address
 * returns 0 (ie NULL) if no such handle exists in the global map.
 */
static unsigned int vmcs_sm_usr_handle_from_pid_and_address(unsigned int pid,
							    unsigned int addr)
{
	struct sm_mmap *map = NULL;
	unsigned int handle = 0;

	if (!sm_state || addr == 0)
		goto out;

	mutex_lock(&(sm_state->map_lock));

	/* Lookup the resource. */
	if (!list_empty(&sm_state->map_list)) {
		list_for_each_entry(map, &sm_state->map_list, map_list) {
			if (map->res_pid != pid)
				continue;
			if (addr < map->res_addr ||
						addr >= (map->res_addr + map->resource->res_size))
				continue;

			pr_debug("[%s]: global map %p (pid %u, addr %lx) -> usr-hdl %x (vc-hdl %x)\n",
				__func__, map, map->res_pid, map->res_addr,
				map->res_usr_hdl, map->res_vc_hdl);

			handle = map->res_usr_hdl;
			break;
		}
	}

	mutex_unlock(&(sm_state->map_lock));

out:
	/*
	 * Use a debug log here as it may be a valid situation that we query
	 * for something that is not mapped yet.
	 *
	 * There are other error log that would pop up accordingly if someone
	 * subsequently tries to use something invalid after being told not to
	 * use it...
	 */
	if (handle == 0)
		pr_debug("[%s]: not a valid map (pid %u, addr %x)\n",
			__func__, pid, addr);

	return handle;
}

#if defined(DO_NOT_USE)
/*
 * Fetch an address corresponding to a mapping of the pid+handle
 * returns 0 (ie NULL) if no such address exists in the global map.
 */
static unsigned int vmcs_sm_usr_address_from_pid_and_vc_handle(unsigned int pid,
							       unsigned int hdl)
{
	struct sm_mmap *map = NULL;
	unsigned int addr = 0;

	if (sm_state == NULL || hdl == 0)
		goto out;

	mutex_lock(&(sm_state->map_lock));

	/* Lookup the resource. */
	if (!list_empty(&sm_state->map_list)) {
		list_for_each_entry(map, &sm_state->map_list, map_list) {
			if (map->res_pid != pid || map->res_vc_hdl != hdl)
				continue;

			pr_debug("[%s]: global map %p (pid %u, vc-hdl %x, usr-hdl %x) -> addr %lx\n",
				__func__, map, map->res_pid, map->res_vc_hdl,
				map->res_usr_hdl, map->res_addr);

			addr = map->res_addr;
			break;
		}
	}

	mutex_unlock(&(sm_state->map_lock));

out:
	/*
	 * Use a debug log here as it may be a valid situation that we query
	 * for something that is not mapped, we do not want a kernel log each
	 * time around.
	 *
	 * There are other error log that would pop up accordingly if someone
	 * subsequently tries to use something invalid after being told not to
	 * use it...
	 */
	if (addr == 0)
		pr_debug("[%s]: not a valid map (pid %u, hdl %x)\n",
			__func__, pid, hdl);

	return addr;
}
#endif

/*
 * Fetch an address corresponding to a mapping of the pid+handle
 * returns 0 (ie NULL) if no such address exists in the global map.
 */
static unsigned int vmcs_sm_usr_address_from_pid_and_usr_handle(unsigned int
								pid,
								unsigned int
								hdl)
{
	struct sm_mmap *map = NULL;
	unsigned int addr = 0;

	if (sm_state == NULL || hdl == 0)
		goto out;

	mutex_lock(&(sm_state->map_lock));

	/* Lookup the resource. */
	if (!list_empty(&sm_state->map_list)) {
		list_for_each_entry(map, &sm_state->map_list, map_list) {
			if (map->res_pid != pid || map->res_usr_hdl != hdl)
				continue;

			pr_debug("[%s]: global map %p (pid %u, vc-hdl %x, usr-hdl %x) -> addr %lx\n",
				__func__, map, map->res_pid, map->res_vc_hdl,
				map->res_usr_hdl, map->res_addr);

			addr = map->res_addr;
			break;
		}
	}

	mutex_unlock(&(sm_state->map_lock));

out:
	/*
	 * Use a debug log here as it may be a valid situation that we query
	 * for something that is not mapped, we do not want a kernel log each
	 * time around.
	 *
	 * There are other error log that would pop up accordingly if someone
	 * subsequently tries to use something invalid after being told not to
	 * use it...
	 */
	if (addr == 0)
		pr_debug("[%s]: not a valid map (pid %u, hdl %x)\n", __func__,
				pid, hdl);

	return addr;
}

/* Adds a resource mapping to the global data list. */
static void vmcs_sm_add_map(struct sm_state_t *state,
			    struct sm_resource_t *resource, struct sm_mmap *map)
{
	mutex_lock(&(state->map_lock));

	/* Add to the global list of mappings */
	list_add(&map->map_list, &state->map_list);

	/* Add to the list of mappings for this resource */
	list_add(&map->resource_map_list, &resource->map_list);
	resource->map_count++;

	mutex_unlock(&(state->map_lock));

	pr_debug("[%s]: added map %p (pid %u, vc-hdl %x, usr-hdl %x, addr %lx)\n",
		__func__, map, map->res_pid, map->res_vc_hdl,
		map->res_usr_hdl, map->res_addr);
}

/* Removes a resource mapping from the global data list. */
static void vmcs_sm_remove_map(struct sm_state_t *state,
			       struct sm_resource_t *resource,
			       struct sm_mmap *map)
{
	mutex_lock(&(state->map_lock));

	/* Remove from the global list of mappings */
	list_del(&map->map_list);

	/* Remove from the list of mapping for this resource */
	list_del(&map->resource_map_list);
	if (resource->map_count > 0)
		resource->map_count--;

	mutex_unlock(&(state->map_lock));

	pr_debug("[%s]: removed map %p (pid %d, vc-hdl %x, usr-hdl %x, addr %lx)\n",
		__func__, map, map->res_pid, map->res_vc_hdl, map->res_usr_hdl,
		map->res_addr);

	kfree(map);
}

/* Read callback for the global state proc entry. */
static int vc_sm_global_state_show(struct seq_file *s, void *v)
{
	struct sm_mmap *map = NULL;
	struct sm_resource_t *resource = NULL;
	int map_count = 0;
	int resource_count = 0;

	if (sm_state == NULL)
		return 0;

	seq_printf(s, "\nVC-ServiceHandle     0x%x\n",
		   (unsigned int)sm_state->sm_handle);

	/* Log all applicable mapping(s). */

	mutex_lock(&(sm_state->map_lock));
	seq_puts(s, "\nResources\n");
	if (!list_empty(&sm_state->resource_list)) {
		list_for_each_entry(resource, &sm_state->resource_list,
				    global_resource_list) {
			resource_count++;

			seq_printf(s, "\nResource                %p\n",
				   resource);
			seq_printf(s, "           PID          %u\n",
				   resource->pid);
			seq_printf(s, "           RES_GUID     0x%x\n",
				   resource->res_guid);
			seq_printf(s, "           LOCK_COUNT   %u\n",
				   resource->lock_count);
			seq_printf(s, "           REF_COUNT    %u\n",
				   resource->ref_count);
			seq_printf(s, "           res_handle   0x%X\n",
				   resource->res_handle);
			seq_printf(s, "           res_base_mem %p\n",
				   resource->res_base_mem);
			seq_printf(s, "           SIZE         %d\n",
				   resource->res_size);
			seq_printf(s, "           DMABUF       %p\n",
				   resource->dma_buf);
			seq_printf(s, "           ATTACH       %p\n",
				   resource->attach);
			seq_printf(s, "           SGT          %p\n",
				   resource->sgt);
			seq_printf(s, "           DMA_ADDR     %pad\n",
				   &resource->dma_addr);
		}
	}
	seq_printf(s, "\n\nTotal resource count:   %d\n\n", resource_count);

	seq_puts(s, "\nMappings\n");
	if (!list_empty(&sm_state->map_list)) {
		list_for_each_entry(map, &sm_state->map_list, map_list) {
			map_count++;

			seq_printf(s, "\nMapping                0x%x\n",
				   (unsigned int)map);
			seq_printf(s, "           TGID        %u\n",
				   map->res_pid);
			seq_printf(s, "           VC-HDL      0x%x\n",
				   map->res_vc_hdl);
			seq_printf(s, "           USR-HDL     0x%x\n",
				   map->res_usr_hdl);
			seq_printf(s, "           USR-ADDR    0x%lx\n",
				   map->res_addr);
			seq_printf(s, "           SIZE        %d\n",
				   map->resource->res_size);
		}
	}

	mutex_unlock(&(sm_state->map_lock));
	seq_printf(s, "\n\nTotal map count:   %d\n\n", map_count);

	return 0;
}

static int vc_sm_global_statistics_show(struct seq_file *s, void *v)
{
	int ix;

	/* Global state tracked statistics. */
	if (sm_state != NULL) {
		seq_puts(s, "\nDeceased Resources Statistics\n");

		seq_printf(s, "\nNatural Cause (%u occurences)\n",
			   sm_state->res_deceased_cnt);
		for (ix = 0; ix < END_ATTEMPT; ix++) {
			if (sm_state->deceased[ix] > 0) {
				seq_printf(s, "                %u\t%s\n",
					   sm_state->deceased[ix],
					   sm_stats_human_read[ix]);
			}
		}
		seq_puts(s, "\n");
		for (ix = 0; ix < END_ATTEMPT; ix++) {
			if (sm_state->deceased[ix + END_ATTEMPT] > 0) {
				seq_printf(s, "                %u\tFAILED %s\n",
					   sm_state->deceased[ix + END_ATTEMPT],
					   sm_stats_human_read[ix]);
			}
		}

		seq_printf(s, "\nForcefull (%u occurences)\n",
			   sm_state->res_terminated_cnt);
		for (ix = 0; ix < END_ATTEMPT; ix++) {
			if (sm_state->terminated[ix] > 0) {
				seq_printf(s, "                %u\t%s\n",
					   sm_state->terminated[ix],
					   sm_stats_human_read[ix]);
			}
		}
		seq_puts(s, "\n");
		for (ix = 0; ix < END_ATTEMPT; ix++) {
			if (sm_state->terminated[ix + END_ATTEMPT] > 0) {
				seq_printf(s, "                %u\tFAILED %s\n",
					   sm_state->terminated[ix +
								END_ATTEMPT],
					   sm_stats_human_read[ix]);
			}
		}
	}

	return 0;
}

#if 0
/* Read callback for the statistics proc entry. */
static int vc_sm_statistics_show(struct seq_file *s, void *v)
{
	int ix;
	struct sm_priv_data_t *file_data;
	struct sm_resource_t *resource;
	int res_count = 0;
	struct sm_pde_t *p_pde;

	p_pde = (struct sm_pde_t *)(s->private);
	file_data = (struct sm_priv_data_t *)(p_pde->priv_data);

	if (file_data == NULL)
		return 0;

	/* Per process statistics. */

	seq_printf(s, "\nStatistics for TGID %d\n", file_data->pid);

	mutex_lock(&(sm_state->map_lock));

	if (!list_empty(&file_data->resource_list)) {
		list_for_each_entry(resource, &file_data->resource_list,
				    resource_list) {
			res_count++;

			seq_printf(s, "\nGUID:         0x%x\n\n",
				   resource->res_guid);
			for (ix = 0; ix < END_ATTEMPT; ix++) {
				if (resource->res_stats[ix] > 0) {
					seq_printf(s,
						   "                %u\t%s\n",
						   resource->res_stats[ix],
						   sm_stats_human_read[ix]);
				}
			}
			seq_puts(s, "\n");
			for (ix = 0; ix < END_ATTEMPT; ix++) {
				if (resource->res_stats[ix + END_ATTEMPT] > 0) {
					seq_printf(s,
						   "                %u\tFAILED %s\n",
						   resource->res_stats[
						   ix + END_ATTEMPT],
						   sm_stats_human_read[ix]);
				}
			}
		}
	}

	mutex_unlock(&(sm_state->map_lock));

	seq_printf(s, "\nResources Count %d\n", res_count);

	return 0;
}
#endif

#if 0
/* Read callback for the allocation proc entry.  */
static int vc_sm_alloc_show(struct seq_file *s, void *v)
{
	struct sm_priv_data_t *file_data;
	struct sm_resource_t *resource;
	int alloc_count = 0;
	struct sm_pde_t *p_pde;

	p_pde = (struct sm_pde_t *)(s->private);
	file_data = (struct sm_priv_data_t *)(p_pde->priv_data);

	if (!file_data)
		return 0;

	/* Per process statistics.  */
	seq_printf(s, "\nAllocation for TGID %d\n", file_data->pid);

	mutex_lock(&(sm_state->map_lock));

	if (!list_empty(&file_data->resource_list)) {
		list_for_each_entry(resource, &file_data->resource_list,
				    resource_list) {
			alloc_count++;

			seq_printf(s, "\nGUID:              0x%x\n",
				   resource->res_guid);
			seq_printf(s, "Lock Count:        %u\n",
				   resource->lock_count);
			seq_printf(s, "Mapped:            %s\n",
				   (resource->map_count ? "yes" : "no"));
			seq_printf(s, "VC-handle:         0x%x\n",
				   resource->res_handle);
			seq_printf(s, "VC-address:        0x%p\n",
				   resource->res_base_mem);
			seq_printf(s, "VC-size (bytes):   %u\n",
				   resource->res_size);
			seq_printf(s, "Cache:             %s\n",
				   sm_cache_map_vector[resource->res_cached]);
		}
	}

	mutex_unlock(&(sm_state->map_lock));

	seq_printf(s, "\n\nTotal allocation count: %d\n\n", alloc_count);

	return 0;
}
#endif

static int vc_sm_seq_file_show(struct seq_file *s, void *v)
{
	struct sm_pde_t *sm_pde;

	sm_pde = (struct sm_pde_t *)(s->private);

	if (sm_pde && sm_pde->show)
		sm_pde->show(s, v);

	return 0;
}

static int vc_sm_single_open(struct inode *inode, struct file *file)
{
	return single_open(file, vc_sm_seq_file_show, inode->i_private);
}

static const struct file_operations vc_sm_debug_fs_fops = {
	.open = vc_sm_single_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/*
 * Adds a resource to the private data list which tracks all the allocated
 * data.
 */
static void vmcs_sm_add_resource(struct sm_priv_data_t *privdata,
				 struct sm_resource_t *resource)
{
	mutex_lock(&(sm_state->map_lock));
	list_add(&resource->resource_list, &privdata->resource_list);
	list_add(&resource->global_resource_list, &sm_state->resource_list);
	mutex_unlock(&(sm_state->map_lock));

	pr_debug("[%s]: added resource %p (base addr %p, hdl %x, size %u, cache %u)\n",
		__func__, resource, resource->res_base_mem,
		resource->res_handle, resource->res_size, resource->res_cached);
}

/*
 * Locates a resource and acquire a reference on it.
 * The resource won't be deleted while there is a reference on it.
 */
static struct sm_resource_t *vmcs_sm_acquire_resource(struct sm_priv_data_t
						      *private,
						      unsigned int res_guid)
{
	struct sm_resource_t *resource, *ret = NULL;

	mutex_lock(&(sm_state->map_lock));

	list_for_each_entry(resource, &private->resource_list, resource_list) {
		if (resource->res_guid != res_guid)
			continue;

		pr_debug("[%s]: located resource %p (guid: %x, base addr %p, hdl %x, size %u, cache %u)\n",
			__func__, resource, resource->res_guid,
			resource->res_base_mem, resource->res_handle,
			resource->res_size, resource->res_cached);
		resource->ref_count++;
		ret = resource;
		break;
	}

	mutex_unlock(&(sm_state->map_lock));

	return ret;
}

/*
 * Locates a resource and acquire a reference on it.
 * The resource won't be deleted while there is a reference on it.
 */
static struct sm_resource_t *vmcs_sm_acquire_first_resource(
		struct sm_priv_data_t *private)
{
	struct sm_resource_t *resource, *ret = NULL;

	mutex_lock(&(sm_state->map_lock));

	list_for_each_entry(resource, &private->resource_list, resource_list) {
		pr_debug("[%s]: located resource %p (guid: %x, base addr %p, hdl %x, size %u, cache %u)\n",
			__func__, resource, resource->res_guid,
			resource->res_base_mem, resource->res_handle,
			resource->res_size, resource->res_cached);
		resource->ref_count++;
		ret = resource;
		break;
	}

	mutex_unlock(&(sm_state->map_lock));

	return ret;
}

/*
 * Locates a resource and acquire a reference on it.
 * The resource won't be deleted while there is a reference on it.
 */
static struct sm_resource_t *vmcs_sm_acquire_global_resource(unsigned int
							     res_guid)
{
	struct sm_resource_t *resource, *ret = NULL;

	mutex_lock(&(sm_state->map_lock));

	list_for_each_entry(resource, &sm_state->resource_list,
			    global_resource_list) {
		if (resource->res_guid != res_guid)
			continue;

		pr_debug("[%s]: located resource %p (guid: %x, base addr %p, hdl %x, size %u, cache %u)\n",
			__func__, resource, resource->res_guid,
			resource->res_base_mem, resource->res_handle,
			resource->res_size, resource->res_cached);
		resource->ref_count++;
		ret = resource;
		break;
	}

	mutex_unlock(&(sm_state->map_lock));

	return ret;
}

/*
 * Release a previously acquired resource.
 * The resource will be deleted when its refcount reaches 0.
 */
static void vmcs_sm_release_resource(struct sm_resource_t *resource, int force)
{
	struct sm_priv_data_t *private = resource->private;
	struct sm_mmap *map, *map_tmp;
	struct sm_resource_t *res_tmp;
	int ret;

	mutex_lock(&(sm_state->map_lock));

	if (--resource->ref_count) {
		if (force)
			pr_err("[%s]: resource %p in use\n", __func__, resource);

		mutex_unlock(&(sm_state->map_lock));
		return;
	}

	/* Time to free the resource. Start by removing it from the list */
	list_del(&resource->resource_list);
	list_del(&resource->global_resource_list);

	/*
	 * Walk the global resource list, find out if the resource is used
	 * somewhere else. In which case we don't want to delete it.
	 */
	list_for_each_entry(res_tmp, &sm_state->resource_list,
			    global_resource_list) {
		if (res_tmp->res_handle == resource->res_handle) {
			resource->res_handle = 0;
			break;
		}
	}

	mutex_unlock(&(sm_state->map_lock));

	pr_debug("[%s]: freeing data - guid %x, hdl %x, base address %p\n",
		__func__, resource->res_guid, resource->res_handle,
		resource->res_base_mem);
	resource->res_stats[FREE]++;

	/* Make sure the resource we're removing is unmapped first */
	if (resource->map_count && !list_empty(&resource->map_list)) {
		down_write(&current->mm->mmap_sem);
		list_for_each_entry_safe(map, map_tmp, &resource->map_list,
					 resource_map_list) {
			ret =
			    do_munmap(current->mm, map->res_addr,
				      resource->res_size, NULL);
			if (ret) {
				pr_err("[%s]: could not unmap resource %p\n",
					__func__, resource);
			}
		}
		up_write(&current->mm->mmap_sem);
	}

	/* Free up the videocore allocated resource. */
	if (resource->res_handle) {
		struct vc_sm_free_t free = {
			resource->res_handle, (uint32_t)resource->res_base_mem
		};
		int status = vc_vchi_sm_free(sm_state->sm_handle, &free,
					     &private->int_trans_id);
		if (status != 0 && status != -EINTR) {
			pr_err("[%s]: failed to free memory on videocore (status: %u, trans_id: %u)\n",
			     __func__, status, private->int_trans_id);
			resource->res_stats[FREE_FAIL]++;
			ret = -EPERM;
		}
	}

	if (resource->sgt)
		dma_buf_unmap_attachment(resource->attach, resource->sgt,
					 DMA_BIDIRECTIONAL);
	if (resource->attach)
		dma_buf_detach(resource->dma_buf, resource->attach);
	if (resource->dma_buf)
		dma_buf_put(resource->dma_buf);

	/* Free up the shared resource. */
	if (resource->res_shared)
		vmcs_sm_release_resource(resource->res_shared, 0);

	/* Free up the local resource tracking this allocation. */
	vc_sm_resource_deceased(resource, force);
	kfree(resource);
}

/*
 * Dump the map table for the driver.  If process is -1, dumps the whole table,
 * if process is a valid pid (non -1) dump only the entries associated with the
 * pid of interest.
 */
static void vmcs_sm_host_walk_map_per_pid(int pid)
{
	struct sm_mmap *map = NULL;

	/* Make sure the device was started properly. */
	if (sm_state == NULL) {
		pr_err("[%s]: invalid device\n", __func__);
		return;
	}

	mutex_lock(&(sm_state->map_lock));

	/* Log all applicable mapping(s). */
	if (!list_empty(&sm_state->map_list)) {
		list_for_each_entry(map, &sm_state->map_list, map_list) {
			if (pid == -1 || map->res_pid == pid) {
				pr_info("[%s]: tgid: %u - vc-hdl: %x, usr-hdl: %x, usr-addr: %lx\n",
				     __func__, map->res_pid, map->res_vc_hdl,
				     map->res_usr_hdl, map->res_addr);
			}
		}
	}

	mutex_unlock(&(sm_state->map_lock));
}

/*
 * Dump the allocation table from host side point of view.  This only dumps the
 * data allocated for this process/device referenced by the file_data.
 */
static void vmcs_sm_host_walk_alloc(struct sm_priv_data_t *file_data)
{
	struct sm_resource_t *resource = NULL;

	/* Make sure the device was started properly. */
	if ((sm_state == NULL) || (file_data == NULL)) {
		pr_err("[%s]: invalid device\n", __func__);
		return;
	}

	mutex_lock(&(sm_state->map_lock));

	if (!list_empty(&file_data->resource_list)) {
		list_for_each_entry(resource, &file_data->resource_list,
				    resource_list) {
			pr_info("[%s]: guid: %x - hdl: %x, vc-mem: %p, size: %u, cache: %u\n",
			     __func__, resource->res_guid, resource->res_handle,
			     resource->res_base_mem, resource->res_size,
			     resource->res_cached);
		}
	}

	mutex_unlock(&(sm_state->map_lock));
}

/* Create support for private data tracking. */
static struct sm_priv_data_t *vc_sm_create_priv_data(pid_t id)
{
	char alloc_name[32];
	struct sm_priv_data_t *file_data = NULL;

	/* Allocate private structure. */
	file_data = kzalloc(sizeof(*file_data), GFP_KERNEL);

	if (!file_data) {
		pr_err("[%s]: cannot allocate file data\n", __func__);
		goto out;
	}

	snprintf(alloc_name, sizeof(alloc_name), "%d", id);

	INIT_LIST_HEAD(&file_data->resource_list);
	file_data->pid = id;
	file_data->dir_pid = debugfs_create_dir(alloc_name,
			sm_state->dir_alloc);
#if 0
  /* TODO: fix this to support querying statistics per pid */

	if (IS_ERR_OR_NULL(file_data->dir_pid)) {
		file_data->dir_pid = NULL;
	} else {
		struct dentry *dir_entry;

		dir_entry = debugfs_create_file(VC_SM_RESOURCES, 0444,
				file_data->dir_pid, file_data,
				vc_sm_debug_fs_fops);

		file_data->dir_res.dir_entry = dir_entry;
		file_data->dir_res.priv_data = file_data;
		file_data->dir_res.show = &vc_sm_alloc_show;

		dir_entry = debugfs_create_file(VC_SM_STATS, 0444,
				file_data->dir_pid, file_data,
				vc_sm_debug_fs_fops);

		file_data->dir_res.dir_entry = dir_entry;
		file_data->dir_res.priv_data = file_data;
		file_data->dir_res.show = &vc_sm_statistics_show;
	}
	pr_debug("[%s]: private data allocated %p\n", __func__, file_data);

#endif
out:
	return file_data;
}

/*
 * Open the device.  Creates a private state to help track all allocation
 * associated with this device.
 */
static int vc_sm_open(struct inode *inode, struct file *file)
{
	int ret = 0;

	/* Make sure the device was started properly. */
	if (!sm_state) {
		pr_err("[%s]: invalid device\n", __func__);
		ret = -EPERM;
		goto out;
	}

	file->private_data = vc_sm_create_priv_data(current->tgid);
	if (file->private_data == NULL) {
		pr_err("[%s]: failed to create data tracker\n", __func__);

		ret = -ENOMEM;
		goto out;
	}

out:
	return ret;
}

/*
 * Close the device.  Free up all resources still associated with this device
 * at the time.
 */
static int vc_sm_release(struct inode *inode, struct file *file)
{
	struct sm_priv_data_t *file_data =
	    (struct sm_priv_data_t *)file->private_data;
	struct sm_resource_t *resource;
	int ret = 0;

	/* Make sure the device was started properly. */
	if (sm_state == NULL || file_data == NULL) {
		pr_err("[%s]: invalid device\n", __func__);
		ret = -EPERM;
		goto out;
	}

	pr_debug("[%s]: using private data %p\n", __func__, file_data);

	if (file_data->restart_sys == -EINTR) {
		struct vc_sm_action_clean_t action_clean;

		pr_debug("[%s]: releasing following EINTR on %u (trans_id: %u) (likely due to signal)...\n",
			__func__, file_data->int_action,
			file_data->int_trans_id);

		action_clean.res_action = file_data->int_action;
		action_clean.action_trans_id = file_data->int_trans_id;

		vc_vchi_sm_clean_up(sm_state->sm_handle, &action_clean);
	}

	while ((resource = vmcs_sm_acquire_first_resource(file_data)) != NULL) {
		vmcs_sm_release_resource(resource, 0);
		vmcs_sm_release_resource(resource, 1);
	}

	/* Remove the corresponding proc entry. */
	debugfs_remove_recursive(file_data->dir_pid);

	/* Terminate the private data. */
	kfree(file_data);

out:
	return ret;
}

static void vcsm_vma_open(struct vm_area_struct *vma)
{
	struct sm_mmap *map = (struct sm_mmap *)vma->vm_private_data;

	pr_debug("[%s]: virt %lx-%lx, pid %i, pfn %i\n",
		__func__, vma->vm_start, vma->vm_end, (int)current->tgid,
		(int)vma->vm_pgoff);

	map->ref_count++;
}

static void vcsm_vma_close(struct vm_area_struct *vma)
{
	struct sm_mmap *map = (struct sm_mmap *)vma->vm_private_data;

	pr_debug("[%s]: virt %lx-%lx, pid %i, pfn %i\n",
		__func__, vma->vm_start, vma->vm_end, (int)current->tgid,
		(int)vma->vm_pgoff);

	map->ref_count--;

	/* Remove from the map table. */
	if (map->ref_count == 0)
		vmcs_sm_remove_map(sm_state, map->resource, map);
}

static int vcsm_vma_fault(struct vm_fault *vmf)
{
	struct sm_mmap *map = (struct sm_mmap *)vmf->vma->vm_private_data;
	struct sm_resource_t *resource = map->resource;
	pgoff_t page_offset;
	unsigned long pfn;
	vm_fault_t ret;

	/* Lock the resource if necessary. */
	if (!resource->lock_count) {
		struct vc_sm_lock_unlock_t lock_unlock;
		struct vc_sm_lock_result_t lock_result;
		int status;

		lock_unlock.res_handle = resource->res_handle;
		lock_unlock.res_mem = (uint32_t)resource->res_base_mem;

		pr_debug("[%s]: attempt to lock data - hdl %x, base address %p\n",
			__func__, lock_unlock.res_handle,
			(void *)lock_unlock.res_mem);

		/* Lock the videocore allocated resource. */
		status = vc_vchi_sm_lock(sm_state->sm_handle,
					 &lock_unlock, &lock_result, 0);
		if (status || !lock_result.res_mem) {
			pr_err("[%s]: failed to lock memory on videocore (status: %u)\n",
					__func__, status);
			resource->res_stats[LOCK_FAIL]++;
			return VM_FAULT_SIGBUS;
		}

		pfn = vcaddr_to_pfn((unsigned long)resource->res_base_mem);
		outer_inv_range(__pfn_to_phys(pfn),
				__pfn_to_phys(pfn) + resource->res_size);

		resource->res_stats[LOCK]++;
		resource->lock_count++;

		/* Keep track of the new base memory. */
		if (lock_result.res_mem &&
		    lock_result.res_old_mem &&
		    (lock_result.res_mem != lock_result.res_old_mem)) {
			resource->res_base_mem = (void *)lock_result.res_mem;
		}
	}

	/* We don't use vmf->pgoff since that has the fake offset */
	page_offset = ((unsigned long)vmf->address - vmf->vma->vm_start);
	pfn = (uint32_t)resource->res_base_mem & 0x3FFFFFFF;
	pfn += mm_vc_mem_phys_addr;
	pfn += page_offset;
	pfn >>= PAGE_SHIFT;

	/* Finally, remap it */
	ret = vmf_insert_pfn(vmf->vma, (unsigned long)vmf->address, pfn);
	if (ret != VM_FAULT_NOPAGE)
		pr_err("[%s]: failed to map page pfn:%lx virt:%lx ret:%d\n", __func__,
			pfn, (unsigned long)vmf->address, ret);
	return ret;
}

static const struct vm_operations_struct vcsm_vm_ops = {
	.open = vcsm_vma_open,
	.close = vcsm_vma_close,
	.fault = vcsm_vma_fault,
};

/* Converts VCSM_CACHE_OP_* to an operating function. */
static void (*cache_op_to_func(const unsigned cache_op))
		(const void*, const void*)
{
	switch (cache_op) {
	case VCSM_CACHE_OP_NOP:
		return NULL;

	case VCSM_CACHE_OP_INV:
		return dmac_inv_range;

	case VCSM_CACHE_OP_CLEAN:
		return dmac_clean_range;

	case VCSM_CACHE_OP_FLUSH:
		return dmac_flush_range;

	default:
		pr_err("[%s]: Invalid cache_op: 0x%08x\n", __func__, cache_op);
		return NULL;
	}
}

/*
 * Clean/invalid/flush cache of which buffer is already pinned (i.e. accessed).
 */
static int clean_invalid_contiguous_mem_2d(const void __user *addr,
		const size_t block_count, const size_t block_size, const size_t stride,
		const unsigned cache_op)
{
	size_t i;
	void (*op_fn)(const void*, const void*);

	if (!block_size) {
		pr_err("[%s]: size cannot be 0\n", __func__);
		return -EINVAL;
	}

	op_fn = cache_op_to_func(cache_op);
	if (op_fn == NULL)
		return -EINVAL;

	for (i = 0; i < block_count; i ++, addr += stride)
		op_fn(addr, addr + block_size);

	return 0;
}

/* Clean/invalid/flush cache of which buffer may be non-pinned. */
/* The caller must lock current->mm->mmap_sem for read. */
static int clean_invalid_mem_walk(unsigned long addr, const size_t size,
		const unsigned cache_op)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	unsigned long pgd_next, pud_next, pmd_next;
	const unsigned long end = ALIGN(addr + size, PAGE_SIZE);
	void (*op_fn)(const void*, const void*);

	addr &= PAGE_MASK;

	if (addr >= end)
		return 0;

	op_fn = cache_op_to_func(cache_op);
	if (op_fn == NULL)
		return -EINVAL;

	/* Walk PGD */
	pgd = pgd_offset(current->mm, addr);
	do {
		pgd_next = pgd_addr_end(addr, end);

		if (pgd_none(*pgd) || pgd_bad(*pgd))
			continue;

		/* Walk PUD */
		pud = pud_offset(pgd, addr);
		do {
			pud_next = pud_addr_end(addr, pgd_next);
			if (pud_none(*pud) || pud_bad(*pud))
				continue;

			/* Walk PMD */
			pmd = pmd_offset(pud, addr);
			do {
				pmd_next = pmd_addr_end(addr, pud_next);
				if (pmd_none(*pmd) || pmd_bad(*pmd))
					continue;

				/* Walk PTE */
				pte = pte_offset_map(pmd, addr);
				do {
					if (pte_none(*pte) || !pte_present(*pte))
						continue;

					op_fn((const void __user*) addr,
							(const void __user*) (addr + PAGE_SIZE));
				} while (pte++, addr += PAGE_SIZE, addr != pmd_next);
				pte_unmap(pte);

			} while (pmd++, addr = pmd_next, addr != pud_next);

		} while (pud++, addr = pud_next, addr != pgd_next);

	} while (pgd++, addr = pgd_next, addr != end);

	return 0;
}

/* Clean/invalid/flush cache of buffer in resource */
static int clean_invalid_resource_walk(const void __user *addr,
		const size_t size, const unsigned cache_op, const int usr_hdl,
		struct sm_resource_t *resource)
{
	int err;
	enum sm_stats_t stat_attempt, stat_failure;
	void __user *res_addr;

	if (resource == NULL) {
		pr_err("[%s]: resource is NULL\n", __func__);
		return -EINVAL;
	}
	if (resource->res_cached != VMCS_SM_CACHE_HOST &&
				resource->res_cached != VMCS_SM_CACHE_BOTH)
		return 0;

	switch (cache_op) {
	case VCSM_CACHE_OP_NOP:
		return 0;
	case VCSM_CACHE_OP_INV:
		stat_attempt = INVALID;
		stat_failure = INVALID_FAIL;
		break;
	case VCSM_CACHE_OP_CLEAN:
		/* Like the original VMCS_SM_CMD_CLEAN_INVALID ioctl handler does. */
		stat_attempt = FLUSH;
		stat_failure = FLUSH_FAIL;
		break;
	case VCSM_CACHE_OP_FLUSH:
		stat_attempt = FLUSH;
		stat_failure = FLUSH_FAIL;
		break;
	default:
		pr_err("[%s]: Invalid cache_op: 0x%08x\n", __func__, cache_op);
		return -EINVAL;
	}
	resource->res_stats[stat_attempt]++;

	if (size > resource->res_size) {
		pr_err("[%s]: size (0x%08zu) is larger than res_size (0x%08zu)\n",
				__func__, size, resource->res_size);
		return -EFAULT;
	}
	res_addr = (void __user*) vmcs_sm_usr_address_from_pid_and_usr_handle(
			current->tgid, usr_hdl);
	if (res_addr == NULL) {
		pr_err("[%s]: Failed to get user address "
				"from pid (%d) and user handle (%d)\n", __func__, current->tgid,
				resource->res_handle);
		return -EINVAL;
	}
	if (!(res_addr <= addr && addr + size <= res_addr + resource->res_size)) {
		pr_err("[%s]: Addr (0x%p-0x%p) out of range (0x%p-0x%p)\n",
				__func__, addr, addr + size, res_addr,
				res_addr + resource->res_size);
		return -EFAULT;
	}

	down_read(&current->mm->mmap_sem);
	err = clean_invalid_mem_walk((unsigned long) addr, size, cache_op);
	up_read(&current->mm->mmap_sem);

	if (err)
		resource->res_stats[stat_failure]++;

	return err;
}

/* Map an allocated data into something that the user space. */
static int vc_sm_mmap(struct file *file, struct vm_area_struct *vma)
{
	int ret = 0;
	struct sm_priv_data_t *file_data =
	    (struct sm_priv_data_t *)file->private_data;
	struct sm_resource_t *resource = NULL;
	struct sm_mmap *map = NULL;

	/* Make sure the device was started properly. */
	if ((sm_state == NULL) || (file_data == NULL)) {
		pr_err("[%s]: invalid device\n", __func__);
		return -EPERM;
	}

	pr_debug("[%s]: private data %p, guid %x\n", __func__, file_data,
		((unsigned int)vma->vm_pgoff << PAGE_SHIFT));

	/*
	 * We lookup to make sure that the data we are being asked to mmap is
	 * something that we allocated.
	 *
	 * We use the offset information as the key to tell us which resource
	 * we are mapping.
	 */
	resource = vmcs_sm_acquire_resource(file_data,
					    ((unsigned int)vma->vm_pgoff <<
					     PAGE_SHIFT));
	if (resource == NULL) {
		pr_err("[%s]: failed to locate resource for guid %x\n", __func__,
			((unsigned int)vma->vm_pgoff << PAGE_SHIFT));
		return -ENOMEM;
	}

	pr_debug("[%s]: guid %x, tgid %u, %u, %u\n",
		__func__, resource->res_guid, current->tgid, resource->pid,
		file_data->pid);

	/* Check permissions. */
	if (resource->pid && (resource->pid != current->tgid)) {
		pr_err("[%s]: current tgid %u != %u owner\n",
			__func__, current->tgid, resource->pid);
		ret = -EPERM;
		goto error;
	}

	/* Verify that what we are asked to mmap is proper. */
	if (resource->res_size != (unsigned int)(vma->vm_end - vma->vm_start)) {
		pr_err("[%s]: size inconsistency (resource: %u - mmap: %u)\n",
			__func__,
			resource->res_size,
			(unsigned int)(vma->vm_end - vma->vm_start));

		ret = -EINVAL;
		goto error;
	}

	/*
	 * Keep track of the tuple in the global resource list such that one
	 * can do a mapping lookup for address/memory handle.
	 */
	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (map == NULL) {
		pr_err("[%s]: failed to allocate global tracking resource\n",
			__func__);
		ret = -ENOMEM;
		goto error;
	}

	map->res_pid = current->tgid;
	map->res_vc_hdl = resource->res_handle;
	map->res_usr_hdl = resource->res_guid;
	map->res_addr = (unsigned long)vma->vm_start;
	map->resource = resource;
	map->vma = vma;
	vmcs_sm_add_map(sm_state, resource, map);

	/*
	 * We are not actually mapping the pages, we just provide a fault
	 * handler to allow pages to be mapped when accessed
	 */
	vma->vm_flags |=
	    VM_IO | VM_PFNMAP | VM_DONTCOPY | VM_DONTEXPAND;
	vma->vm_ops = &vcsm_vm_ops;
	vma->vm_private_data = map;

	/* vm_pgoff is the first PFN of the mapped memory */
	vma->vm_pgoff = (unsigned long)resource->res_base_mem & 0x3FFFFFFF;
	vma->vm_pgoff += mm_vc_mem_phys_addr;
	vma->vm_pgoff >>= PAGE_SHIFT;

	if ((resource->res_cached == VMCS_SM_CACHE_NONE) ||
	    (resource->res_cached == VMCS_SM_CACHE_VC)) {
		/* Allocated non host cached memory, honour it. */
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	}

	pr_debug("[%s]: resource %p (guid %x) - cnt %u, base address %p, handle %x, size %u (%u), cache %u\n",
		__func__,
		resource, resource->res_guid, resource->lock_count,
		resource->res_base_mem, resource->res_handle,
		resource->res_size, (unsigned int)(vma->vm_end - vma->vm_start),
		resource->res_cached);

	pr_debug("[%s]: resource %p (base address %p, handle %x) - map-count %d, usr-addr %x\n",
		__func__, resource, resource->res_base_mem,
		resource->res_handle, resource->map_count,
		(unsigned int)vma->vm_start);

	vcsm_vma_open(vma);
	resource->res_stats[MAP]++;
	vmcs_sm_release_resource(resource, 0);

	if (resource->map) {
		/* We don't use vmf->pgoff since that has the fake offset */
		unsigned long addr;

		for (addr = vma->vm_start; addr < vma->vm_end; addr += PAGE_SIZE) {
			/* Finally, remap it */
			unsigned long pfn = (unsigned long)resource->res_base_mem & 0x3FFFFFFF;

			pfn += mm_vc_mem_phys_addr;
			pfn += addr - vma->vm_start;
			pfn >>= PAGE_SHIFT;
			ret = vmf_insert_pfn(vma, addr, pfn);
		}
	}

	return 0;

error:
	resource->res_stats[MAP_FAIL]++;
	vmcs_sm_release_resource(resource, 0);
	return ret;
}

/* Allocate a shared memory handle and block. */
static int vc_sm_ioctl_alloc(struct sm_priv_data_t *private,
			     struct vmcs_sm_ioctl_alloc *ioparam)
{
	int ret = 0;
	int status;
	struct sm_resource_t *resource;
	struct vc_sm_alloc_t alloc = { 0 };
	struct vc_sm_alloc_result_t result = { 0 };
	enum vmcs_sm_cache_e cached = ioparam->cached;
	bool map = false;

	/* flag to requst buffer is mapped up front, rather than lazily */
	if (cached & 0x80) {
		map = true;
		cached &= ~0x80;
	}

	/* Setup our allocation parameters */
	alloc.type = ((cached == VMCS_SM_CACHE_VC)
		      || (cached ==
			  VMCS_SM_CACHE_BOTH)) ? VC_SM_ALLOC_CACHED :
	    VC_SM_ALLOC_NON_CACHED;
	alloc.base_unit = ioparam->size;
	alloc.num_unit = ioparam->num;
	alloc.allocator = current->tgid;
	/* Align to kernel page size */
	alloc.alignement = 4096;
	/* Align the size to the kernel page size */
	alloc.base_unit =
	    (alloc.base_unit + alloc.alignement - 1) & ~(alloc.alignement - 1);
	if (*ioparam->name) {
		memcpy(alloc.name, ioparam->name, sizeof(alloc.name) - 1);
	} else {
		memcpy(alloc.name, VMCS_SM_RESOURCE_NAME_DEFAULT,
		       sizeof(VMCS_SM_RESOURCE_NAME_DEFAULT));
	}

	pr_debug("[%s]: attempt to allocate \"%s\" data - type %u, base %u (%u), num %u, alignement %u\n",
		__func__, alloc.name, alloc.type, ioparam->size,
		alloc.base_unit, alloc.num_unit, alloc.alignement);

	/* Allocate local resource to track this allocation. */
	resource = kzalloc(sizeof(*resource), GFP_KERNEL);
	if (!resource) {
		ret = -ENOMEM;
		goto error;
	}
	INIT_LIST_HEAD(&resource->map_list);
	resource->ref_count++;
	resource->pid = current->tgid;

	/* Allocate the videocore resource. */
	status = vc_vchi_sm_alloc(sm_state->sm_handle, &alloc, &result,
				  &private->int_trans_id);
	if (status == -EINTR) {
		pr_debug("[%s]: requesting allocate memory action restart (trans_id: %u)\n",
			__func__, private->int_trans_id);
		ret = -ERESTARTSYS;
		private->restart_sys = -EINTR;
		private->int_action = VC_SM_MSG_TYPE_ALLOC;
		goto error;
	} else if (status != 0 || !result.res_mem) {
		pr_err("[%s]: failed to allocate memory on videocore (status: %u, trans_id: %u)\n",
		     __func__, status, private->int_trans_id);
		ret = -ENOMEM;
		resource->res_stats[ALLOC_FAIL]++;
		goto error;
	}

	/* Keep track of the resource we created. */
	resource->private = private;
	resource->res_handle = result.res_handle;
	resource->res_base_mem = (void *)result.res_mem;
	resource->res_size = alloc.base_unit * alloc.num_unit;
	resource->res_cached = cached;
	resource->map = map;

	/*
	 * Kernel/user GUID.  This global identifier is used for mmap'ing the
	 * allocated region from user space, it is passed as the mmap'ing
	 * offset, we use it to 'hide' the videocore handle/address.
	 */
	mutex_lock(&sm_state->lock);
	resource->res_guid = ++sm_state->guid;
	mutex_unlock(&sm_state->lock);
	resource->res_guid <<= PAGE_SHIFT;

	vmcs_sm_add_resource(private, resource);

	pr_debug("[%s]: allocated data - guid %x, hdl %x, base address %p, size %d, cache %d\n",
		__func__, resource->res_guid, resource->res_handle,
		resource->res_base_mem, resource->res_size,
		resource->res_cached);

	/* We're done */
	resource->res_stats[ALLOC]++;
	ioparam->handle = resource->res_guid;
	return 0;

error:
	pr_err("[%s]: failed to allocate \"%s\" data (%i) - type %u, base %u (%u), num %u, alignment %u\n",
	     __func__, alloc.name, ret, alloc.type, ioparam->size,
	     alloc.base_unit, alloc.num_unit, alloc.alignement);
	if (resource != NULL) {
		vc_sm_resource_deceased(resource, 1);
		kfree(resource);
	}
	return ret;
}

/* Share an allocate memory handle and block.*/
static int vc_sm_ioctl_alloc_share(struct sm_priv_data_t *private,
				   struct vmcs_sm_ioctl_alloc_share *ioparam)
{
	struct sm_resource_t *resource, *shared_resource;
	int ret = 0;

	pr_debug("[%s]: attempt to share resource %u\n", __func__,
			ioparam->handle);

	shared_resource = vmcs_sm_acquire_global_resource(ioparam->handle);
	if (shared_resource == NULL) {
		ret = -ENOMEM;
		goto error;
	}

	/* Allocate local resource to track this allocation. */
	resource = kzalloc(sizeof(*resource), GFP_KERNEL);
	if (resource == NULL) {
		pr_err("[%s]: failed to allocate local tracking resource\n",
			__func__);
		ret = -ENOMEM;
		goto error;
	}
	INIT_LIST_HEAD(&resource->map_list);
	resource->ref_count++;
	resource->pid = current->tgid;

	/* Keep track of the resource we created. */
	resource->private = private;
	resource->res_handle = shared_resource->res_handle;
	resource->res_base_mem = shared_resource->res_base_mem;
	resource->res_size = shared_resource->res_size;
	resource->res_cached = shared_resource->res_cached;
	resource->res_shared = shared_resource;

	mutex_lock(&sm_state->lock);
	resource->res_guid = ++sm_state->guid;
	mutex_unlock(&sm_state->lock);
	resource->res_guid <<= PAGE_SHIFT;

	vmcs_sm_add_resource(private, resource);

	pr_debug("[%s]: allocated data - guid %x, hdl %x, base address %p, size %d, cache %d\n",
		__func__, resource->res_guid, resource->res_handle,
		resource->res_base_mem, resource->res_size,
		resource->res_cached);

	/* We're done */
	resource->res_stats[ALLOC]++;
	ioparam->handle = resource->res_guid;
	ioparam->size = resource->res_size;
	return 0;

error:
	pr_err("[%s]: failed to share %u\n", __func__, ioparam->handle);
	if (shared_resource != NULL)
		vmcs_sm_release_resource(shared_resource, 0);

	return ret;
}

/* Free a previously allocated shared memory handle and block.*/
static int vc_sm_ioctl_free(struct sm_priv_data_t *private,
			    struct vmcs_sm_ioctl_free *ioparam)
{
	struct sm_resource_t *resource =
	    vmcs_sm_acquire_resource(private, ioparam->handle);

	if (resource == NULL) {
		pr_err("[%s]: resource for guid %u does not exist\n", __func__,
			ioparam->handle);
		return -EINVAL;
	}

	/* Check permissions. */
	if (resource->pid && (resource->pid != current->tgid)) {
		pr_err("[%s]: current tgid %u != %u owner\n",
			__func__, current->tgid, resource->pid);
		vmcs_sm_release_resource(resource, 0);
		return -EPERM;
	}

	vmcs_sm_release_resource(resource, 0);
	vmcs_sm_release_resource(resource, 0);
	return 0;
}

/* Resize a previously allocated shared memory handle and block. */
static int vc_sm_ioctl_resize(struct sm_priv_data_t *private,
			      struct vmcs_sm_ioctl_resize *ioparam)
{
	int ret = 0;
	int status;
	struct vc_sm_resize_t resize;
	struct sm_resource_t *resource;

	/* Locate resource from GUID. */
	resource = vmcs_sm_acquire_resource(private, ioparam->handle);
	if (!resource) {
		pr_err("[%s]: failed resource - guid %x\n",
				__func__, ioparam->handle);
		ret = -EFAULT;
		goto error;
	}

	/*
	 * If the resource is locked, its reference count will be not NULL,
	 * in which case we will not be allowed to resize it anyways, so
	 * reject the attempt here.
	 */
	if (resource->lock_count != 0) {
		pr_err("[%s]: cannot resize - guid %x, ref-cnt %d\n",
		     __func__, ioparam->handle, resource->lock_count);
		ret = -EFAULT;
		goto error;
	}

	/* Check permissions. */
	if (resource->pid && (resource->pid != current->tgid)) {
		pr_err("[%s]: current tgid %u != %u owner\n", __func__,
				current->tgid, resource->pid);
		ret = -EPERM;
		goto error;
	}

	if (resource->map_count != 0) {
		pr_err("[%s]: cannot resize - guid %x, ref-cnt %d\n",
		     __func__, ioparam->handle, resource->map_count);
		ret = -EFAULT;
		goto error;
	}

	resize.res_handle = resource->res_handle;
	resize.res_mem = (uint32_t)resource->res_base_mem;
	resize.res_new_size = ioparam->new_size;

	pr_debug("[%s]: attempt to resize data - guid %x, hdl %x, base address %p\n",
		__func__, ioparam->handle, resize.res_handle,
		(void *)resize.res_mem);

	/* Resize the videocore allocated resource. */
	status = vc_vchi_sm_resize(sm_state->sm_handle, &resize,
				   &private->int_trans_id);
	if (status == -EINTR) {
		pr_debug("[%s]: requesting resize memory action restart (trans_id: %u)\n",
			__func__, private->int_trans_id);
		ret = -ERESTARTSYS;
		private->restart_sys = -EINTR;
		private->int_action = VC_SM_MSG_TYPE_RESIZE;
		goto error;
	} else if (status) {
		pr_err("[%s]: failed to resize memory on videocore (status: %u, trans_id: %u)\n",
		     __func__, status, private->int_trans_id);
		ret = -EPERM;
		goto error;
	}

	pr_debug("[%s]: success to resize data - hdl %x, size %d -> %d\n",
		__func__, resize.res_handle, resource->res_size,
		resize.res_new_size);

	/* Successfully resized, save the information and inform the user. */
	ioparam->old_size = resource->res_size;
	resource->res_size = resize.res_new_size;

error:
	if (resource)
		vmcs_sm_release_resource(resource, 0);

	return ret;
}

/* Lock a previously allocated shared memory handle and block. */
static int vc_sm_ioctl_lock(struct sm_priv_data_t *private,
			    struct vmcs_sm_ioctl_lock_unlock *ioparam,
			    int change_cache, enum vmcs_sm_cache_e cache_type,
			    unsigned int vc_addr)
{
	int status;
	struct vc_sm_lock_unlock_t lock;
	struct vc_sm_lock_result_t result;
	struct sm_resource_t *resource;
	int ret = 0;
	struct sm_mmap *map, *map_tmp;
	unsigned long phys_addr;

	map = NULL;

	/* Locate resource from GUID. */
	resource = vmcs_sm_acquire_resource(private, ioparam->handle);
	if (resource == NULL) {
		ret = -EINVAL;
		goto error;
	}

	/* Check permissions. */
	if (resource->pid && (resource->pid != current->tgid)) {
		pr_err("[%s]: current tgid %u != %u owner\n", __func__,
				current->tgid, resource->pid);
		ret = -EPERM;
		goto error;
	}

	lock.res_handle = resource->res_handle;
	lock.res_mem = (uint32_t)resource->res_base_mem;

	/* Take the lock and get the address to be mapped. */
	if (vc_addr == 0) {
		pr_debug("[%s]: attempt to lock data - guid %x, hdl %x, base address %p\n",
			__func__, ioparam->handle, lock.res_handle,
			(void *)lock.res_mem);

		/* Lock the videocore allocated resource. */
		status = vc_vchi_sm_lock(sm_state->sm_handle, &lock, &result,
					 &private->int_trans_id);
		if (status == -EINTR) {
			pr_debug("[%s]: requesting lock memory action restart (trans_id: %u)\n",
				__func__, private->int_trans_id);
			ret = -ERESTARTSYS;
			private->restart_sys = -EINTR;
			private->int_action = VC_SM_MSG_TYPE_LOCK;
			goto error;
		} else if (status ||
			   (!status && !(void *)result.res_mem)) {
			pr_err("[%s]: failed to lock memory on videocore (status: %u, trans_id: %u)\n",
			     __func__, status, private->int_trans_id);
			ret = -EPERM;
			resource->res_stats[LOCK_FAIL]++;
			goto error;
		}

		pr_debug("[%s]: succeed to lock data - hdl %x, base address %p (%p), ref-cnt %d\n",
			__func__, lock.res_handle, (void *)result.res_mem,
			(void *)lock.res_mem, resource->lock_count);
	}
	/* Lock assumed taken already, address to be mapped is known. */
	else
		resource->res_base_mem = (void *)vc_addr;

	resource->res_stats[LOCK]++;
	resource->lock_count++;

	/* Keep track of the new base memory allocation if it has changed. */
	if ((vc_addr == 0) &&
	    ((void *)result.res_mem) &&
	    ((void *)result.res_old_mem) &&
	    (result.res_mem != result.res_old_mem)) {
		resource->res_base_mem = (void *)result.res_mem;

		/* Kernel allocated resources. */
		if (resource->pid == 0) {
			if (!list_empty(&resource->map_list)) {
				list_for_each_entry_safe(map, map_tmp,
							 &resource->map_list,
							 resource_map_list) {
					if (map->res_addr) {
						iounmap((void *)map->res_addr);
						map->res_addr = 0;

						vmcs_sm_remove_map(sm_state,
								map->resource,
								map);
						break;
					}
				}
			}
		}
	}

	if (change_cache)
		resource->res_cached = cache_type;

	if (resource->map_count) {
		ioparam->addr =
		    vmcs_sm_usr_address_from_pid_and_usr_handle(
				    current->tgid, ioparam->handle);

		pr_debug("[%s] map_count %d private->pid %d current->tgid %d hnd %x addr %u\n",
			__func__, resource->map_count, private->pid,
			current->tgid, ioparam->handle, ioparam->addr);
	} else {
		/* Kernel allocated resources. */
		if (resource->pid == 0) {
			pr_debug("[%s]: attempt mapping kernel resource - guid %x, hdl %x\n",
				__func__, ioparam->handle, lock.res_handle);

			ioparam->addr = 0;

			map = kzalloc(sizeof(*map), GFP_KERNEL);
			if (map == NULL) {
				pr_err("[%s]: failed allocating tracker\n",
						__func__);
				ret = -ENOMEM;
				goto error;
			} else {
				phys_addr = (uint32_t)resource->res_base_mem &
				    0x3FFFFFFF;
				phys_addr += mm_vc_mem_phys_addr;
				if (resource->res_cached
						== VMCS_SM_CACHE_HOST) {
					ioparam->addr = (unsigned long)
					/* TODO - make cached work */
					    ioremap_nocache(phys_addr,
							   resource->res_size);

					pr_debug("[%s]: mapping kernel - guid %x, hdl %x - cached mapping %u\n",
						__func__, ioparam->handle,
						lock.res_handle, ioparam->addr);
				} else {
					ioparam->addr = (unsigned long)
					    ioremap_nocache(phys_addr,
							    resource->res_size);

					pr_debug("[%s]: mapping kernel- guid %x, hdl %x - non cached mapping %u\n",
						__func__, ioparam->handle,
						lock.res_handle, ioparam->addr);
				}

				map->res_pid = 0;
				map->res_vc_hdl = resource->res_handle;
				map->res_usr_hdl = resource->res_guid;
				map->res_addr = ioparam->addr;
				map->resource = resource;
				map->vma = NULL;

				vmcs_sm_add_map(sm_state, resource, map);
			}
		} else
			ioparam->addr = 0;
	}

error:
	if (resource)
		vmcs_sm_release_resource(resource, 0);

	return ret;
}

/* Unlock a previously allocated shared memory handle and block.*/
static int vc_sm_ioctl_unlock(struct sm_priv_data_t *private,
			      struct vmcs_sm_ioctl_lock_unlock *ioparam,
			      int flush, int wait_reply, int no_vc_unlock)
{
	int status;
	struct vc_sm_lock_unlock_t unlock;
	struct sm_mmap *map, *map_tmp;
	struct sm_resource_t *resource;
	int ret = 0;

	map = NULL;

	/* Locate resource from GUID. */
	resource = vmcs_sm_acquire_resource(private, ioparam->handle);
	if (resource == NULL) {
		ret = -EINVAL;
		goto error;
	}

	/* Check permissions. */
	if (resource->pid && (resource->pid != current->tgid)) {
		pr_err("[%s]: current tgid %u != %u owner\n",
			__func__, current->tgid, resource->pid);
		ret = -EPERM;
		goto error;
	}

	unlock.res_handle = resource->res_handle;
	unlock.res_mem = (uint32_t)resource->res_base_mem;

	pr_debug("[%s]: attempt to unlock data - guid %x, hdl %x, base address %p\n",
		__func__, ioparam->handle, unlock.res_handle,
		(void *)unlock.res_mem);

	/* User space allocated resources. */
	if (resource->pid) {
		/* Flush if requested */
		if (resource->res_cached && flush) {
			dma_addr_t phys_addr = 0;

			resource->res_stats[FLUSH]++;

			phys_addr =
			    (dma_addr_t)((uint32_t)resource->res_base_mem &
					 0x3FFFFFFF);
			phys_addr += (dma_addr_t)mm_vc_mem_phys_addr;

			/* L1 cache flush */
			down_read(&current->mm->mmap_sem);
			list_for_each_entry(map, &resource->map_list,
					    resource_map_list) {
				if (map->vma) {
					const unsigned long start = map->vma->vm_start;
					const unsigned long end = map->vma->vm_end;

					ret = clean_invalid_mem_walk(start, end - start,
							VCSM_CACHE_OP_FLUSH);
					if (ret)
						goto error;
				}
			}
			up_read(&current->mm->mmap_sem);

			/* L2 cache flush */
			outer_clean_range(phys_addr,
					  phys_addr +
					  (size_t) resource->res_size);
		}

		/* We need to zap all the vmas associated with this resource */
		if (resource->lock_count == 1) {
			down_read(&current->mm->mmap_sem);
			list_for_each_entry(map, &resource->map_list,
					    resource_map_list) {
				if (map->vma) {
					zap_vma_ptes(map->vma,
						     map->vma->vm_start,
						     map->vma->vm_end -
						     map->vma->vm_start);
				}
			}
			up_read(&current->mm->mmap_sem);
		}
	}
	/* Kernel allocated resources. */
	else {
		/* Global + Taken in this context */
		if (resource->ref_count == 2) {
			if (!list_empty(&resource->map_list)) {
				list_for_each_entry_safe(map, map_tmp,
						&resource->map_list,
						resource_map_list) {
					if (map->res_addr) {
						if (flush &&
								(resource->res_cached ==
									VMCS_SM_CACHE_HOST)) {
							unsigned long
								phys_addr;
							phys_addr = (uint32_t)
								resource->res_base_mem & 0x3FFFFFFF;
							phys_addr +=
								mm_vc_mem_phys_addr;

							/* L1 cache flush */
							dmac_flush_range((const
										void
										*)
									map->res_addr, (const void *)
									(map->res_addr + resource->res_size));

							/* L2 cache flush */
							outer_clean_range
								(phys_addr,
								 phys_addr +
								 (size_t)
								 resource->res_size);
						}

						iounmap((void *)map->res_addr);
						map->res_addr = 0;

						vmcs_sm_remove_map(sm_state,
								map->resource,
								map);
						break;
					}
				}
			}
		}
	}

	if (resource->lock_count) {
		/* Bypass the videocore unlock. */
		if (no_vc_unlock)
			status = 0;
		/* Unlock the videocore allocated resource. */
		else {
			status =
			    vc_vchi_sm_unlock(sm_state->sm_handle, &unlock,
					      &private->int_trans_id,
					      wait_reply);
			if (status == -EINTR) {
				pr_debug("[%s]: requesting unlock memory action restart (trans_id: %u)\n",
					__func__, private->int_trans_id);

				ret = -ERESTARTSYS;
				resource->res_stats[UNLOCK]--;
				private->restart_sys = -EINTR;
				private->int_action = VC_SM_MSG_TYPE_UNLOCK;
				goto error;
			} else if (status != 0) {
				pr_err("[%s]: failed to unlock vc mem (status: %u, trans_id: %u)\n",
				     __func__, status, private->int_trans_id);

				ret = -EPERM;
				resource->res_stats[UNLOCK_FAIL]++;
				goto error;
			}
		}

		resource->res_stats[UNLOCK]++;
		resource->lock_count--;
	}

	pr_debug("[%s]: success to unlock data - hdl %x, base address %p, ref-cnt %d\n",
		__func__, unlock.res_handle, (void *)unlock.res_mem,
		resource->lock_count);

error:
	if (resource)
		vmcs_sm_release_resource(resource, 0);

	return ret;
}

/* Import a contiguous block of memory to be shared with VC. */
static int vc_sm_ioctl_import_dmabuf(struct sm_priv_data_t *private,
				     struct vmcs_sm_ioctl_import_dmabuf *ioparam,
				     struct dma_buf *src_dma_buf)
{
	int ret = 0;
	int status;
	struct sm_resource_t *resource = NULL;
	struct vc_sm_import import = { 0 };
	struct vc_sm_import_result result = { 0 };
	struct dma_buf *dma_buf;
	struct dma_buf_attachment *attach = NULL;
	struct sg_table *sgt = NULL;

	/* Setup our allocation parameters */
	if (src_dma_buf) {
		get_dma_buf(src_dma_buf);
		dma_buf = src_dma_buf;
	} else {
		dma_buf = dma_buf_get(ioparam->dmabuf_fd);
	}
	if (IS_ERR(dma_buf))
		return PTR_ERR(dma_buf);

	attach = dma_buf_attach(dma_buf, &sm_state->pdev->dev);
	if (IS_ERR(attach)) {
		ret = PTR_ERR(attach);
		goto error;
	}

	sgt = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto error;
	}

	/* Verify that the address block is contiguous */
	if (sgt->nents != 1) {
		ret = -ENOMEM;
		goto error;
	}

	import.type = ((ioparam->cached == VMCS_SM_CACHE_VC) ||
		       (ioparam->cached == VMCS_SM_CACHE_BOTH)) ?
				VC_SM_ALLOC_CACHED : VC_SM_ALLOC_NON_CACHED;
	import.addr = (uint32_t)sg_dma_address(sgt->sgl);
	import.size = sg_dma_len(sgt->sgl);
	import.allocator = current->tgid;

	if (*ioparam->name)
		memcpy(import.name, ioparam->name, sizeof(import.name) - 1);
	else
		memcpy(import.name, VMCS_SM_RESOURCE_NAME_DEFAULT,
		       sizeof(VMCS_SM_RESOURCE_NAME_DEFAULT));

	pr_debug("[%s]: attempt to import \"%s\" data - type %u, addr %p, size %u\n",
		 __func__, import.name, import.type,
		 (void *)import.addr, import.size);

	/* Allocate local resource to track this allocation. */
	resource = kzalloc(sizeof(*resource), GFP_KERNEL);
	if (!resource) {
		ret = -ENOMEM;
		goto error;
	}
	INIT_LIST_HEAD(&resource->map_list);
	resource->ref_count++;
	resource->pid = current->tgid;

	/* Allocate the videocore resource. */
	status = vc_vchi_sm_import(sm_state->sm_handle, &import, &result,
				   &private->int_trans_id);
	if (status == -EINTR) {
		pr_debug("[%s]: requesting import memory action restart (trans_id: %u)\n",
			 __func__, private->int_trans_id);
		ret = -ERESTARTSYS;
		private->restart_sys = -EINTR;
		private->int_action = VC_SM_MSG_TYPE_IMPORT;
		goto error;
	} else if (status || !result.res_handle) {
		pr_debug("[%s]: failed to import memory on videocore (status: %u, trans_id: %u)\n",
			 __func__, status, private->int_trans_id);
		ret = -ENOMEM;
		resource->res_stats[ALLOC_FAIL]++;
		goto error;
	}

	/* Keep track of the resource we created. */
	resource->private = private;
	resource->res_handle = result.res_handle;
	resource->res_size = import.size;
	resource->res_cached = ioparam->cached;

	resource->dma_buf = dma_buf;
	resource->attach = attach;
	resource->sgt = sgt;
	resource->dma_addr = sg_dma_address(sgt->sgl);

	/*
	 * Kernel/user GUID.  This global identifier is used for mmap'ing the
	 * allocated region from user space, it is passed as the mmap'ing
	 * offset, we use it to 'hide' the videocore handle/address.
	 */
	mutex_lock(&sm_state->lock);
	resource->res_guid = ++sm_state->guid;
	mutex_unlock(&sm_state->lock);
	resource->res_guid <<= PAGE_SHIFT;

	vmcs_sm_add_resource(private, resource);

	/* We're done */
	resource->res_stats[IMPORT]++;
	ioparam->handle = resource->res_guid;
	return 0;

error:
	if (resource) {
		resource->res_stats[IMPORT_FAIL]++;
		vc_sm_resource_deceased(resource, 1);
		kfree(resource);
	}
	if (sgt)
		dma_buf_unmap_attachment(attach, sgt, DMA_BIDIRECTIONAL);
	if (attach)
		dma_buf_detach(dma_buf, attach);
	dma_buf_put(dma_buf);
	return ret;
}

/* Handle control from host. */
static long vc_sm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	unsigned int cmdnr = _IOC_NR(cmd);
	struct sm_priv_data_t *file_data =
	    (struct sm_priv_data_t *)file->private_data;
	struct sm_resource_t *resource = NULL;

	/* Validate we can work with this device. */
	if ((sm_state == NULL) || (file_data == NULL)) {
		pr_err("[%s]: invalid device\n", __func__);
		ret = -EPERM;
		goto out;
	}

	pr_debug("[%s]: cmd %x tgid %u, owner %u\n", __func__, cmdnr,
			current->tgid, file_data->pid);

	/* Action is a re-post of a previously interrupted action? */
	if (file_data->restart_sys == -EINTR) {
		struct vc_sm_action_clean_t action_clean;

		pr_debug("[%s]: clean up of action %u (trans_id: %u) following EINTR\n",
			__func__, file_data->int_action,
			file_data->int_trans_id);

		action_clean.res_action = file_data->int_action;
		action_clean.action_trans_id = file_data->int_trans_id;

		vc_vchi_sm_clean_up(sm_state->sm_handle, &action_clean);

		file_data->restart_sys = 0;
	}

	/* Now process the command. */
	switch (cmdnr) {
		/* New memory allocation.
		 */
	case VMCS_SM_CMD_ALLOC:
		{
			struct vmcs_sm_ioctl_alloc ioparam;

			/* Get the parameter data. */
			if (copy_from_user
			    (&ioparam, (void *)arg, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-from-user for cmd %x\n",
						__func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}

			ret = vc_sm_ioctl_alloc(file_data, &ioparam);
			if (!ret &&
			    (copy_to_user((void *)arg,
					  &ioparam, sizeof(ioparam)) != 0)) {
				struct vmcs_sm_ioctl_free freeparam = {
					ioparam.handle
				};
				pr_err("[%s]: failed to copy-to-user for cmd %x\n",
						__func__, cmdnr);
				vc_sm_ioctl_free(file_data, &freeparam);
				ret = -EFAULT;
			}

			/* Done. */
			goto out;
		}
		break;

		/* Share existing memory allocation. */
	case VMCS_SM_CMD_ALLOC_SHARE:
		{
			struct vmcs_sm_ioctl_alloc_share ioparam;

			/* Get the parameter data. */
			if (copy_from_user
			    (&ioparam, (void *)arg, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-from-user for cmd %x\n",
						__func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}

			ret = vc_sm_ioctl_alloc_share(file_data, &ioparam);

			/* Copy result back to user. */
			if (!ret
			    && copy_to_user((void *)arg, &ioparam,
					    sizeof(ioparam)) != 0) {
				struct vmcs_sm_ioctl_free freeparam = {
					ioparam.handle
				};
				pr_err("[%s]: failed to copy-to-user for cmd %x\n",
						__func__, cmdnr);
				vc_sm_ioctl_free(file_data, &freeparam);
				ret = -EFAULT;
			}

			/* Done. */
			goto out;
		}
		break;

	case VMCS_SM_CMD_IMPORT_DMABUF:
		{
			struct vmcs_sm_ioctl_import_dmabuf ioparam;

			/* Get the parameter data. */
			if (copy_from_user
			    (&ioparam, (void *)arg, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-from-user for cmd %x\n",
				       __func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}

			ret = vc_sm_ioctl_import_dmabuf(file_data, &ioparam,
							NULL);
			if (!ret &&
			    (copy_to_user((void *)arg,
					  &ioparam, sizeof(ioparam)) != 0)) {
				struct vmcs_sm_ioctl_free freeparam = {
					ioparam.handle
				};
				pr_err("[%s]: failed to copy-to-user for cmd %x\n",
				       __func__, cmdnr);
				vc_sm_ioctl_free(file_data, &freeparam);
				ret = -EFAULT;
			}

			/* Done. */
			goto out;
		}
		break;

		/* Lock (attempt to) *and* register a cache behavior change. */
	case VMCS_SM_CMD_LOCK_CACHE:
		{
			struct vmcs_sm_ioctl_lock_cache ioparam;
			struct vmcs_sm_ioctl_lock_unlock lock;

			/* Get parameter data. */
			if (copy_from_user
			    (&ioparam, (void *)arg, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-from-user for cmd %x\n",
						__func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}

			lock.handle = ioparam.handle;
			ret =
			    vc_sm_ioctl_lock(file_data, &lock, 1,
					     ioparam.cached, 0);

			/* Done. */
			goto out;
		}
		break;

		/* Lock (attempt to) existing memory allocation. */
	case VMCS_SM_CMD_LOCK:
		{
			struct vmcs_sm_ioctl_lock_unlock ioparam;

			/* Get parameter data. */
			if (copy_from_user
			    (&ioparam, (void *)arg, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-from-user for cmd %x\n",
						__func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}

			ret = vc_sm_ioctl_lock(file_data, &ioparam, 0, 0, 0);

			/* Copy result back to user. */
			if (copy_to_user((void *)arg, &ioparam, sizeof(ioparam))
			    != 0) {
				pr_err("[%s]: failed to copy-to-user for cmd %x\n",
				     __func__, cmdnr);
				ret = -EFAULT;
			}

			/* Done. */
			goto out;
		}
		break;

		/* Unlock (attempt to) existing memory allocation. */
	case VMCS_SM_CMD_UNLOCK:
		{
			struct vmcs_sm_ioctl_lock_unlock ioparam;

			/* Get parameter data. */
			if (copy_from_user
			    (&ioparam, (void *)arg, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-from-user for cmd %x\n",
						__func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}

			ret = vc_sm_ioctl_unlock(file_data, &ioparam, 0, 1, 0);

			/* Done. */
			goto out;
		}
		break;

		/* Resize (attempt to) existing memory allocation. */
	case VMCS_SM_CMD_RESIZE:
		{
			struct vmcs_sm_ioctl_resize ioparam;

			/* Get parameter data. */
			if (copy_from_user
			    (&ioparam, (void *)arg, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-from-user for cmd %x\n",
						__func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}

			ret = vc_sm_ioctl_resize(file_data, &ioparam);

			/* Copy result back to user. */
			if (copy_to_user((void *)arg, &ioparam, sizeof(ioparam))
			    != 0) {
				pr_err("[%s]: failed to copy-to-user for cmd %x\n",
				     __func__, cmdnr);
				ret = -EFAULT;
			}
			goto out;
		}
		break;

		/* Terminate existing memory allocation.
		 */
	case VMCS_SM_CMD_FREE:
		{
			struct vmcs_sm_ioctl_free ioparam;

			/* Get parameter data.
			 */
			if (copy_from_user
			    (&ioparam, (void *)arg, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-from-user for cmd %x\n",
						__func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}

			ret = vc_sm_ioctl_free(file_data, &ioparam);

			/* Done.
			 */
			goto out;
		}
		break;

		/* Walk allocation on videocore, information shows up in the
		 ** videocore log.
		 */
	case VMCS_SM_CMD_VC_WALK_ALLOC:
		{
			pr_debug("[%s]: invoking walk alloc\n", __func__);

			if (vc_vchi_sm_walk_alloc(sm_state->sm_handle) != 0)
				pr_err("[%s]: failed to walk-alloc on videocore\n",
				     __func__);

			/* Done.
			 */
			goto out;
		}
		break;
		/* Walk mapping table on host, information shows up in the
		 ** kernel log.
		 */
	case VMCS_SM_CMD_HOST_WALK_MAP:
		{
			/* Use pid of -1 to tell to walk the whole map. */
			vmcs_sm_host_walk_map_per_pid(-1);

			/* Done. */
			goto out;
		}
		break;

		/* Walk mapping table per process on host.  */
	case VMCS_SM_CMD_HOST_WALK_PID_ALLOC:
		{
			struct vmcs_sm_ioctl_walk ioparam;

			/* Get parameter data.  */
			if (copy_from_user(&ioparam,
					   (void *)arg, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-from-user for cmd %x\n",
						__func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}

			vmcs_sm_host_walk_alloc(file_data);

			/* Done. */
			goto out;
		}
		break;

		/* Walk allocation per process on host.  */
	case VMCS_SM_CMD_HOST_WALK_PID_MAP:
		{
			struct vmcs_sm_ioctl_walk ioparam;

			/* Get parameter data. */
			if (copy_from_user(&ioparam,
					   (void *)arg, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-from-user for cmd %x\n",
						__func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}

			vmcs_sm_host_walk_map_per_pid(ioparam.pid);

			/* Done. */
			goto out;
		}
		break;

		/* Gets the size of the memory associated with a user handle. */
	case VMCS_SM_CMD_SIZE_USR_HANDLE:
		{
			struct vmcs_sm_ioctl_size ioparam;

			/* Get parameter data. */
			if (copy_from_user(&ioparam,
					   (void *)arg, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-from-user for cmd %x\n",
						__func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}

			/* Locate resource from GUID. */
			resource =
			    vmcs_sm_acquire_resource(file_data, ioparam.handle);
			if (resource != NULL) {
				ioparam.size = resource->res_size;
				vmcs_sm_release_resource(resource, 0);
			} else {
				ioparam.size = 0;
			}

			if (copy_to_user((void *)arg,
					 &ioparam, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-to-user for cmd %x\n",
				     __func__, cmdnr);
				ret = -EFAULT;
			}

			/* Done. */
			goto out;
		}
		break;

		/* Verify we are dealing with a valid resource. */
	case VMCS_SM_CMD_CHK_USR_HANDLE:
		{
			struct vmcs_sm_ioctl_chk ioparam;

			/* Get parameter data. */
			if (copy_from_user(&ioparam,
					   (void *)arg, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-from-user for cmd %x\n",
						__func__, cmdnr);

				ret = -EFAULT;
				goto out;
			}

			/* Locate resource from GUID. */
			resource =
			    vmcs_sm_acquire_resource(file_data, ioparam.handle);
			if (resource == NULL)
				ret = -EINVAL;
			/*
			 * If the resource is cacheable, return additional
			 * information that may be needed to flush the cache.
			 */
			else if ((resource->res_cached == VMCS_SM_CACHE_HOST) ||
				 (resource->res_cached == VMCS_SM_CACHE_BOTH)) {
				ioparam.addr =
				    vmcs_sm_usr_address_from_pid_and_usr_handle
				    (current->tgid, ioparam.handle);
				ioparam.size = resource->res_size;
				ioparam.cache = resource->res_cached;
			} else {
				ioparam.addr = 0;
				ioparam.size = 0;
				ioparam.cache = resource->res_cached;
			}

			if (resource)
				vmcs_sm_release_resource(resource, 0);

			if (copy_to_user((void *)arg,
					 &ioparam, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-to-user for cmd %x\n",
						__func__, cmdnr);
				ret = -EFAULT;
			}

			/* Done. */
			goto out;
		}
		break;

		/*
		 * Maps a user handle given the process and the virtual address.
		 */
	case VMCS_SM_CMD_MAPPED_USR_HANDLE:
		{
			struct vmcs_sm_ioctl_map ioparam;

			/* Get parameter data. */
			if (copy_from_user(&ioparam,
					   (void *)arg, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-from-user for cmd %x\n",
						__func__, cmdnr);

				ret = -EFAULT;
				goto out;
			}

			ioparam.handle =
			    vmcs_sm_usr_handle_from_pid_and_address(
					    ioparam.pid, ioparam.addr);

			resource =
			    vmcs_sm_acquire_resource(file_data, ioparam.handle);
			if ((resource != NULL)
			    && ((resource->res_cached == VMCS_SM_CACHE_HOST)
				|| (resource->res_cached ==
				    VMCS_SM_CACHE_BOTH))) {
				ioparam.size = resource->res_size;
			} else {
				ioparam.size = 0;
			}

			if (resource)
				vmcs_sm_release_resource(resource, 0);

			if (copy_to_user((void *)arg,
					 &ioparam, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-to-user for cmd %x\n",
				     __func__, cmdnr);
				ret = -EFAULT;
			}

			/* Done. */
			goto out;
		}
		break;

		/*
		 * Maps a videocore handle given process and virtual address.
		 */
	case VMCS_SM_CMD_MAPPED_VC_HDL_FROM_ADDR:
		{
			struct vmcs_sm_ioctl_map ioparam;

			/* Get parameter data. */
			if (copy_from_user(&ioparam,
					   (void *)arg, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-from-user for cmd %x\n",
						__func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}

			ioparam.handle = vmcs_sm_vc_handle_from_pid_and_address(
					    ioparam.pid, ioparam.addr);

			if (copy_to_user((void *)arg,
					 &ioparam, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-to-user for cmd %x\n",
				     __func__, cmdnr);

				ret = -EFAULT;
			}

			/* Done. */
			goto out;
		}
		break;

		/* Maps a videocore handle given process and user handle. */
	case VMCS_SM_CMD_MAPPED_VC_HDL_FROM_HDL:
		{
			struct vmcs_sm_ioctl_map ioparam;

			/* Get parameter data. */
			if (copy_from_user(&ioparam,
					   (void *)arg, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-from-user for cmd %x\n",
						__func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}

			/* Locate resource from GUID. */
			resource =
			    vmcs_sm_acquire_resource(file_data, ioparam.handle);
			if (resource != NULL) {
				ioparam.handle = resource->res_handle;
				vmcs_sm_release_resource(resource, 0);
			} else {
				ioparam.handle = 0;
			}

			if (copy_to_user((void *)arg,
					 &ioparam, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-to-user for cmd %x\n",
				     __func__, cmdnr);

				ret = -EFAULT;
			}

			/* Done. */
			goto out;
		}
		break;

		/*
		 * Maps a videocore address given process and videocore handle.
		 */
	case VMCS_SM_CMD_MAPPED_VC_ADDR_FROM_HDL:
		{
			struct vmcs_sm_ioctl_map ioparam;

			/* Get parameter data. */
			if (copy_from_user(&ioparam,
					   (void *)arg, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-from-user for cmd %x\n",
						__func__, cmdnr);

				ret = -EFAULT;
				goto out;
			}

			/* Locate resource from GUID. */
			resource =
			    vmcs_sm_acquire_resource(file_data, ioparam.handle);
			if (resource != NULL) {
				ioparam.addr =
					(unsigned int)resource->res_base_mem;
				vmcs_sm_release_resource(resource, 0);
			} else {
				ioparam.addr = 0;
			}

			if (copy_to_user((void *)arg,
					 &ioparam, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-to-user for cmd %x\n",
				     __func__, cmdnr);
				ret = -EFAULT;
			}

			/* Done. */
			goto out;
		}
		break;

		/* Maps a user address given process and vc handle. */
	case VMCS_SM_CMD_MAPPED_USR_ADDRESS:
		{
			struct vmcs_sm_ioctl_map ioparam;

			/* Get parameter data. */
			if (copy_from_user(&ioparam,
					   (void *)arg, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-from-user for cmd %x\n",
						__func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}

			/*
			 * Return the address information from the mapping,
			 * 0 (ie NULL) if it cannot locate the actual mapping.
			 */
			ioparam.addr =
			    vmcs_sm_usr_address_from_pid_and_usr_handle
			    (ioparam.pid, ioparam.handle);

			if (copy_to_user((void *)arg,
					 &ioparam, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-to-user for cmd %x\n",
				     __func__, cmdnr);
				ret = -EFAULT;
			}

			/* Done. */
			goto out;
		}
		break;

		/* Flush the cache for a given mapping. */
	case VMCS_SM_CMD_FLUSH:
		{
			struct vmcs_sm_ioctl_cache ioparam;

			/* Get parameter data. */
			if (copy_from_user(&ioparam,
					   (void *)arg, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-from-user for cmd %x\n",
						__func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}

			/* Locate resource from GUID. */
			resource =
			    vmcs_sm_acquire_resource(file_data, ioparam.handle);
			if (resource == NULL) {
				ret = -EINVAL;
				goto out;
			}

			ret = clean_invalid_resource_walk((void __user*) ioparam.addr,
					ioparam.size, VCSM_CACHE_OP_FLUSH, ioparam.handle,
					resource);
			vmcs_sm_release_resource(resource, 0);
			if (ret)
				goto out;
		}
		break;

		/* Invalidate the cache for a given mapping. */
	case VMCS_SM_CMD_INVALID:
		{
			struct vmcs_sm_ioctl_cache ioparam;

			/* Get parameter data. */
			if (copy_from_user(&ioparam,
					   (void *)arg, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-from-user for cmd %x\n",
						__func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}

			/* Locate resource from GUID. */
			resource =
			    vmcs_sm_acquire_resource(file_data, ioparam.handle);
			if (resource == NULL) {
				ret = -EINVAL;
				goto out;
			}

			ret = clean_invalid_resource_walk((void __user*) ioparam.addr,
					ioparam.size, VCSM_CACHE_OP_INV, ioparam.handle, resource);
			vmcs_sm_release_resource(resource, 0);
			if (ret)
				goto out;
		}
		break;

	/* Flush/Invalidate the cache for a given mapping. */
	case VMCS_SM_CMD_CLEAN_INVALID:
		{
			int i;
			struct vmcs_sm_ioctl_clean_invalid ioparam;

			/* Get parameter data. */
			if (copy_from_user(&ioparam,
					   (void *)arg, sizeof(ioparam)) != 0) {
				pr_err("[%s]: failed to copy-from-user for cmd %x\n",
						__func__, cmdnr);
				ret = -EFAULT;
				goto out;
			}
			for (i = 0; i < sizeof(ioparam.s) / sizeof(*ioparam.s); i++) {
				if (ioparam.s[i].cmd == VCSM_CACHE_OP_NOP)
					break;

				/* Locate resource from GUID. */
				resource =
					vmcs_sm_acquire_resource(file_data, ioparam.s[i].handle);
				if (resource == NULL) {
					ret = -EINVAL;
					goto out;
				}

				ret = clean_invalid_resource_walk(
						(void __user*) ioparam.s[i].addr, ioparam.s[i].size,
						ioparam.s[i].cmd, ioparam.s[i].handle, resource);
				vmcs_sm_release_resource(resource, 0);
				if (ret)
					goto out;
			}
		}
		break;
	/*
	 * Flush/Invalidate the cache for a given mapping.
	 * Blocks must be pinned (i.e. accessed) before this call.
	 */
	case VMCS_SM_CMD_CLEAN_INVALID2:
		{
				int i;
				struct vmcs_sm_ioctl_clean_invalid2 ioparam;
				struct vmcs_sm_ioctl_clean_invalid_block *block = NULL;

				/* Get parameter data. */
				if (copy_from_user(&ioparam,
						   (void *)arg, sizeof(ioparam)) != 0) {
					pr_err("[%s]: failed to copy-from-user header for cmd %x\n",
							__func__, cmdnr);
					ret = -EFAULT;
					goto out;
				}
				block = kmalloc(ioparam.op_count *
						sizeof(struct vmcs_sm_ioctl_clean_invalid_block),
						GFP_KERNEL);
				if (!block) {
					ret = -EFAULT;
					goto out;
				}
				if (copy_from_user(block,
						   (void *)(arg + sizeof(ioparam)), ioparam.op_count * sizeof(struct vmcs_sm_ioctl_clean_invalid_block)) != 0) {
					pr_err("[%s]: failed to copy-from-user payload for cmd %x\n",
							__func__, cmdnr);
					ret = -EFAULT;
					goto out;
				}

				for (i = 0; i < ioparam.op_count; i++) {
					const struct vmcs_sm_ioctl_clean_invalid_block * const op = block + i;

					if (op->invalidate_mode == VCSM_CACHE_OP_NOP)
						continue;

					ret = clean_invalid_contiguous_mem_2d(
							(void __user*) op->start_address, op->block_count,
							op->block_size, op->inter_block_stride,
							op->invalidate_mode);
					if (ret)
						break;
				}
				kfree(block);
			}
		break;

	default:
		{
			ret = -EINVAL;
			goto out;
		}
		break;
	}

out:
	return ret;
}

/* Device operations that we managed in this driver. */
static const struct file_operations vmcs_sm_ops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = vc_sm_ioctl,
	.open = vc_sm_open,
	.release = vc_sm_release,
	.mmap = vc_sm_mmap,
};

/* Creation of device. */
static int vc_sm_create_sharedmemory(void)
{
	int ret;

	if (sm_state == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	/* Create a device class for creating dev nodes. */
	sm_state->sm_class = class_create(THIS_MODULE, "vc-sm");
	if (IS_ERR(sm_state->sm_class)) {
		pr_err("[%s]: unable to create device class\n", __func__);
		ret = PTR_ERR(sm_state->sm_class);
		goto out;
	}

	/* Create a character driver. */
	ret = alloc_chrdev_region(&sm_state->sm_devid,
				  DEVICE_MINOR, 1, DEVICE_NAME);
	if (ret != 0) {
		pr_err("[%s]: unable to allocate device number\n", __func__);
		goto out_dev_class_destroy;
	}

	cdev_init(&sm_state->sm_cdev, &vmcs_sm_ops);
	ret = cdev_add(&sm_state->sm_cdev, sm_state->sm_devid, 1);
	if (ret != 0) {
		pr_err("[%s]: unable to register device\n", __func__);
		goto out_chrdev_unreg;
	}

	/* Create a device node. */
	sm_state->sm_dev = device_create(sm_state->sm_class,
					 NULL,
					 MKDEV(MAJOR(sm_state->sm_devid),
					       DEVICE_MINOR), NULL,
					 DEVICE_NAME);
	if (IS_ERR(sm_state->sm_dev)) {
		pr_err("[%s]: unable to create device node\n", __func__);
		ret = PTR_ERR(sm_state->sm_dev);
		goto out_chrdev_del;
	}

	goto out;

out_chrdev_del:
	cdev_del(&sm_state->sm_cdev);
out_chrdev_unreg:
	unregister_chrdev_region(sm_state->sm_devid, 1);
out_dev_class_destroy:
	class_destroy(sm_state->sm_class);
	sm_state->sm_class = NULL;
out:
	return ret;
}

/* Termination of the device. */
static int vc_sm_remove_sharedmemory(void)
{
	int ret;

	if (sm_state == NULL) {
		/* Nothing to do. */
		ret = 0;
		goto out;
	}

	/* Remove the sharedmemory character driver. */
	cdev_del(&sm_state->sm_cdev);

	/* Unregister region. */
	unregister_chrdev_region(sm_state->sm_devid, 1);

	ret = 0;
	goto out;

out:
	return ret;
}

/* Videocore connected.  */
static void vc_sm_connected_init(void)
{
	int ret;
	VCHI_INSTANCE_T vchi_instance;

	pr_info("[%s]: start\n", __func__);

	/*
	 * Initialize and create a VCHI connection for the shared memory service
	 * running on videocore.
	 */
	ret = vchi_initialise(&vchi_instance);
	if (ret != 0) {
		pr_err("[%s]: failed to initialise VCHI instance (ret=%d)\n",
			__func__, ret);

		ret = -EIO;
		goto err_free_mem;
	}

	ret = vchi_connect(vchi_instance);
	if (ret != 0) {
		pr_err("[%s]: failed to connect VCHI instance (ret=%d)\n",
			__func__, ret);

		ret = -EIO;
		goto err_free_mem;
	}

	/* Initialize an instance of the shared memory service. */
	sm_state->sm_handle =
	    vc_vchi_sm_init(vchi_instance);
	if (sm_state->sm_handle == NULL) {
		pr_err("[%s]: failed to initialize shared memory service\n",
			__func__);

		ret = -EPERM;
		goto err_free_mem;
	}

	/* Create a debug fs directory entry (root). */
	sm_state->dir_root = debugfs_create_dir(VC_SM_DIR_ROOT_NAME, NULL);
	if (!sm_state->dir_root) {
		pr_err("[%s]: failed to create \'%s\' directory entry\n",
			__func__, VC_SM_DIR_ROOT_NAME);

		ret = -EPERM;
		goto err_stop_sm_service;
	}

	sm_state->dir_state.show = &vc_sm_global_state_show;
	sm_state->dir_state.dir_entry = debugfs_create_file(VC_SM_STATE,
			0444, sm_state->dir_root, &sm_state->dir_state,
			&vc_sm_debug_fs_fops);

	sm_state->dir_stats.show = &vc_sm_global_statistics_show;
	sm_state->dir_stats.dir_entry = debugfs_create_file(VC_SM_STATS,
			0444, sm_state->dir_root, &sm_state->dir_stats,
			&vc_sm_debug_fs_fops);

	/* Create the proc entry children. */
	sm_state->dir_alloc = debugfs_create_dir(VC_SM_DIR_ALLOC_NAME,
			sm_state->dir_root);

	/* Create a shared memory device. */
	ret = vc_sm_create_sharedmemory();
	if (ret != 0) {
		pr_err("[%s]: failed to create shared memory device\n",
			__func__);
		goto err_remove_debugfs;
	}

	INIT_LIST_HEAD(&sm_state->map_list);
	INIT_LIST_HEAD(&sm_state->resource_list);

	sm_state->data_knl = vc_sm_create_priv_data(0);
	if (sm_state->data_knl == NULL) {
		pr_err("[%s]: failed to create kernel private data tracker\n",
			__func__);
		goto err_remove_shared_memory;
	}

	/* Done! */
	sm_inited = 1;
	goto out;

err_remove_shared_memory:
	vc_sm_remove_sharedmemory();
err_remove_debugfs:
	debugfs_remove_recursive(sm_state->dir_root);
err_stop_sm_service:
	vc_vchi_sm_stop(&sm_state->sm_handle);
err_free_mem:
	kfree(sm_state);
out:
	pr_info("[%s]: end - returning %d\n", __func__, ret);
}

/* Driver loading. */
static int bcm2835_vcsm_probe(struct platform_device *pdev)
{
	pr_info("vc-sm: Videocore shared memory driver\n");

	sm_state = kzalloc(sizeof(*sm_state), GFP_KERNEL);
	if (!sm_state)
		return -ENOMEM;
	sm_state->pdev = pdev;
	mutex_init(&sm_state->lock);
	mutex_init(&sm_state->map_lock);

	vchiq_add_connected_callback(vc_sm_connected_init);
	return 0;
}

/* Driver unloading. */
static int bcm2835_vcsm_remove(struct platform_device *pdev)
{
	pr_debug("[%s]: start\n", __func__);
	if (sm_inited) {
		/* Remove shared memory device. */
		vc_sm_remove_sharedmemory();

		/* Remove all proc entries. */
		debugfs_remove_recursive(sm_state->dir_root);

		/* Stop the videocore shared memory service. */
		vc_vchi_sm_stop(&sm_state->sm_handle);

		/* Free the memory for the state structure. */
		mutex_destroy(&(sm_state->map_lock));
		kfree(sm_state);
	}

	pr_debug("[%s]: end\n", __func__);
	return 0;
}

#if defined(__KERNEL__)
/* Allocate a shared memory handle and block. */
int vc_sm_alloc(struct vc_sm_alloc_t *alloc, int *handle)
{
	struct vmcs_sm_ioctl_alloc ioparam = { 0 };
	int ret;
	struct sm_resource_t *resource;

	/* Validate we can work with this device. */
	if (sm_state == NULL || alloc == NULL || handle == NULL) {
		pr_err("[%s]: invalid input\n", __func__);
		return -EPERM;
	}

	ioparam.size = alloc->base_unit;
	ioparam.num = alloc->num_unit;
	ioparam.cached =
	    alloc->type == VC_SM_ALLOC_CACHED ? VMCS_SM_CACHE_VC : 0;

	ret = vc_sm_ioctl_alloc(sm_state->data_knl, &ioparam);

	if (ret == 0) {
		resource =
		    vmcs_sm_acquire_resource(sm_state->data_knl,
					     ioparam.handle);
		if (resource) {
			resource->pid = 0;
			vmcs_sm_release_resource(resource, 0);

			/* Assign valid handle at this time. */
			*handle = ioparam.handle;
		} else {
			ret = -ENOMEM;
		}
	}

	return ret;
}
EXPORT_SYMBOL_GPL(vc_sm_alloc);

/* Get an internal resource handle mapped from the external one. */
int vc_sm_int_handle(int handle)
{
	struct sm_resource_t *resource;
	int ret = 0;

	/* Validate we can work with this device. */
	if (sm_state == NULL || handle == 0) {
		pr_err("[%s]: invalid input\n", __func__);
		return 0;
	}

	/* Locate resource from GUID. */
	resource = vmcs_sm_acquire_resource(sm_state->data_knl, handle);
	if (resource) {
		ret = resource->res_handle;
		vmcs_sm_release_resource(resource, 0);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(vc_sm_int_handle);

/* Free a previously allocated shared memory handle and block. */
int vc_sm_free(int handle)
{
	struct vmcs_sm_ioctl_free ioparam = { handle };

	/* Validate we can work with this device. */
	if (sm_state == NULL || handle == 0) {
		pr_err("[%s]: invalid input\n", __func__);
		return -EPERM;
	}

	return vc_sm_ioctl_free(sm_state->data_knl, &ioparam);
}
EXPORT_SYMBOL_GPL(vc_sm_free);

/* Lock a memory handle for use by kernel. */
int vc_sm_lock(int handle, enum vc_sm_lock_cache_mode mode,
	       unsigned long *data)
{
	struct vmcs_sm_ioctl_lock_unlock ioparam;
	int ret;

	/* Validate we can work with this device. */
	if (sm_state == NULL || handle == 0 || data == NULL) {
		pr_err("[%s]: invalid input\n", __func__);
		return -EPERM;
	}

	*data = 0;

	ioparam.handle = handle;
	ret = vc_sm_ioctl_lock(sm_state->data_knl,
			       &ioparam,
			       1,
			       ((mode ==
				 VC_SM_LOCK_CACHED) ? VMCS_SM_CACHE_HOST :
				VMCS_SM_CACHE_NONE), 0);

	*data = ioparam.addr;
	return ret;
}
EXPORT_SYMBOL_GPL(vc_sm_lock);

/* Unlock a memory handle in use by kernel. */
int vc_sm_unlock(int handle, int flush, int no_vc_unlock)
{
	struct vmcs_sm_ioctl_lock_unlock ioparam;

	/* Validate we can work with this device. */
	if (sm_state == NULL || handle == 0) {
		pr_err("[%s]: invalid input\n", __func__);
		return -EPERM;
	}

	ioparam.handle = handle;
	return vc_sm_ioctl_unlock(sm_state->data_knl,
				  &ioparam, flush, 0, no_vc_unlock);
}
EXPORT_SYMBOL_GPL(vc_sm_unlock);

/* Map a shared memory region for use by kernel. */
int vc_sm_map(int handle, unsigned int sm_addr,
	      enum vc_sm_lock_cache_mode mode, unsigned long *data)
{
	struct vmcs_sm_ioctl_lock_unlock ioparam;
	int ret;

	/* Validate we can work with this device. */
	if (sm_state == NULL || handle == 0 || data == NULL || sm_addr == 0) {
		pr_err("[%s]: invalid input\n", __func__);
		return -EPERM;
	}

	*data = 0;

	ioparam.handle = handle;
	ret = vc_sm_ioctl_lock(sm_state->data_knl,
			       &ioparam,
			       1,
			       ((mode ==
				 VC_SM_LOCK_CACHED) ? VMCS_SM_CACHE_HOST :
				VMCS_SM_CACHE_NONE), sm_addr);

	*data = ioparam.addr;
	return ret;
}
EXPORT_SYMBOL_GPL(vc_sm_map);

/* Import a dmabuf to be shared with VC. */
int vc_sm_import_dmabuf(struct dma_buf *dmabuf, int *handle)
{
	struct vmcs_sm_ioctl_import_dmabuf ioparam = { 0 };
	int ret;
	struct sm_resource_t *resource;

	/* Validate we can work with this device. */
	if (!sm_state || !dmabuf || !handle) {
		pr_err("[%s]: invalid input\n", __func__);
		return -EPERM;
	}

	ioparam.cached = 0;
	strcpy(ioparam.name, "KRNL DMABUF");

	ret = vc_sm_ioctl_import_dmabuf(sm_state->data_knl, &ioparam, dmabuf);

	if (!ret) {
		resource = vmcs_sm_acquire_resource(sm_state->data_knl,
						    ioparam.handle);
		if (resource) {
			resource->pid = 0;
			vmcs_sm_release_resource(resource, 0);

			/* Assign valid handle at this time.*/
			*handle = ioparam.handle;
		} else {
			ret = -ENOMEM;
		}
	}

	return ret;
}
EXPORT_SYMBOL_GPL(vc_sm_import_dmabuf);
#endif

/*
 *   Register the driver with device tree
 */

static const struct of_device_id bcm2835_vcsm_of_match[] = {
	{.compatible = "raspberrypi,bcm2835-vcsm",},
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(of, bcm2835_vcsm_of_match);

static struct platform_driver bcm2835_vcsm_driver = {
	.probe = bcm2835_vcsm_probe,
	.remove = bcm2835_vcsm_remove,
	.driver = {
		   .name = DRIVER_NAME,
		   .owner = THIS_MODULE,
		   .of_match_table = bcm2835_vcsm_of_match,
		   },
};

module_platform_driver(bcm2835_vcsm_driver);

MODULE_AUTHOR("Broadcom");
MODULE_DESCRIPTION("VideoCore SharedMemory Driver");
MODULE_LICENSE("GPL v2");
