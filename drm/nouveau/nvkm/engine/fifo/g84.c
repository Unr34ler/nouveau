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
#include "nv50.h"
#include "nv04.h"

#include <core/client.h>
#include <core/engctx.h>
#include <core/ramht.h>
#include <subdev/bar.h>
#include <subdev/mmu.h>
#include <subdev/timer.h>

#include <nvif/class.h>
#include <nvif/unpack.h>

/*******************************************************************************
 * FIFO channel objects
 ******************************************************************************/

static int
g84_fifo_context_attach(struct nvkm_object *parent, struct nvkm_object *object)
{
	struct nvkm_bar *bar = nvkm_bar(parent);
	struct nv50_fifo_base *base = (void *)parent->parent;
	struct nvkm_gpuobj *ectx = (void *)object;
	u64 limit = ectx->addr + ectx->size - 1;
	u64 start = ectx->addr;
	u32 addr;

	switch (nv_engidx(object->engine)) {
	case NVDEV_ENGINE_SW    : return 0;
	case NVDEV_ENGINE_GR    : addr = 0x0020; break;
	case NVDEV_ENGINE_VP    :
	case NVDEV_ENGINE_MSPDEC: addr = 0x0040; break;
	case NVDEV_ENGINE_MSPPP :
	case NVDEV_ENGINE_MPEG  : addr = 0x0060; break;
	case NVDEV_ENGINE_BSP   :
	case NVDEV_ENGINE_MSVLD : addr = 0x0080; break;
	case NVDEV_ENGINE_CIPHER:
	case NVDEV_ENGINE_SEC   : addr = 0x00a0; break;
	case NVDEV_ENGINE_CE0   : addr = 0x00c0; break;
	default:
		return -EINVAL;
	}

	nv_engctx(ectx)->addr = nv_gpuobj(base)->addr >> 12;
	nv_wo32(base->eng, addr + 0x00, 0x00190000);
	nv_wo32(base->eng, addr + 0x04, lower_32_bits(limit));
	nv_wo32(base->eng, addr + 0x08, lower_32_bits(start));
	nv_wo32(base->eng, addr + 0x0c, upper_32_bits(limit) << 24 |
					upper_32_bits(start));
	nv_wo32(base->eng, addr + 0x10, 0x00000000);
	nv_wo32(base->eng, addr + 0x14, 0x00000000);
	bar->flush(bar);
	return 0;
}

static int
g84_fifo_context_detach(struct nvkm_object *parent, bool suspend,
			struct nvkm_object *object)
{
	struct nv50_fifo *fifo = (void *)parent->engine;
	struct nv50_fifo_base *base = (void *)parent->parent;
	struct nv50_fifo_chan *chan = (void *)parent;
	struct nvkm_device *device = fifo->base.engine.subdev.device;
	struct nvkm_bar *bar = device->bar;
	u32 addr, save, engn;
	bool done;

	switch (nv_engidx(object->engine)) {
	case NVDEV_ENGINE_SW    : return 0;
	case NVDEV_ENGINE_GR    : engn = 0; addr = 0x0020; break;
	case NVDEV_ENGINE_VP    :
	case NVDEV_ENGINE_MSPDEC: engn = 3; addr = 0x0040; break;
	case NVDEV_ENGINE_MSPPP :
	case NVDEV_ENGINE_MPEG  : engn = 1; addr = 0x0060; break;
	case NVDEV_ENGINE_BSP   :
	case NVDEV_ENGINE_MSVLD : engn = 5; addr = 0x0080; break;
	case NVDEV_ENGINE_CIPHER:
	case NVDEV_ENGINE_SEC   : engn = 4; addr = 0x00a0; break;
	case NVDEV_ENGINE_CE0   : engn = 2; addr = 0x00c0; break;
	default:
		return -EINVAL;
	}

	save = nvkm_mask(device, 0x002520, 0x0000003f, 1 << engn);
	nvkm_wr32(device, 0x0032fc, nv_gpuobj(base)->addr >> 12);
	done = nvkm_msec(device, 2000,
		if (nvkm_rd32(device, 0x0032fc) != 0xffffffff)
			break;
	) >= 0;
	nvkm_wr32(device, 0x002520, save);
	if (!done) {
		nv_error(fifo, "channel %d [%s] unload timeout\n",
			 chan->base.chid, nvkm_client_name(chan));
		if (suspend)
			return -EBUSY;
	}

	nv_wo32(base->eng, addr + 0x00, 0x00000000);
	nv_wo32(base->eng, addr + 0x04, 0x00000000);
	nv_wo32(base->eng, addr + 0x08, 0x00000000);
	nv_wo32(base->eng, addr + 0x0c, 0x00000000);
	nv_wo32(base->eng, addr + 0x10, 0x00000000);
	nv_wo32(base->eng, addr + 0x14, 0x00000000);
	bar->flush(bar);
	return 0;
}

static int
g84_fifo_object_attach(struct nvkm_object *parent,
		       struct nvkm_object *object, u32 handle)
{
	struct nv50_fifo_chan *chan = (void *)parent;
	u32 context;

	if (nv_iclass(object, NV_GPUOBJ_CLASS))
		context = nv_gpuobj(object)->node->offset >> 4;
	else
		context = 0x00000004; /* just non-zero */

	switch (nv_engidx(object->engine)) {
	case NVDEV_ENGINE_DMAOBJ:
	case NVDEV_ENGINE_SW    : context |= 0x00000000; break;
	case NVDEV_ENGINE_GR    : context |= 0x00100000; break;
	case NVDEV_ENGINE_MPEG  :
	case NVDEV_ENGINE_MSPPP : context |= 0x00200000; break;
	case NVDEV_ENGINE_ME    :
	case NVDEV_ENGINE_CE0   : context |= 0x00300000; break;
	case NVDEV_ENGINE_VP    :
	case NVDEV_ENGINE_MSPDEC: context |= 0x00400000; break;
	case NVDEV_ENGINE_CIPHER:
	case NVDEV_ENGINE_SEC   :
	case NVDEV_ENGINE_VIC   : context |= 0x00500000; break;
	case NVDEV_ENGINE_BSP   :
	case NVDEV_ENGINE_MSVLD : context |= 0x00600000; break;
	default:
		return -EINVAL;
	}

	return nvkm_ramht_insert(chan->ramht, 0, handle, context);
}

static int
g84_fifo_chan_ctor_dma(struct nvkm_object *parent, struct nvkm_object *engine,
		       struct nvkm_oclass *oclass, void *data, u32 size,
		       struct nvkm_object **pobject)
{
	union {
		struct nv03_channel_dma_v0 v0;
	} *args = data;
	struct nvkm_bar *bar = nvkm_bar(parent);
	struct nv50_fifo_base *base = (void *)parent;
	struct nv50_fifo_chan *chan;
	int ret;

	nv_ioctl(parent, "create channel dma size %d\n", size);
	if (nvif_unpack(args->v0, 0, 0, false)) {
		nv_ioctl(parent, "create channel dma vers %d pushbuf %08x "
				 "offset %016llx\n", args->v0.version,
			 args->v0.pushbuf, args->v0.offset);
	} else
		return ret;

	ret = nvkm_fifo_channel_create(parent, engine, oclass, 0, 0xc00000,
				       0x2000, args->v0.pushbuf,
				       (1ULL << NVDEV_ENGINE_DMAOBJ) |
				       (1ULL << NVDEV_ENGINE_SW) |
				       (1ULL << NVDEV_ENGINE_GR) |
				       (1ULL << NVDEV_ENGINE_MPEG) |
				       (1ULL << NVDEV_ENGINE_ME) |
				       (1ULL << NVDEV_ENGINE_VP) |
				       (1ULL << NVDEV_ENGINE_CIPHER) |
				       (1ULL << NVDEV_ENGINE_SEC) |
				       (1ULL << NVDEV_ENGINE_BSP) |
				       (1ULL << NVDEV_ENGINE_MSVLD) |
				       (1ULL << NVDEV_ENGINE_MSPDEC) |
				       (1ULL << NVDEV_ENGINE_MSPPP) |
				       (1ULL << NVDEV_ENGINE_CE0) |
				       (1ULL << NVDEV_ENGINE_VIC), &chan);
	*pobject = nv_object(chan);
	if (ret)
		return ret;

	args->v0.chid = chan->base.chid;

	ret = nvkm_ramht_new(nv_object(chan), nv_object(chan), 0x8000, 16,
			     &chan->ramht);
	if (ret)
		return ret;

	nv_parent(chan)->context_attach = g84_fifo_context_attach;
	nv_parent(chan)->context_detach = g84_fifo_context_detach;
	nv_parent(chan)->object_attach = g84_fifo_object_attach;
	nv_parent(chan)->object_detach = nv50_fifo_object_detach;

	nv_wo32(base->ramfc, 0x08, lower_32_bits(args->v0.offset));
	nv_wo32(base->ramfc, 0x0c, upper_32_bits(args->v0.offset));
	nv_wo32(base->ramfc, 0x10, lower_32_bits(args->v0.offset));
	nv_wo32(base->ramfc, 0x14, upper_32_bits(args->v0.offset));
	nv_wo32(base->ramfc, 0x3c, 0x003f6078);
	nv_wo32(base->ramfc, 0x44, 0x01003fff);
	nv_wo32(base->ramfc, 0x48, chan->base.pushgpu->node->offset >> 4);
	nv_wo32(base->ramfc, 0x4c, 0xffffffff);
	nv_wo32(base->ramfc, 0x60, 0x7fffffff);
	nv_wo32(base->ramfc, 0x78, 0x00000000);
	nv_wo32(base->ramfc, 0x7c, 0x30000001);
	nv_wo32(base->ramfc, 0x80, ((chan->ramht->bits - 9) << 27) |
				   (4 << 24) /* SEARCH_FULL */ |
				   (chan->ramht->gpuobj.node->offset >> 4));
	nv_wo32(base->ramfc, 0x88, base->cache->addr >> 10);
	nv_wo32(base->ramfc, 0x98, nv_gpuobj(base)->addr >> 12);
	bar->flush(bar);
	return 0;
}

static int
g84_fifo_chan_ctor_ind(struct nvkm_object *parent, struct nvkm_object *engine,
		       struct nvkm_oclass *oclass, void *data, u32 size,
		       struct nvkm_object **pobject)
{
	union {
		struct nv50_channel_gpfifo_v0 v0;
	} *args = data;
	struct nvkm_bar *bar = nvkm_bar(parent);
	struct nv50_fifo_base *base = (void *)parent;
	struct nv50_fifo_chan *chan;
	u64 ioffset, ilength;
	int ret;

	nv_ioctl(parent, "create channel gpfifo size %d\n", size);
	if (nvif_unpack(args->v0, 0, 0, false)) {
		nv_ioctl(parent, "create channel gpfifo vers %d pushbuf %08x "
				 "ioffset %016llx ilength %08x\n",
			 args->v0.version, args->v0.pushbuf, args->v0.ioffset,
			 args->v0.ilength);
	} else
		return ret;

	ret = nvkm_fifo_channel_create(parent, engine, oclass, 0, 0xc00000,
				       0x2000, args->v0.pushbuf,
				       (1ULL << NVDEV_ENGINE_DMAOBJ) |
				       (1ULL << NVDEV_ENGINE_SW) |
				       (1ULL << NVDEV_ENGINE_GR) |
				       (1ULL << NVDEV_ENGINE_MPEG) |
				       (1ULL << NVDEV_ENGINE_ME) |
				       (1ULL << NVDEV_ENGINE_VP) |
				       (1ULL << NVDEV_ENGINE_CIPHER) |
				       (1ULL << NVDEV_ENGINE_SEC) |
				       (1ULL << NVDEV_ENGINE_BSP) |
				       (1ULL << NVDEV_ENGINE_MSVLD) |
				       (1ULL << NVDEV_ENGINE_MSPDEC) |
				       (1ULL << NVDEV_ENGINE_MSPPP) |
				       (1ULL << NVDEV_ENGINE_CE0) |
				       (1ULL << NVDEV_ENGINE_VIC), &chan);
	*pobject = nv_object(chan);
	if (ret)
		return ret;

	args->v0.chid = chan->base.chid;

	ret = nvkm_ramht_new(nv_object(chan), nv_object(chan), 0x8000, 16,
			     &chan->ramht);
	if (ret)
		return ret;

	nv_parent(chan)->context_attach = g84_fifo_context_attach;
	nv_parent(chan)->context_detach = g84_fifo_context_detach;
	nv_parent(chan)->object_attach = g84_fifo_object_attach;
	nv_parent(chan)->object_detach = nv50_fifo_object_detach;

	ioffset = args->v0.ioffset;
	ilength = order_base_2(args->v0.ilength / 8);

	nv_wo32(base->ramfc, 0x3c, 0x403f6078);
	nv_wo32(base->ramfc, 0x44, 0x01003fff);
	nv_wo32(base->ramfc, 0x48, chan->base.pushgpu->node->offset >> 4);
	nv_wo32(base->ramfc, 0x50, lower_32_bits(ioffset));
	nv_wo32(base->ramfc, 0x54, upper_32_bits(ioffset) | (ilength << 16));
	nv_wo32(base->ramfc, 0x60, 0x7fffffff);
	nv_wo32(base->ramfc, 0x78, 0x00000000);
	nv_wo32(base->ramfc, 0x7c, 0x30000001);
	nv_wo32(base->ramfc, 0x80, ((chan->ramht->bits - 9) << 27) |
				   (4 << 24) /* SEARCH_FULL */ |
				   (chan->ramht->gpuobj.node->offset >> 4));
	nv_wo32(base->ramfc, 0x88, base->cache->addr >> 10);
	nv_wo32(base->ramfc, 0x98, nv_gpuobj(base)->addr >> 12);
	bar->flush(bar);
	return 0;
}

static int
g84_fifo_chan_init(struct nvkm_object *object)
{
	struct nv50_fifo *fifo = (void *)object->engine;
	struct nv50_fifo_base *base = (void *)object->parent;
	struct nv50_fifo_chan *chan = (void *)object;
	struct nvkm_gpuobj *ramfc = base->ramfc;
	struct nvkm_device *device = fifo->base.engine.subdev.device;
	u32 chid = chan->base.chid;
	int ret;

	ret = nvkm_fifo_channel_init(&chan->base);
	if (ret)
		return ret;

	nvkm_wr32(device, 0x002600 + (chid * 4), 0x80000000 | ramfc->addr >> 8);
	nv50_fifo_playlist_update(fifo);
	return 0;
}

static struct nvkm_ofuncs
g84_fifo_ofuncs_dma = {
	.ctor = g84_fifo_chan_ctor_dma,
	.dtor = nv50_fifo_chan_dtor,
	.init = g84_fifo_chan_init,
	.fini = nv50_fifo_chan_fini,
	.map  = _nvkm_fifo_channel_map,
	.rd32 = _nvkm_fifo_channel_rd32,
	.wr32 = _nvkm_fifo_channel_wr32,
	.ntfy = _nvkm_fifo_channel_ntfy
};

static struct nvkm_ofuncs
g84_fifo_ofuncs_ind = {
	.ctor = g84_fifo_chan_ctor_ind,
	.dtor = nv50_fifo_chan_dtor,
	.init = g84_fifo_chan_init,
	.fini = nv50_fifo_chan_fini,
	.map  = _nvkm_fifo_channel_map,
	.rd32 = _nvkm_fifo_channel_rd32,
	.wr32 = _nvkm_fifo_channel_wr32,
	.ntfy = _nvkm_fifo_channel_ntfy
};

static struct nvkm_oclass
g84_fifo_sclass[] = {
	{ G82_CHANNEL_DMA, &g84_fifo_ofuncs_dma },
	{ G82_CHANNEL_GPFIFO, &g84_fifo_ofuncs_ind },
	{}
};

/*******************************************************************************
 * FIFO context - basically just the instmem reserved for the channel
 ******************************************************************************/

static int
g84_fifo_context_ctor(struct nvkm_object *parent, struct nvkm_object *engine,
		      struct nvkm_oclass *oclass, void *data, u32 size,
		      struct nvkm_object **pobject)
{
	struct nv50_fifo_base *base;
	int ret;

	ret = nvkm_fifo_context_create(parent, engine, oclass, NULL, 0x10000,
				       0x1000, NVOBJ_FLAG_HEAP, &base);
	*pobject = nv_object(base);
	if (ret)
		return ret;

	ret = nvkm_gpuobj_new(nv_object(base), nv_object(base), 0x0200, 0,
			      NVOBJ_FLAG_ZERO_ALLOC, &base->eng);
	if (ret)
		return ret;

	ret = nvkm_gpuobj_new(nv_object(base), nv_object(base), 0x4000, 0,
			      0, &base->pgd);
	if (ret)
		return ret;

	ret = nvkm_vm_ref(nvkm_client(parent)->vm, &base->vm, base->pgd);
	if (ret)
		return ret;

	ret = nvkm_gpuobj_new(nv_object(base), nv_object(base), 0x1000,
			      0x400, NVOBJ_FLAG_ZERO_ALLOC, &base->cache);
	if (ret)
		return ret;

	ret = nvkm_gpuobj_new(nv_object(base), nv_object(base), 0x0100,
			      0x100, NVOBJ_FLAG_ZERO_ALLOC, &base->ramfc);
	if (ret)
		return ret;

	return 0;
}

static struct nvkm_oclass
g84_fifo_cclass = {
	.handle = NV_ENGCTX(FIFO, 0x84),
	.ofuncs = &(struct nvkm_ofuncs) {
		.ctor = g84_fifo_context_ctor,
		.dtor = nv50_fifo_context_dtor,
		.init = _nvkm_fifo_context_init,
		.fini = _nvkm_fifo_context_fini,
		.rd32 = _nvkm_fifo_context_rd32,
		.wr32 = _nvkm_fifo_context_wr32,
	},
};

/*******************************************************************************
 * PFIFO engine
 ******************************************************************************/

static void
g84_fifo_uevent_init(struct nvkm_event *event, int type, int index)
{
	struct nvkm_fifo *fifo = container_of(event, typeof(*fifo), uevent);
	struct nvkm_device *device = fifo->engine.subdev.device;
	nvkm_mask(device, 0x002140, 0x40000000, 0x40000000);
}

static void
g84_fifo_uevent_fini(struct nvkm_event *event, int type, int index)
{
	struct nvkm_fifo *fifo = container_of(event, typeof(*fifo), uevent);
	struct nvkm_device *device = fifo->engine.subdev.device;
	nvkm_mask(device, 0x002140, 0x40000000, 0x00000000);
}

static const struct nvkm_event_func
g84_fifo_uevent_func = {
	.ctor = nvkm_fifo_uevent_ctor,
	.init = g84_fifo_uevent_init,
	.fini = g84_fifo_uevent_fini,
};

static int
g84_fifo_ctor(struct nvkm_object *parent, struct nvkm_object *engine,
	      struct nvkm_oclass *oclass, void *data, u32 size,
	      struct nvkm_object **pobject)
{
	struct nv50_fifo *fifo;
	int ret;

	ret = nvkm_fifo_create(parent, engine, oclass, 1, 127, &fifo);
	*pobject = nv_object(fifo);
	if (ret)
		return ret;

	ret = nvkm_gpuobj_new(nv_object(fifo), NULL, 128 * 4, 0x1000, 0,
			      &fifo->playlist[0]);
	if (ret)
		return ret;

	ret = nvkm_gpuobj_new(nv_object(fifo), NULL, 128 * 4, 0x1000, 0,
			      &fifo->playlist[1]);
	if (ret)
		return ret;

	ret = nvkm_event_init(&g84_fifo_uevent_func, 1, 1, &fifo->base.uevent);
	if (ret)
		return ret;

	nv_subdev(fifo)->unit = 0x00000100;
	nv_subdev(fifo)->intr = nv04_fifo_intr;
	nv_engine(fifo)->cclass = &g84_fifo_cclass;
	nv_engine(fifo)->sclass = g84_fifo_sclass;
	fifo->base.pause = nv04_fifo_pause;
	fifo->base.start = nv04_fifo_start;
	return 0;
}

struct nvkm_oclass *
g84_fifo_oclass = &(struct nvkm_oclass) {
	.handle = NV_ENGINE(FIFO, 0x84),
	.ofuncs = &(struct nvkm_ofuncs) {
		.ctor = g84_fifo_ctor,
		.dtor = nv50_fifo_dtor,
		.init = nv50_fifo_init,
		.fini = _nvkm_fifo_fini,
	},
};
