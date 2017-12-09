/**
 * avbox - Toolkit for Embedded Multimedia Applications
 * Copyright (C) 2016-2017 Fernando Rodriguez
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 3 as 
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#ifdef HAVE_CONFIG_H
#	include "../../config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define LOG_MODULE "video-drm"

#include "../log.h"
#include "../debug.h"
#include "../linkedlist.h"
#include "video-drv.h"

#ifdef ENABLE_OPENGL
#	include <EGL/egl.h>
#	include <EGL/eglext.h>
#	include <gbm.h>
#	include "video-opengl.h"
#endif


struct mbv_drm_dev;


struct mbv_buf
{
	int pitch;
	uint32_t sz;
	uint32_t hnd;
	uint32_t fb;
	uint8_t *map;
};


struct mbv_surface
{
	pthread_mutex_t lock;

	uint32_t w;
	uint32_t h;
	uint32_t x;
	uint32_t y;
	uint32_t realx;
	uint32_t realy;
	uint32_t n_buffers;
	uint32_t active_buffer;
	uint32_t mapped_buffer;

	struct mbv_surface *real;
	struct mbv_surface *parent;
	struct mbv_drm_dev *dev;
	struct mbv_buf buffers[2];
};


LISTABLE_STRUCT(mbv_drm_dev,
	int fd;
	uint32_t conn;
	uint32_t crtc;
	drmModeModeInfo mode;
	drmModeCrtc *saved_crtc;
	struct mbv_surface root;
);


LIST_DECLARE_STATIC(devices);
static struct mbv_drm_dev *default_dev = NULL;

#ifdef ENABLE_OPENGL
static struct gbm_device *gbm_dev;
static struct gbm_surface *gbm_surface;
static EGLDisplay egl_display;
static EGLContext egl_ctx;
static uint32_t egl_framebuffer = 0;
static int egl_enabled = 0;
#endif


static struct mbv_surface *
surface_new(struct mbv_surface *parent,
	const int x, const int y, const int w, const int h)
{
	struct mbv_surface *inst;

	/* DEBUG_PRINT("video-drm", "Entering surface_new()"); */

	/* allocate memory for the surface object */
	if ((inst = malloc(sizeof(struct mbv_surface))) == NULL) {
		LOG_VPRINT_ERROR("Could not create surface: %s",
			strerror(errno));
		return NULL;
	}

	inst->w = w;
	inst->h = h;
	inst->x = x;
	inst->y = y;

	inst->n_buffers = 1;
	inst->active_buffer = 0;
	inst->buffers[0].hnd = 0;	/* buffer is in system memory */
	inst->buffers[0].pitch = w * 4;
	inst->buffers[0].sz = inst->buffers[0].pitch * h;
	inst->buffers[0].fb = 0;

	if (parent == NULL) {
		inst->parent = NULL;
		inst->real = inst;
		inst->realx = inst->x;
		inst->realy = inst->y;
		inst->dev = default_dev;

		/* allocate a buffer for surface in system memory */
		if ((inst->buffers[0].map = malloc(inst->buffers[0].sz)) == NULL) {
			LOG_VPRINT_ERROR("Could not allocate surface buffer: %s",
				strerror(errno));
			free(inst);
			return NULL;
		}
	} else {
		inst->parent = parent;
		inst->dev = parent->dev;
		inst->real = parent->real;
		if (parent->real == parent) {
			inst->realx = inst->x + (parent->realx - parent->x);
			inst->realy = inst->y + (parent->realy - parent->y);
		} else {
			inst->realx = inst->x + parent->realx;
			inst->realy = inst->y + parent->realy;
		}
		inst->buffers[0].map = NULL;
	}

	if (pthread_mutex_init(&inst->lock, NULL) != 0) {
		LOG_PRINT_ERROR("Could not initialize surface mutex!");
		if (inst->buffers[0].map != NULL) {
			free(inst->buffers[0].map);
		}
		free(inst);
		return NULL;
	}

	/*
	DEBUG_VPRINT("video-drm", "surface = %p", inst);
	DEBUG_VPRINT("video-drm", "surface.parent = %p", inst->parent);
	DEBUG_VPRINT("video-drm", "surface.real = %p", inst->real);
	DEBUG_VPRINT("video-drm", "surface.x = %i", inst->x);
	DEBUG_VPRINT("video-drm", "surface.y = %i", inst->y);
	DEBUG_VPRINT("video-drm", "surface.realx = %i", inst->realx);
	DEBUG_VPRINT("video-drm", "surface.realy = %i", inst->realy);
	DEBUG_VPRINT("video-drm", "surface.buffers[0].map = %p", inst->buffers[0].map);
	*/

	return inst;
}


static void *
surface_lock(struct mbv_surface * const inst,
	unsigned int flags, int *pitch)
{
	struct mbv_surface * const realinst = inst->real;

	/* DEBUG_VPRINT("video-drm", "Entering surface_lock(front=%i)",
		(flags & MBV_LOCKFLAGS_FRONT) != 0); */

	assert(inst != NULL);
	assert(inst->real != NULL);

	pthread_mutex_lock(&inst->lock);

	const int buffer = (flags & MBV_LOCKFLAGS_FRONT) ?
		(realinst->active_buffer + 1) % realinst->n_buffers :
		realinst->active_buffer;

	realinst->mapped_buffer = buffer;
	*pitch = realinst->buffers[buffer].pitch;
	assert(realinst->buffers[buffer].map != NULL);

	if (realinst->buffers[buffer].hnd != 0) {
		madvise(realinst->buffers[buffer].map,
			realinst->buffers[buffer].sz,
			MADV_WILLNEED);
	}

	/* DEBUG_VPRINT("video-drm", "map=%p,x=%i,y=%i,realx=%i,realy=%i,w=%i,h=%i,p=%i)",
		inst->map, inst->x, inst->y, inst->realx, inst->realy, inst->w, inst->h, *pitch); */

	if (inst == realinst) {
		return inst->buffers[buffer].map;
	}

	uint8_t *map  = realinst->buffers[buffer].map +
		(inst->realy * (*pitch)) + inst->realx * 4;
	/* DEBUG_VPRINT("video-drm", "surface_lock() returning %p", map); */
	return map;
}


static void
surface_unlock(struct mbv_surface * const inst)
{
	/* DEBUG_PRINT("video-drm", "Entering surface_unlock()"); */
	assert(inst != NULL);
	if (inst->buffers[0].hnd != 0) {
		madvise(inst->buffers[inst->mapped_buffer].map,
			inst->buffers[inst->mapped_buffer].sz,
			MADV_DONTNEED);
	}
	pthread_mutex_unlock(&inst->lock);
}


static int
surface_blitbuf(struct mbv_surface * const surface,
	void *buf, int pitch, unsigned int flags, int w, int h, int x, int y)
{
	/* DEBUG_VPRINT("video-drm", "Entering surface_blitbuf(front=%i)",
		(flags & MBV_BLITFLAGS_FRONT) != 0);
	DEBUG_VPRINT("video-drm", "w=%i h=%i x=%i, y=%i",
		w, h, x, y); */

	int stride, dst_pitch;
	const int stride_sz = w * 4;
	const int stride_offset = x * 4;
	uint8_t *dst;
	unsigned int lockflags = MBV_LOCKFLAGS_WRITE;

	if (flags & MBV_BLITFLAGS_FRONT) {
		lockflags |= MBV_LOCKFLAGS_FRONT;
	}

	dst = surface_lock(surface, lockflags, &dst_pitch);
	if (dst == NULL) {
		LOG_PRINT_ERROR("Could not lock surface");
		surface_unlock(surface);
		return -1;
	}

	dst += y * dst_pitch;

	for (stride = 0; stride < h; stride++) {
		memcpy(dst + (stride * dst_pitch) + stride_offset,
			((uint8_t*)buf) + (stride * pitch), stride_sz);	
	}

	surface_unlock(surface);
	return 0;
}


static int
surface_blit(struct mbv_surface * const dst,
	struct mbv_surface * const src,
	unsigned int flags, int x, int y)
{
	void *buf;
	int pitch, ret;

	/* DEBUG_PRINT("video-drm", "Entering surface_blit()"); */

	/* lock the src surface to get access to it's
	 * buffer */
	if ((buf = surface_lock(src, MBV_LOCKFLAGS_READ, &pitch)) == NULL) {
		LOG_PRINT_ERROR("Could not lock surface!");
		return -1;
	}

	/* blit the buffer */
	ret = surface_blitbuf(dst, buf, pitch, flags,
		src->w, src->h, x, y);
	surface_unlock(src);
	return ret;
}


static void
surface_update(struct mbv_surface * const surface,
	int blitflags, const int update)
{
	/* DEBUG_VPRINT("video-drm", "Entering surface_update(update=%i)",
		update); */

	assert(surface != NULL);
	(void) blitflags;

	if (surface->real != surface) {
		/* DEBUG_PRINT("video-drm", "Not update necessary for subsurfaces!"); */
		return;
	}

	if (surface == &surface->dev->root) {
		/* DEBUG_VPRINT("video-drm", "Flipping root surface fb=%u",
			surface->buffers[surface->active_buffer].fb); */

		pthread_mutex_lock(&surface->dev->root.lock);

		/* flip buffers */
		if (drmModeSetCrtc(surface->dev->fd,
			surface->dev->crtc,
			surface->buffers[surface->active_buffer].fb, 0, 0,
			&surface->dev->conn, 1,
			&surface->dev->mode)) {
			LOG_VPRINT_ERROR("Cannot flip CRTC for connector %u (%d): %s",
				surface->dev->conn, errno, strerror(errno));
		}

		/* track the active buffer */
		surface->active_buffer = (surface->active_buffer + 1) %
			surface->n_buffers;

		pthread_mutex_unlock(&surface->dev->root.lock);
	} else {
		const unsigned int blitflags =
			(update) ? MBV_BLITFLAGS_FRONT : MBV_BLITFLAGS_NONE;
		/* DEBUG_VPRINT("video-drm", "Updating surface (update=%i)",
			update); */
		surface_blit(&surface->dev->root, surface, blitflags, surface->realx, surface->realy);
	}
}


static void
surface_destroy(struct mbv_surface *inst)
{
	assert(inst != NULL);
	if (inst->buffers[inst->active_buffer].hnd != 0) {
		/* free dumb buffer */
		abort();
	} else {
		if (inst->buffers[0].map != NULL) {
			assert(inst->real == inst);
			free(inst->buffers[0].map);
		}
	}
	free(inst);
}


static int
mbv_drm_mkfb(struct mbv_drm_dev * const dev,
	const int w, const int h)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_map_dumb mreq = { 0 };
	struct drm_mode_destroy_dumb dreq;
	int pitch;
	uint8_t *buf;
	int ret;

	DEBUG_PRINT("video-drm", "Creating framebuffers");

	assert(dev != NULL);

	struct mbv_surface * const surface = &dev->root;

	surface->real = surface;
	surface->x = surface->realx = 0;
	surface->y = surface->realy = 0;
	surface->w = w;
	surface->h = h;
	surface->parent = NULL;
	surface->n_buffers = 2;
	surface->active_buffer = 0;
	surface->buffers[0].hnd = 0;
	surface->buffers[1].hnd = 0;
	surface->buffers[0].fb = 0;
	surface->buffers[1].fb = 0;
	surface->dev = dev;

	if (pthread_mutex_init(&surface->lock, NULL) > 0) {
		LOG_PRINT_ERROR("Could not initialize root surface mutex!");
		return -1;
	} else {
	}

	/* create dumb back buffer */
	DEBUG_PRINT("video-drm", "Creating dumb buffers");
	memset(&creq, 0, sizeof(struct drm_mode_create_dumb));
	creq.width = w;
	creq.height = h;
	creq.bpp = 32;
	ret = drmIoctl(dev->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
	if (ret < 0) {
		LOG_VPRINT_ERROR("Cannot create dumb buffer (%d) %s",
			errno, strerror(errno));
		return -errno;
	}

	assert(creq.pitch <= INT_MAX);
	surface->buffers[0].hnd = creq.handle;
	surface->buffers[0].pitch = creq.pitch;
	surface->buffers[0].sz = creq.size;

	DEBUG_VPRINT("video-drm", "buffer[0].hnd = 0x%x", surface->buffers[0].hnd);
	DEBUG_VPRINT("video-drm", "buffer[0].pitch = %i", surface->buffers[0].pitch);
	DEBUG_VPRINT("video-drm", "buffer[0].sz = %u", surface->buffers[0].sz);
	
	memset(&creq, 0, sizeof(struct drm_mode_create_dumb));
	creq.width = w;
	creq.height = h;
	creq.bpp = 32;
	ret = drmIoctl(dev->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
	if (ret < 0) {
		LOG_VPRINT_ERROR("Cannot create dumb buffer (%d) %s",
			errno, strerror(errno));
		return -errno;
	}

	assert(creq.pitch <= INT_MAX);
	surface->buffers[1].hnd = creq.handle;
	surface->buffers[1].pitch = creq.pitch;
	surface->buffers[1].sz = creq.size;

	DEBUG_VPRINT("video-drm", "buffer[1].hnd = 0x%x", surface->buffers[1].hnd);
	DEBUG_VPRINT("video-drm", "buffer[1].pitch = %i", surface->buffers[1].pitch);
	DEBUG_VPRINT("video-drm", "buffer[1].sz = %u", surface->buffers[1].sz);

	/* create framebuffer object for the dumb-buffers */
	DEBUG_PRINT("video-drm", "Creating framebuffer objects");
	ret = drmModeAddFB(dev->fd, w, h, 24, 32, surface->buffers[0].pitch,
		surface->buffers[0].hnd, &surface->buffers[0].fb);
	if (ret) {
		LOG_VPRINT_ERROR("Cannot create framebuffer (%d) %s",
			errno, strerror(errno));
		ret = -errno;
		goto err_destroy;
	}

	ret = drmModeAddFB(dev->fd, w, h, 24, 32, surface->buffers[1].pitch,
		surface->buffers[1].hnd, &surface->buffers[1].fb);
	if (ret) {
		LOG_VPRINT_ERROR("Cannot create framebuffer (%d) %s",
			errno, strerror(errno));
		ret = -errno;
		goto err_destroy;
	}

	DEBUG_VPRINT("video-drm", "buffer[0].fb = 0x%x", surface->buffers[0].fb);
	DEBUG_VPRINT("video-drm", "buffer[1].fb = 0x%x", surface->buffers[1].fb);

	/* prepare buffer for memory mapping */
	/* DEBUG_PRINT("video-drm", "Preparing to map surface"); */
	mreq.handle = surface->buffers[0].hnd;
	assert(mreq.offset == 0);
	if (drmIoctl(dev->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq)) {
		LOG_VPRINT_ERROR("Cannot map dumb buffer (%d) %s",
			errno, strerror(errno));
		goto err_destroy;
	}

	/* perform actual memory mapping */
	/* DEBUG_VPRINT("video-drm", "Mapping surface (offset=%u)",
		mreq.offset); */
	surface->buffers[0].map = mmap(0, surface->buffers[0].sz,
		PROT_READ | PROT_WRITE, MAP_SHARED, dev->fd, mreq.offset);
	if (surface->buffers[0].map == MAP_FAILED) {
		LOG_VPRINT_ERROR("Cannot mmap dumb buffer (%d) %s",
			errno, strerror(errno));
		goto err_destroy;
	}

	/* prepare buffer for memory mapping */
	/* DEBUG_PRINT("video-drm", "Preparing to map surface"); */
	mreq.handle = surface->buffers[1].hnd;
	mreq.offset = 0;
	if (drmIoctl(dev->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq)) {
		LOG_VPRINT_ERROR("Cannot map dumb buffer (%d) %s",
			errno, strerror(errno));
		goto err_destroy;
	}

	/* perform actual memory mapping */
	/* DEBUG_VPRINT("video-drm", "Mapping surface (offset=%u)",
		mreq.offset); */
	surface->buffers[1].map = mmap(0, surface->buffers[1].sz,
		PROT_READ | PROT_WRITE, MAP_SHARED, dev->fd, mreq.offset);
	if (surface->buffers[1].map == MAP_FAILED) {
		LOG_VPRINT_ERROR("Cannot mmap dumb buffer (%d) %s",
			errno, strerror(errno));
		goto err_destroy;
	}

	DEBUG_VPRINT("video-drm", "buffer[0].map = %p", surface->buffers[0].map);
	DEBUG_VPRINT("video-drm", "buffer[1].map = %p", surface->buffers[1].map);

	/* clear the framebuffer to 0 */
	DEBUG_PRINT("video-drm", "Clearing framebuffers");
	if ((buf = surface_lock(surface, MBV_LOCKFLAGS_WRITE, &pitch)) == NULL) {
		LOG_PRINT_ERROR("Could not lock surface!");
		goto err_fb;
	}
	memset(buf, 0, surface->buffers[0].sz);
	surface_unlock(surface);

	DEBUG_PRINT("video-drm", "Flipping framebuffer");
	surface_update(surface, MBV_BLITFLAGS_NONE, 1);

	DEBUG_PRINT("video-drm", "Framebuffers created");
	return 0;

err_fb:
	drmModeRmFB(dev->fd, surface->buffers[0].fb);
	if (surface->buffers[1].fb != 0) {
		drmModeRmFB(dev->fd, surface->buffers[0].fb);
	}
err_destroy:
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = surface->buffers[0].hnd;
	drmIoctl(dev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	if (surface->buffers[1].hnd != 0) {
		memset(&dreq, 0, sizeof(dreq));
		dreq.handle = surface->buffers[1].hnd;
		drmIoctl(dev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	}
	return ret;
}


static int
mbv_drm_findcrtc(struct mbv_drm_dev *dev,
	drmModeRes *res, drmModeConnector *conn)
{
	drmModeEncoder *enc;
	unsigned int i, j;
	int32_t crtc;
	struct mbv_drm_dev *iter;

	/* first try the currently conected encoder+crtc */
	if (conn->encoder_id) {
		enc = drmModeGetEncoder(dev->fd, conn->encoder_id);
	} else {
		enc = NULL;
	}

	if (enc) {
		if (enc->crtc_id) {
			crtc = enc->crtc_id;
			LIST_FOREACH(struct mbv_drm_dev*, iter, &devices) {
				if (iter->crtc == crtc) {
					crtc = -1;
					break;
				}
			}

			if (crtc >= 0) {
				drmModeFreeEncoder(enc);
				dev->crtc = crtc;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	/* If the connector is not currently bound to an encoder or if the
	 * encoder+crtc is already used by another connector (actually unlikely
	 * but lets be safe), iterate all other available encoders to find a
	 * matching CRTC. */
	for (i = 0; i < conn->count_encoders; ++i) {
		enc = drmModeGetEncoder(dev->fd, conn->encoders[i]);
		if (!enc) {
			LOG_VPRINT_ERROR("Cannot retrieve encoder %u:%u (%d): %s",
				i, conn->encoders[i], errno, strerror(errno));
			continue;
		}

		/* iterate all global CRTCs */
		for (j = 0; j < res->count_crtcs; ++j) {
			/* check whether this CRTC works with the encoder */
			if (!(enc->possible_crtcs & (1 << j))) {
				continue;
			}

			/* check that no other device already uses this CRTC */
			crtc = res->crtcs[j];
			LIST_FOREACH(struct mbv_drm_dev*, iter, &devices) {
				if (iter->crtc == crtc) {
					crtc = -1;
					break;
				}
			}

			/* we have found a CRTC, so save it and return */
			if (crtc >= 0) {
				drmModeFreeEncoder(enc);
				dev->crtc = crtc;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	LOG_VPRINT_ERROR("Cannot find suitable CRTC for connector %u",
		conn->connector_id);
	return -ENOENT;
}


static int
mbv_drm_setupdev(struct mbv_drm_dev *dev,
	drmModeRes *res, drmModeConnector *conn, int mode_index)
{
	int ret;

	/* check if a monitor is connected */
	if (conn->connection != DRM_MODE_CONNECTED) {
		DEBUG_VPRINT("video-drm", "Ignoring unused connector %u",
			conn->connector_id);
		return -ENOENT;
	}

	/* check if there is at least one valid mode */
	if (conn->count_modes == 0) {
		DEBUG_VPRINT("video-drm", "No valid mode for connector %u",
			conn->connector_id);
		return -EFAULT;
	}

	/* copy the mode information into our device structure */
	memcpy(&dev->mode, &conn->modes[mode_index], sizeof(dev->mode));

	DEBUG_VPRINT("video-drm", "Mode for connector %u is %ux%u",
		conn->connector_id, conn->modes[mode_index].hdisplay,
		conn->modes[mode_index].vdisplay);

	/* find a crtc for this connector */
	if ((ret = mbv_drm_findcrtc(dev, res, conn)) != 0) {
		LOG_VPRINT_ERROR("No valid CRTC for connector %u",
			conn->connector_id);
		return ret;
	}

	DEBUG_VPRINT("video-drm", "Device CRTC = 0x%x",
		dev->crtc);

	/* create a front framebuffer for this CRTC */
	ret = mbv_drm_mkfb(dev,
		conn->modes[0].hdisplay, conn->modes[0].vdisplay);
	if (ret) {
		LOG_VPRINT_ERROR("Cannot create framebuffers for connector %u",
			conn->connector_id);
		return ret;
	}

	/* save crtc */
	dev->saved_crtc = drmModeGetCrtc(dev->fd, dev->crtc);

	DEBUG_VPRINT("video-drm", "Connector %u ready.",
		conn->connector_id);

	return 0;
}


static int
mbv_drm_prepare(int fd, int mode_index)
{
	drmModeRes *res;
	drmModeConnector *conn;
	unsigned int i;
	struct mbv_drm_dev *dev;
	int ret;

	DEBUG_PRINT("video-drm", "Initializing modesetting devices");

	/* retrieve resources */
	res = drmModeGetResources(fd);
	if (!res) {
		LOG_VPRINT_ERROR("Cannot retrieve DRM resources (%d): %s",
			errno, strerror(errno));
		return -errno;
	}

	/* iterate all connectors */
	for (i = 0; i < res->count_connectors; ++i) {
		/* get information for each connector */
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn) {
			LOG_VPRINT_ERROR("Cannot retrieve DRM connector %u:%u (%d): %s",
				i, res->connectors[i], errno, strerror(errno));
			continue;
		}

		/* list supported modes */
		for (ret = 0; ret < conn->count_modes; ret++) {
			DEBUG_VPRINT("video-drm", "Mode: %i %s %ix%i@%i vscan=%i htotal=%i vtotal=%i",
				ret, conn->modes[i].name, conn->modes[i].hdisplay, conn->modes[i].vdisplay,
				conn->modes[i].vrefresh, conn->modes[i].vscan,
				conn->modes[i].htotal, conn->modes[i].vtotal);
		}

		/* create a device structure */
		dev = malloc(sizeof(*dev));
		memset(dev, 0, sizeof(*dev));
		dev->conn = conn->connector_id;
		dev->fd = fd;

		/* call helper function to prepare this connector */
		ret = mbv_drm_setupdev(dev, res, conn, mode_index);
		if (ret) {
			if (ret != -ENOENT) {
				errno = -ret;
				LOG_VPRINT_ERROR("Cannot setup device for connector %u:%u (%d): %s",
					i, res->connectors[i], errno, strerror(errno));
			}
			free(dev);
			drmModeFreeConnector(conn);
			continue;
		}

		if (default_dev == NULL) {
			default_dev = dev;
		}

		/* free connector data and link device into global list */
		drmModeFreeConnector(conn);
		LIST_ADD(&devices, dev);
	}

	/* free resources again */
	drmModeFreeResources(res);

	DEBUG_PRINT("video-drm", "Modesetting initialized!");

	return 0;
}

#ifdef ENABLE_OPENGL
static int
create_egl_framebuffer(EGLDisplay egl_display,
	EGLSurface egl_surface, struct gbm_surface *gbm_surface,
	struct mbv_drm_dev * const dev, uint32_t * const fb)
{
	int ret;
	struct gbm_bo *bo;

	/* get the buffer object for the front buffer */
	eglSwapBuffers(egl_display, egl_surface);
	if ((bo = gbm_surface_lock_front_buffer(gbm_surface)) == NULL) {
		LOG_PRINT_ERROR("Could not get the surface's buffer object");
		return -1;
	}

	DEBUG_VPRINT(LOG_MODULE, "bo_handle=%d bo_stride=%d",
		gbm_bo_get_handle(bo).u32, gbm_bo_get_stride(bo));

	/* create a framebuffer for the buffer object */
	if ((ret = drmModeAddFB(dev->fd, dev->root.w, dev->root.h, 24, 32,
		gbm_bo_get_stride(bo), gbm_bo_get_handle(bo).u32, fb)) != 0) {
		LOG_VPRINT_ERROR("Could not create GBM fb: %s",
			strerror(ret));
		gbm_surface_release_buffer(gbm_surface, bo);
		return -1;
	}

	gbm_surface_release_buffer(gbm_surface, bo);
	return 0;
}
#endif


static void
wait_for_vsync(void)
{
	drmVBlank vb;
	vb.request.type = DRM_VBLANK_RELATIVE;
	vb.request.sequence = 1;
	vb.request.signal = 0;
	drmWaitVBlank(default_dev->fd, &vb);
}


static struct mbv_surface *
init(struct mbv_drv_funcs * const driver,
	int argc, char **argv, int * const w, int * const h)
{
	int i, ret, fd = -1;
	int mode_index = 0;
	const char *card = "/dev/dri/card0";
	uint64_t has_dumb;

	assert(w != NULL);
	assert(h != NULL);

	DEBUG_VPRINT("video-drm", "Using card '%s'",
		card);
	
	for (i = 0; i < argc; i++) {
		if (!strncmp("--video:mode_index=", argv[i], 19)) {
			mode_index = atoi(&argv[i][18]);
			DEBUG_VPRINT("video-drm", "Specified mode index: %i",
				mode_index);
		}
	}

	LIST_INIT(&devices);

	/* open the DRM device */
	fd = open(card, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ret = -errno;
		LOG_VPRINT_ERROR("Cannot open '%s': %s",
			card, strerror(errno));
		goto end;
	}

	/* check that the device has dumb buffer support */
	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0 || !has_dumb) {
	    	LOG_VPRINT_ERROR("DRM device '%s' does not support dumb buffers!",
			card);
		close(fd);
		ret = -EOPNOTSUPP;
		goto end;
	}

	/* prepare all connectors and CRTCs */
	ret = mbv_drm_prepare(fd, mode_index);
	if (ret) {
		goto end;
	}

	*w = default_dev->root.w;
	*h = default_dev->root.h;
	ret = 0;

#ifdef ENABLE_OPENGL

	/* if the no-accel argumant was provided skip
	 * GL initialization */
	for (i = 0; i < argc; i++) {
		if (!strncmp(argv[i], "--no-accel", 10)) {
			goto end;
		}
	}


	const EGLint configAttribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_BLUE_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_RED_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_NONE
	};
	const EGLint surfaceAttribs[] = {
		EGL_RENDER_BUFFER, EGL_SINGLE_BUFFER,
		EGL_NONE
	};

	EGLint major, minor, n_configs, err;
	EGLConfig cfg;
	EGLSurface egl_surface;
	void *gl_surface;

	/* create a framebuffer bo */
	gbm_dev = gbm_create_device(fd);
	gbm_surface = gbm_surface_create(gbm_dev, default_dev->root.w, default_dev->root.h,
		GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (gbm_surface == NULL) {
		LOG_PRINT_ERROR("Could not create GBM surface!");
		goto end;
	}

	#if 1
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display = NULL;
		get_platform_display = (void *) eglGetProcAddress("eglGetPlatformDisplayEXT");
	assert(get_platform_display != NULL);


	#define EGL_PLATFORM_GBM_KHR              0x31D7

	if ((egl_display = get_platform_display(EGL_PLATFORM_GBM_KHR, (void*) gbm_dev, NULL)) == EGL_NO_DISPLAY) {
		LOG_PRINT_ERROR("Could not create EGL display!");
		gbm_surface_destroy(gbm_surface);
		gbm_device_destroy(gbm_dev);
		goto end;
	}
	#else
	/* get display */
	if ((egl_display = eglGetDisplay((void*) gbm_dev)) == EGL_NO_DISPLAY) {
		LOG_PRINT_ERROR("Could not create EGL display!");
		goto end;
	}
	#endif

	if (eglInitialize(egl_display, &major, &minor) == EGL_FALSE) {
		LOG_PRINT_ERROR("Could not initialize EGL display!");
		gbm_surface_destroy(gbm_surface);
		gbm_device_destroy(gbm_dev);
		goto end;
	}
	if (eglChooseConfig(egl_display, configAttribs, &cfg, 1, &n_configs) == EGL_FALSE) {
		LOG_PRINT_ERROR("Could not find EGL config!");
		gbm_surface_destroy(gbm_surface);
		gbm_device_destroy(gbm_dev);
		eglTerminate(egl_display);
		goto end;
	}
	if (eglBindAPI(EGL_OPENGL_API) == EGL_FALSE) {
		LOG_PRINT_ERROR("Could not bind OpenGL API!");
		gbm_surface_destroy(gbm_surface);
		gbm_device_destroy(gbm_dev);
		eglTerminate(egl_display);
		goto end;
	}
	if ((egl_ctx = eglCreateContext(egl_display, cfg, EGL_NO_CONTEXT, NULL)) == EGL_NO_CONTEXT) {
		LOG_PRINT_ERROR("Could not create egl context!");
		if ((err = eglGetError()) != EGL_SUCCESS) {
			LOG_VPRINT_ERROR("GL error: 0x%x", err);
		}
		gbm_surface_destroy(gbm_surface);
		gbm_device_destroy(gbm_dev);
		eglTerminate(egl_display);
		goto end;
	}
	if ((egl_surface = eglCreateWindowSurface(egl_display, cfg,
		(EGLNativeWindowType) gbm_surface, surfaceAttribs)) == EGL_NO_SURFACE) {
		LOG_PRINT_ERROR("Could not create window surface");
		gbm_surface_destroy(gbm_surface);
		gbm_device_destroy(gbm_dev);
		eglDestroyContext(egl_display, egl_ctx);
		eglTerminate(egl_display);
		goto end;
	}
	if (eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_ctx) == EGL_FALSE) {
		LOG_PRINT_ERROR("Could not make GL context current!");
		gbm_surface_destroy(gbm_surface);
		gbm_device_destroy(gbm_dev);
		eglDestroyContext(egl_display, egl_ctx);
		eglTerminate(egl_display);
		goto end;
	}

	/* create a DRM framebuffer from the bo */
	if (create_egl_framebuffer(egl_display, egl_surface,
		gbm_surface, default_dev, &egl_framebuffer) == -1) {
		LOG_PRINT_ERROR("Could not make GL context current!");
		gbm_surface_destroy(gbm_surface);
		gbm_device_destroy(gbm_dev);
		eglDestroyContext(egl_display, egl_ctx);
		eglTerminate(egl_display);
		goto end;
	}

	/* an set it as the current framebuffer */
	if (drmModeSetCrtc(default_dev->fd,
		default_dev->crtc,
		egl_framebuffer, 0, 0,
		&default_dev->conn, 1,
		&default_dev->mode)) {
		LOG_PRINT_ERROR("Could not set EGL framebuffer");
		gbm_surface_destroy(gbm_surface);
		gbm_device_destroy(gbm_dev);
		eglDestroyContext(egl_display, egl_ctx);
		eglTerminate(egl_display);
		goto end;
	}

	/* now that we have everything setup we can attempt to
	 * start the GL driver */
	if ((gl_surface = avbox_video_glinit(driver, *w, *h, wait_for_vsync)) != NULL) {
		egl_enabled = 1;
		return gl_surface;
	} else {
		LOG_PRINT_ERROR("Could not initialize GL driver! Using software driver.");
		gbm_surface_destroy(gbm_surface);
		gbm_device_destroy(gbm_dev);
		eglDestroyContext(egl_display, egl_ctx);
		eglTerminate(egl_display);
	}

#endif

end:
	if (ret) {
		errno = -ret;
		LOG_VPRINT_ERROR("Modesetting failed with error %d: %s",
			errno, strerror(errno));
		if (fd != -1) {
			close(fd);
		}
		return NULL;
	}
	return &default_dev->root;
}


/*
 * This cleans up all the devices we created during
 * modeset_prepare(). It resets the CRTCs to their saved
 * states and deallocates all memory.
 */
static void
shutdown()
{
	struct mbv_drm_dev *iter;
	struct drm_mode_destroy_dumb dreq;

#ifdef ENABLE_OPENGL
	if (egl_enabled) {
		drmModeRmFB(default_dev->fd, egl_framebuffer);
		gbm_surface_destroy(gbm_surface);
		gbm_device_destroy(gbm_dev);
		eglDestroyContext(egl_display, egl_ctx);
		eglTerminate(egl_display);
	}
#endif

	LIST_FOREACH_SAFE(struct mbv_drm_dev*, iter, &devices, {
		/* remove from global list */
		LIST_REMOVE(iter);

		/* restore saved CRTC configuration */
		drmModeSetCrtc(iter->fd,
			       iter->saved_crtc->crtc_id,
			       iter->saved_crtc->buffer_id,
			       iter->saved_crtc->x,
			       iter->saved_crtc->y,
			       &iter->conn,
			       1,
			       &iter->saved_crtc->mode);
		drmModeFreeCrtc(iter->saved_crtc);

		/* delete framebuffers */
		drmModeRmFB(iter->fd, iter->root.buffers[0].fb);
		drmModeRmFB(iter->fd, iter->root.buffers[1].fb);

		/* delete dumb buffers */
		memset(&dreq, 0, sizeof(dreq));
		dreq.handle = iter->root.buffers[0].hnd;
		drmIoctl(iter->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
		memset(&dreq, 0, sizeof(dreq));
		dreq.handle = iter->root.buffers[1].hnd;
		drmIoctl(iter->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);

		/* free allocated memory */
		free(iter);
	});
}


void
mbv_drm_initft(struct mbv_drv_funcs * const funcs)
{
	funcs->init = &init;
	funcs->surface_new = &surface_new;
	funcs->surface_lock = &surface_lock;
	funcs->surface_unlock = &surface_unlock;
	funcs->surface_blitbuf = &surface_blitbuf;
	funcs->surface_blit = &surface_blit;
	funcs->surface_scaleblit = NULL;
	funcs->surface_update = &surface_update;
	funcs->surface_destroy = &surface_destroy;
	funcs->shutdown = &shutdown;
}
