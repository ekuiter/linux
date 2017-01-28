#ifndef _UAPI_ASM_E820_TYPES_H
#define _UAPI_ASM_E820_TYPES_H

/*
 * This is the maximum number of entries in struct boot_params::e820_table (the zeropage),
 * which is part of the x86 boot protocol ABI:
 */
#define E820_MAX_ENTRIES_ZEROPAGE 128

#ifndef __ASSEMBLY__

enum e820_type {
	E820_TYPE_RAM		= 1,
	E820_TYPE_RESERVED	= 2,
	E820_TYPE_ACPI		= 3,
	E820_TYPE_NVS		= 4,
	E820_TYPE_UNUSABLE	= 5,
	E820_TYPE_PMEM		= 7,

	/*
	 * This is a non-standardized way to represent ADR or
	 * NVDIMM regions that persist over a reboot.
	 *
	 * The kernel will ignore their special capabilities
	 * unless the CONFIG_X86_PMEM_LEGACY=y option is set.
	 *
	 * ( Note that older platforms also used 6 for the same
	 *   type of memory, but newer versions switched to 12 as
	 *   6 was assigned differently. Some time they will learn... )
	 */
	E820_TYPE_PRAM		= 12,

	/*
	 * Reserved RAM used by the kernel itself if
	 * CONFIG_INTEL_TXT=y is enabled, memory of this type
	 * will be included in the S3 integrity calculation
	 * and so should not include any memory that the BIOS
	 * might alter over the S3 transition:
	 */
	E820_TYPE_RESERVED_KERN	= 128,
};

/*
 * A single E820 map entry, describing a memory range of [addr...addr+size-1],
 * of 'type' memory type:
 */
struct e820_entry {
	__u64			addr;
	__u64			size;
	enum e820_type		type;
} __attribute__((packed));

#endif /* __ASSEMBLY__ */

#endif /* _UAPI_ASM_E820_TYPES_H */
