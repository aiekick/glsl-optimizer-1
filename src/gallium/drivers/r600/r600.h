/*
 * Copyright 2010 Jerome Glisse <glisse@freedesktop.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *      Jerome Glisse
 */
#ifndef R600_H
#define R600_H

#include <stdint.h>
#include <stdio.h>

#define RADEON_CTX_MAX_PM4	(64 * 1024 / 4)

#define R600_ERR(fmt, args...) \
	fprintf(stderr, "EE %s/%s:%d - "fmt, __FILE__, __func__, __LINE__, ##args)

typedef uint64_t		u64;
typedef uint32_t		u32;
typedef uint16_t		u16;
typedef uint8_t			u8;

struct radeon;

enum radeon_family {
	CHIP_UNKNOWN,
	CHIP_R100,
	CHIP_RV100,
	CHIP_RS100,
	CHIP_RV200,
	CHIP_RS200,
	CHIP_R200,
	CHIP_RV250,
	CHIP_RS300,
	CHIP_RV280,
	CHIP_R300,
	CHIP_R350,
	CHIP_RV350,
	CHIP_RV380,
	CHIP_R420,
	CHIP_R423,
	CHIP_RV410,
	CHIP_RS400,
	CHIP_RS480,
	CHIP_RS600,
	CHIP_RS690,
	CHIP_RS740,
	CHIP_RV515,
	CHIP_R520,
	CHIP_RV530,
	CHIP_RV560,
	CHIP_RV570,
	CHIP_R580,
	CHIP_R600,
	CHIP_RV610,
	CHIP_RV630,
	CHIP_RV670,
	CHIP_RV620,
	CHIP_RV635,
	CHIP_RS780,
	CHIP_RS880,
	CHIP_RV770,
	CHIP_RV730,
	CHIP_RV710,
	CHIP_RV740,
	CHIP_CEDAR,
	CHIP_REDWOOD,
	CHIP_JUNIPER,
	CHIP_CYPRESS,
	CHIP_HEMLOCK,
	CHIP_LAST,
};

enum radeon_family r600_get_family(struct radeon *rw);

/*
 * radeon object functions
 */
#if 0
struct radeon_bo {
	unsigned			refcount;
	unsigned			handle;
	unsigned			size;
	unsigned			alignment;
	unsigned			map_count;
	void				*data;
};
struct radeon_bo *radeon_bo(struct radeon *radeon, unsigned handle,
			unsigned size, unsigned alignment, void *ptr);
int radeon_bo_map(struct radeon *radeon, struct radeon_bo *bo);
void radeon_bo_unmap(struct radeon *radeon, struct radeon_bo *bo);
struct radeon_bo *radeon_bo_incref(struct radeon *radeon, struct radeon_bo *bo);
struct radeon_bo *radeon_bo_decref(struct radeon *radeon, struct radeon_bo *bo);
int radeon_bo_wait(struct radeon *radeon, struct radeon_bo *bo);
#endif
/* lowlevel WS bo */
struct radeon_ws_bo;
struct radeon_ws_bo *radeon_ws_bo(struct radeon *radeon,
				  unsigned size, unsigned alignment, unsigned usage);
struct radeon_ws_bo *radeon_ws_bo_handle(struct radeon *radeon,
					 unsigned handle);
void *radeon_ws_bo_map(struct radeon *radeon, struct radeon_ws_bo *bo, unsigned usage, void *ctx);
void radeon_ws_bo_unmap(struct radeon *radeon, struct radeon_ws_bo *bo);
void radeon_ws_bo_reference(struct radeon *radeon, struct radeon_ws_bo **dst,
			    struct radeon_ws_bo *src);
int radeon_ws_bo_wait(struct radeon *radeon, struct radeon_ws_bo *bo);

/* R600/R700 STATES */
#define R600_GROUP_MAX			16
#define R600_BLOCK_MAX_BO		32
#define R600_BLOCK_MAX_REG		128

enum r600_group_id {
	R600_GROUP_CONFIG = 0,
	R600_GROUP_CONTEXT,
	R600_GROUP_ALU_CONST,
	R600_GROUP_RESOURCE,
	R600_GROUP_SAMPLER,
	R600_GROUP_CTL_CONST,
	R600_GROUP_LOOP_CONST,
	R600_GROUP_BOOL_CONST,
	R600_NGROUPS
};

struct r600_pipe_reg {
	unsigned			group_id;
	u32				offset;
	u32				mask;
	u32				value;
	struct radeon_ws_bo		*bo;
};

struct r600_pipe_state {
	unsigned			id;
	unsigned			nregs;
	struct r600_pipe_reg		regs[R600_BLOCK_MAX_REG];
};

static inline void r600_pipe_state_add_reg(struct r600_pipe_state *state,
					unsigned group_id, u32 offset,
					u32 value, u32 mask,
					struct radeon_ws_bo *bo)
{
	state->regs[state->nregs].group_id = group_id;
	state->regs[state->nregs].offset = offset;
	state->regs[state->nregs].value = value;
	state->regs[state->nregs].mask = mask;
	state->regs[state->nregs].bo = bo;
	state->nregs++;
	assert(state->nregs < R600_BLOCK_MAX_REG);
}

#define R600_BLOCK_STATUS_ENABLED	(1 << 0)
#define R600_BLOCK_STATUS_DIRTY		(1 << 1)

struct r600_block_reloc {
	struct radeon_ws_bo	*bo;
	unsigned		nreloc;
	unsigned		bo_pm4_index[R600_BLOCK_MAX_BO];
};

struct r600_group_block {
	unsigned		status;
	unsigned		start_offset;
	unsigned		pm4_ndwords;
	unsigned		nbo;
	unsigned		nreg;
	u32			pm4[R600_BLOCK_MAX_REG];
	unsigned		pm4_bo_index[R600_BLOCK_MAX_REG];
	struct r600_block_reloc	reloc[R600_BLOCK_MAX_BO];
};

struct r600_group {
	unsigned		start_offset;
	unsigned		end_offset;
	unsigned		nblocks;
	struct r600_group_block	*blocks;
	unsigned		*offset_block_id;
};

#pragma pack(1)
struct r600_reloc {
	uint32_t	handle;
	uint32_t	read_domain;
	uint32_t	write_domain;
	uint32_t	flags;
};
#pragma pack()

struct r600_context {
	struct radeon		*radeon;
	unsigned		ngroups;
	struct r600_group	groups[R600_GROUP_MAX];
	unsigned		pm4_ndwords;
	unsigned		pm4_cdwords;
	unsigned		pm4_dirty_cdwords;
	unsigned		ctx_pm4_ndwords;
	unsigned		nreloc;
	unsigned		creloc;
	struct r600_reloc	*reloc;
	struct radeon_ws_bo	**bo;
	u32			*pm4;
};

struct r600_draw {
	u32			vgt_num_indices;
	u32			vgt_num_instances;
	u32			vgt_index_type;
	u32			vgt_draw_initiator;
	u32			indices_bo_offset;
	struct radeon_ws_bo	*indices;
};

int r600_context_init(struct r600_context *ctx, struct radeon *radeon);
void r600_context_fini(struct r600_context *ctx);
void r600_context_pipe_state_set(struct r600_context *ctx, struct r600_pipe_state *state);
void r600_context_pipe_state_set_ps_resource(struct r600_context *ctx, struct r600_pipe_state *state, unsigned rid);
void r600_context_pipe_state_set_vs_resource(struct r600_context *ctx, struct r600_pipe_state *state, unsigned rid);
void r600_context_pipe_state_set_ps_sampler(struct r600_context *ctx, struct r600_pipe_state *state, unsigned id);
void r600_context_pipe_state_set_vs_sampler(struct r600_context *ctx, struct r600_pipe_state *state, unsigned id);
void r600_context_flush(struct r600_context *ctx);
void r600_context_dump_bof(struct r600_context *ctx, const char *file);
void r600_context_draw(struct r600_context *ctx, const struct r600_draw *draw);

#endif