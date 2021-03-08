// SPDX-License-Identifier: GPL-2.0-only
/*
 * powerpc code to implement the kexec_file_load syscall
 *
 * Copyright (C) 2004  Adam Litke (agl@us.ibm.com)
 * Copyright (C) 2004  IBM Corp.
 * Copyright (C) 2004,2005  Milton D Miller II, IBM Corporation
 * Copyright (C) 2005  R Sharada (sharada@in.ibm.com)
 * Copyright (C) 2006  Mohan Kumar M (mohan@in.ibm.com)
 * Copyright (C) 2016  IBM Corporation
 *
 * Based on kexec-tools' kexec-elf-ppc64.c, fs2dt.c.
 * Heavily modified for the kernel by
 * Thiago Jung Bauermann <bauerman@linux.vnet.ibm.com>.
 */

#include <linux/slab.h>
#include <linux/kexec.h>
#include <linux/of_fdt.h>
#include <linux/libfdt.h>
#include <asm/setup.h>
#include <asm/ima.h>

#define SLAVE_CODE_SIZE		256	/* First 0x100 bytes */

/**
 * setup_kdump_cmdline - Prepend "elfcorehdr=<addr> " to command line
 *                       of kdump kernel for exporting the core.
 * @image:               Kexec image
 * @cmdline:             Command line parameters to update.
 * @cmdline_len:         Length of the cmdline parameters.
 *
 * kdump segment must be setup before calling this function.
 *
 * Returns new cmdline buffer for kdump kernel on success, NULL otherwise.
 */
char *setup_kdump_cmdline(struct kimage *image, char *cmdline,
			  unsigned long cmdline_len)
{
	int elfcorehdr_strlen;
	char *cmdline_ptr;

	cmdline_ptr = kzalloc(COMMAND_LINE_SIZE, GFP_KERNEL);
	if (!cmdline_ptr)
		return NULL;

	elfcorehdr_strlen = sprintf(cmdline_ptr, "elfcorehdr=0x%lx ",
				    image->elf_load_addr);

	if (elfcorehdr_strlen + cmdline_len > COMMAND_LINE_SIZE) {
		pr_err("Appending elfcorehdr=<addr> exceeds cmdline size\n");
		kfree(cmdline_ptr);
		return NULL;
	}

	memcpy(cmdline_ptr + elfcorehdr_strlen, cmdline, cmdline_len);
	// Ensure it's nul terminated
	cmdline_ptr[COMMAND_LINE_SIZE - 1] = '\0';
	return cmdline_ptr;
}

/**
 * setup_purgatory - initialize the purgatory's global variables
 * @image:		kexec image.
 * @slave_code:		Slave code for the purgatory.
 * @fdt:		Flattened device tree for the next kernel.
 * @kernel_load_addr:	Address where the kernel is loaded.
 * @fdt_load_addr:	Address where the flattened device tree is loaded.
 *
 * Return: 0 on success, or negative errno on error.
 */
int setup_purgatory(struct kimage *image, const void *slave_code,
		    const void *fdt, unsigned long kernel_load_addr,
		    unsigned long fdt_load_addr)
{
	unsigned int *slave_code_buf, master_entry;
	int ret;

	slave_code_buf = kmalloc(SLAVE_CODE_SIZE, GFP_KERNEL);
	if (!slave_code_buf)
		return -ENOMEM;

	/* Get the slave code from the new kernel and put it in purgatory. */
	ret = kexec_purgatory_get_set_symbol(image, "purgatory_start",
					     slave_code_buf, SLAVE_CODE_SIZE,
					     true);
	if (ret) {
		kfree(slave_code_buf);
		return ret;
	}

	master_entry = slave_code_buf[0];
	memcpy(slave_code_buf, slave_code, SLAVE_CODE_SIZE);
	slave_code_buf[0] = master_entry;
	ret = kexec_purgatory_get_set_symbol(image, "purgatory_start",
					     slave_code_buf, SLAVE_CODE_SIZE,
					     false);
	kfree(slave_code_buf);

	ret = kexec_purgatory_get_set_symbol(image, "kernel", &kernel_load_addr,
					     sizeof(kernel_load_addr), false);
	if (ret)
		return ret;
	ret = kexec_purgatory_get_set_symbol(image, "dt_offset", &fdt_load_addr,
					     sizeof(fdt_load_addr), false);
	if (ret)
		return ret;

	return 0;
}

/**
 * delete_fdt_mem_rsv - delete memory reservation with given address and size
 *
 * Return: 0 on success, or negative errno on error.
 */
int delete_fdt_mem_rsv(void *fdt, unsigned long start, unsigned long size)
{
	int i, ret, num_rsvs = fdt_num_mem_rsv(fdt);

	for (i = 0; i < num_rsvs; i++) {
		uint64_t rsv_start, rsv_size;

		ret = fdt_get_mem_rsv(fdt, i, &rsv_start, &rsv_size);
		if (ret) {
			pr_err("Malformed device tree.\n");
			return -EINVAL;
		}

		if (rsv_start == start && rsv_size == size) {
			ret = fdt_del_mem_rsv(fdt, i);
			if (ret) {
				pr_err("Error deleting device tree reservation.\n");
				return -EINVAL;
			}

			return 0;
		}
	}

	return -ENOENT;
}

/*
 * setup_new_fdt - modify /chosen and memory reservation for the next kernel
 * @image:		kexec image being loaded.
 * @fdt:		Flattened device tree for the next kernel.
 * @initrd_load_addr:	Address where the next initrd will be loaded.
 * @initrd_len:		Size of the next initrd, or 0 if there will be none.
 * @cmdline:		Command line for the next kernel, or NULL if there will
 *			be none.
 *
 * Return: 0 on success, or negative errno on error.
 */
int setup_new_fdt(const struct kimage *image, void *fdt,
		  unsigned long initrd_load_addr, unsigned long initrd_len,
		  const char *cmdline)
{
	int ret;

	ret = setup_ima_buffer(image, fdt, fdt_path_offset(fdt, "/chosen"));
	if (ret)
		pr_err("Error setting up the new device tree.\n");

	return ret;
}
