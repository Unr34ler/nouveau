/*
 * Copyright 2012 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Ben Skeggs
 */
#include "nv04.h"

#include <core/client.h>
#include <core/engctx.h>
#include <core/ramht.h>
#include <subdev/instmem.h>

#include <nvif/class.h>
#include <nvif/unpack.h>

static struct ramfc_desc
nv17_ramfc[] = {
	{ 32,  0, 0x00,  0, NV04_PFIFO_CACHE1_DMA_PUT },
	{ 32,  0, 0x04,  0, NV04_PFIFO_CACHE1_DMA_GET },
	{ 32,  0, 0x08,  0, NV10_PFIFO_CACHE1_REF_CNT },
	{ 16,  0, 0x0c,  0, NV04_PFIFO_CACHE1_DMA_INSTANCE },
	{ 16, 16, 0x0c,  0, NV04_PFIFO_CACHE1_DMA_DCOUNT },
	{ 32,  0, 0x10,  0, NV04_PFIFO_CACHE1_DMA_STATE },
	{ 32,  0, 0x14,  0, NV04_PFIFO_CACHE1_DMA_FETCH },
	{ 32,  0, 0x18,  0, NV04_PFIFO_CACHE1_ENGINE },
	{ 32,  0, 0x1c,  0, NV04_PFIFO_CACHE1_PULL1 },
	{ 32,  0, 0x20,  0, NV10_PFIFO_CACHE1_ACQUIRE_VALUE },
	{ 32,  0, 0x24,  0, NV10_PFIFO_CACHE1_ACQUIRE_TIMESTAMP },
	{ 32,  0, 0x28,  0, NV10_PFIFO_CACHE1_ACQUIRE_TIMEOUT },
	{ 32,  0, 0x2c,  0, NV10_PFIFO_CACHE1_SEMAPHORE },
	{ 32,  0, 0x30,  0, NV10_PFIFO_CACHE1_DMA_SUBROUTINE },
	{}
};

/*******************************************************************************
 * FIFO channel objects
 ******************************************************************************/

static int
nv17_fifo_chan_ctor(struct nvkm_object *parent,
		    struct nvkm_object *engine,
		    struct nvkm_oclass *oclass, void *data, u32 size,
		    struct nvkm_object **pobject)
{
	union {
		struct nv03_channel_dma_v0 v0;
	} *args = data;
	struct nv04_fifo *fifo = (void *)engine;
	struct nv04_fifo_chan *chan;
	int ret;

	nvif_ioctl(parent, "create channel dma size %d\n", size);
	if (nvif_unpack(args->v0, 0, 0, false)) {
		nvif_ioctl(parent, "create channel dma vers %d pushbuf %llx "
				   "offset %08x\n", args->v0.version,
			   args->v0.pushbuf, args->v0.offset);
	} else
		return ret;

	ret = nvkm_fifo_channel_create(parent, engine, oclass, 0, 0x800000,
				       0x10000, args->v0.pushbuf,
				       (1ULL << NVDEV_ENGINE_DMAOBJ) |
				       (1ULL << NVDEV_ENGINE_SW) |
				       (1ULL << NVDEV_ENGINE_GR) |
				       (1ULL << NVDEV_ENGINE_MPEG), /* NV31- */
				       &chan);
	*pobject = nv_object(chan);
	if (ret)
		return ret;

	args->v0.chid = chan->base.chid;

	nv_parent(chan)->object_attach = nv04_fifo_object_attach;
	nv_parent(chan)->object_detach = nv04_fifo_object_detach;
	nv_parent(chan)->context_attach = nv04_fifo_context_attach;
	chan->ramfc = chan->base.chid * 64;

	nvkm_kmap(fifo->ramfc);
	nvkm_wo32(fifo->ramfc, chan->ramfc + 0x00, args->v0.offset);
	nvkm_wo32(fifo->ramfc, chan->ramfc + 0x04, args->v0.offset);
	nvkm_wo32(fifo->ramfc, chan->ramfc + 0x0c, chan->base.pushgpu->addr >> 4);
	nvkm_wo32(fifo->ramfc, chan->ramfc + 0x14,
			     NV_PFIFO_CACHE1_DMA_FETCH_TRIG_128_BYTES |
			     NV_PFIFO_CACHE1_DMA_FETCH_SIZE_128_BYTES |
#ifdef __BIG_ENDIAN
			     NV_PFIFO_CACHE1_BIG_ENDIAN |
#endif
			     NV_PFIFO_CACHE1_DMA_FETCH_MAX_REQS_8);
	nvkm_done(fifo->ramfc);
	return 0;
}

static struct nvkm_ofuncs
nv17_fifo_ofuncs = {
	.ctor = nv17_fifo_chan_ctor,
	.dtor = nv04_fifo_chan_dtor,
	.init = nv04_fifo_chan_init,
	.fini = nv04_fifo_chan_fini,
	.map  = _nvkm_fifo_channel_map,
	.rd32 = _nvkm_fifo_channel_rd32,
	.wr32 = _nvkm_fifo_channel_wr32,
	.ntfy = _nvkm_fifo_channel_ntfy
};

static struct nvkm_oclass
nv17_fifo_sclass[] = {
	{ NV17_CHANNEL_DMA, &nv17_fifo_ofuncs },
	{}
};

/*******************************************************************************
 * FIFO context - basically just the instmem reserved for the channel
 ******************************************************************************/

static struct nvkm_oclass
nv17_fifo_cclass = {
	.handle = NV_ENGCTX(FIFO, 0x17),
	.ofuncs = &(struct nvkm_ofuncs) {
		.ctor = nv04_fifo_context_ctor,
		.dtor = _nvkm_fifo_context_dtor,
		.init = _nvkm_fifo_context_init,
		.fini = _nvkm_fifo_context_fini,
		.rd32 = _nvkm_fifo_context_rd32,
		.wr32 = _nvkm_fifo_context_wr32,
	},
};

/*******************************************************************************
 * PFIFO engine
 ******************************************************************************/

static int
nv17_fifo_ctor(struct nvkm_object *parent, struct nvkm_object *engine,
	       struct nvkm_oclass *oclass, void *data, u32 size,
	       struct nvkm_object **pobject)
{
	struct nvkm_device *device = (void *)parent;
	struct nvkm_instmem *imem = device->imem;
	struct nv04_fifo *fifo;
	int ret;

	ret = nvkm_fifo_create(parent, engine, oclass, 0, 31, &fifo);
	*pobject = nv_object(fifo);
	if (ret)
		return ret;

	nvkm_ramht_ref(imem->ramht, &fifo->ramht);
	nvkm_gpuobj_ref(imem->ramro, &fifo->ramro);
	nvkm_gpuobj_ref(imem->ramfc, &fifo->ramfc);

	nv_subdev(fifo)->unit = 0x00000100;
	nv_subdev(fifo)->intr = nv04_fifo_intr;
	nv_engine(fifo)->cclass = &nv17_fifo_cclass;
	nv_engine(fifo)->sclass = nv17_fifo_sclass;
	fifo->base.pause = nv04_fifo_pause;
	fifo->base.start = nv04_fifo_start;
	fifo->ramfc_desc = nv17_ramfc;
	return 0;
}

static int
nv17_fifo_init(struct nvkm_object *object)
{
	struct nv04_fifo *fifo = (void *)object;
	struct nvkm_device *device = fifo->base.engine.subdev.device;
	int ret;

	ret = nvkm_fifo_init(&fifo->base);
	if (ret)
		return ret;

	nvkm_wr32(device, NV04_PFIFO_DELAY_0, 0x000000ff);
	nvkm_wr32(device, NV04_PFIFO_DMA_TIMESLICE, 0x0101ffff);

	nvkm_wr32(device, NV03_PFIFO_RAMHT, (0x03 << 24) /* search 128 */ |
				       ((fifo->ramht->bits - 9) << 16) |
				        (fifo->ramht->gpuobj.addr >> 8));
	nvkm_wr32(device, NV03_PFIFO_RAMRO, fifo->ramro->addr >> 8);
	nvkm_wr32(device, NV03_PFIFO_RAMFC, fifo->ramfc->addr >> 8 | 0x00010000);

	nvkm_wr32(device, NV03_PFIFO_CACHE1_PUSH1, fifo->base.max);

	nvkm_wr32(device, NV03_PFIFO_INTR_0, 0xffffffff);
	nvkm_wr32(device, NV03_PFIFO_INTR_EN_0, 0xffffffff);

	nvkm_wr32(device, NV03_PFIFO_CACHE1_PUSH0, 1);
	nvkm_wr32(device, NV04_PFIFO_CACHE1_PULL0, 1);
	nvkm_wr32(device, NV03_PFIFO_CACHES, 1);
	return 0;
}

struct nvkm_oclass *
nv17_fifo_oclass = &(struct nvkm_oclass) {
	.handle = NV_ENGINE(FIFO, 0x17),
	.ofuncs = &(struct nvkm_ofuncs) {
		.ctor = nv17_fifo_ctor,
		.dtor = nv04_fifo_dtor,
		.init = nv17_fifo_init,
		.fini = _nvkm_fifo_fini,
	},
};
