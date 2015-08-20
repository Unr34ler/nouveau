/*
 * Copyright 2014 Red Hat Inc.
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
 * Authors: Ben Skeggs <bskeggs@redhat.com>
 */
#include <core/ioctl.h>
#include <core/client.h>
#include <core/engine.h>
#include <core/handle.h>
#include <core/namedb.h>

#include <nvif/unpack.h>
#include <nvif/ioctl.h>

static int
nvkm_ioctl_nop(struct nvkm_handle *handle, void *data, u32 size)
{
	struct nvkm_object *object = handle->object;
	union {
		struct nvif_ioctl_nop_v0 v0;
	} *args = data;
	int ret;

	nvif_ioctl(object, "nop size %d\n", size);
	if (nvif_unpack(args->v0, 0, 0, false)) {
		nvif_ioctl(object, "nop vers %lld\n", args->v0.version);
		args->v0.version = NVIF_VERSION_LATEST;
	}

	return ret;
}

static int
nvkm_ioctl_sclass(struct nvkm_handle *handle, void *data, u32 size)
{
	struct nvkm_object *object = handle->object;
	union {
		struct nvif_ioctl_sclass_v0 v0;
	} *args = data;
	struct nvkm_oclass oclass;
	int ret, i = 0;

	nvif_ioctl(object, "sclass size %d\n", size);
	if (nvif_unpack(args->v0, 0, 0, true)) {
		nvif_ioctl(object, "sclass vers %d count %d\n",
			   args->v0.version, args->v0.count);
		if (size != args->v0.count * sizeof(args->v0.oclass[0]))
			return -EINVAL;

		if (object->oclass) {
			if (nv_iclass(object, NV_PARENT_CLASS)) {
				ret = nvkm_parent_lclass(object,
							 args->v0.oclass,
							 args->v0.count);
			}

			args->v0.count = ret;
			return 0;
		}

		while (object->func->sclass &&
		       object->func->sclass(object, i, &oclass) >= 0) {
			if (i < args->v0.count) {
				args->v0.oclass[i].oclass = oclass.base.oclass;
				args->v0.oclass[i].minver = oclass.base.minver;
				args->v0.oclass[i].maxver = oclass.base.maxver;
			}
			i++;
		}

		args->v0.count = i;
	}

	return ret;
}

static int
nvkm_ioctl_new_old(struct nvkm_handle *handle, void *data, u32 size)
{
	union {
		struct nvif_ioctl_new_v0 v0;
	} *args = data;
	struct nvkm_client *client = nvkm_client(handle->object);
	struct nvkm_object *engctx = NULL;
	struct nvkm_object *object = NULL;
	struct nvkm_parent *parent;
	struct nvkm_engine *engine;
	struct nvkm_oclass *oclass;
	u32 _handle, _oclass;
	int ret;

	nvif_ioctl(handle->object, "new size %d\n", size);
	if (nvif_unpack(args->v0, 0, 0, true)) {
		_handle = args->v0.handle;
		_oclass = args->v0.oclass;
	} else
		return ret;

	nvif_ioctl(handle->object, "new vers %d handle %08x class %08x "
				   "route %02x token %llx object %016llx\n",
		   args->v0.version, _handle, _oclass,
		   args->v0.route, args->v0.token, args->v0.object);

	if (!nv_iclass(handle->object, NV_PARENT_CLASS)) {
		nvif_debug(handle->object, "cannot have children (ctor)\n");
		ret = -ENODEV;
		goto fail_class;
	}

	parent = nv_parent(handle->object);

	/* check that parent supports the requested subclass */
	ret = nvkm_parent_sclass(&parent->object, _oclass,
				 (struct nvkm_object **)&engine, &oclass);
	if (ret) {
		nvif_debug(&parent->object, "illegal class 0x%04x\n", _oclass);
		goto fail_class;
	}

	/* make sure engine init has been completed *before* any objects
	 * it controls are created - the constructors may depend on
	 * state calculated at init (ie. default context construction)
	 */
	if (engine) {
		engine = nvkm_engine_ref(engine);
		if (IS_ERR(engine)) {
			ret = PTR_ERR(engine);
			engine = NULL;
			goto fail_class;
		}
	}

	/* if engine requires it, create a context object to insert
	 * between the parent and its children (eg. PGRAPH context)
	 */
	if (engine && engine->cclass) {
		ret = nvkm_object_old(&parent->object, &engine->subdev.object,
				      engine->cclass, data, size, &engctx);
		if (ret)
			goto fail_engctx;
	} else {
		nvkm_object_ref(&parent->object, &engctx);
	}

	/* finally, create new object and bind it to its handle */
	ret = nvkm_object_old(engctx, &engine->subdev.object, oclass,
			      data, size, &object);
	if (ret)
		goto fail_ctor;

	object->handle = _handle;

	ret = nvkm_object_inc(object);
	if (ret)
		goto fail_init;

	ret = nvkm_handle_create(handle, _handle, object, &handle);
	if (ret)
		goto fail_handle;

	ret = nvkm_handle_init(handle);
	handle->route = args->v0.route;
	handle->token = args->v0.token;
	if (ret)
		nvkm_handle_destroy(handle);

	handle->handle = args->v0.object;
	nvkm_client_insert(client, handle);
	client->data = object;
fail_handle:
	nvkm_object_dec(object, false);
fail_init:
	nvkm_object_ref(NULL, &object);
fail_ctor:
	nvkm_object_ref(NULL, &engctx);
fail_engctx:
	nvkm_engine_unref(&engine);
fail_class:
	return ret;
}

static int
nvkm_ioctl_new(struct nvkm_handle *handle, void *data, u32 size)
{
	union {
		struct nvif_ioctl_new_v0 v0;
	} *args = data;
	struct nvkm_client *client = handle->object->client;
	struct nvkm_object *parent = handle->object;
	struct nvkm_object *object = NULL;
	struct nvkm_oclass oclass;
	int ret, i = 0;

	if (parent->oclass)
		return nvkm_ioctl_new_old(handle, data, size);

	nvif_ioctl(parent, "new size %d\n", size);
	if (nvif_unpack(args->v0, 0, 0, true)) {
		nvif_ioctl(parent, "new vers %d handle %08x class %08x "
				   "route %02x token %llx object %016llx\n",
			   args->v0.version, args->v0.handle, args->v0.oclass,
			   args->v0.route, args->v0.token, args->v0.object);
	} else
		return ret;

	if (!parent->func->sclass) {
		nvif_ioctl(parent, "cannot have children\n");
		return -EINVAL;
	}

	do {
		memset(&oclass, 0x00, sizeof(oclass));
		oclass.client = client;
		oclass.handle = args->v0.handle;
		oclass.object = args->v0.object;
		oclass.parent = parent;
		ret = parent->func->sclass(parent, i++, &oclass);
		if (ret)
			return ret;
	} while (oclass.base.oclass != args->v0.oclass);

	if (oclass.engine) {
		oclass.engine = nvkm_engine_ref(oclass.engine);
		if (IS_ERR(oclass.engine))
			return PTR_ERR(oclass.engine);
	}

	ret = oclass.ctor(&oclass, data, size, &object);
	if (ret)
		goto fail_object;

	ret = nvkm_object_inc(object);
	if (ret)
		goto fail_object;

	ret = nvkm_handle_create(handle, args->v0.handle, object, &handle);
	if (ret)
		goto fail_handle;

	ret = nvkm_handle_init(handle);
	handle->route = args->v0.route;
	handle->token = args->v0.token;
	if (ret)
		nvkm_handle_destroy(handle);

	handle->handle = args->v0.object;
	nvkm_client_insert(client, handle);
	client->data = object;
fail_handle:
	nvkm_object_dec(object, false);
fail_object:
	nvkm_object_ref(NULL, &object);
	nvkm_engine_unref(&oclass.engine);
	return ret;
}

static int
nvkm_ioctl_del(struct nvkm_handle *handle, void *data, u32 size)
{
	struct nvkm_object *object = handle->object;
	union {
		struct nvif_ioctl_del none;
	} *args = data;
	int ret;

	nvif_ioctl(object, "delete size %d\n", size);
	if (nvif_unvers(args->none)) {
		nvif_ioctl(object, "delete\n");
		nvkm_handle_fini(handle, false);
		nvkm_handle_destroy(handle);
	}

	return ret;
}

static int
nvkm_ioctl_mthd(struct nvkm_handle *handle, void *data, u32 size)
{
	struct nvkm_object *object = handle->object;
	union {
		struct nvif_ioctl_mthd_v0 v0;
	} *args = data;
	int ret;

	nvif_ioctl(object, "mthd size %d\n", size);
	if (nvif_unpack(args->v0, 0, 0, true)) {
		nvif_ioctl(object, "mthd vers %d mthd %02x\n",
			   args->v0.version, args->v0.method);
		ret = nvkm_object_mthd(object, args->v0.method, data, size);
	}

	return ret;
}


static int
nvkm_ioctl_rd(struct nvkm_handle *handle, void *data, u32 size)
{
	struct nvkm_object *object = handle->object;
	union {
		struct nvif_ioctl_rd_v0 v0;
	} *args = data;
	union {
		u8  b08;
		u16 b16;
		u32 b32;
	} v;
	int ret;

	nvif_ioctl(object, "rd size %d\n", size);
	if (nvif_unpack(args->v0, 0, 0, false)) {
		nvif_ioctl(object, "rd vers %d size %d addr %016llx\n",
			   args->v0.version, args->v0.size, args->v0.addr);
		switch (args->v0.size) {
		case 1:
			ret = nvkm_object_rd08(object, args->v0.addr, &v.b08);
			args->v0.data = v.b08;
			break;
		case 2:
			ret = nvkm_object_rd16(object, args->v0.addr, &v.b16);
			args->v0.data = v.b16;
			break;
		case 4:
			ret = nvkm_object_rd32(object, args->v0.addr, &v.b32);
			args->v0.data = v.b32;
			break;
		default:
			ret = -EINVAL;
			break;
		}
	}

	return ret;
}

static int
nvkm_ioctl_wr(struct nvkm_handle *handle, void *data, u32 size)
{
	struct nvkm_object *object = handle->object;
	union {
		struct nvif_ioctl_wr_v0 v0;
	} *args = data;
	int ret;

	nvif_ioctl(object, "wr size %d\n", size);
	if (nvif_unpack(args->v0, 0, 0, false)) {
		nvif_ioctl(object,
			   "wr vers %d size %d addr %016llx data %08x\n",
			   args->v0.version, args->v0.size, args->v0.addr,
			   args->v0.data);
	} else
		return ret;

	switch (args->v0.size) {
	case 1: return nvkm_object_wr08(object, args->v0.addr, args->v0.data);
	case 2: return nvkm_object_wr16(object, args->v0.addr, args->v0.data);
	case 4: return nvkm_object_wr32(object, args->v0.addr, args->v0.data);
	default:
		break;
	}

	return -EINVAL;
}

static int
nvkm_ioctl_map(struct nvkm_handle *handle, void *data, u32 size)
{
	struct nvkm_object *object = handle->object;
	union {
		struct nvif_ioctl_map_v0 v0;
	} *args = data;
	int ret;

	nvif_ioctl(object, "map size %d\n", size);
	if (nvif_unpack(args->v0, 0, 0, false)) {
		nvif_ioctl(object, "map vers %d\n", args->v0.version);
		ret = nvkm_object_map(object, &args->v0.handle,
					      &args->v0.length);
	}

	return ret;
}

static int
nvkm_ioctl_unmap(struct nvkm_handle *handle, void *data, u32 size)
{
	struct nvkm_object *object = handle->object;
	union {
		struct nvif_ioctl_unmap none;
	} *args = data;
	int ret;

	nvif_ioctl(object, "unmap size %d\n", size);
	if (nvif_unvers(args->none)) {
		nvif_ioctl(object, "unmap\n");
	}

	return ret;
}

static int
nvkm_ioctl_ntfy_new(struct nvkm_handle *handle, void *data, u32 size)
{
	struct nvkm_object *object = handle->object;
	union {
		struct nvif_ioctl_ntfy_new_v0 v0;
	} *args = data;
	struct nvkm_event *event;
	int ret;

	nvif_ioctl(object, "ntfy new size %d\n", size);
	if (nvif_unpack(args->v0, 0, 0, true)) {
		nvif_ioctl(object, "ntfy new vers %d event %02x\n",
			   args->v0.version, args->v0.event);
		ret = nvkm_object_ntfy(object, args->v0.event, &event);
		if (ret == 0) {
			ret = nvkm_client_notify_new(object, event, data, size);
			if (ret >= 0) {
				args->v0.index = ret;
				ret = 0;
			}
		}
	}

	return ret;
}

static int
nvkm_ioctl_ntfy_del(struct nvkm_handle *handle, void *data, u32 size)
{
	struct nvkm_client *client = nvkm_client(handle->object);
	struct nvkm_object *object = handle->object;
	union {
		struct nvif_ioctl_ntfy_del_v0 v0;
	} *args = data;
	int ret;

	nvif_ioctl(object, "ntfy del size %d\n", size);
	if (nvif_unpack(args->v0, 0, 0, false)) {
		nvif_ioctl(object, "ntfy del vers %d index %d\n",
			   args->v0.version, args->v0.index);
		ret = nvkm_client_notify_del(client, args->v0.index);
	}

	return ret;
}

static int
nvkm_ioctl_ntfy_get(struct nvkm_handle *handle, void *data, u32 size)
{
	struct nvkm_client *client = nvkm_client(handle->object);
	struct nvkm_object *object = handle->object;
	union {
		struct nvif_ioctl_ntfy_get_v0 v0;
	} *args = data;
	int ret;

	nvif_ioctl(object, "ntfy get size %d\n", size);
	if (nvif_unpack(args->v0, 0, 0, false)) {
		nvif_ioctl(object, "ntfy get vers %d index %d\n",
			   args->v0.version, args->v0.index);
		ret = nvkm_client_notify_get(client, args->v0.index);
	}

	return ret;
}

static int
nvkm_ioctl_ntfy_put(struct nvkm_handle *handle, void *data, u32 size)
{
	struct nvkm_client *client = nvkm_client(handle->object);
	struct nvkm_object *object = handle->object;
	union {
		struct nvif_ioctl_ntfy_put_v0 v0;
	} *args = data;
	int ret;

	nvif_ioctl(object, "ntfy put size %d\n", size);
	if (nvif_unpack(args->v0, 0, 0, false)) {
		nvif_ioctl(object, "ntfy put vers %d index %d\n",
			   args->v0.version, args->v0.index);
		ret = nvkm_client_notify_put(client, args->v0.index);
	}

	return ret;
}

static struct {
	int version;
	int (*func)(struct nvkm_handle *, void *, u32);
}
nvkm_ioctl_v0[] = {
	{ 0x00, nvkm_ioctl_nop },
	{ 0x00, nvkm_ioctl_sclass },
	{ 0x00, nvkm_ioctl_new },
	{ 0x00, nvkm_ioctl_del },
	{ 0x00, nvkm_ioctl_mthd },
	{ 0x00, nvkm_ioctl_rd },
	{ 0x00, nvkm_ioctl_wr },
	{ 0x00, nvkm_ioctl_map },
	{ 0x00, nvkm_ioctl_unmap },
	{ 0x00, nvkm_ioctl_ntfy_new },
	{ 0x00, nvkm_ioctl_ntfy_del },
	{ 0x00, nvkm_ioctl_ntfy_get },
	{ 0x00, nvkm_ioctl_ntfy_put },
};

static int
nvkm_ioctl_path(struct nvkm_client *client, u64 handle, u32 type,
		void *data, u32 size, u8 owner, u8 *route, u64 *token)
{
	struct nvkm_handle *object;
	int ret;

	if (handle)
		object = nvkm_client_search(client, handle);
	else
		object = client->root;
	if (unlikely(!object)) {
		nvif_ioctl(&client->namedb.parent.object, "object not found\n");
		return -ENOENT;
	}

	if (owner != NVIF_IOCTL_V0_OWNER_ANY && owner != object->route) {
		nvif_ioctl(&client->namedb.parent.object, "route != owner\n");
		return -EACCES;
	}
	*route = object->route;
	*token = object->token;

	if (ret = -EINVAL, type < ARRAY_SIZE(nvkm_ioctl_v0)) {
		if (nvkm_ioctl_v0[type].version == 0)
			ret = nvkm_ioctl_v0[type].func(object, data, size);
	}

	return ret;
}

int
nvkm_ioctl(struct nvkm_client *client, bool supervisor,
	   void *data, u32 size, void **hack)
{
	struct nvkm_object *object = &client->namedb.parent.object;
	union {
		struct nvif_ioctl_v0 v0;
	} *args = data;
	int ret;

	client->super = supervisor;
	nvif_ioctl(object, "size %d\n", size);

	if (nvif_unpack(args->v0, 0, 0, true)) {
		nvif_ioctl(object,
			   "vers %d type %02x object %016llx owner %02x\n",
			   args->v0.version, args->v0.type, args->v0.object,
			   args->v0.owner);
		ret = nvkm_ioctl_path(client, args->v0.object, args->v0.type,
				      data, size, args->v0.owner,
				      &args->v0.route, &args->v0.token);
	}

	nvif_ioctl(object, "return %d\n", ret);
	if (hack) {
		*hack = client->data;
		client->data = NULL;
	}

	client->super = false;
	return ret;
}
