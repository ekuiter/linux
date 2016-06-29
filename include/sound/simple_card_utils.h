/*
 * simple_card_core.h
 *
 * Copyright (c) 2016 Kuninori Morimoto <kuninori.morimoto.gx@renesas.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef __SIMPLE_CARD_CORE_H
#define __SIMPLE_CARD_CORE_H

#include <sound/soc.h>

int asoc_simple_card_parse_daifmt(struct device *dev,
				  struct device_node *node,
				  struct device_node *codec,
				  char *prefix,
				  unsigned int *retfmt);

#endif /* __SIMPLE_CARD_CORE_H */
