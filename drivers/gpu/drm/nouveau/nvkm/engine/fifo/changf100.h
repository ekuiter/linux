/* SPDX-License-Identifier: MIT */
#ifndef __GF100_FIFO_CHAN_H__
#define __GF100_FIFO_CHAN_H__
#define gf100_fifo_chan(p) container_of((p), struct gf100_fifo_chan, base)
#include "chan.h"
#include "gf100.h"

struct gf100_fifo_chan {
	struct nvkm_fifo_chan base;

#define GF100_FIFO_ENGN_GR     0
#define GF100_FIFO_ENGN_MSPDEC 1
#define GF100_FIFO_ENGN_MSPPP  2
#define GF100_FIFO_ENGN_MSVLD  3
#define GF100_FIFO_ENGN_CE0    4
#define GF100_FIFO_ENGN_CE1    5
#define GF100_FIFO_ENGN_SW     15
};

extern const struct nvkm_fifo_chan_oclass gf100_fifo_gpfifo_oclass;
#endif
