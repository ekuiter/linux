#if defined(__i386__) || defined(__x86_64__)
#include "../../arch/x86/include/asm/barrier.h"
#elif defined(__arm__)
#include "../../arch/arm/include/asm/barrier.h"
#elif defined(__aarch64__)
#include "../../arch/arm64/include/asm/barrier.h"
#elif defined(__powerpc__)
#include "../../arch/powerpc/include/asm/barrier.h"
#elif defined(__s390__)
#include "../../arch/s390/include/asm/barrier.h"
#elif defined(__sh__)
#include "../../arch/sh/include/asm/barrier.h"
#elif defined(__sparc__)
#include "../../arch/sparc/include/asm/barrier.h"
#elif defined(__alpha__)
#include "../../arch/alpha/include/asm/barrier.h"
#elif defined(__ia64__)
#include "../../arch/ia64/include/asm/barrier.h"
#endif
