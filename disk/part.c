// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2001
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 */

#include <common.h>
#include <blk.h>
#include <command.h>
#include <env.h>
#include <errno.h>
#include <ide.h>
#include <log.h>
#include <malloc.h>
#include <part.h>
#include <ubifs_uboot.h>

#undef	PART_DEBUG

#ifdef	PART_DEBUG
#define	PRINTF(fmt,args...)	printf (fmt ,##args)
#else
#define PRINTF(fmt,args...)
#endif

/* Check all partition types */
#define PART_TYPE_ALL		-1

static struct part_driver *part_driver_lookup_type(struct blk_desc *dev_desc)
{
	struct part_driver *drv =
		ll_entry_start(struct part_driver, part_driver);
	const int n_ents = ll_entry_count(struct part_driver, part_driver);
	struct part_driver *entry;

	if (dev_desc->part_type == PART_TYPE_UNKNOWN) {
		for (entry = drv; entry != drv + n_ents; entry++) {
			int ret;

			ret = entry->test(dev_desc);
			if (!ret) {
				dev_desc->part_type = entry->part_type;
				return entry;
			}
		}
	} else {
		for (entry = drv; entry != drv + n_ents; entry++) {
			if (dev_desc->part_type == entry->part_type)
				return entry;
		}
	}

	/* Not found */
	return NULL;
}

#ifdef CONFIG_HAVE_BLOCK_DEVICE
static struct blk_desc *get_dev_hwpart(const char *ifname, int dev, int hwpart)
{
	struct blk_desc *dev_desc;
	int ret;

	dev_desc = blk_get_devnum_by_typename(ifname, dev);
	if (!dev_desc) {
		debug("%s: No device for iface '%s', dev %d\n", __func__,
		      ifname, dev);
		return NULL;
	}
	ret = blk_dselect_hwpart(dev_desc, hwpart);
	if (ret) {
		debug("%s: Failed to select h/w partition: err-%d\n", __func__,
		      ret);
		return NULL;
	}

	return dev_desc;
}

struct blk_desc *blk_get_dev(const char *ifname, int dev)
{
	return get_dev_hwpart(ifname, dev, 0);
}
#else
struct blk_desc *get_dev_hwpart(const char *ifname, int dev, int hwpart)
{
	return NULL;
}

struct blk_desc *blk_get_dev(const char *ifname, int dev)
{
	return NULL;
}
#endif

#ifdef CONFIG_HAVE_BLOCK_DEVICE

/* ------------------------------------------------------------------------- */
/*
 * reports device info to the user
 */

#ifdef CONFIG_LBA48
typedef uint64_t lba512_t;
#else
typedef lbaint_t lba512_t;
#endif

/*
 * Overflowless variant of (block_count * mul_by / 2**right_shift)
 * when 2**right_shift > mul_by
 */
static lba512_t lba512_muldiv(lba512_t block_count, lba512_t mul_by,
			      int right_shift)
{
	lba512_t bc_quot, bc_rem;

	/* x * m / d == x / d * m + (x % d) * m / d */
	bc_quot = block_count >> right_shift;
	bc_rem  = block_count - (bc_quot << right_shift);
	return bc_quot * mul_by + ((bc_rem * mul_by) >> right_shift);
}

void dev_print (struct blk_desc *dev_desc)
{
	lba512_t lba512; /* number of blocks if 512bytes block size */

	if (dev_desc->type == DEV_TYPE_UNKNOWN) {
		pr_crit ("not available\n");
		return;
	}

	switch (dev_desc->if_type) {
	case IF_TYPE_SCSI:
		pr_crit ("(%d:%d) Vendor: %s Prod.: %s Rev: %s\n",
			dev_desc->target,dev_desc->lun,
			dev_desc->vendor,
			dev_desc->product,
			dev_desc->revision);
		break;
	case IF_TYPE_ATAPI:
	case IF_TYPE_IDE:
	case IF_TYPE_SATA:
		pr_crit ("Model: %s Firm: %s Ser#: %s\n",
			dev_desc->vendor,
			dev_desc->revision,
			dev_desc->product);
		break;
	case IF_TYPE_SD:
	case IF_TYPE_MMC:
	case IF_TYPE_USB:
	case IF_TYPE_NVME:
	case IF_TYPE_PVBLOCK:
	case IF_TYPE_HOST:
		pr_crit ("Vendor: %s Rev: %s Prod: %s\n",
			dev_desc->vendor,
			dev_desc->revision,
			dev_desc->product);
		break;
	case IF_TYPE_VIRTIO:
		pr_crit("%s VirtIO Block Device\n", dev_desc->vendor);
		break;
	case IF_TYPE_DOC:
		pr_crit("device type DOC\n");
		return;
	case IF_TYPE_UNKNOWN:
		pr_crit("device type unknown\n");
		return;
	default:
		pr_crit("Unhandled device type: %i\n", dev_desc->if_type);
		return;
	}
	pr_crit ("            Type: ");
	if (dev_desc->removable)
		pr_crit ("Removable ");
	switch (dev_desc->type & 0x1F) {
	case DEV_TYPE_HARDDISK:
		pr_crit ("Hard Disk");
		break;
	case DEV_TYPE_CDROM:
		pr_crit ("CD ROM");
		break;
	case DEV_TYPE_OPDISK:
		pr_crit ("Optical Device");
		break;
	case DEV_TYPE_TAPE:
		pr_crit ("Tape");
		break;
	default:
		pr_crit ("# %02X #", dev_desc->type & 0x1F);
		break;
	}
	pr_crit ("\n");
	if (dev_desc->lba > 0L && dev_desc->blksz > 0L) {
		ulong mb, mb_quot, mb_rem, gb, gb_quot, gb_rem;
		lbaint_t lba;

		lba = dev_desc->lba;

		lba512 = (lba * (dev_desc->blksz/512));
		/* round to 1 digit */
		/* 2048 = (1024 * 1024) / 512 MB */
		mb = lba512_muldiv(lba512, 10, 11);

		mb_quot	= mb / 10;
		mb_rem	= mb - (10 * mb_quot);

		gb = mb / 1024;
		gb_quot	= gb / 10;
		gb_rem	= gb - (10 * gb_quot);
#ifdef CONFIG_LBA48
		if (dev_desc->lba48){
			pr_crit ("            Supports 48-bit addressing\n");
		}
#endif
#if defined(CONFIG_SYS_64BIT_LBA)
		pr_crit ("            Capacity: %lu.%lu MB = %lu.%lu GB (%llu x %lu)\n",
			mb_quot, mb_rem,
			gb_quot, gb_rem,
			lba,
			dev_desc->blksz);
#else
		pr_crit ("            Capacity: %lu.%lu MB = %lu.%lu GB (%lu x %lu)\n",
			mb_quot, mb_rem,
			gb_quot, gb_rem,
			(ulong)lba,
			dev_desc->blksz);
#endif
	} else {
		pr_crit ("            Capacity: not available\n");
	}
}
#endif

#ifdef CONFIG_HAVE_BLOCK_DEVICE

void part_init(struct blk_desc *dev_desc)
{
	struct part_driver *drv =
		ll_entry_start(struct part_driver, part_driver);
	const int n_ents = ll_entry_count(struct part_driver, part_driver);
	struct part_driver *entry;

	blkcache_invalidate(dev_desc->if_type, dev_desc->devnum);

	dev_desc->part_type = PART_TYPE_UNKNOWN;
	for (entry = drv; entry != drv + n_ents; entry++) {
		int ret;

		ret = entry->test(dev_desc);
		debug("%s: try '%s': ret=%d\n", __func__, entry->name, ret);
		if (!ret) {
			dev_desc->part_type = entry->part_type;
			break;
		}
	}
}

static void print_part_header(const char *type, struct blk_desc *dev_desc)
{
#if CONFIG_IS_ENABLED(MAC_PARTITION) || \
	CONFIG_IS_ENABLED(DOS_PARTITION) || \
	CONFIG_IS_ENABLED(ISO_PARTITION) || \
	CONFIG_IS_ENABLED(AMIGA_PARTITION) || \
	CONFIG_IS_ENABLED(EFI_PARTITION)
	pr_crit ("\nPartition Map for ");
	switch (dev_desc->if_type) {
	case IF_TYPE_IDE:
		pr_crit ("IDE");
		break;
	case IF_TYPE_SATA:
		pr_crit ("SATA");
		break;
	case IF_TYPE_SCSI:
		pr_crit ("SCSI");
		break;
	case IF_TYPE_ATAPI:
		pr_crit ("ATAPI");
		break;
	case IF_TYPE_USB:
		pr_crit ("USB");
		break;
	case IF_TYPE_DOC:
		pr_crit ("DOC");
		break;
	case IF_TYPE_MMC:
		pr_crit ("MMC");
		break;
	case IF_TYPE_HOST:
		pr_crit ("HOST");
		break;
	case IF_TYPE_NVME:
		pr_crit ("NVMe");
		break;
	case IF_TYPE_PVBLOCK:
		pr_crit("PV BLOCK");
		break;
	case IF_TYPE_VIRTIO:
		pr_crit("VirtIO");
		break;
	case IF_TYPE_EFI_MEDIA:
		pr_crit("EFI");
		break;
	default:
		pr_crit("UNKNOWN");
		break;
	}
	pr_crit (" device %d  --   Partition Type: %s\n\n",
			dev_desc->devnum, type);
#endif /* any CONFIG_..._PARTITION */
}

void part_print(struct blk_desc *dev_desc)
{
	struct part_driver *drv;

	drv = part_driver_lookup_type(dev_desc);
	if (!drv) {
		pr_crit("## Unknown partition table type %x\n",
		       dev_desc->part_type);
		return;
	}

	PRINTF("## Testing for valid %s partition ##\n", drv->name);
	print_part_header(drv->name, dev_desc);
	if (drv->print)
		drv->print(dev_desc);
}

#endif /* CONFIG_HAVE_BLOCK_DEVICE */

int part_get_info(struct blk_desc *dev_desc, int part,
		       struct disk_partition *info)
{
#ifdef CONFIG_HAVE_BLOCK_DEVICE
	struct part_driver *drv;

#if CONFIG_IS_ENABLED(PARTITION_UUIDS)
	/* The common case is no UUID support */
	info->uuid[0] = 0;
#endif
#ifdef CONFIG_PARTITION_TYPE_GUID
	info->type_guid[0] = 0;
#endif

	drv = part_driver_lookup_type(dev_desc);
	if (!drv) {
		debug("## Unknown partition table type %x\n",
		      dev_desc->part_type);
		return -EPROTONOSUPPORT;
	}
	if (!drv->get_info) {
		PRINTF("## Driver %s does not have the get_info() method\n",
		       drv->name);
		return -ENOSYS;
	}
	if (drv->get_info(dev_desc, part, info) == 0) {
		PRINTF("## Valid %s partition found ##\n", drv->name);
		return 0;
	}
#endif /* CONFIG_HAVE_BLOCK_DEVICE */

	return -ENOENT;
}

int part_get_info_whole_disk(struct blk_desc *dev_desc,
			     struct disk_partition *info)
{
	info->start = 0;
	info->size = dev_desc->lba;
	info->blksz = dev_desc->blksz;
	info->bootable = 0;
	strcpy((char *)info->type, BOOT_PART_TYPE);
	strcpy((char *)info->name, "Whole Disk");
#if CONFIG_IS_ENABLED(PARTITION_UUIDS)
	info->uuid[0] = 0;
#endif
#ifdef CONFIG_PARTITION_TYPE_GUID
	info->type_guid[0] = 0;
#endif

	return 0;
}

int blk_get_device_by_str(const char *ifname, const char *dev_hwpart_str,
			  struct blk_desc **dev_desc)
{
	char *ep;
	char *dup_str = NULL;
	const char *dev_str, *hwpart_str;
	int dev, hwpart;

	hwpart_str = strchr(dev_hwpart_str, '.');
	if (hwpart_str) {
		dup_str = strdup(dev_hwpart_str);
		dup_str[hwpart_str - dev_hwpart_str] = 0;
		dev_str = dup_str;
		hwpart_str++;
	} else {
		dev_str = dev_hwpart_str;
		hwpart = 0;
	}

	dev = hextoul(dev_str, &ep);
	if (*ep) {
		PRINTF("** Bad device specification %s %s **\n",
		       ifname, dev_str);
		dev = -EINVAL;
		goto cleanup;
	}

	if (hwpart_str) {
		hwpart = hextoul(hwpart_str, &ep);
		if (*ep) {
			pr_err("** Bad HW partition specification %s %s **\n",
			    ifname, hwpart_str);
			dev = -EINVAL;
			goto cleanup;
		}
	}

	*dev_desc = get_dev_hwpart(ifname, dev, hwpart);
	if (!(*dev_desc) || ((*dev_desc)->type == DEV_TYPE_UNKNOWN)) {
		debug("** Bad device %s %s **\n", ifname, dev_hwpart_str);
		dev = -ENODEV;
		goto cleanup;
	}

#ifdef CONFIG_HAVE_BLOCK_DEVICE
	/*
	 * Updates the partition table for the specified hw partition.
	 * Always should be done, otherwise hw partition 0 will return stale
	 * data after displaying a non-zero hw partition.
	 */
	if ((*dev_desc)->if_type == IF_TYPE_MMC)
		part_init(*dev_desc);
#endif

cleanup:
	free(dup_str);
	return dev;
}

#define PART_UNSPECIFIED -2
#define PART_AUTO -1
int blk_get_device_part_str(const char *ifname, const char *dev_part_str,
			     struct blk_desc **dev_desc,
			     struct disk_partition *info, int allow_whole_dev)
{
	int ret;
	const char *part_str;
	char *dup_str = NULL;
	const char *dev_str;
	int dev;
	char *ep;
	int p;
	int part;
	struct disk_partition tmpinfo;

#if IS_ENABLED(CONFIG_SANDBOX) || IS_ENABLED(CONFIG_SEMIHOSTING)
	/*
	 * Special-case a pseudo block device "hostfs", to allow access to the
	 * host's own filesystem.
	 */
	if (0 == strcmp(ifname, "hostfs")) {
		*dev_desc = NULL;
		info->start = 0;
		info->size = 0;
		info->blksz = 0;
		info->bootable = 0;
		strcpy((char *)info->type, BOOT_PART_TYPE);
		strcpy((char *)info->name, "Host filesystem");
#if CONFIG_IS_ENABLED(PARTITION_UUIDS)
		info->uuid[0] = 0;
#endif
#ifdef CONFIG_PARTITION_TYPE_GUID
		info->type_guid[0] = 0;
#endif

		return 0;
	}
#endif

#if IS_ENABLED(CONFIG_CMD_UBIFS) && !IS_ENABLED(CONFIG_SPL_BUILD)
	/*
	 * Special-case ubi, ubi goes through a mtd, rather than through
	 * a regular block device.
	 */
	if (0 == strcmp(ifname, "ubi")) {
		if (!ubifs_is_mounted()) {
			pr_err("UBIFS not mounted, use ubifsmount to mount volume first!\n");
			return -EINVAL;
		}

		*dev_desc = NULL;
		memset(info, 0, sizeof(*info));
		strcpy((char *)info->type, BOOT_PART_TYPE);
		strcpy((char *)info->name, "UBI");
#if CONFIG_IS_ENABLED(PARTITION_UUIDS)
		info->uuid[0] = 0;
#endif
		return 0;
	}
#endif

	/* If no dev_part_str, use bootdevice environment variable */
	if (!dev_part_str || !strlen(dev_part_str) ||
	    !strcmp(dev_part_str, "-"))
		dev_part_str = env_get("bootdevice");

	/* If still no dev_part_str, it's an error */
	if (!dev_part_str) {
		pr_err("** No device specified **\n");
		ret = -ENODEV;
		goto cleanup;
	}

	/* Separate device and partition ID specification */
	part_str = strchr(dev_part_str, ':');
	if (part_str) {
		dup_str = strdup(dev_part_str);
		dup_str[part_str - dev_part_str] = 0;
		dev_str = dup_str;
		part_str++;
	} else {
		dev_str = dev_part_str;
	}

	/* Look up the device */
	dev = blk_get_device_by_str(ifname, dev_str, dev_desc);
	if (dev < 0) {
		PRINTF("** Bad device specification %s %s **\n",
		       ifname, dev_str);
		ret = dev;
		goto cleanup;
	}

	/* Convert partition ID string to number */
	if (!part_str || !*part_str) {
		part = PART_UNSPECIFIED;
	} else if (!strcmp(part_str, "auto")) {
		part = PART_AUTO;
	} else {
		/* Something specified -> use exactly that */
		part = (int)hextoul(part_str, &ep);
		/*
		 * Less than whole string converted,
		 * or request for whole device, but caller requires partition.
		 */
		if (*ep || (part == 0 && !allow_whole_dev)) {
			pr_err("** Bad partition specification %s %s **\n",
			    ifname, dev_part_str);
			ret = -ENOENT;
			goto cleanup;
		}
	}

	/*
	 * No partition table on device,
	 * or user requested partition 0 (entire device).
	 */
	if (((*dev_desc)->part_type == PART_TYPE_UNKNOWN) ||
	    (part == 0)) {
		if (!(*dev_desc)->lba) {
			pr_err("** Bad device size - %s %s **\n", ifname,
			       dev_str);
			ret = -EINVAL;
			goto cleanup;
		}

		/*
		 * If user specified a partition ID other than 0,
		 * or the calling command only accepts partitions,
		 * it's an error.
		 */
		if ((part > 0) || (!allow_whole_dev)) {
			pr_err("** No partition table - %s %s **\n", ifname,
			       dev_str);
			ret = -EPROTONOSUPPORT;
			goto cleanup;
		}

		(*dev_desc)->log2blksz = LOG2((*dev_desc)->blksz);

		part_get_info_whole_disk(*dev_desc, info);

		ret = 0;
		goto cleanup;
	}

	/*
	 * Now there's known to be a partition table,
	 * not specifying a partition means to pick partition 1.
	 */
	if (part == PART_UNSPECIFIED)
		part = 1;

	/*
	 * If user didn't specify a partition number, or did specify something
	 * other than "auto", use that partition number directly.
	 */
	if (part != PART_AUTO) {
		ret = part_get_info(*dev_desc, part, info);
		if (ret) {
			pr_err("** Invalid partition %d **\n", part);
			goto cleanup;
		}
	} else {
		/*
		 * Find the first bootable partition.
		 * If none are bootable, fall back to the first valid partition.
		 */
		part = 0;
		for (p = 1; p <= MAX_SEARCH_PARTITIONS; p++) {
			ret = part_get_info(*dev_desc, p, info);
			if (ret)
				continue;

			/*
			 * First valid partition, or new better partition?
			 * If so, save partition ID.
			 */
			if (!part || info->bootable)
				part = p;

			/* Best possible partition? Stop searching. */
			if (info->bootable)
				break;

			/*
			 * We now need to search further for best possible.
			 * If we what we just queried was the best so far,
			 * save the info since we over-write it next loop.
			 */
			if (part == p)
				tmpinfo = *info;
		}
		/* If we found any acceptable partition */
		if (part) {
			/*
			 * If we searched all possible partition IDs,
			 * return the first valid partition we found.
			 */
			if (p == MAX_SEARCH_PARTITIONS + 1)
				*info = tmpinfo;
		} else {
			pr_err("** No valid partitions found **\n");
			goto cleanup;
		}
	}
	if (strncmp((char *)info->type, BOOT_PART_TYPE, sizeof(info->type)) != 0) {
		pr_err("** Invalid partition type \"%.32s\""
			" (expect \"" BOOT_PART_TYPE "\")\n",
			info->type);
		ret  = -EINVAL;
		goto cleanup;
	}

	(*dev_desc)->log2blksz = LOG2((*dev_desc)->blksz);

	ret = part;
	goto cleanup;

cleanup:
	free(dup_str);
	return ret;
}

int part_get_info_by_name_type(struct blk_desc *dev_desc, const char *name,
			       struct disk_partition *info, int part_type)
{
	struct part_driver *part_drv;
	int ret;
	int i;

	part_drv = part_driver_lookup_type(dev_desc);
	if (!part_drv)
		return -1;

	if (!part_drv->get_info) {
		log_debug("## Driver %s does not have the get_info() method\n",
			  part_drv->name);
		return -ENOSYS;
	}

	for (i = 1; i < part_drv->max_entries; i++) {
		ret = part_drv->get_info(dev_desc, i, info);
		if (ret != 0) {
			/* no more entries in table */
			break;
		}
		if (strcmp(name, (const char *)info->name) == 0) {
			/* matched */
			return i;
		}
	}

	return -ENOENT;
}

int part_get_info_by_name(struct blk_desc *dev_desc, const char *name,
			  struct disk_partition *info)
{
	return part_get_info_by_name_type(dev_desc, name, info, PART_TYPE_ALL);
}

/**
 * Get partition info from device number and partition name.
 *
 * Parse a device number and partition name string in the form of
 * "devicenum.hwpartnum#partition_name", for example "0.1#misc". devicenum and
 * hwpartnum are both optional, defaulting to 0. If the partition is found,
 * sets dev_desc and part_info accordingly with the information of the
 * partition with the given partition_name.
 *
 * @param[in] dev_iface Device interface
 * @param[in] dev_part_str Input string argument, like "0.1#misc"
 * @param[out] dev_desc Place to store the device description pointer
 * @param[out] part_info Place to store the partition information
 * Return: 0 on success, or a negative on error
 */
static int part_get_info_by_dev_and_name(const char *dev_iface,
					 const char *dev_part_str,
					 struct blk_desc **dev_desc,
					 struct disk_partition *part_info)
{
	char *dup_str = NULL;
	const char *dev_str, *part_str;
	int ret;

	/* Separate device and partition name specification */
	if (dev_part_str)
		part_str = strchr(dev_part_str, '#');
	else
		part_str = NULL;

	if (part_str) {
		dup_str = strdup(dev_part_str);
		dup_str[part_str - dev_part_str] = 0;
		dev_str = dup_str;
		part_str++;
	} else {
		return -EINVAL;
	}

	ret = blk_get_device_by_str(dev_iface, dev_str, dev_desc);
	if (ret < 0)
		goto cleanup;

	ret = part_get_info_by_name(*dev_desc, part_str, part_info);
	if (ret < 0){
		pr_crit("Could not find \"%s\" partition\n", part_str);
	}

cleanup:
	free(dup_str);
	return ret;
}

int part_get_info_by_dev_and_name_or_num(const char *dev_iface,
					 const char *dev_part_str,
					 struct blk_desc **dev_desc,
					 struct disk_partition *part_info,
					 int allow_whole_dev)
{
	int ret;

	/* Split the part_name if passed as "$dev_num#part_name". */
	ret = part_get_info_by_dev_and_name(dev_iface, dev_part_str,
					    dev_desc, part_info);
	if (ret >= 0)
		return ret;
	/*
	 * Couldn't lookup by name, try looking up the partition description
	 * directly.
	 */
	ret = blk_get_device_part_str(dev_iface, dev_part_str,
				      dev_desc, part_info, allow_whole_dev);
	if (ret < 0){
		pr_err("Couldn't find partition %s %s\n",
		       dev_iface, dev_part_str);
	}
	return ret;
}

void part_set_generic_name(const struct blk_desc *dev_desc,
	int part_num, char *name)
{
	char *devtype;

	switch (dev_desc->if_type) {
	case IF_TYPE_IDE:
	case IF_TYPE_SATA:
	case IF_TYPE_ATAPI:
		devtype = "hd";
		break;
	case IF_TYPE_SCSI:
		devtype = "sd";
		break;
	case IF_TYPE_USB:
		devtype = "usbd";
		break;
	case IF_TYPE_DOC:
		devtype = "docd";
		break;
	case IF_TYPE_MMC:
	case IF_TYPE_SD:
		devtype = "mmcsd";
		break;
	default:
		devtype = "xx";
		break;
	}

	sprintf(name, "%s%c%d", devtype, 'a' + dev_desc->devnum, part_num);
}
