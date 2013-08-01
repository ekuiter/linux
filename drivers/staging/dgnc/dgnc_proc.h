/*
 * Copyright 2003 Digi International (www.digi.com)
 *      Scott H Kilau <Scott_Kilau at digi dot com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED; without even the
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *      NOTE: THIS IS A SHARED HEADER. DO NOT CHANGE CODING STYLE!!!
 *
 *
 *     $Id: dgnc_proc.h,v 1.1.1.1 2009/05/20 12:19:19 markh Exp $
 *
 *  Description:
 *
 *     Describes the private structures used to manipulate the "special"
 *     proc constructs (not read-only) used by the Digi Neo software.
 *     The concept is borrowed heavily from the "sysctl" interface of
 *     the kernel.  I decided not to use the structures and functions
 *     provided by the kernel for two reasons:
 *
 *       1. Due to the planned use of "/proc" in the Neo driver, many
 *          of the functions of the "sysctl" interface would go unused.
 *          A simpler interface will be easier to maintain.
 *
 *       2. I'd rather divorce our "added package" from the kernel internals.
 *          If the "sysctl" structures should change, I will be insulated
 *          from those changes.  These "/proc" entries won't be under the
 *          "sys" tree anyway, so there is no need to maintain a strict
 *          dependence relationship.
 *
 *  Author:
 *
 *     Scott H Kilau
 *
 */

#ifndef _DGNC_RW_PROC_H
#define _DGNC_RW_PROC_H

/*
 *  The list of DGNC entries with r/w capabilities. 
 *  These magic numbers are used for identification purposes.
 */
enum {
	DGNC_INFO = 1,			/* Get info about the running module	*/
	DGNC_MKNOD = 2,			/* Get info about driver devices	*/
	DGNC_BOARD_INFO = 3,		/* Get info about the specific board	*/
	DGNC_BOARD_VPD = 4,		/* Get info about the board's VPD	*/
	DGNC_BOARD_TTYSTATS = 5,	/* Get info about the board's tty stats	*/
	DGNC_BOARD_TTYINTR = 6,		/* Get info about the board's tty intrs	*/
	DGNC_BOARD_TTYFLAGS = 7,	/* Get info about the board's tty flags	*/
	DGNC_BOARD_MKNOD = 8,		/* Get info about board devices		*/
	DGNC_PORT_INFO = 9,		/* Get info about the specific port	*/
	DGNC_PORT_SNIFF = 10,		/* Sniff data in/out of specific port	*/
	DGNC_PORT_CUSTOM_TTYNAME = 11,	/* Get info about UDEV tty name		*/
	DGNC_PORT_CUSTOM_PRNAME = 12,	/* Get info about UDEV pr name		*/
};

/*
 *  Directions for proc handlers
 */
enum {
        INBOUND = 1,		/* Data being written to kernel */
        OUTBOUND = 2,		/* Data being read from the kernel */
};

/*
 *  Each entry in a DGNC proc directory is described with a
 *  "dgnc_proc_entry" structure.  A collection of these
 *  entries (in an array) represents the members associated
 *  with a particular "/proc" directory, and is referred to
 *  as a table.  All "tables" are terminated by an entry with
 *  zeros for every member.
 *
 *  The structure members are as follows:
 *
 *    int magic              -- ID number associated with this particular
 *                              entry.  Should be unique across all of
 *                              DGNC.
 *
 *    const char *name       -- ASCII name associated with the /proc entry.
 *
 *    mode_t mode            -- File access permisssions for the /proc entry.
 *
 *    dgnc_proc_entry *child -- When set, this entry refers to a directory,
 *                              and points to the table which describes the
 *                              entries in the subdirectory
 *
 *    dgnc_proc_handler *open_handler -- When set, points to the fxn which
 *                                       does any "extra" open stuff.
 *
 *    dgnc_proc_handler *close_handler -- When set, points to the fxn which
 *                                        does any "extra" close stuff.
 *
 *    dgnc_proc_handler *read_handler -- When set, points to the fxn which
 *                                       handle outbound data flow
 *
 *    dgnc_proc_handler *write_handler -- When set, points to the fxn which
 *                                        handles inbound data flow
 *
 *    struct proc_dir_entry *de -- Pointer to the directory entry for this
 *                                 object once registered.  Used to grab
 *                                 the handle of the object for
 *                                 unregistration
 *
 *    void *data;		   When set, points to the parent structure
 *
 */

struct dgnc_proc_entry {
	int		magic;		/* Integer identifier	*/
	const char	*name;		/* ASCII identifier	*/
	mode_t		mode;		/* File access permissions */
	struct dgnc_proc_entry *child;	/* Child pointer	*/

	int (*open_handler) (struct dgnc_proc_entry *table, int dir, struct file *filp,   
		void *buffer, ssize_t *lenp, loff_t *ppos); 
	int (*close_handler) (struct dgnc_proc_entry *table, int dir, struct file *filp,   
		void *buffer, ssize_t *lenp, loff_t *ppos); 
	int (*read_handler) (struct dgnc_proc_entry *table, int dir, struct file *filp,   
		char __user *buffer, ssize_t *lenp, loff_t *ppos); 
	int (*write_handler) (struct dgnc_proc_entry *table, int dir, struct file *filp,   
		const char __user *buffer, ssize_t *lenp, loff_t *ppos); 

	struct proc_dir_entry *de;	/* proc entry pointer	*/
	struct semaphore excl_sem;	/* Protects exclusive access var	*/
	int		excl_cnt;	/* Counts number of curr accesses	*/
	void		*data;		/* Allows storing a pointer to parent	*/
};

void dgnc_proc_register_basic_prescan(void);
void dgnc_proc_register_basic_postscan(int board_num);
void dgnc_proc_unregister_all(void);


#endif /* _DGNC_RW_PROC_H */
