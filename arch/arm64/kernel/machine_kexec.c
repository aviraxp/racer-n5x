/*
 * kexec for arm64
 *
 * Copyright (C) Linaro.
 * Copyright (C) Futurewei Technologies.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/* Bypass purgatory for debugging. */
static const int bypass_purgatory = 1;

#include <linux/kexec.h>
#include <linux/libfdt_env.h>
#include <linux/of_fdt.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <asm/cacheflush.h>
#include <asm/system_misc.h>

/* Global variables for the relocate_kernel routine. */
extern const unsigned char relocate_new_kernel[];
extern const unsigned long relocate_new_kernel_size;
extern unsigned long arm64_kexec_dtb_addr;
extern unsigned long arm64_kexec_kimage_head;
extern unsigned long arm64_kexec_kimage_start;
#ifdef CONFIG_KEXEC_HARDBOOT
extern unsigned long arm64_kexec_hardboot;
void (*kexec_hardboot_hook)(void);
#endif

/**
 * kexec_is_kernel - Helper routine to check the kernel header signature.
 */
static bool kexec_is_kernel(const void *image)
{
	struct arm64_image_header {
		uint8_t pe_sig[2];
		uint16_t branch_code[3];
		uint64_t text_offset;
		uint64_t image_size;
		uint8_t flags[8];
		uint64_t reserved_1[3];
		uint8_t magic[4];
		uint32_t pe_header;
	} h;

        if (copy_from_user(&h, image, sizeof(struct arm64_image_header)))
		return false;

	if (!h.text_offset)
		return false;

	return (h.magic[0] == 'A'
		&& h.magic[1] == 'R'
		&& h.magic[2] == 'M'
		&& h.magic[3] == 0x64U);
}

/**
 * kexec_find_kernel_seg - Helper routine to find the kernel segment.
 */
static const struct kexec_segment *kexec_find_kernel_seg(
	const struct kimage *kimage)
{
	int i;

	for (i = 0; i < kimage->nr_segments; i++) {
		if (kexec_is_kernel(kimage->segment[i].buf))
			return &kimage->segment[i];
	}

	BUG();
	return NULL;
}

/**
 * kexec_is_dtb - Helper routine to check the device tree header signature.
 */
static bool kexec_is_dtb(const void *dtb)
{
	__be32 magic;

	if (get_user(magic, (__be32 *)dtb))
		return false;

	return fdt32_to_cpu(magic) == OF_DT_HEADER;
}

/**
 * kexec_find_dtb_seg - Helper routine to find the dtb segment.
 */
static const struct kexec_segment *kexec_find_dtb_seg(
	const struct kimage *kimage)
{
	int i;

	for (i = 0; i < kimage->nr_segments; i++) {
		if (kexec_is_dtb(kimage->segment[i].buf))
			return &kimage->segment[i];
	}

	BUG();
	return NULL;
}

static struct bypass {
	unsigned long kernel;
	unsigned long dtb;
} bypass;

static void fill_bypass(const struct kimage *kimage)
{
	const struct kexec_segment *seg;

	seg = kexec_find_kernel_seg(kimage);
	BUG_ON(!seg || !seg->mem);
	bypass.kernel = seg->mem;

	seg = kexec_find_dtb_seg(kimage);
	BUG_ON(!seg || !seg->mem);
	bypass.dtb = seg->mem;

	pr_debug("%s: kernel: %016lx\n", __func__, bypass.kernel);
	pr_debug("%s: dtb:    %016lx\n", __func__, bypass.dtb);
}

/**
 * kexec_image_info - For debugging output.
 */
#define kexec_image_info(_i) _kexec_image_info(__func__, __LINE__, _i)
static void _kexec_image_info(const char *func, int line,
	const struct kimage *image)
{
	unsigned long i;

#if !defined(DEBUG)
	return;
#endif
	pr_devel("%s:%d:\n", func, line);
	pr_devel("  kexec image info:\n");
	pr_devel("    type:        %d\n", image->type);
	pr_devel("    start:       %lx\n", image->start);
	pr_devel("    head:        %lx\n", image->head);
	pr_devel("    nr_segments: %lu\n", image->nr_segments);

	for (i = 0; i < image->nr_segments; i++) {
		pr_devel("      segment[%lu]: %016lx - %016lx, %lx bytes, %lu pages%s\n",
			i,
			image->segment[i].mem,
			image->segment[i].mem + image->segment[i].memsz,
			image->segment[i].memsz,
			image->segment[i].memsz /  PAGE_SIZE,
			(kexec_is_dtb(image->segment[i].buf) ?
				", dtb segment" : ""));
	}
}

void machine_kexec_cleanup(struct kimage *image)
{
	/* Empty routine needed to avoid build errors. */
}

/**
 * machine_kexec_prepare - Prepare for a kexec reboot.
 *
 * Called from the core kexec code when a kernel image is loaded.
 */
int machine_kexec_prepare(struct kimage *image)
{
#ifdef CONFIG_KEXEC_HARDBOOT
	unsigned long *hardboot_page;
#endif

	kexec_image_info(image);
	fill_bypass(image);
	if (bypass_purgatory) {
		arm64_kexec_kimage_start = bypass.kernel;
		arm64_kexec_dtb_addr = bypass.dtb;
	} else {
		arm64_kexec_kimage_start = image->start;
		arm64_kexec_dtb_addr = 0;
	}
#ifdef CONFIG_KEXEC_HARDBOOT
	arm64_kexec_hardboot = image->hardboot;

	// debug; please remove
	hardboot_page = ioremap(KEXEC_HB_PAGE_ADDR, SZ_1M);
	pr_info("Last hardboot status: %lx\n", hardboot_page[0]);
	iounmap(hardboot_page);
#endif

	return 0;
}

/**
 * kexec_list_flush - Helper to flush the kimage list to PoC.
 */
static void kexec_list_flush(unsigned long kimage_head)
{
	void *dest;
	unsigned long *entry;

	for (entry = &kimage_head, dest = NULL; ; entry++) {
		unsigned int flag = *entry &
			(IND_DESTINATION | IND_INDIRECTION | IND_DONE |
			IND_SOURCE);
		void *addr = phys_to_virt(*entry & PAGE_MASK);

		switch (flag) {
		case IND_INDIRECTION:
			entry = (unsigned long *)addr - 1;
			__flush_dcache_area(addr, PAGE_SIZE);
			break;
		case IND_DESTINATION:
			dest = addr;
			break;
		case IND_SOURCE:
			__flush_dcache_area(addr, PAGE_SIZE);
			dest += PAGE_SIZE;
			break;
		case IND_DONE:
			return;
		default:
			BUG();
		}
	}
}


/**
 * kexec_list_hardboot_create_post_reboot_list -
 * modify existing destination list to copy kernel to temp region;
 * create new destination list in hardboot page to copy from temp region
 * to final location
 */
static void kexec_list_hardboot_create_post_reboot_list(
	unsigned long kimage_head, unsigned long *newlist_start,
	unsigned long tempdest_phys)
{
	/* so the entries are in the format:
	 * IND_DESTINATION -> where to go
	 * IND_SOURCE -> where to read one page
	 * IND_SOURCE -> where to read the next page (and so on)
	 * For existing: rewrite IND_DESTINATION to store to temp location; leave IND_SOURCE intact
	 * For new: copy original IND_DESTINATION, rewrite new IND_SOURCE to read from temp location
	 * We do not copy indirection (new list will be flat)
	 */
	void *dest;
	unsigned long *entry;
	unsigned long *newlist = newlist_start;

	for (entry = &kimage_head, dest = NULL; ; entry++) {
		unsigned int flag = *entry &
			(IND_DESTINATION | IND_INDIRECTION | IND_DONE |
			IND_SOURCE);
		void *addr = phys_to_virt(*entry & PAGE_MASK);

		switch (flag) {
		case IND_INDIRECTION:
			entry = (unsigned long *)addr - 1;
			break;
		case IND_DESTINATION:
			// new list: copy original IND_DESTINATION
			*newlist++ = *entry;
			// old list: rewrite to store to temp location
			*entry = flag | tempdest_phys;
			break;
		case IND_SOURCE:
			// new list: rewrite to read from temp location
			*newlist++ = flag | tempdest_phys;
			// new list: add to new temp destination address
			tempdest_phys += PAGE_SIZE;
			break;
		case IND_DONE:
			*newlist++ = *entry; // new list: copy original IND_DONE
			return;
		default:
			BUG();
		}
	}
}

/**
 * machine_kexec - Do the kexec reboot.
 *
 * Called from the core kexec code for a sys_reboot with LINUX_REBOOT_CMD_KEXEC.
 */
void machine_kexec(struct kimage *image)
{
	phys_addr_t reboot_code_buffer_phys;
	void *reboot_code_buffer;

	BUG_ON(num_online_cpus() > 1);

	arm64_kexec_kimage_head = image->head;

	reboot_code_buffer_phys = page_to_phys(image->control_code_page);
	reboot_code_buffer = phys_to_virt(reboot_code_buffer_phys);

	kexec_image_info(image);

	pr_devel("%s:%d: control_code_page:        %p\n", __func__, __LINE__,
		image->control_code_page);
	pr_devel("%s:%d: reboot_code_buffer_phys:  %pa\n", __func__, __LINE__,
		&reboot_code_buffer_phys);
	pr_devel("%s:%d: reboot_code_buffer:       %p\n", __func__, __LINE__,
		reboot_code_buffer);
	pr_devel("%s:%d: relocate_new_kernel:      %p\n", __func__, __LINE__,
		relocate_new_kernel);
	pr_devel("%s:%d: relocate_new_kernel_size: 0x%lx(%lu) bytes\n",
		__func__, __LINE__, relocate_new_kernel_size,
		relocate_new_kernel_size);

	pr_devel("%s:%d: kexec_dtb_addr:           %lx\n", __func__, __LINE__,
		arm64_kexec_dtb_addr);
	pr_devel("%s:%d: kexec_kimage_head:        %lx\n", __func__, __LINE__,
		arm64_kexec_kimage_head);
	pr_devel("%s:%d: kexec_kimage_start:       %lx\n", __func__, __LINE__,
		arm64_kexec_kimage_start);

	/*
	 * Copy relocate_new_kernel to the reboot_code_buffer for use
	 * after the kernel is shut down.
	 */
	memcpy(reboot_code_buffer, relocate_new_kernel,
		relocate_new_kernel_size);

	/* Flush the reboot_code_buffer in preparation for its execution. */
	__flush_dcache_area(reboot_code_buffer, relocate_new_kernel_size);

#ifdef CONFIG_KEXEC_HARDBOOT
	if (image->hardboot) {
		// hardboot reserve should be 1MB.
		unsigned long hardboot_reserve = KEXEC_HB_PAGE_ADDR;
		void *hardboot_map = ioremap(hardboot_reserve, SZ_1M);
		// post reboot reloc code is 4K inside the hardboot page
		void* post_reboot_code_buffer = hardboot_map + PAGE_SIZE;
		// post reboot reloc list is 8K after the hardboot page.
		unsigned long post_reboot_list_loc = hardboot_reserve +
			(PAGE_SIZE * 2);
		unsigned long *hardboot_list_loc_virt = hardboot_map +
			(PAGE_SIZE * 2);

		// temp space is 64MB in front of hardboot reserve.
		// Must be big enough to hold kernel, initrd, and dtb.
		unsigned long tempdest = hardboot_reserve - (SZ_1M * 64);

		// create new relocation list for post reboot reloc
		// TODO: check for overflow of temp space and hardboot page
		kexec_list_hardboot_create_post_reboot_list(image->head,
			hardboot_list_loc_virt, tempdest);

		// setup post-reboot reloc code
		arm64_kexec_kimage_head = IND_INDIRECTION | post_reboot_list_loc;
		arm64_kexec_hardboot = 0;

		// copy relocation code to hardboot page for post-reboot reloc
		memcpy(post_reboot_code_buffer, relocate_new_kernel,
			relocate_new_kernel_size);

		// flush the entire hardboot page
		__flush_dcache_area(hardboot_map, SZ_1M);
		// unmap the page
		iounmap(hardboot_map);
	}
#endif

	/* Flush the kimage list. */
	kexec_list_flush(image->head);

#ifdef CONFIG_KEXEC_HARDBOOT
	/* Run any final machine-specific shutdown code. */
	if (image->hardboot && kexec_hardboot_hook)
		kexec_hardboot_hook();
#endif

	pr_info("Bye!\n");

	/* Disable all DAIF exceptions. */
	asm volatile ("msr daifset, #0xf" : : : "memory");

	/*
	 * soft_restart() will shutdown the MMU, disable data caches, then
	 * transfer control to the reboot_code_buffer which contains a copy of
	 * the relocate_new_kernel routine.  relocate_new_kernel will use
	 * physical addressing to relocate the new kernel to its final position
	 * and then will transfer control to the entry point of the new kernel.
	 */
	soft_restart(reboot_code_buffer_phys);

	BUG(); /* Should never get here. */
}

void machine_crash_shutdown(struct pt_regs *regs)
{
	/* Empty routine needed to avoid build errors. */
}

#ifdef CONFIG_KEXEC_HARDBOOT
bool arch_kexec_is_hardboot_buffer_range(unsigned long start,
	unsigned long end) {
	unsigned long hardboot_reserve = KEXEC_HB_PAGE_ADDR;
	unsigned long tempdest = hardboot_reserve - (SZ_1M * 64);
	// reserve is the end, tempdest is the start of the buffer
	return start < hardboot_reserve && end >= tempdest;
}
#endif
