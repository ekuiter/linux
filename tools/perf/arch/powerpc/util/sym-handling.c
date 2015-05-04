/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.
 *
 * Copyright (C) 2015 Naveen N. Rao, IBM Corporation
 */

#include "debug.h"
#include "symbol.h"
#include "map.h"

#ifdef HAVE_LIBELF_SUPPORT
bool elf__needs_adjust_symbols(GElf_Ehdr ehdr)
{
	return ehdr.e_type == ET_EXEC ||
	       ehdr.e_type == ET_REL ||
	       ehdr.e_type == ET_DYN;
}
#endif

#if !defined(_CALL_ELF) || _CALL_ELF != 2
int arch__choose_best_symbol(struct symbol *syma,
			     struct symbol *symb __maybe_unused)
{
	char *sym = syma->name;

	/* Skip over any initial dot */
	if (*sym == '.')
		sym++;

	/* Avoid "SyS" kernel syscall aliases */
	if (strlen(sym) >= 3 && !strncmp(sym, "SyS", 3))
		return SYMBOL_B;
	if (strlen(sym) >= 10 && !strncmp(sym, "compat_SyS", 10))
		return SYMBOL_B;

	return SYMBOL_A;
}

/* Allow matching against dot variants */
int arch__compare_symbol_names(const char *namea, const char *nameb)
{
	/* Skip over initial dot */
	if (*namea == '.')
		namea++;
	if (*nameb == '.')
		nameb++;

	return strcmp(namea, nameb);
}
#endif
