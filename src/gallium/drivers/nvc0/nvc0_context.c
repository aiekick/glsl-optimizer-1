/*
 * Copyright 2010 Christoph Bumiller
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
 * THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "draw/draw_context.h"
#include "pipe/p_defines.h"

#include "nvc0_context.h"
#include "nvc0_screen.h"
#include "nvc0_resource.h"

#include "nouveau/nouveau_reloc.h"

static void
nvc0_flush(struct pipe_context *pipe, unsigned flags,
           struct pipe_fence_handle **fence)
{
   struct nvc0_context *nvc0 = nvc0_context(pipe);
   struct nouveau_channel *chan = nvc0->screen->base.channel;

   if (flags & PIPE_FLUSH_TEXTURE_CACHE) {
      BEGIN_RING(chan, RING_3D(SERIALIZE), 1);
      OUT_RING  (chan, 0);
      BEGIN_RING(chan, RING_3D(TEX_CACHE_CTL), 1);
      OUT_RING  (chan, 0x00);
   }

   if (fence) {
      nvc0_screen_fence_new(nvc0->screen, (struct nvc0_fence **)fence, TRUE);
   }

   if (flags & (PIPE_FLUSH_SWAPBUFFERS | PIPE_FLUSH_FRAME)) {
      FIRE_RING(chan);

      nvc0_screen_fence_next(nvc0->screen);
   }
}

static void
nvc0_destroy(struct pipe_context *pipe)
{
   struct nvc0_context *nvc0 = nvc0_context(pipe);

   draw_destroy(nvc0->draw);

   if (nvc0->screen->cur_ctx == nvc0)
      nvc0->screen->cur_ctx = NULL;

   FREE(nvc0);
}

struct pipe_context *
nvc0_create(struct pipe_screen *pscreen, void *priv)
{
   struct pipe_winsys *pipe_winsys = pscreen->winsys;
   struct nvc0_screen *screen = nvc0_screen(pscreen);
   struct nvc0_context *nvc0;

   nvc0 = CALLOC_STRUCT(nvc0_context);
   if (!nvc0)
      return NULL;
   nvc0->screen = screen;

   nvc0->pipe.winsys = pipe_winsys;
   nvc0->pipe.screen = pscreen;
   nvc0->pipe.priv = priv;

   nvc0->pipe.destroy = nvc0_destroy;

   nvc0->pipe.draw_vbo = nvc0_draw_vbo;
   nvc0->pipe.clear = nvc0_clear;

   nvc0->pipe.flush = nvc0_flush;

   screen->base.channel->user_private = nvc0;

   nvc0_init_surface_functions(nvc0);
   nvc0_init_state_functions(nvc0);
   nvc0_init_resource_functions(&nvc0->pipe);

   nvc0->draw = draw_create(&nvc0->pipe);
   assert(nvc0->draw);
   draw_set_rasterize_stage(nvc0->draw, nvc0_draw_render_stage(nvc0));

   return &nvc0->pipe;
}

struct resident {
   struct nvc0_resource *res;
   uint32_t flags;
};

void
nvc0_bufctx_add_resident(struct nvc0_context *nvc0, int ctx,
                         struct nvc0_resource *resource, uint32_t flags)
{
   struct resident rsd = { resource, flags };

   if (!resource->bo)
      return;

   /* We don't need to reference the resource here, it will be referenced
    * in the context/state, and bufctx will be reset when state changes.
    */
   util_dynarray_append(&nvc0->residents[ctx], struct resident, rsd);
}

void
nvc0_bufctx_del_resident(struct nvc0_context *nvc0, int ctx,
                         struct nvc0_resource *resource)
{
   struct resident *rsd, *top;
   unsigned i;

   for (i = 0; i < nvc0->residents[ctx].size / sizeof(struct resident); ++i) {
      rsd = util_dynarray_element(&nvc0->residents[ctx], struct resident, i);

      if (rsd->res == resource) {
         top = util_dynarray_pop_ptr(&nvc0->residents[ctx], struct resident);
         if (rsd != top)
            *rsd = *top;
         break;
      }
   }
}

void
nvc0_bufctx_emit_relocs(struct nvc0_context *nvc0)
{
   struct resident *rsd;
   struct util_dynarray *array;
   unsigned ctx, i;

   for (ctx = 0; ctx < NVC0_BUFCTX_COUNT; ++ctx) {
      array = &nvc0->residents[ctx];

      for (i = 0; i < array->size / sizeof(struct resident); ++i) {
         rsd = util_dynarray_element(array, struct resident, i);

         nvc0_resource_validate(rsd->res, rsd->flags);
      }
   }

   nvc0_screen_make_buffers_resident(nvc0->screen);
}