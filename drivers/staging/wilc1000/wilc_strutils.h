#ifndef __WILC_STRUTILS_H__
#define __WILC_STRUTILS_H__

/*!
 *  @file	wilc_strutils.h
 *  @brief	Basic string utilities
 *  @author	syounan
 *  @sa		wilc_oswrapper.h top level OS wrapper file
 *  @date	16 Aug 2010
 *  @version	1.0
 */

#include <linux/types.h>
#include <linux/string.h>
#include "wilc_errorsupport.h"


/*!
 *  @brief	Internal implementation for memory copy
 *  @param[in]	pvTarget the target buffer to which the data is copied into
 *  @param[in]	pvSource pointer to the second memory location
 *  @param[in]	u32Count the size of the data to copy
 *  @note	this function should not be used directly, use WILC_memcpy instead
 *  @author	syounan
 *  @date	18 Aug 2010
 *  @version	1.0
 */
void WILC_memcpy_INTERNAL(void *pvTarget, const void *pvSource, u32 u32Count);

/*!
 *  @brief	Copies the contents of a memory buffer into another
 *  @param[in]	pvTarget the target buffer to which the data is copied into
 *  @param[in]	pvSource pointer to the second memory location
 *  @param[in]	u32Count the size of the data to copy
 *  @return	WILC_SUCCESS if copy is successfully handeled
 *              WILC_FAIL if copy failed
 *  @note	this function repeats the functionality of standard memcpy,
 *              however memcpy is undefined if the two buffers overlap but this
 *              implementation will check for overlap and report error
 *  @author	syounan
 *  @date	18 Aug 2010
 *  @version	1.0
 */
static WILC_ErrNo WILC_memcpy(void *pvTarget, const void *pvSource, u32 u32Count)
{
	if (
		(((u8 *)pvTarget <= (u8 *)pvSource)
		 && (((u8 *)pvTarget + u32Count) > (u8 *)pvSource))

		|| (((u8 *)pvSource <= (u8 *)pvTarget)
		    && (((u8 *)pvSource + u32Count) > (u8 *)pvTarget))
		) {
		/* ovelapped memory, return Error */
		return WILC_FAIL;
	} else {
		WILC_memcpy_INTERNAL(pvTarget, pvSource, u32Count);
		return WILC_SUCCESS;
	}
}



/*!
 *  @brief	Compares two strings up to u32Count characters
 *  @details	Compares 2 strings reporting which is bigger, NULL is considered
 *              the smallest string, then a zero length string then all other
 *              strings depending on thier ascii characters order with small case
 *              converted to uppder case
 *  @param[in]	pcStr1 the first string, NULL is valid and considered smaller
 *              than any other non-NULL string (incliding zero lenght strings)
 *  @param[in]	pcStr2 the second string, NULL is valid and considered smaller
 *              than any other non-NULL string (incliding zero lenght strings)
 *  @param[in]	u32Count copying will proceed until a null character in pcStr1 or
 *              pcStr2 is encountered or u32Count of bytes copied
 *  @return	0 if the 2 strings are equal, 1 if pcStr1 is bigger than pcStr2,
 *              -1 if pcStr1 smaller than pcStr2
 *  @author	aabozaeid
 *  @date	7 Dec 2010
 *  @version	1.0
 */
s32 WILC_strncmp(const char *pcStr1, const char *pcStr2,
			 u32 u32Count);


#endif
