/**************************************************************************
 *
 * Copyright 2010 Christian König
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/
#ifndef vl_vertex_buffers_h
#define vl_vertex_buffers_h

#include <pipe/p_state.h>
#include <pipe/p_video_state.h>

#include "vl_defines.h"
#include "vl_types.h"

/* vertex buffers act as a todo list
 * uploading all the usefull informations to video ram
 * so a vertex shader can work with them
 */

/* inputs to the vertex shaders */
enum VS_INPUT
{
   VS_I_RECT = 0,
   VS_I_VPOS = 1,

   VS_I_BLOCK_NUM = 2,

   VS_I_MV_TOP = 2,
   VS_I_MV_BOTTOM = 3,

   NUM_VS_INPUTS = 4
};

struct vl_vertex_buffer
{
   unsigned width, height;

   struct {
      struct pipe_resource    *resource;
      struct pipe_transfer    *transfer;
      struct pipe_ycbcr_block *vertex_stream;
   } ycbcr[VL_MAX_PLANES];

   struct {
      struct pipe_resource     *resource;
      struct pipe_transfer     *transfer;
      struct pipe_motionvector *vertex_stream;
   } mv[VL_MAX_REF_FRAMES];
};

struct pipe_vertex_buffer vl_vb_upload_quads(struct pipe_context *pipe);

struct pipe_vertex_buffer vl_vb_upload_pos(struct pipe_context *pipe, unsigned width, unsigned height);

struct pipe_vertex_buffer vl_vb_upload_block_num(struct pipe_context *pipe, unsigned num_blocks);

void *vl_vb_get_ves_ycbcr(struct pipe_context *pipe);

void *vl_vb_get_ves_mv(struct pipe_context *pipe);

bool vl_vb_init(struct vl_vertex_buffer *buffer,
                struct pipe_context *pipe,
                unsigned width, unsigned height);

unsigned vl_vb_attributes_per_plock(struct vl_vertex_buffer *buffer);

void vl_vb_map(struct vl_vertex_buffer *buffer, struct pipe_context *pipe);

struct pipe_vertex_buffer vl_vb_get_ycbcr(struct vl_vertex_buffer *buffer, int component);

struct pipe_ycbcr_block *vl_vb_get_ycbcr_stream(struct vl_vertex_buffer *buffer, int component);

struct pipe_vertex_buffer vl_vb_get_mv(struct vl_vertex_buffer *buffer, int ref_frame);

unsigned vl_vb_get_mv_stream_stride(struct vl_vertex_buffer *buffer);

struct pipe_motionvector *vl_vb_get_mv_stream(struct vl_vertex_buffer *buffer, int ref_frame);

void vl_vb_unmap(struct vl_vertex_buffer *buffer, struct pipe_context *pipe);

void vl_vb_cleanup(struct vl_vertex_buffer *buffer);

#endif /* vl_vertex_buffers_h */
