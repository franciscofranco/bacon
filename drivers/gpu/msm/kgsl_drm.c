/* Copyright (c) 2009-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/* Implements an interface between KGSL and the DRM subsystem.  For now this
 * is pretty simple, but it will take on more of the workload as time goes
 * on
 */
#include "drmP.h"
#include "drm.h"

#include <linux/msm_ion.h>
#include <linux/genlock.h>

#include "kgsl.h"
#include "kgsl_device.h"
#include "kgsl_drm.h"
#include "kgsl_mmu.h"
#include "kgsl_sharedmem.h"

#define DRIVER_AUTHOR           "Qualcomm"
#define DRIVER_NAME             "kgsl"
#define DRIVER_DESC             "KGSL DRM"
#define DRIVER_DATE             "20121107"

#define DRIVER_MAJOR            2
#define DRIVER_MINOR            1
#define DRIVER_PATCHLEVEL       1

#define DRM_KGSL_GEM_FLAG_MAPPED (1 << 0)

#define ENTRY_EMPTY -1
#define ENTRY_NEEDS_CLEANUP -2

#define DRM_KGSL_NOT_INITED -1
#define DRM_KGSL_INITED   1

#define DRM_KGSL_NUM_FENCE_ENTRIES (DRM_KGSL_HANDLE_WAIT_ENTRIES << 2)
#define DRM_KGSL_HANDLE_WAIT_ENTRIES 5

/* Returns true if the memory type is in PMEM */

#ifdef CONFIG_KERNEL_PMEM_SMI_REGION
#define TYPE_IS_PMEM(_t) \
  (((_t & DRM_KGSL_GEM_TYPE_MEM_MASK) == DRM_KGSL_GEM_TYPE_EBI) || \
   ((_t & DRM_KGSL_GEM_TYPE_MEM_MASK) == DRM_KGSL_GEM_TYPE_SMI) || \
   ((_t) & DRM_KGSL_GEM_TYPE_PMEM))
#else
#define TYPE_IS_PMEM(_t) \
  (((_t & DRM_KGSL_GEM_TYPE_MEM_MASK) == DRM_KGSL_GEM_TYPE_EBI) || \
   ((_t) & (DRM_KGSL_GEM_TYPE_PMEM | DRM_KGSL_GEM_PMEM_EBI)))
#endif

/* Returns true if the memory type is regular */

#define TYPE_IS_MEM(_t) \
  (((_t & DRM_KGSL_GEM_TYPE_MEM_MASK) == DRM_KGSL_GEM_TYPE_KMEM) || \
   ((_t & DRM_KGSL_GEM_TYPE_MEM_MASK) == DRM_KGSL_GEM_TYPE_KMEM_NOCACHE) || \
   ((_t) & DRM_KGSL_GEM_TYPE_MEM))

#define TYPE_IS_FD(_t) ((_t) & DRM_KGSL_GEM_TYPE_FD_MASK)

/* Returns true if KMEM region is uncached */

#define IS_MEM_UNCACHED(_t) \
  ((_t == DRM_KGSL_GEM_TYPE_KMEM_NOCACHE) || \
   (_t == DRM_KGSL_GEM_TYPE_KMEM) || \
   (TYPE_IS_MEM(_t) && (_t & DRM_KGSL_GEM_CACHE_WCOMBINE)))

struct drm_kgsl_gem_object_wait_list_entry {
	struct list_head list;
	int pid;
	int in_use;
	wait_queue_head_t process_wait_q;
};

struct drm_kgsl_gem_object_fence {
	int32_t fence_id;
	unsigned int num_buffers;
	int ts_valid;
	unsigned int timestamp;
	int ts_device;
	int lockpid;
	struct list_head buffers_in_fence;
};

struct drm_kgsl_gem_object_fence_list_entry {
	struct list_head list;
	int in_use;
	struct drm_gem_object *gem_obj;
};

static int32_t fence_id = 0x1;

static struct drm_kgsl_gem_object_fence
			  gem_buf_fence[DRM_KGSL_NUM_FENCE_ENTRIES];

struct drm_kgsl_gem_object {
	struct drm_gem_object *obj;
	uint32_t type;
	struct kgsl_memdesc memdesc;
	struct kgsl_pagetable *pagetable;
	struct ion_handle *ion_handle;
	uint64_t mmap_offset;
	int bufcount;
	int flags;
	struct list_head list;
	int active;

	struct {
		uint32_t offset;
		uint32_t gpuaddr;
	} bufs[DRM_KGSL_GEM_MAX_BUFFERS];

	struct genlock_handle *glock_handle[DRM_KGSL_GEM_MAX_BUFFERS];

	int bound;
	int lockpid;
	/* Put these here to avoid allocing all the time */
	struct drm_kgsl_gem_object_wait_list_entry
	wait_entries[DRM_KGSL_HANDLE_WAIT_ENTRIES];
	/* Each object can only appear in a single fence */
	struct drm_kgsl_gem_object_fence_list_entry
	fence_entries[DRM_KGSL_NUM_FENCE_ENTRIES];

	struct list_head wait_list;
};

static struct ion_client *kgsl_drm_ion_client;

static int kgsl_drm_inited = DRM_KGSL_NOT_INITED;

/* This is a global list of all the memory currently mapped in the MMU */
static struct list_head kgsl_mem_list;

struct kgsl_drm_device_priv {
	struct kgsl_device *device[KGSL_DEVICE_MAX];
	struct kgsl_device_private *devpriv[KGSL_DEVICE_MAX];
};

static int
kgsl_gem_memory_allocated(struct drm_gem_object *obj)
{
	struct drm_kgsl_gem_object *priv = obj->driver_private;
	return priv->memdesc.size ? 1 : 0;
}

static int
kgsl_gem_alloc_memory(struct drm_gem_object *obj)
{
	struct drm_kgsl_gem_object *priv = obj->driver_private;
	struct kgsl_mmu *mmu;
	struct sg_table *sg_table;
	struct scatterlist *s;
	int index;
	int result = 0;

	/* Return if the memory is already allocated */

	if (kgsl_gem_memory_allocated(obj) || TYPE_IS_FD(priv->type))
		return 0;

	if (priv->pagetable == NULL) {
		/* Hard coded to use A2X device for MSM7X27 and MSM8625
		 * Others to use A3X device
		 */
#if defined(CONFIG_ARCH_MSM7X27) || defined(CONFIG_ARCH_MSM8625)
		mmu = &kgsl_get_device(KGSL_DEVICE_2D0)->mmu;
#else
		mmu = &kgsl_get_device(KGSL_DEVICE_3D0)->mmu;
#endif

		priv->pagetable = kgsl_mmu_getpagetable(mmu,
					KGSL_MMU_GLOBAL_PT);

		if (priv->pagetable == NULL) {
			DRM_ERROR("Unable to get the GPU MMU pagetable\n");
			return -EINVAL;
		}
	}

	if (TYPE_IS_PMEM(priv->type)) {
		if (priv->type == DRM_KGSL_GEM_TYPE_EBI ||
		    priv->type & DRM_KGSL_GEM_PMEM_EBI) {
			priv->ion_handle = ion_alloc(kgsl_drm_ion_client,
				obj->size * priv->bufcount, PAGE_SIZE,
				ION_HEAP(ION_SF_HEAP_ID), 0);
			if (IS_ERR_OR_NULL(priv->ion_handle)) {
				DRM_ERROR(
				"Unable to allocate ION Phys memory handle\n");
				return -ENOMEM;
			}

			priv->memdesc.pagetable = priv->pagetable;

			result = ion_phys(kgsl_drm_ion_client,
				priv->ion_handle, (ion_phys_addr_t *)
				&priv->memdesc.physaddr, &priv->memdesc.size);
			if (result) {
				DRM_ERROR(
				"Unable to get ION Physical memory address\n");
				ion_free(kgsl_drm_ion_client,
					priv->ion_handle);
				priv->ion_handle = NULL;
				return result;
			}

			result = memdesc_sg_phys(&priv->memdesc,
				priv->memdesc.physaddr, priv->memdesc.size);
			if (result) {
				DRM_ERROR(
				"Unable to get sg list\n");
				ion_free(kgsl_drm_ion_client,
					priv->ion_handle);
				priv->ion_handle = NULL;
				return result;
			}

			result = kgsl_mmu_get_gpuaddr(priv->pagetable,
							&priv->memdesc);
			if (result) {
				DRM_ERROR(
				"kgsl_mmu_get_gpuaddr failed. result = %d\n",
				result);
				ion_free(kgsl_drm_ion_client,
					priv->ion_handle);
				priv->ion_handle = NULL;
				return result;
			}
			result = kgsl_mmu_map(priv->pagetable, &priv->memdesc);
			if (result) {
				DRM_ERROR(
				"kgsl_mmu_map failed.  result = %d\n", result);
				kgsl_mmu_put_gpuaddr(priv->pagetable,
							&priv->memdesc);
				ion_free(kgsl_drm_ion_client,
					priv->ion_handle);
				priv->ion_handle = NULL;
				return result;
			}
		}
		else
			return -EINVAL;

	} else if (TYPE_IS_MEM(priv->type)) {

		if (priv->type == DRM_KGSL_GEM_TYPE_KMEM ||
			priv->type & DRM_KGSL_GEM_CACHE_MASK)
				list_add(&priv->list, &kgsl_mem_list);

		priv->memdesc.pagetable = priv->pagetable;

		priv->ion_handle = ion_alloc(kgsl_drm_ion_client,
				obj->size * priv->bufcount, PAGE_SIZE,
				ION_HEAP(ION_IOMMU_HEAP_ID), 0);
		if (IS_ERR_OR_NULL(priv->ion_handle)) {
			DRM_ERROR(
				"Unable to allocate ION IOMMU memory handle\n");
				return -ENOMEM;
		}

		sg_table = ion_sg_table(kgsl_drm_ion_client,
				priv->ion_handle);
		if (IS_ERR_OR_NULL(priv->ion_handle)) {
			DRM_ERROR(
			"Unable to get ION sg table\n");
			goto memerr;
		}

		priv->memdesc.sg = sg_table->sgl;

		/* Calculate the size of the memdesc from the sglist */

		priv->memdesc.sglen = 0;

		for (s = priv->memdesc.sg; s != NULL; s = sg_next(s)) {
			priv->memdesc.size += s->length;
			priv->memdesc.sglen++;
		}

		result = kgsl_mmu_get_gpuaddr(priv->pagetable, &priv->memdesc);
		if (result) {
			DRM_ERROR(
			"kgsl_mmu_get_gpuaddr failed.  result = %d\n", result);
			goto memerr;
		}
		result = kgsl_mmu_map(priv->pagetable, &priv->memdesc);
		if (result) {
			DRM_ERROR(
			"kgsl_mmu_map failed.  result = %d\n", result);
			kgsl_mmu_put_gpuaddr(priv->pagetable, &priv->memdesc);
			goto memerr;
		}

	} else
		return -EINVAL;

	for (index = 0; index < priv->bufcount; index++) {
		priv->bufs[index].offset = index * obj->size;
		priv->bufs[index].gpuaddr =
			priv->memdesc.gpuaddr +
			priv->bufs[index].offset;
	}
	priv->flags |= DRM_KGSL_GEM_FLAG_MAPPED;


	return 0;

memerr:
	ion_free(kgsl_drm_ion_client,
		priv->ion_handle);
	priv->ion_handle = NULL;
	return -ENOMEM;

}

static void
kgsl_gem_free_memory(struct drm_gem_object *obj)
{
	struct drm_kgsl_gem_object *priv = obj->driver_private;
	int index;

	if (!kgsl_gem_memory_allocated(obj) || TYPE_IS_FD(priv->type))
		return;

	if (priv->memdesc.gpuaddr) {
		kgsl_mmu_unmap(priv->memdesc.pagetable, &priv->memdesc);
		kgsl_mmu_put_gpuaddr(priv->memdesc.pagetable, &priv->memdesc);
	}

	/* ION will take care of freeing the sg table. */
	priv->memdesc.sg = NULL;
	priv->memdesc.sglen = 0;

	if (priv->ion_handle)
		ion_free(kgsl_drm_ion_client, priv->ion_handle);

	priv->ion_handle = NULL;

	memset(&priv->memdesc, 0, sizeof(priv->memdesc));

	for (index = 0; index < priv->bufcount; index++) {
		if (priv->glock_handle[index])
			genlock_put_handle(priv->glock_handle[index]);
	}

	kgsl_mmu_putpagetable(priv->pagetable);
	priv->pagetable = NULL;

	if ((priv->type == DRM_KGSL_GEM_TYPE_KMEM) ||
	    (priv->type & DRM_KGSL_GEM_CACHE_MASK))
		list_del(&priv->list);

	priv->flags &= ~DRM_KGSL_GEM_FLAG_MAPPED;

}

int
kgsl_gem_init_object(struct drm_gem_object *obj)
{
	struct drm_kgsl_gem_object *priv;
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (priv == NULL) {
		DRM_ERROR("Unable to create GEM object\n");
		return -ENOMEM;
	}

	obj->driver_private = priv;
	priv->obj = obj;

	return 0;
}

void
kgsl_gem_free_object(struct drm_gem_object *obj)
{
	kgsl_gem_free_memory(obj);
	drm_gem_object_release(obj);
	kfree(obj->driver_private);
}

int
kgsl_gem_obj_addr(int drm_fd, int handle, unsigned long *start,
			unsigned long *len)
{
	struct file *filp;
	struct drm_device *dev;
	struct drm_file *file_priv;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;
	int ret = 0;

	filp = fget(drm_fd);
	if (unlikely(filp == NULL)) {
		DRM_ERROR("Unable to get the DRM file descriptor\n");
		return -EINVAL;
	}
	file_priv = filp->private_data;
	if (unlikely(file_priv == NULL)) {
		DRM_ERROR("Unable to get the file private data\n");
		fput(filp);
		return -EINVAL;
	}
	dev = file_priv->minor->dev;
	if (unlikely(dev == NULL)) {
		DRM_ERROR("Unable to get the minor device\n");
		fput(filp);
		return -EINVAL;
	}

	obj = drm_gem_object_lookup(dev, file_priv, handle);
	if (unlikely(obj == NULL)) {
		DRM_ERROR("Invalid GEM handle %x\n", handle);
		fput(filp);
		return -EBADF;
	}

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	/* We can only use the MDP for PMEM regions */

	if (TYPE_IS_PMEM(priv->type)) {
		*start = priv->memdesc.physaddr +
			priv->bufs[priv->active].offset;

		*len = priv->memdesc.size;
	} else {
		*start = 0;
		*len = 0;
		ret = -EINVAL;
	}

	drm_gem_object_unreference(obj);
	mutex_unlock(&dev->struct_mutex);

	fput(filp);
	return ret;
}

static int
kgsl_gem_init_obj(struct drm_device *dev,
		  struct drm_file *file_priv,
		  struct drm_gem_object *obj,
		  int *handle)
{
	struct drm_kgsl_gem_object *priv;
	int ret, i;

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	memset(&priv->memdesc, 0, sizeof(priv->memdesc));
	priv->bufcount = 1;
	priv->active = 0;
	priv->bound = 0;

	priv->type = DRM_KGSL_GEM_TYPE_KMEM;

	ret = drm_gem_handle_create(file_priv, obj, handle);

	drm_gem_object_unreference(obj);
	INIT_LIST_HEAD(&priv->wait_list);

	for (i = 0; i < DRM_KGSL_HANDLE_WAIT_ENTRIES; i++) {
		INIT_LIST_HEAD((struct list_head *) &priv->wait_entries[i]);
		priv->wait_entries[i].pid = 0;
		init_waitqueue_head(&priv->wait_entries[i].process_wait_q);
	}

	for (i = 0; i < DRM_KGSL_NUM_FENCE_ENTRIES; i++) {
		INIT_LIST_HEAD((struct list_head *) &priv->fence_entries[i]);
		priv->fence_entries[i].in_use = 0;
		priv->fence_entries[i].gem_obj = obj;
	}

	mutex_unlock(&dev->struct_mutex);
	return ret;
}

int
kgsl_gem_create_ioctl(struct drm_device *dev, void *data,
		      struct drm_file *file_priv)
{
	struct drm_kgsl_gem_create *create = data;
	struct drm_gem_object *obj;
	int ret, handle;

	/* Page align the size so we can allocate multiple buffers */
	create->size = ALIGN(create->size, 4096);

	obj = drm_gem_object_alloc(dev, create->size);

	if (obj == NULL) {
		DRM_ERROR("Unable to allocate the GEM object\n");
		return -ENOMEM;
	}

	ret = kgsl_gem_init_obj(dev, file_priv, obj, &handle);
	if (ret) {
		drm_gem_object_release(obj);
		DRM_ERROR("Unable to initialize GEM object ret = %d\n", ret);
		return ret;
	}

	create->handle = handle;
	return 0;
}

int
kgsl_gem_create_fd_ioctl(struct drm_device *dev, void *data,
			      struct drm_file *file_priv)
{
	struct drm_kgsl_gem_create_fd *args = data;
	struct file *file;
	dev_t rdev;
	struct fb_info *info;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;
	int ret, put_needed, handle;

	file = fget_light(args->fd, &put_needed);

	if (file == NULL) {
		DRM_ERROR("Unable to get the file object\n");
		return -EBADF;
	}

	rdev = file->f_dentry->d_inode->i_rdev;

	/* Only framebuffer objects are supported ATM */

	if (MAJOR(rdev) != FB_MAJOR) {
		DRM_ERROR("File descriptor is not a framebuffer\n");
		ret = -EBADF;
		goto error_fput;
	}

	info = registered_fb[MINOR(rdev)];

	if (info == NULL) {
		DRM_ERROR("Framebuffer minor %d is not registered\n",
			  MINOR(rdev));
		ret = -EBADF;
		goto error_fput;
	}

	obj = drm_gem_object_alloc(dev, info->fix.smem_len);

	if (obj == NULL) {
		DRM_ERROR("Unable to allocate GEM object\n");
		ret = -ENOMEM;
		goto error_fput;
	}

	ret = kgsl_gem_init_obj(dev, file_priv, obj, &handle);

	if (ret)
		goto error_fput;

	mutex_lock(&dev->struct_mutex);

	priv = obj->driver_private;
	priv->memdesc.physaddr = info->fix.smem_start;
	priv->type = DRM_KGSL_GEM_TYPE_FD_FBMEM;

	mutex_unlock(&dev->struct_mutex);
	args->handle = handle;

error_fput:
	fput_light(file, put_needed);

	return ret;
}

int
kgsl_gem_create_from_ion_ioctl(struct drm_device *dev, void *data,
		      struct drm_file *file_priv)
{
	struct drm_kgsl_gem_create_from_ion *args = data;
	struct drm_gem_object *obj;
	struct ion_handle *ion_handle;
	struct drm_kgsl_gem_object *priv;
	struct sg_table *sg_table;
	struct scatterlist *s;
	int ret, handle;
	unsigned long size;
	struct kgsl_mmu *mmu;

	ion_handle = ion_import_dma_buf(kgsl_drm_ion_client, args->ion_fd);
	if (IS_ERR_OR_NULL(ion_handle)) {
		DRM_ERROR("Unable to import dmabuf.  Error number = %d\n",
			(int)PTR_ERR(ion_handle));
		return -EINVAL;
	}

	ion_handle_get_size(kgsl_drm_ion_client, ion_handle, &size);

	if (size == 0) {
		ion_free(kgsl_drm_ion_client, ion_handle);
		DRM_ERROR(
		"cannot create GEM object from zero size ION buffer\n");
		return -EINVAL;
	}

	obj = drm_gem_object_alloc(dev, size);

	if (obj == NULL) {
		ion_free(kgsl_drm_ion_client, ion_handle);
		DRM_ERROR("Unable to allocate the GEM object\n");
		return -ENOMEM;
	}

	ret = kgsl_gem_init_obj(dev, file_priv, obj, &handle);
	if (ret) {
		ion_free(kgsl_drm_ion_client, ion_handle);
		drm_gem_object_release(obj);
		DRM_ERROR("Unable to initialize GEM object ret = %d\n", ret);
		return ret;
	}

	priv = obj->driver_private;
	priv->ion_handle = ion_handle;

	priv->type = DRM_KGSL_GEM_TYPE_KMEM;
	list_add(&priv->list, &kgsl_mem_list);

#if defined(CONFIG_ARCH_MSM7X27) || defined(CONFIG_ARCH_MSM8625)
	mmu = &kgsl_get_device(KGSL_DEVICE_2D0)->mmu;
#else
	mmu = &kgsl_get_device(KGSL_DEVICE_3D0)->mmu;
#endif

	priv->pagetable = kgsl_mmu_getpagetable(mmu, KGSL_MMU_GLOBAL_PT);

	priv->memdesc.pagetable = priv->pagetable;

	sg_table = ion_sg_table(kgsl_drm_ion_client,
		priv->ion_handle);
	if (IS_ERR_OR_NULL(priv->ion_handle)) {
		DRM_ERROR("Unable to get ION sg table\n");
		ion_free(kgsl_drm_ion_client,
			priv->ion_handle);
		priv->ion_handle = NULL;
		kgsl_mmu_putpagetable(priv->pagetable);
		drm_gem_object_release(obj);
		kfree(priv);
		return -ENOMEM;
	}

	priv->memdesc.sg = sg_table->sgl;

	/* Calculate the size of the memdesc from the sglist */

	priv->memdesc.sglen = 0;

	for (s = priv->memdesc.sg; s != NULL; s = sg_next(s)) {
		priv->memdesc.size += s->length;
		priv->memdesc.sglen++;
	}

	ret = kgsl_mmu_get_gpuaddr(priv->pagetable, &priv->memdesc);
	if (ret) {
		DRM_ERROR("kgsl_mmu_get_gpuaddr failed.  ret = %d\n", ret);
		ion_free(kgsl_drm_ion_client,
			priv->ion_handle);
		priv->ion_handle = NULL;
		kgsl_mmu_putpagetable(priv->pagetable);
		drm_gem_object_release(obj);
		kfree(priv);
		return -ENOMEM;
	}
	ret = kgsl_mmu_map(priv->pagetable, &priv->memdesc);
	if (ret) {
		DRM_ERROR("kgsl_mmu_map failed.  ret = %d\n", ret);
		kgsl_mmu_put_gpuaddr(priv->pagetable, &priv->memdesc);
		ion_free(kgsl_drm_ion_client,
			priv->ion_handle);
		priv->ion_handle = NULL;
		kgsl_mmu_putpagetable(priv->pagetable);
		drm_gem_object_release(obj);
		kfree(priv);
		return -ENOMEM;
	}

	priv->bufs[0].offset = 0;
	priv->bufs[0].gpuaddr = priv->memdesc.gpuaddr;
	priv->flags |= DRM_KGSL_GEM_FLAG_MAPPED;

	args->handle = handle;
	return 0;
}

int
kgsl_gem_get_ion_fd_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct drm_kgsl_gem_get_ion_fd *args = data;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;
	int ret = 0;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);

	if (obj == NULL) {
		DRM_ERROR("Invalid GEM handle %x\n", args->handle);
		return -EBADF;
	}

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	if (TYPE_IS_FD(priv->type))
		ret = -EINVAL;
	else if (TYPE_IS_PMEM(priv->type) || TYPE_IS_MEM(priv->type)) {
		if (priv->ion_handle) {
			args->ion_fd = ion_share_dma_buf_fd(
				kgsl_drm_ion_client, priv->ion_handle);
			if (args->ion_fd < 0) {
				DRM_ERROR(
				"Could not share ion buffer. Error = %d\n",
					args->ion_fd);
				ret = -EINVAL;
			}
		} else {
			DRM_ERROR("GEM object has no ion memory allocated.\n");
			ret = -EINVAL;
		}
	}

	drm_gem_object_unreference(obj);
	mutex_unlock(&dev->struct_mutex);

	return ret;
}

int
kgsl_gem_setmemtype_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct drm_kgsl_gem_memtype *args = data;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;
	int ret = 0;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);

	if (obj == NULL) {
		DRM_ERROR("Invalid GEM handle %x\n", args->handle);
		return -EBADF;
	}

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	if (TYPE_IS_FD(priv->type))
		ret = -EINVAL;
	else {
		if (TYPE_IS_PMEM(args->type) || TYPE_IS_MEM(args->type))
			priv->type = args->type;
		else
			ret = -EINVAL;
	}

	drm_gem_object_unreference(obj);
	mutex_unlock(&dev->struct_mutex);

	return ret;
}

int
kgsl_gem_getmemtype_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *file_priv)
{
	struct drm_kgsl_gem_memtype *args = data;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);

	if (obj == NULL) {
		DRM_ERROR("Invalid GEM handle %x\n", args->handle);
		return -EBADF;
	}

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	args->type = priv->type;

	drm_gem_object_unreference(obj);
	mutex_unlock(&dev->struct_mutex);

	return 0;
}

int
kgsl_gem_unbind_gpu_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	return 0;
}

int
kgsl_gem_bind_gpu_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	return 0;
}

/* Allocate the memory and prepare it for CPU mapping */

int
kgsl_gem_alloc_ioctl(struct drm_device *dev, void *data,
		    struct drm_file *file_priv)
{
	struct drm_kgsl_gem_alloc *args = data;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;
	int ret;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);

	if (obj == NULL) {
		DRM_ERROR("Invalid GEM handle %x\n", args->handle);
		return -EBADF;
	}

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	ret = kgsl_gem_alloc_memory(obj);

	if (ret) {
		DRM_ERROR("Unable to allocate object memory\n");
	}

	args->offset = 0;

	drm_gem_object_unreference(obj);
	mutex_unlock(&dev->struct_mutex);

	return ret;
}

int
kgsl_gem_mmap_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	/* Ion is used for mmap at this time */
	return 0;
}

/* This function is deprecated */

int
kgsl_gem_prep_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct drm_kgsl_gem_prep *args = data;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;
	int ret;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);

	if (obj == NULL) {
		DRM_ERROR("Invalid GEM handle %x\n", args->handle);
		return -EBADF;
	}

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	ret = kgsl_gem_alloc_memory(obj);
	if (ret) {
		DRM_ERROR("Unable to allocate object memory\n");
		drm_gem_object_unreference(obj);
		mutex_unlock(&dev->struct_mutex);
		return ret;
	}

	drm_gem_object_unreference(obj);
	mutex_unlock(&dev->struct_mutex);

	return 0;
}

int
kgsl_gem_get_bufinfo_ioctl(struct drm_device *dev, void *data,
			   struct drm_file *file_priv)
{
	struct drm_kgsl_gem_bufinfo *args = data;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;
	int ret = -EINVAL;
	int index;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);

	if (obj == NULL) {
		DRM_ERROR("Invalid GEM handle %x\n", args->handle);
		return -EBADF;
	}

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	if (!kgsl_gem_memory_allocated(obj)) {
		DRM_ERROR("Memory not allocated for this object\n");
		goto out;
	}

	for (index = 0; index < priv->bufcount; index++) {
		args->offset[index] = priv->bufs[index].offset;
		args->gpuaddr[index] = priv->bufs[index].gpuaddr;
	}

	args->count = priv->bufcount;
	args->active = priv->active;

	ret = 0;

out:
	drm_gem_object_unreference(obj);
	mutex_unlock(&dev->struct_mutex);

	return ret;
}

/* Get the genlock handles base off the GEM handle
 */

int
kgsl_gem_get_glock_handles_ioctl(struct drm_device *dev, void *data,
					struct drm_file *file_priv)
{
	struct drm_kgsl_gem_glockinfo *args = data;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;
	int index;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);

	if (obj == NULL) {
		DRM_ERROR("Invalid GEM handle %x\n", args->handle);
		return -EBADF;
	}

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	for (index = 0; index < priv->bufcount; index++) {
		args->glockhandle[index] = genlock_get_fd_handle(
						priv->glock_handle[index]);
	}

	drm_gem_object_unreference(obj);
	mutex_unlock(&dev->struct_mutex);
	return 0;
}

int
kgsl_gem_set_glock_handles_ioctl(struct drm_device *dev, void *data,
					struct drm_file *file_priv)
{
	struct drm_kgsl_gem_glockinfo *args = data;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;
	int index;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);

	if (obj == NULL) {
		DRM_ERROR("Invalid GEM handle %x\n", args->handle);
		return -EBADF;
	}

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	for (index = 0; index < priv->bufcount; index++) {
		priv->glock_handle[index] = genlock_get_handle_fd(
						args->glockhandle[index]);
	}

	drm_gem_object_unreference(obj);
	mutex_unlock(&dev->struct_mutex);

	return 0;
}

int
kgsl_gem_set_bufcount_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *file_priv)
{
	struct drm_kgsl_gem_bufcount *args = data;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;
	int ret = -EINVAL;

	if (args->bufcount < 1 || args->bufcount > DRM_KGSL_GEM_MAX_BUFFERS)
		return -EINVAL;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);

	if (obj == NULL) {
		DRM_ERROR("Invalid GEM handle %x\n", args->handle);
		return -EBADF;
	}

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	/* It is too much math to worry about what happens if we are already
	   allocated, so just bail if we are */

	if (kgsl_gem_memory_allocated(obj)) {
		DRM_ERROR("Memory already allocated - cannot change"
			  "number of buffers\n");
		goto out;
	}

	priv->bufcount = args->bufcount;
	ret = 0;

out:
	drm_gem_object_unreference(obj);
	mutex_unlock(&dev->struct_mutex);

	return ret;
}

int
kgsl_gem_get_bufcount_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *file_priv)
{
	struct drm_kgsl_gem_bufcount *args = data;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);

	if (obj == NULL) {
		DRM_ERROR("Invalid GEM handle %x\n", args->handle);
		return -EBADF;
	}

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	args->bufcount =  priv->bufcount;

	drm_gem_object_unreference(obj);
	mutex_unlock(&dev->struct_mutex);

	return 0;
}

int
kgsl_gem_set_active_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *file_priv)
{
	struct drm_kgsl_gem_active *args = data;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;
	int ret = -EINVAL;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);

	if (obj == NULL) {
		DRM_ERROR("Invalid GEM handle %x\n", args->handle);
		return -EBADF;
	}

	mutex_lock(&dev->struct_mutex);
	priv = obj->driver_private;

	if (args->active < 0 || args->active >= priv->bufcount) {
		DRM_ERROR("Invalid active buffer %d\n", args->active);
		goto out;
	}

	priv->active = args->active;
	ret = 0;

out:
	drm_gem_object_unreference(obj);
	mutex_unlock(&dev->struct_mutex);

	return ret;
}

int kgsl_gem_kmem_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct drm_gem_object *obj = vma->vm_private_data;
	struct drm_device *dev = obj->dev;
	struct drm_kgsl_gem_object *priv;
	unsigned long offset;
	struct page *page;
	int i;

	mutex_lock(&dev->struct_mutex);

	priv = obj->driver_private;

	offset = (unsigned long) vmf->virtual_address - vma->vm_start;
	i = offset >> PAGE_SHIFT;
	page = sg_page(&(priv->memdesc.sg[i]));

	if (!page) {
		mutex_unlock(&dev->struct_mutex);
		return VM_FAULT_SIGBUS;
	}

	get_page(page);
	vmf->page = page;

	mutex_unlock(&dev->struct_mutex);
	return 0;
}

int kgsl_gem_phys_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct drm_gem_object *obj = vma->vm_private_data;
	struct drm_device *dev = obj->dev;
	struct drm_kgsl_gem_object *priv;
	unsigned long offset, pfn;
	int ret = 0;

	offset = ((unsigned long) vmf->virtual_address - vma->vm_start) >>
		PAGE_SHIFT;

	mutex_lock(&dev->struct_mutex);

	priv = obj->driver_private;

	pfn = (priv->memdesc.physaddr >> PAGE_SHIFT) + offset;
	ret = vm_insert_pfn(vma,
			    (unsigned long) vmf->virtual_address, pfn);
	mutex_unlock(&dev->struct_mutex);

	switch (ret) {
	case -ENOMEM:
	case -EAGAIN:
		return VM_FAULT_OOM;
	case -EFAULT:
		return VM_FAULT_SIGBUS;
	default:
		return VM_FAULT_NOPAGE;
	}
}

void
cleanup_fence(struct drm_kgsl_gem_object_fence *fence, int check_waiting)
{
	int j;
	struct drm_kgsl_gem_object_fence_list_entry *this_fence_entry = NULL;
	struct drm_kgsl_gem_object *unlock_obj;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object_wait_list_entry *lock_next;

	fence->ts_valid = 0;
	fence->timestamp = -1;
	fence->ts_device = -1;

	/* Walk the list of buffers in this fence and clean up the */
	/* references. Note that this can cause memory allocations */
	/* to be freed */
	for (j = fence->num_buffers; j > 0; j--) {
		this_fence_entry =
				(struct drm_kgsl_gem_object_fence_list_entry *)
				fence->buffers_in_fence.prev;

		this_fence_entry->in_use = 0;
		obj = this_fence_entry->gem_obj;
		unlock_obj = obj->driver_private;

		/* Delete it from the list */

		list_del(&this_fence_entry->list);

		/* we are unlocking - see if there are other pids waiting */
		if (check_waiting) {
			if (!list_empty(&unlock_obj->wait_list)) {
				lock_next =
				(struct drm_kgsl_gem_object_wait_list_entry *)
					unlock_obj->wait_list.prev;

				list_del((struct list_head *)&lock_next->list);

				unlock_obj->lockpid = 0;
				wake_up_interruptible(
						&lock_next->process_wait_q);
				lock_next->pid = 0;

			} else {
				/* List is empty so set pid to 0 */
				unlock_obj->lockpid = 0;
			}
		}

		drm_gem_object_unreference(obj);
	}
	/* here all the buffers in the fence are released */
	/* clear the fence entry */
	fence->fence_id = ENTRY_EMPTY;
}

int
find_empty_fence(void)
{
	int i;

	for (i = 0; i < DRM_KGSL_NUM_FENCE_ENTRIES; i++) {
		if (gem_buf_fence[i].fence_id == ENTRY_EMPTY) {
			gem_buf_fence[i].fence_id = fence_id++;
			gem_buf_fence[i].ts_valid = 0;
			INIT_LIST_HEAD(&(gem_buf_fence[i].buffers_in_fence));
			if (fence_id == 0xFFFFFFF0)
				fence_id = 1;
			return i;
		} else {

			/* Look for entries to be cleaned up */
			if (gem_buf_fence[i].fence_id == ENTRY_NEEDS_CLEANUP)
				cleanup_fence(&gem_buf_fence[i], 0);
		}
	}

	return ENTRY_EMPTY;
}

int
find_fence(int index)
{
	int i;

	for (i = 0; i < DRM_KGSL_NUM_FENCE_ENTRIES; i++) {
		if (gem_buf_fence[i].fence_id == index)
			return i;
	}

	return ENTRY_EMPTY;
}

void
wakeup_fence_entries(struct drm_kgsl_gem_object_fence *fence)
{
    struct drm_kgsl_gem_object_fence_list_entry *this_fence_entry = NULL;
	struct drm_kgsl_gem_object_wait_list_entry *lock_next;
	struct drm_kgsl_gem_object *unlock_obj;
	struct drm_gem_object *obj;

	/* TS has expired when we get here */
	fence->ts_valid = 0;
	fence->timestamp = -1;
	fence->ts_device = -1;

	list_for_each_entry(this_fence_entry, &fence->buffers_in_fence, list) {
		obj = this_fence_entry->gem_obj;
		unlock_obj = obj->driver_private;

		if (!list_empty(&unlock_obj->wait_list)) {
			lock_next =
				(struct drm_kgsl_gem_object_wait_list_entry *)
					unlock_obj->wait_list.prev;

			/* Unblock the pid */
			lock_next->pid = 0;

			/* Delete it from the list */
			list_del((struct list_head *)&lock_next->list);

			unlock_obj->lockpid = 0;
			wake_up_interruptible(&lock_next->process_wait_q);

		} else {
			/* List is empty so set pid to 0 */
			unlock_obj->lockpid = 0;
		}
	}
	fence->fence_id = ENTRY_NEEDS_CLEANUP;  /* Mark it as needing cleanup */
}

int
kgsl_gem_lock_handle_ioctl(struct drm_device *dev, void *data,
						   struct drm_file *file_priv)
{
	/* The purpose of this function is to lock a given set of handles. */
	/* The driver will maintain a list of locked handles. */
	/* If a request comes in for a handle that's locked the thread will */
	/* block until it's no longer in use. */

	struct drm_kgsl_gem_lock_handles *args = data;
	struct drm_gem_object *obj;
	struct drm_kgsl_gem_object *priv;
	struct drm_kgsl_gem_object_fence_list_entry *this_fence_entry = NULL;
	struct drm_kgsl_gem_object_fence *fence;
	struct drm_kgsl_gem_object_wait_list_entry *lock_item;
	int i, j;
	int result = 0;
	uint32_t *lock_list;
	uint32_t *work_list = NULL;
	int32_t fence_index;

	/* copy in the data from user space */
	lock_list = kzalloc(sizeof(uint32_t) * args->num_handles, GFP_KERNEL);
	if (!lock_list) {
		DRM_ERROR("Unable allocate memory for lock list\n");
		result = -ENOMEM;
		goto error;
	}

	if (copy_from_user(lock_list, args->handle_list,
			   sizeof(uint32_t) * args->num_handles)) {
		DRM_ERROR("Unable to copy the lock list from the user\n");
		result = -EFAULT;
		goto free_handle_list;
	}


	work_list = lock_list;
	mutex_lock(&dev->struct_mutex);

	/* build the fence for this group of handles */
	fence_index = find_empty_fence();
	if (fence_index == ENTRY_EMPTY) {
		DRM_ERROR("Unable to find a empty fence\n");
		args->lock_id = 0xDEADBEEF;
		result = -EFAULT;
		goto out_unlock;
	}

	fence = &gem_buf_fence[fence_index];
	gem_buf_fence[fence_index].num_buffers = args->num_handles;
	args->lock_id = gem_buf_fence[fence_index].fence_id;

	for (j = args->num_handles; j > 0; j--, lock_list++) {
		obj = drm_gem_object_lookup(dev, file_priv, *lock_list);

		if (obj == NULL) {
			DRM_ERROR("Invalid GEM handle %x\n", *lock_list);
			result = -EBADF;
			goto out_unlock;
		}

		priv = obj->driver_private;
		this_fence_entry = NULL;

		/* get a fence entry to hook into the fence */
		for (i = 0; i < DRM_KGSL_NUM_FENCE_ENTRIES; i++) {
			if (!priv->fence_entries[i].in_use) {
				this_fence_entry = &priv->fence_entries[i];
				this_fence_entry->in_use = 1;
				break;
			}
		}

		if (this_fence_entry == NULL) {
			fence->num_buffers = 0;
			fence->fence_id = ENTRY_EMPTY;
			args->lock_id = 0xDEADBEAD;
			result = -EFAULT;
			drm_gem_object_unreference(obj);
			goto out_unlock;
		}

		/* We're trying to lock - add to a fence */
		list_add((struct list_head *)this_fence_entry,
				 &gem_buf_fence[fence_index].buffers_in_fence);
		if (priv->lockpid) {

			if (priv->lockpid == args->pid) {
				/* now that things are running async this  */
				/* happens when an op isn't done */
				/* so it's already locked by the calling pid */
					continue;
			}


			/* if a pid already had it locked */
			/* create and add to wait list */
			for (i = 0; i < DRM_KGSL_HANDLE_WAIT_ENTRIES; i++) {
				if (priv->wait_entries[i].in_use == 0) {
					/* this one is empty */
					lock_item = &priv->wait_entries[i];
				    lock_item->in_use = 1;
					lock_item->pid = args->pid;
					INIT_LIST_HEAD((struct list_head *)
						&priv->wait_entries[i]);
					break;
				}
			}

			if (i == DRM_KGSL_HANDLE_WAIT_ENTRIES) {

				result =  -EFAULT;
				drm_gem_object_unreference(obj);
				goto out_unlock;
			}

			list_add_tail((struct list_head *)&lock_item->list,
							&priv->wait_list);
			mutex_unlock(&dev->struct_mutex);
			/* here we need to block */
			wait_event_interruptible_timeout(
					priv->wait_entries[i].process_wait_q,
					(priv->lockpid == 0),
					msecs_to_jiffies(64));
			mutex_lock(&dev->struct_mutex);
			lock_item->in_use = 0;
		}

		/* Getting here means no one currently holds the lock */
		priv->lockpid = args->pid;

		args->lock_id = gem_buf_fence[fence_index].fence_id;
	}
	fence->lockpid = args->pid;

out_unlock:
	mutex_unlock(&dev->struct_mutex);

free_handle_list:
	kfree(work_list);

error:
	return result;
}

int
kgsl_gem_unlock_handle_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file_priv)
{
	struct drm_kgsl_gem_unlock_handles *args = data;
	int result = 0;
	int32_t fence_index;

	mutex_lock(&dev->struct_mutex);
	fence_index = find_fence(args->lock_id);
	if (fence_index == ENTRY_EMPTY) {
		DRM_ERROR("Invalid lock ID: %x\n", args->lock_id);
		result = -EFAULT;
		goto out_unlock;
	}

	cleanup_fence(&gem_buf_fence[fence_index], 1);

out_unlock:
	mutex_unlock(&dev->struct_mutex);

	return result;
}


int
kgsl_gem_unlock_on_ts_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct drm_kgsl_gem_unlock_on_ts *args = data;
	int result = 0;
	int ts_done = 0;
	int32_t fence_index, ts_device;
	struct drm_kgsl_gem_object_fence *fence;
	struct kgsl_device *device;

	if (args->type == DRM_KGSL_GEM_TS_3D)
		ts_device = KGSL_DEVICE_3D0;
	else if (args->type == DRM_KGSL_GEM_TS_2D)
		ts_device = KGSL_DEVICE_2D0;
	else {
		result = -EINVAL;
		goto error;
	}

	device = kgsl_get_device(ts_device);
	ts_done = kgsl_check_timestamp(device, NULL, args->timestamp);

	mutex_lock(&dev->struct_mutex);

	fence_index = find_fence(args->lock_id);
	if (fence_index == ENTRY_EMPTY) {
		DRM_ERROR("Invalid lock ID: %x\n", args->lock_id);
		result = -EFAULT;
		goto out_unlock;
	}

	fence = &gem_buf_fence[fence_index];
	fence->ts_device = ts_device;

	if (!ts_done)
		fence->ts_valid = 1;
	else
		cleanup_fence(fence, 1);


out_unlock:
	mutex_unlock(&dev->struct_mutex);

error:
	return result;
}

struct drm_ioctl_desc kgsl_drm_ioctls[] = {
	DRM_IOCTL_DEF_DRV(KGSL_GEM_CREATE, kgsl_gem_create_ioctl, 0),
	DRM_IOCTL_DEF_DRV(KGSL_GEM_PREP, kgsl_gem_prep_ioctl, 0),
	DRM_IOCTL_DEF_DRV(KGSL_GEM_SETMEMTYPE, kgsl_gem_setmemtype_ioctl, 0),
	DRM_IOCTL_DEF_DRV(KGSL_GEM_GETMEMTYPE, kgsl_gem_getmemtype_ioctl, 0),
	DRM_IOCTL_DEF_DRV(KGSL_GEM_BIND_GPU, kgsl_gem_bind_gpu_ioctl, 0),
	DRM_IOCTL_DEF_DRV(KGSL_GEM_UNBIND_GPU, kgsl_gem_unbind_gpu_ioctl, 0),
	DRM_IOCTL_DEF_DRV(KGSL_GEM_ALLOC, kgsl_gem_alloc_ioctl, 0),
	DRM_IOCTL_DEF_DRV(KGSL_GEM_MMAP, kgsl_gem_mmap_ioctl, 0),
	DRM_IOCTL_DEF_DRV(KGSL_GEM_GET_BUFINFO, kgsl_gem_get_bufinfo_ioctl, 0),
	DRM_IOCTL_DEF_DRV(KGSL_GEM_GET_ION_FD, kgsl_gem_get_ion_fd_ioctl, 0),
	DRM_IOCTL_DEF_DRV(KGSL_GEM_CREATE_FROM_ION,
				kgsl_gem_create_from_ion_ioctl, 0),
	DRM_IOCTL_DEF_DRV(KGSL_GEM_SET_BUFCOUNT,
				kgsl_gem_set_bufcount_ioctl, 0),
	DRM_IOCTL_DEF_DRV(KGSL_GEM_GET_BUFCOUNT,
				kgsl_gem_get_bufcount_ioctl, 0),
	DRM_IOCTL_DEF_DRV(KGSL_GEM_SET_GLOCK_HANDLES_INFO,
				kgsl_gem_set_glock_handles_ioctl, 0),
	DRM_IOCTL_DEF_DRV(KGSL_GEM_GET_GLOCK_HANDLES_INFO,
				kgsl_gem_get_glock_handles_ioctl, 0),
	DRM_IOCTL_DEF_DRV(KGSL_GEM_SET_ACTIVE, kgsl_gem_set_active_ioctl, 0),
	DRM_IOCTL_DEF_DRV(KGSL_GEM_LOCK_HANDLE,
				  kgsl_gem_lock_handle_ioctl, 0),
	DRM_IOCTL_DEF_DRV(KGSL_GEM_UNLOCK_HANDLE,
				  kgsl_gem_unlock_handle_ioctl, 0),
	DRM_IOCTL_DEF_DRV(KGSL_GEM_UNLOCK_ON_TS,
				  kgsl_gem_unlock_on_ts_ioctl, 0),
	DRM_IOCTL_DEF_DRV(KGSL_GEM_CREATE_FD, kgsl_gem_create_fd_ioctl,
		      DRM_MASTER),
};

static const struct file_operations kgsl_drm_driver_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.mmap = drm_gem_mmap,
	.poll = drm_poll,
	.fasync = drm_fasync,
};

static struct drm_driver driver = {
	.driver_features = DRIVER_GEM,
	.gem_init_object = kgsl_gem_init_object,
	.gem_free_object = kgsl_gem_free_object,
	.ioctls = kgsl_drm_ioctls,
	.fops = &kgsl_drm_driver_fops,
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,
};

int kgsl_drm_init(struct platform_device *dev)
{
	int i;

	/* Only initialize once */
	if (kgsl_drm_inited == DRM_KGSL_INITED)
		return 0;

	kgsl_drm_inited = DRM_KGSL_INITED;

	driver.num_ioctls = DRM_ARRAY_SIZE(kgsl_drm_ioctls);

	INIT_LIST_HEAD(&kgsl_mem_list);

	for (i = 0; i < DRM_KGSL_NUM_FENCE_ENTRIES; i++) {
		gem_buf_fence[i].num_buffers = 0;
		gem_buf_fence[i].ts_valid = 0;
		gem_buf_fence[i].fence_id = ENTRY_EMPTY;
	}

	/* Create ION Client */
	kgsl_drm_ion_client = msm_ion_client_create(
			0xffffffff, "kgsl_drm");
	if (!kgsl_drm_ion_client) {
		DRM_ERROR("Unable to create ION client\n");
		return -ENOMEM;
	}

	return drm_platform_init(&driver, dev);
}

void kgsl_drm_exit(void)
{
	kgsl_drm_inited = DRM_KGSL_NOT_INITED;

	if (kgsl_drm_ion_client)
		ion_client_destroy(kgsl_drm_ion_client);
	kgsl_drm_ion_client = NULL;

	drm_platform_exit(&driver, driver.kdriver.platform_device);
}
