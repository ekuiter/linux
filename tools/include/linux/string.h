#ifndef _TOOLS_LINUX_STRING_H_
#define _TOOLS_LINUX_STRING_H_


#include <linux/types.h>	/* for size_t */

void *memdup(const void *src, size_t len);

int strtobool(const char *s, bool *res);

#endif /* _LINUX_STRING_H_ */
