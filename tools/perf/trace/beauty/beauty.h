#ifndef _PERF_TRACE_BEAUTY_H
#define _PERF_TRACE_BEAUTY_H

#include <linux/types.h>

struct trace;
struct thread;

/**
 * @val: value of syscall argument being formatted
 * @args: All the args, use syscall_args__val(arg, nth) to access one
 * @thread: tid state (maps, pid, tid, etc)
 * @trace: 'perf trace' internals: all threads, etc
 * @parm: private area, may be an strarray, for instance
 * @idx: syscall arg idx (is this the first?)
 * @mask: a syscall arg may mask another arg, see syscall_arg__scnprintf_futex_op
 */

struct syscall_arg {
	unsigned long val;
	unsigned char *args;
	struct thread *thread;
	struct trace  *trace;
	void	      *parm;
	u8	      idx;
	u8	      mask;
};

unsigned long syscall_arg__val(struct syscall_arg *arg, u8 idx);

size_t syscall_arg__scnprintf_strarrays(char *bf, size_t size, struct syscall_arg *arg);
#define SCA_STRARRAYS syscall_arg__scnprintf_strarrays

size_t syscall_arg__scnprintf_hex(char *bf, size_t size, struct syscall_arg *arg);
#define SCA_HEX syscall_arg__scnprintf_hex

size_t syscall_arg__scnprintf_int(char *bf, size_t size, struct syscall_arg *arg);
#define SCA_INT syscall_arg__scnprintf_int

size_t syscall_arg__scnprintf_long(char *bf, size_t size, struct syscall_arg *arg);
#define SCA_LONG syscall_arg__scnprintf_long

size_t syscall_arg__scnprintf_fcntl_cmd(char *bf, size_t size, struct syscall_arg *arg);
#define SCA_FCNTL_CMD syscall_arg__scnprintf_fcntl_cmd

size_t syscall_arg__scnprintf_fcntl_arg(char *bf, size_t size, struct syscall_arg *arg);
#define SCA_FCNTL_ARG syscall_arg__scnprintf_fcntl_arg

size_t syscall_arg__scnprintf_statx_flags(char *bf, size_t size, struct syscall_arg *arg);
#define SCA_STATX_FLAGS syscall_arg__scnprintf_statx_flags

size_t syscall_arg__scnprintf_statx_mask(char *bf, size_t size, struct syscall_arg *arg);
#define SCA_STATX_MASK syscall_arg__scnprintf_statx_mask

size_t open__scnprintf_flags(unsigned long flags, char *bf, size_t size);

void syscall_arg__set_ret_scnprintf(struct syscall_arg *arg,
				    size_t (*ret_scnprintf)(unsigned long val, char *bf, size_t size));

#endif /* _PERF_TRACE_BEAUTY_H */
