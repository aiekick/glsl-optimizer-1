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
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "xf86drm.h"
#include "r600.h"
#include "r600d.h"
#include "r600_priv.h"
#include "radeon_drm.h"
#include "bof.h"
#include "pipe/p_compiler.h"
#include "util/u_inlines.h"
#include <pipebuffer/pb_bufmgr.h>

struct radeon_ws_bo {
	struct pipe_reference reference;
	struct pb_buffer *pb;
};

struct radeon_bo {
	struct pipe_reference		reference;
	unsigned			handle;
	unsigned			size;
	unsigned			alignment;
	unsigned			map_count;
	void				*data;
};
struct radeon_bo *radeon_bo_pb_get_bo(struct pb_buffer *_buf);
int radeon_bo_map(struct radeon *radeon, struct radeon_bo *bo);
void radeon_bo_unmap(struct radeon *radeon, struct radeon_bo *bo);

unsigned radeon_ws_bo_get_handle(struct radeon_ws_bo *pb_bo);

static int r600_group_id_register_offset(unsigned offset)
{
	if (offset >= R600_CONFIG_REG_OFFSET && offset < R600_CONFIG_REG_END) {
		return R600_GROUP_CONFIG;
	}
	if (offset >= R600_CONTEXT_REG_OFFSET && offset < R600_CONTEXT_REG_END) {
		return R600_GROUP_CONTEXT;
	}
	if (offset >= R600_ALU_CONST_OFFSET && offset < R600_ALU_CONST_END) {
		return R600_GROUP_ALU_CONST;
	}
	if (offset >= R600_RESOURCE_OFFSET && offset < R600_RESOURCE_END) {
		return R600_GROUP_RESOURCE;
	}
	if (offset >= R600_SAMPLER_OFFSET && offset < R600_SAMPLER_END) {
		return R600_GROUP_SAMPLER;
	}
	if (offset >= R600_CTL_CONST_OFFSET && offset < R600_CTL_CONST_END) {
		return R600_GROUP_CTL_CONST;
	}
	if (offset >= R600_LOOP_CONST_OFFSET && offset < R600_LOOP_CONST_END) {
		return R600_GROUP_LOOP_CONST;
	}
	if (offset >= R600_BOOL_CONST_OFFSET && offset < R600_BOOL_CONST_END) {
		return R600_GROUP_BOOL_CONST;
	}
	return -1;
}

static int r600_context_add_block(struct r600_context *ctx, const struct r600_reg *reg, unsigned nreg)
{
	struct r600_group_block *block, *tmp;
	struct r600_group *group;
	int group_id, id;

	for (unsigned i = 0, n = 0; i < nreg; i += n) {
		u32 j, r;
		/* find number of consecutive registers */
		for (j = i + 1, r = reg[i].offset + 4, n = 1; j < (nreg - i); j++, n++, r+=4) {
			if (r != reg[j].offset) {
				break;
			}
		}

		/* find into which group this block is */
		group_id = r600_group_id_register_offset(reg[i].offset);
		assert(group_id >= 0);
		group = &ctx->groups[group_id];

		/* allocate new block */
		tmp = realloc(group->blocks, (group->nblocks + 1) * sizeof(struct r600_group_block));
		if (tmp == NULL) {
			return -ENOMEM;
		}
		group->blocks = tmp;
		block = &group->blocks[group->nblocks++];
		for (int j = 0; j < n; j++) {
			group->offset_block_id[((reg[i].offset - group->start_offset) >> 2) + j] = group->nblocks - 1;
		}

		/* initialize block */
		memset(block, 0, sizeof(struct r600_group_block));
		block->start_offset = reg[i].offset;
		block->pm4_ndwords = n;
		block->nreg = n;
		for (j = 0; j < n; j++) {
			if (reg[i+j].need_bo) {
				block->nbo++;
				assert(block->nbo < R600_BLOCK_MAX_BO);
				block->pm4_bo_index[j] = block->nbo;
				block->pm4[block->pm4_ndwords++] = PKT3(PKT3_NOP, 0);
				block->pm4[block->pm4_ndwords++] = 0x00000000;
				block->reloc[block->nbo].bo_pm4_index[block->reloc[block->nbo].nreloc++] = block->pm4_ndwords - 1;
			}
		}
		for (j = 0; j < n; j++) {
			if (reg[i+j].flush_flags) {
				block->pm4[block->pm4_ndwords++] = PKT3(PKT3_SURFACE_SYNC, 3);
				block->pm4[block->pm4_ndwords++] = reg[i+j].flush_flags;
				block->pm4[block->pm4_ndwords++] = 0xFFFFFFFF;
				block->pm4[block->pm4_ndwords++] = 0x00000000;
				block->pm4[block->pm4_ndwords++] = 0x0000000A;
				block->pm4[block->pm4_ndwords++] = PKT3(PKT3_NOP, 0);
				block->pm4[block->pm4_ndwords++] = 0x00000000;
				id = block->pm4_bo_index[j];
				block->reloc[id].bo_pm4_index[block->reloc[id].nreloc++] = block->pm4_ndwords - 1;
			}
		}
		/* check that we stay in limit */
		assert(block->pm4_ndwords < R600_BLOCK_MAX_REG);
	}
	return 0;
}

static int r600_group_init(struct r600_group *group, unsigned start_offset, unsigned end_offset)
{
	group->start_offset = start_offset;
	group->end_offset = end_offset;
	group->nblocks = 0;
	group->blocks = NULL;
	group->offset_block_id = calloc((end_offset - start_offset) >> 2, sizeof(unsigned));
	if (group->offset_block_id == NULL)
		return -ENOMEM;
	return 0;
}

static void r600_group_fini(struct r600_group *group)
{
	free(group->offset_block_id);
	free(group->blocks);
}

/* R600/R700 configuration */
static const struct r600_reg r600_reg_list[] = {
	{0, 0, R_008C00_SQ_CONFIG},
	{0, 0, R_008C04_SQ_GPR_RESOURCE_MGMT_1},
	{0, 0, R_008C08_SQ_GPR_RESOURCE_MGMT_2},
	{0, 0, R_008C0C_SQ_THREAD_RESOURCE_MGMT},
	{0, 0, R_008C10_SQ_STACK_RESOURCE_MGMT_1},
	{0, 0, R_008C14_SQ_STACK_RESOURCE_MGMT_2},
	{0, 0, R_008D8C_SQ_DYN_GPR_CNTL_PS_FLUSH_REQ},
	{0, 0, R_009508_TA_CNTL_AUX},
	{0, 0, R_009714_VC_ENHANCE},
	{0, 0, R_009830_DB_DEBUG},
	{0, 0, R_009838_DB_WATERMARKS},
	{0, 0, R_028350_SX_MISC},
	{0, 0, R_0286C8_SPI_THREAD_GROUPING},
	{0, 0, R_0288A8_SQ_ESGS_RING_ITEMSIZE},
	{0, 0, R_0288AC_SQ_GSVS_RING_ITEMSIZE},
	{0, 0, R_0288B0_SQ_ESTMP_RING_ITEMSIZE},
	{0, 0, R_0288B4_SQ_GSTMP_RING_ITEMSIZE},
	{0, 0, R_0288B8_SQ_VSTMP_RING_ITEMSIZE},
	{0, 0, R_0288BC_SQ_PSTMP_RING_ITEMSIZE},
	{0, 0, R_0288C0_SQ_FBUF_RING_ITEMSIZE},
	{0, 0, R_0288C4_SQ_REDUC_RING_ITEMSIZE},
	{0, 0, R_0288C8_SQ_GS_VERT_ITEMSIZE},
	{0, 0, R_028A10_VGT_OUTPUT_PATH_CNTL},
	{0, 0, R_028A14_VGT_HOS_CNTL},
	{0, 0, R_028A18_VGT_HOS_MAX_TESS_LEVEL},
	{0, 0, R_028A1C_VGT_HOS_MIN_TESS_LEVEL},
	{0, 0, R_028A20_VGT_HOS_REUSE_DEPTH},
	{0, 0, R_028A24_VGT_GROUP_PRIM_TYPE},
	{0, 0, R_028A28_VGT_GROUP_FIRST_DECR},
	{0, 0, R_028A2C_VGT_GROUP_DECR},
	{0, 0, R_028A30_VGT_GROUP_VECT_0_CNTL},
	{0, 0, R_028A34_VGT_GROUP_VECT_1_CNTL},
	{0, 0, R_028A38_VGT_GROUP_VECT_0_FMT_CNTL},
	{0, 0, R_028A3C_VGT_GROUP_VECT_1_FMT_CNTL},
	{0, 0, R_028A40_VGT_GS_MODE},
	{0, 0, R_028A4C_PA_SC_MODE_CNTL},
	{0, 0, R_028AB0_VGT_STRMOUT_EN},
	{0, 0, R_028AB4_VGT_REUSE_OFF},
	{0, 0, R_028AB8_VGT_VTX_CNT_EN},
	{0, 0, R_028B20_VGT_STRMOUT_BUFFER_EN},
	{0, 0, R_028028_DB_STENCIL_CLEAR},
	{0, 0, R_02802C_DB_DEPTH_CLEAR},
	{1, 0, R_028040_CB_COLOR0_BASE},
	{0, 0, R_0280A0_CB_COLOR0_INFO},
	{0, 0, R_028060_CB_COLOR0_SIZE},
	{0, 0, R_028080_CB_COLOR0_VIEW},
	{1, 0, R_0280E0_CB_COLOR0_FRAG},
	{1, 0, R_0280C0_CB_COLOR0_TILE},
	{0, 0, R_028100_CB_COLOR0_MASK},
	{1, 0, R_028044_CB_COLOR1_BASE},
	{0, 0, R_0280A4_CB_COLOR1_INFO},
	{0, 0, R_028064_CB_COLOR1_SIZE},
	{0, 0, R_028084_CB_COLOR1_VIEW},
	{1, 0, R_0280E4_CB_COLOR1_FRAG},
	{1, 0, R_0280C4_CB_COLOR1_TILE},
	{0, 0, R_028104_CB_COLOR1_MASK},
	{1, 0, R_028048_CB_COLOR2_BASE},
	{0, 0, R_0280A8_CB_COLOR2_INFO},
	{0, 0, R_028068_CB_COLOR2_SIZE},
	{0, 0, R_028088_CB_COLOR2_VIEW},
	{1, 0, R_0280E8_CB_COLOR2_FRAG},
	{1, 0, R_0280C8_CB_COLOR2_TILE},
	{0, 0, R_028108_CB_COLOR2_MASK},
	{1, 0, R_02804C_CB_COLOR3_BASE},
	{0, 0, R_0280AC_CB_COLOR3_INFO},
	{0, 0, R_02806C_CB_COLOR3_SIZE},
	{0, 0, R_02808C_CB_COLOR3_VIEW},
	{1, 0, R_0280EC_CB_COLOR3_FRAG},
	{1, 0, R_0280CC_CB_COLOR3_TILE},
	{0, 0, R_02810C_CB_COLOR3_MASK},
	{1, 0, R_028050_CB_COLOR4_BASE},
	{0, 0, R_0280B0_CB_COLOR4_INFO},
	{0, 0, R_028070_CB_COLOR4_SIZE},
	{0, 0, R_028090_CB_COLOR4_VIEW},
	{1, 0, R_0280F0_CB_COLOR4_FRAG},
	{1, 0, R_0280D0_CB_COLOR4_TILE},
	{0, 0, R_028110_CB_COLOR4_MASK},
	{1, 0, R_028054_CB_COLOR5_BASE},
	{0, 0, R_0280B4_CB_COLOR5_INFO},
	{0, 0, R_028074_CB_COLOR5_SIZE},
	{0, 0, R_028094_CB_COLOR5_VIEW},
	{1, 0, R_0280F4_CB_COLOR5_FRAG},
	{1, 0, R_0280D4_CB_COLOR5_TILE},
	{0, 0, R_028114_CB_COLOR5_MASK},
	{1, 0, R_028058_CB_COLOR6_BASE},
	{0, 0, R_0280B8_CB_COLOR6_INFO},
	{0, 0, R_028078_CB_COLOR6_SIZE},
	{0, 0, R_028098_CB_COLOR6_VIEW},
	{1, 0, R_0280F8_CB_COLOR6_FRAG},
	{1, 0, R_0280D8_CB_COLOR6_TILE},
	{0, 0, R_028118_CB_COLOR6_MASK},
	{1, 0, R_02805C_CB_COLOR7_BASE},
	{0, 0, R_0280BC_CB_COLOR7_INFO},
	{0, 0, R_02807C_CB_COLOR7_SIZE},
	{0, 0, R_02809C_CB_COLOR7_VIEW},
	{1, 0, R_0280FC_CB_COLOR7_FRAG},
	{1, 0, R_0280DC_CB_COLOR7_TILE},
	{0, 0, R_02811C_CB_COLOR7_MASK},
	{0, 0, R_028120_CB_CLEAR_RED},
	{0, 0, R_028124_CB_CLEAR_GREEN},
	{0, 0, R_028128_CB_CLEAR_BLUE},
	{0, 0, R_02812C_CB_CLEAR_ALPHA},
	{0, 0, R_02823C_CB_SHADER_MASK},
	{0, 0, R_028238_CB_TARGET_MASK},
	{0, 0, R_028410_SX_ALPHA_TEST_CONTROL},
	{0, 0, R_028414_CB_BLEND_RED},
	{0, 0, R_028418_CB_BLEND_GREEN},
	{0, 0, R_02841C_CB_BLEND_BLUE},
	{0, 0, R_028420_CB_BLEND_ALPHA},
	{0, 0, R_028424_CB_FOG_RED},
	{0, 0, R_028428_CB_FOG_GREEN},
	{0, 0, R_02842C_CB_FOG_BLUE},
	{0, 0, R_028430_DB_STENCILREFMASK},
	{0, 0, R_028434_DB_STENCILREFMASK_BF},
	{0, 0, R_028438_SX_ALPHA_REF},
	{0, 0, R_0286DC_SPI_FOG_CNTL},
	{0, 0, R_0286E0_SPI_FOG_FUNC_SCALE},
	{0, 0, R_0286E4_SPI_FOG_FUNC_BIAS},
	{0, 0, R_028780_CB_BLEND0_CONTROL},
	{0, 0, R_028784_CB_BLEND1_CONTROL},
	{0, 0, R_028788_CB_BLEND2_CONTROL},
	{0, 0, R_02878C_CB_BLEND3_CONTROL},
	{0, 0, R_028790_CB_BLEND4_CONTROL},
	{0, 0, R_028794_CB_BLEND5_CONTROL},
	{0, 0, R_028798_CB_BLEND6_CONTROL},
	{0, 0, R_02879C_CB_BLEND7_CONTROL},
	{0, 0, R_0287A0_CB_SHADER_CONTROL},
	{0, 0, R_028800_DB_DEPTH_CONTROL},
	{0, 0, R_028804_CB_BLEND_CONTROL},
	{0, 0, R_028808_CB_COLOR_CONTROL},
	{0, 0, R_02880C_DB_SHADER_CONTROL},
	{0, 0, R_028C04_PA_SC_AA_CONFIG},
	{0, 0, R_028C1C_PA_SC_AA_SAMPLE_LOCS_MCTX},
	{0, 0, R_028C20_PA_SC_AA_SAMPLE_LOCS_8S_WD1_MCTX},
	{0, 0, R_028C30_CB_CLRCMP_CONTROL},
	{0, 0, R_028C34_CB_CLRCMP_SRC},
	{0, 0, R_028C38_CB_CLRCMP_DST},
	{0, 0, R_028C3C_CB_CLRCMP_MSK},
	{0, 0, R_028C48_PA_SC_AA_MASK},
	{0, 0, R_028D2C_DB_SRESULTS_COMPARE_STATE1},
	{0, 0, R_028D44_DB_ALPHA_TO_MASK},
	{1, 0, R_02800C_DB_DEPTH_BASE},
	{0, 0, R_028000_DB_DEPTH_SIZE},
	{0, 0, R_028004_DB_DEPTH_VIEW},
	{0, 0, R_028010_DB_DEPTH_INFO},
	{0, 0, R_028D0C_DB_RENDER_CONTROL},
	{0, 0, R_028D10_DB_RENDER_OVERRIDE},
	{0, 0, R_028D24_DB_HTILE_SURFACE},
	{0, 0, R_028D30_DB_PRELOAD_CONTROL},
	{0, 0, R_028D34_DB_PREFETCH_LIMIT},
	{0, 0, R_028030_PA_SC_SCREEN_SCISSOR_TL},
	{0, 0, R_028034_PA_SC_SCREEN_SCISSOR_BR},
	{0, 0, R_028200_PA_SC_WINDOW_OFFSET},
	{0, 0, R_028204_PA_SC_WINDOW_SCISSOR_TL},
	{0, 0, R_028208_PA_SC_WINDOW_SCISSOR_BR},
	{0, 0, R_02820C_PA_SC_CLIPRECT_RULE},
	{0, 0, R_028210_PA_SC_CLIPRECT_0_TL},
	{0, 0, R_028214_PA_SC_CLIPRECT_0_BR},
	{0, 0, R_028218_PA_SC_CLIPRECT_1_TL},
	{0, 0, R_02821C_PA_SC_CLIPRECT_1_BR},
	{0, 0, R_028220_PA_SC_CLIPRECT_2_TL},
	{0, 0, R_028224_PA_SC_CLIPRECT_2_BR},
	{0, 0, R_028228_PA_SC_CLIPRECT_3_TL},
	{0, 0, R_02822C_PA_SC_CLIPRECT_3_BR},
	{0, 0, R_028230_PA_SC_EDGERULE},
	{0, 0, R_028240_PA_SC_GENERIC_SCISSOR_TL},
	{0, 0, R_028244_PA_SC_GENERIC_SCISSOR_BR},
	{0, 0, R_028250_PA_SC_VPORT_SCISSOR_0_TL},
	{0, 0, R_028254_PA_SC_VPORT_SCISSOR_0_BR},
	{0, 0, R_0282D0_PA_SC_VPORT_ZMIN_0},
	{0, 0, R_0282D4_PA_SC_VPORT_ZMAX_0},
	{0, 0, R_02843C_PA_CL_VPORT_XSCALE_0},
	{0, 0, R_028440_PA_CL_VPORT_XOFFSET_0},
	{0, 0, R_028444_PA_CL_VPORT_YSCALE_0},
	{0, 0, R_028448_PA_CL_VPORT_YOFFSET_0},
	{0, 0, R_02844C_PA_CL_VPORT_ZSCALE_0},
	{0, 0, R_028450_PA_CL_VPORT_ZOFFSET_0},
	{0, 0, R_0286D4_SPI_INTERP_CONTROL_0},
	{0, 0, R_028810_PA_CL_CLIP_CNTL},
	{0, 0, R_028814_PA_SU_SC_MODE_CNTL},
	{0, 0, R_028818_PA_CL_VTE_CNTL},
	{0, 0, R_02881C_PA_CL_VS_OUT_CNTL},
	{0, 0, R_028820_PA_CL_NANINF_CNTL},
	{0, 0, R_028A00_PA_SU_POINT_SIZE},
	{0, 0, R_028A04_PA_SU_POINT_MINMAX},
	{0, 0, R_028A08_PA_SU_LINE_CNTL},
	{0, 0, R_028A0C_PA_SC_LINE_STIPPLE},
	{0, 0, R_028A48_PA_SC_MPASS_PS_CNTL},
	{0, 0, R_028C00_PA_SC_LINE_CNTL},
	{0, 0, R_028C0C_PA_CL_GB_VERT_CLIP_ADJ},
	{0, 0, R_028C10_PA_CL_GB_VERT_DISC_ADJ},
	{0, 0, R_028C14_PA_CL_GB_HORZ_CLIP_ADJ},
	{0, 0, R_028C18_PA_CL_GB_HORZ_DISC_ADJ},
	{0, 0, R_028DF8_PA_SU_POLY_OFFSET_DB_FMT_CNTL},
	{0, 0, R_028DFC_PA_SU_POLY_OFFSET_CLAMP},
	{0, 0, R_028E00_PA_SU_POLY_OFFSET_FRONT_SCALE},
	{0, 0, R_028E04_PA_SU_POLY_OFFSET_FRONT_OFFSET},
	{0, 0, R_028E08_PA_SU_POLY_OFFSET_BACK_SCALE},
	{0, 0, R_028E0C_PA_SU_POLY_OFFSET_BACK_OFFSET},
	{0, 0, R_028E20_PA_CL_UCP0_X},
	{0, 0, R_028E24_PA_CL_UCP0_Y},
	{0, 0, R_028E28_PA_CL_UCP0_Z},
	{0, 0, R_028E2C_PA_CL_UCP0_W},
	{0, 0, R_028E30_PA_CL_UCP1_X},
	{0, 0, R_028E34_PA_CL_UCP1_Y},
	{0, 0, R_028E38_PA_CL_UCP1_Z},
	{0, 0, R_028E3C_PA_CL_UCP1_W},
	{0, 0, R_028E40_PA_CL_UCP2_X},
	{0, 0, R_028E44_PA_CL_UCP2_Y},
	{0, 0, R_028E48_PA_CL_UCP2_Z},
	{0, 0, R_028E4C_PA_CL_UCP2_W},
	{0, 0, R_028E50_PA_CL_UCP3_X},
	{0, 0, R_028E54_PA_CL_UCP3_Y},
	{0, 0, R_028E58_PA_CL_UCP3_Z},
	{0, 0, R_028E5C_PA_CL_UCP3_W},
	{0, 0, R_028E60_PA_CL_UCP4_X},
	{0, 0, R_028E64_PA_CL_UCP4_Y},
	{0, 0, R_028E68_PA_CL_UCP4_Z},
	{0, 0, R_028E6C_PA_CL_UCP4_W},
	{0, 0, R_028E70_PA_CL_UCP5_X},
	{0, 0, R_028E74_PA_CL_UCP5_Y},
	{0, 0, R_028E78_PA_CL_UCP5_Z},
	{0, 0, R_028E7C_PA_CL_UCP5_W},
	{0, 0, R_028380_SQ_VTX_SEMANTIC_0},
	{0, 0, R_028384_SQ_VTX_SEMANTIC_1},
	{0, 0, R_028388_SQ_VTX_SEMANTIC_2},
	{0, 0, R_02838C_SQ_VTX_SEMANTIC_3},
	{0, 0, R_028390_SQ_VTX_SEMANTIC_4},
	{0, 0, R_028394_SQ_VTX_SEMANTIC_5},
	{0, 0, R_028398_SQ_VTX_SEMANTIC_6},
	{0, 0, R_02839C_SQ_VTX_SEMANTIC_7},
	{0, 0, R_0283A0_SQ_VTX_SEMANTIC_8},
	{0, 0, R_0283A4_SQ_VTX_SEMANTIC_9},
	{0, 0, R_0283A8_SQ_VTX_SEMANTIC_10},
	{0, 0, R_0283AC_SQ_VTX_SEMANTIC_11},
	{0, 0, R_0283B0_SQ_VTX_SEMANTIC_12},
	{0, 0, R_0283B4_SQ_VTX_SEMANTIC_13},
	{0, 0, R_0283B8_SQ_VTX_SEMANTIC_14},
	{0, 0, R_0283BC_SQ_VTX_SEMANTIC_15},
	{0, 0, R_0283C0_SQ_VTX_SEMANTIC_16},
	{0, 0, R_0283C4_SQ_VTX_SEMANTIC_17},
	{0, 0, R_0283C8_SQ_VTX_SEMANTIC_18},
	{0, 0, R_0283CC_SQ_VTX_SEMANTIC_19},
	{0, 0, R_0283D0_SQ_VTX_SEMANTIC_20},
	{0, 0, R_0283D4_SQ_VTX_SEMANTIC_21},
	{0, 0, R_0283D8_SQ_VTX_SEMANTIC_22},
	{0, 0, R_0283DC_SQ_VTX_SEMANTIC_23},
	{0, 0, R_0283E0_SQ_VTX_SEMANTIC_24},
	{0, 0, R_0283E4_SQ_VTX_SEMANTIC_25},
	{0, 0, R_0283E8_SQ_VTX_SEMANTIC_26},
	{0, 0, R_0283EC_SQ_VTX_SEMANTIC_27},
	{0, 0, R_0283F0_SQ_VTX_SEMANTIC_28},
	{0, 0, R_0283F4_SQ_VTX_SEMANTIC_29},
	{0, 0, R_0283F8_SQ_VTX_SEMANTIC_30},
	{0, 0, R_0283FC_SQ_VTX_SEMANTIC_31},
	{0, 0, R_028614_SPI_VS_OUT_ID_0},
	{0, 0, R_028618_SPI_VS_OUT_ID_1},
	{0, 0, R_02861C_SPI_VS_OUT_ID_2},
	{0, 0, R_028620_SPI_VS_OUT_ID_3},
	{0, 0, R_028624_SPI_VS_OUT_ID_4},
	{0, 0, R_028628_SPI_VS_OUT_ID_5},
	{0, 0, R_02862C_SPI_VS_OUT_ID_6},
	{0, 0, R_028630_SPI_VS_OUT_ID_7},
	{0, 0, R_028634_SPI_VS_OUT_ID_8},
	{0, 0, R_028638_SPI_VS_OUT_ID_9},
	{0, 0, R_0286C4_SPI_VS_OUT_CONFIG},
	{1, 0, R_028858_SQ_PGM_START_VS},
	{0, S_0085F0_SH_ACTION_ENA(1), R_028868_SQ_PGM_RESOURCES_VS},
	{1, 0, R_028894_SQ_PGM_START_FS},
	{0, S_0085F0_SH_ACTION_ENA(1), R_0288A4_SQ_PGM_RESOURCES_FS},
	{0, 0, R_0288D0_SQ_PGM_CF_OFFSET_VS},
	{0, 0, R_0288DC_SQ_PGM_CF_OFFSET_FS},
	{0, 0, R_028644_SPI_PS_INPUT_CNTL_0},
	{0, 0, R_028648_SPI_PS_INPUT_CNTL_1},
	{0, 0, R_02864C_SPI_PS_INPUT_CNTL_2},
	{0, 0, R_028650_SPI_PS_INPUT_CNTL_3},
	{0, 0, R_028654_SPI_PS_INPUT_CNTL_4},
	{0, 0, R_028658_SPI_PS_INPUT_CNTL_5},
	{0, 0, R_02865C_SPI_PS_INPUT_CNTL_6},
	{0, 0, R_028660_SPI_PS_INPUT_CNTL_7},
	{0, 0, R_028664_SPI_PS_INPUT_CNTL_8},
	{0, 0, R_028668_SPI_PS_INPUT_CNTL_9},
	{0, 0, R_02866C_SPI_PS_INPUT_CNTL_10},
	{0, 0, R_028670_SPI_PS_INPUT_CNTL_11},
	{0, 0, R_028674_SPI_PS_INPUT_CNTL_12},
	{0, 0, R_028678_SPI_PS_INPUT_CNTL_13},
	{0, 0, R_02867C_SPI_PS_INPUT_CNTL_14},
	{0, 0, R_028680_SPI_PS_INPUT_CNTL_15},
	{0, 0, R_028684_SPI_PS_INPUT_CNTL_16},
	{0, 0, R_028688_SPI_PS_INPUT_CNTL_17},
	{0, 0, R_02868C_SPI_PS_INPUT_CNTL_18},
	{0, 0, R_028690_SPI_PS_INPUT_CNTL_19},
	{0, 0, R_028694_SPI_PS_INPUT_CNTL_20},
	{0, 0, R_028698_SPI_PS_INPUT_CNTL_21},
	{0, 0, R_02869C_SPI_PS_INPUT_CNTL_22},
	{0, 0, R_0286A0_SPI_PS_INPUT_CNTL_23},
	{0, 0, R_0286A4_SPI_PS_INPUT_CNTL_24},
	{0, 0, R_0286A8_SPI_PS_INPUT_CNTL_25},
	{0, 0, R_0286AC_SPI_PS_INPUT_CNTL_26},
	{0, 0, R_0286B0_SPI_PS_INPUT_CNTL_27},
	{0, 0, R_0286B4_SPI_PS_INPUT_CNTL_28},
	{0, 0, R_0286B8_SPI_PS_INPUT_CNTL_29},
	{0, 0, R_0286BC_SPI_PS_INPUT_CNTL_30},
	{0, 0, R_0286C0_SPI_PS_INPUT_CNTL_31},
	{0, 0, R_0286CC_SPI_PS_IN_CONTROL_0},
	{0, 0, R_0286D0_SPI_PS_IN_CONTROL_1},
	{0, 0, R_0286D8_SPI_INPUT_Z},
	{1, S_0085F0_SH_ACTION_ENA(1), R_028840_SQ_PGM_START_PS},
	{0, 0, R_028850_SQ_PGM_RESOURCES_PS},
	{0, 0, R_028854_SQ_PGM_EXPORTS_PS},
	{0, 0, R_0288CC_SQ_PGM_CF_OFFSET_PS},
	{0, 0, R_008958_VGT_PRIMITIVE_TYPE},
	{0, 0, R_028400_VGT_MAX_VTX_INDX},
	{0, 0, R_028404_VGT_MIN_VTX_INDX},
	{0, 0, R_028408_VGT_INDX_OFFSET},
	{0, 0, R_02840C_VGT_MULTI_PRIM_IB_RESET_INDX},
	{0, 0, R_028A84_VGT_PRIMITIVEID_EN},
	{0, 0, R_028A94_VGT_MULTI_PRIM_IB_RESET_EN},
	{0, 0, R_028AA0_VGT_INSTANCE_STEP_RATE_0},
	{0, 0, R_028AA4_VGT_INSTANCE_STEP_RATE_1},
};

/* SHADER CONSTANT R600/R700 */
static int r600_state_constant_init(struct r600_context *ctx, u32 offset)
{
	struct r600_reg r600_shader_constant[] = {
		{0, 0, R_030000_SQ_ALU_CONSTANT0_0},
		{0, 0, R_030004_SQ_ALU_CONSTANT1_0},
		{0, 0, R_030008_SQ_ALU_CONSTANT2_0},
		{0, 0, R_03000C_SQ_ALU_CONSTANT3_0},
	};
	unsigned nreg = sizeof(r600_shader_constant)/sizeof(struct r600_reg);

	for (int i = 0; i < nreg; i++) {
		r600_shader_constant[i].offset += offset;
	}
	return r600_context_add_block(ctx, r600_shader_constant, nreg);
}

/* SHADER RESOURCE R600/R700 */
static int r600_state_resource_init(struct r600_context *ctx, u32 offset)
{
	struct r600_reg r600_shader_resource[] = {
		{0, 0, R_038000_RESOURCE0_WORD0},
		{0, 0, R_038004_RESOURCE0_WORD1},
		{1, S_0085F0_TC_ACTION_ENA(1) | S_0085F0_VC_ACTION_ENA(1), R_038008_RESOURCE0_WORD2},
		{1, S_0085F0_TC_ACTION_ENA(1) | S_0085F0_VC_ACTION_ENA(1), R_03800C_RESOURCE0_WORD3},
		{0, 0, R_038010_RESOURCE0_WORD4},
		{0, 0, R_038014_RESOURCE0_WORD5},
		{0, 0, R_038018_RESOURCE0_WORD6},
	};
	unsigned nreg = sizeof(r600_shader_resource)/sizeof(struct r600_reg);

	for (int i = 0; i < nreg; i++) {
		r600_shader_resource[i].offset += offset;
	}
	return r600_context_add_block(ctx, r600_shader_resource, nreg);
}

/* SHADER SAMPLER R600/R700 */
static int r600_state_sampler_init(struct r600_context *ctx, u32 offset)
{
	struct r600_reg r600_shader_sampler[] = {
		{0, 0, R_03C000_SQ_TEX_SAMPLER_WORD0_0},
		{0, 0, R_03C004_SQ_TEX_SAMPLER_WORD1_0},
		{0, 0, R_03C008_SQ_TEX_SAMPLER_WORD2_0},
	};
	unsigned nreg = sizeof(r600_shader_sampler)/sizeof(struct r600_reg);

	for (int i = 0; i < nreg; i++) {
		r600_shader_sampler[i].offset += offset;
	}
	return r600_context_add_block(ctx, r600_shader_sampler, nreg);
}

/* SHADER SAMPLER BORDER R600/R700 */
static int r600_state_sampler_border_init(struct r600_context *ctx, u32 offset)
{
	struct r600_reg r600_shader_sampler_border[] = {
		{0, 0, R_00A400_TD_PS_SAMPLER0_BORDER_RED},
		{0, 0, R_00A404_TD_PS_SAMPLER0_BORDER_GREEN},
		{0, 0, R_00A408_TD_PS_SAMPLER0_BORDER_BLUE},
		{0, 0, R_00A40C_TD_PS_SAMPLER0_BORDER_ALPHA},
	};
	unsigned nreg = sizeof(r600_shader_sampler_border)/sizeof(struct r600_reg);

	for (int i = 0; i < nreg; i++) {
		r600_shader_sampler_border[i].offset += offset;
	}
	return r600_context_add_block(ctx, r600_shader_sampler_border, nreg);
}

/* initialize */
void r600_context_fini(struct r600_context *ctx)
{
	for (int i = 0; i < ctx->ngroups; i++) {
		r600_group_fini(&ctx->groups[i]);
	}
	free(ctx->reloc);
	free(ctx->pm4);
	memset(ctx, 0, sizeof(struct r600_context));
}

int r600_context_init(struct r600_context *ctx, struct radeon *radeon)
{
	int r;

	memset(ctx, 0, sizeof(struct r600_context));
	ctx->radeon = radeon;
	/* initialize groups */
	r = r600_group_init(&ctx->groups[R600_GROUP_CONFIG], R600_CONFIG_REG_OFFSET, R600_CONFIG_REG_END);
	if (r) {
		goto out_err;
	}
	r = r600_group_init(&ctx->groups[R600_GROUP_CTL_CONST], R600_CTL_CONST_OFFSET, R600_CTL_CONST_END);
	if (r) {
		goto out_err;
	}
	r = r600_group_init(&ctx->groups[R600_GROUP_LOOP_CONST], R600_LOOP_CONST_OFFSET, R600_LOOP_CONST_END);
	if (r) {
		goto out_err;
	}
	r = r600_group_init(&ctx->groups[R600_GROUP_BOOL_CONST], R600_BOOL_CONST_OFFSET, R600_BOOL_CONST_END);
	if (r) {
		goto out_err;
	}
	r = r600_group_init(&ctx->groups[R600_GROUP_SAMPLER], R600_SAMPLER_OFFSET, R600_SAMPLER_END);
	if (r) {
		goto out_err;
	}
	r = r600_group_init(&ctx->groups[R600_GROUP_RESOURCE], R600_RESOURCE_OFFSET, R600_RESOURCE_END);
	if (r) {
		goto out_err;
	}
	r = r600_group_init(&ctx->groups[R600_GROUP_ALU_CONST], R600_ALU_CONST_OFFSET, R600_ALU_CONST_END);
	if (r) {
		goto out_err;
	}
	r = r600_group_init(&ctx->groups[R600_GROUP_CONTEXT], R600_CONTEXT_REG_OFFSET, R600_CONTEXT_REG_END);
	if (r) {
		goto out_err;
	}
	ctx->ngroups = R600_NGROUPS;

	/* add blocks */
	r = r600_context_add_block(ctx, r600_reg_list, sizeof(r600_reg_list)/sizeof(struct r600_reg));
	if (r)
		goto out_err;

	/* PS SAMPLER BORDER */
	for (int j = 0, offset = 0; j < 18; j++, offset += 0x10) {
		r = r600_state_sampler_border_init(ctx, offset);
		if (r)
			goto out_err;
	}

	/* VS SAMPLER BORDER */
	for (int j = 0, offset = 0x200; j < 18; j++, offset += 0x10) {
		r = r600_state_sampler_border_init(ctx, offset);
		if (r)
			goto out_err;
	}
	/* PS SAMPLER */
	for (int j = 0, offset = 0; j < 18; j++, offset += 0xC) {
		r = r600_state_sampler_init(ctx, offset);
		if (r)
			goto out_err;
	}
	/* VS SAMPLER */
	for (int j = 0, offset = 0xD8; j < 18; j++, offset += 0xC) {
		r = r600_state_sampler_init(ctx, offset);
		if (r)
			goto out_err;
	}
	/* PS RESOURCE */
	for (int j = 0, offset = 0; j < 160; j++, offset += 0x1C) {
		r = r600_state_resource_init(ctx, offset);
		if (r)
			goto out_err;
	}
	/* VS RESOURCE */
	for (int j = 0, offset = 0x1180; j < 160; j++, offset += 0x1C) {
		r = r600_state_resource_init(ctx, offset);
		if (r)
			goto out_err;
	}
	/* PS CONSTANT */
	for (int j = 0, offset = 0; j < 256; j++, offset += 0x10) {
		r = r600_state_constant_init(ctx, offset);
		if (r)
			goto out_err;
	}
	/* VS CONSTANT */
	for (int j = 0, offset = 0x1000; j < 256; j++, offset += 0x10) {
		r = r600_state_constant_init(ctx, offset);
		if (r)
			goto out_err;
	}

	/* allocate cs variables */
	ctx->nreloc = RADEON_CTX_MAX_PM4;
	ctx->reloc = calloc(ctx->nreloc, sizeof(struct r600_reloc));
	if (ctx->reloc == NULL) {
		r = -ENOMEM;
		goto out_err;
	}
	ctx->bo = calloc(ctx->nreloc, sizeof(void *));
	if (ctx->bo == NULL) {
		r = -ENOMEM;
		goto out_err;
	}
	ctx->pm4_ndwords = RADEON_CTX_MAX_PM4;
	ctx->pm4 = calloc(ctx->pm4_ndwords, 4);
	if (ctx->pm4 == NULL) {
		r = -ENOMEM;
		goto out_err;
	}
	return 0;
out_err:
	r600_context_fini(ctx);
	return r;
}

static void r600_context_bo_reloc(struct r600_context *ctx, u32 *pm4, struct radeon_ws_bo *bo)
{
	int i, reloc_id;
	unsigned handle = radeon_ws_bo_get_handle(bo);

	assert(bo != NULL);
	for (i = 0, reloc_id = -1; i < ctx->creloc; i++) {
		if (ctx->reloc[i].handle == handle) {
			reloc_id = i * sizeof(struct r600_reloc) / 4;
			/* set PKT3 to point to proper reloc */
			*pm4 = reloc_id;
		}
	}
	if (reloc_id == -1) {
		/* add new relocation */
		if (ctx->creloc >= ctx->nreloc) {
			r600_context_flush(ctx);
		}
		reloc_id = ctx->creloc * sizeof(struct r600_reloc) / 4;
		ctx->reloc[ctx->creloc].handle = handle;
		ctx->reloc[ctx->creloc].read_domain = RADEON_GEM_DOMAIN_GTT;
		ctx->reloc[ctx->creloc].write_domain = RADEON_GEM_DOMAIN_GTT;
		ctx->reloc[ctx->creloc].flags = 0;
		radeon_ws_bo_reference(ctx->radeon, &ctx->bo[ctx->creloc], bo);
		ctx->creloc++;
		/* set PKT3 to point to proper reloc */
		*pm4 = reloc_id;
	}
}

void r600_context_pipe_state_set(struct r600_context *ctx, struct r600_pipe_state *state)
{
	struct r600_group *group;
	struct r600_group_block *block;

	for (int i = 0; i < state->nregs; i++) {
		unsigned id;
		group = &ctx->groups[state->regs[i].group_id];
		id = group->offset_block_id[(state->regs[i].offset - group->start_offset) >> 2];
		block = &group->blocks[id];
		id = (state->regs[i].offset - block->start_offset) >> 2;
		block->pm4[id] &= ~state->regs[i].mask;
		block->pm4[id] |= state->regs[i].value;
		if (block->pm4_bo_index[id]) {
			/* find relocation */
			id = block->pm4_bo_index[id];
			radeon_ws_bo_reference(ctx->radeon, &block->reloc[id].bo, state->regs[i].bo);
			for (int j = 0; j < block->reloc[id].nreloc; j++) {
				r600_context_bo_reloc(ctx, &block->pm4[block->reloc[id].bo_pm4_index[j]],
							block->reloc[id].bo);
			}
		}
		block->status |= R600_BLOCK_STATUS_ENABLED;
		block->status |= R600_BLOCK_STATUS_DIRTY;
		ctx->pm4_dirty_cdwords += 2 + block->pm4_ndwords;
	}
}

static inline void r600_context_pipe_state_set_resource(struct r600_context *ctx, struct r600_pipe_state *state, unsigned offset)
{
	struct r600_group_block *block;
	unsigned id;

	offset -= ctx->groups[R600_GROUP_RESOURCE].start_offset;
	id = ctx->groups[R600_GROUP_RESOURCE].offset_block_id[offset >> 2];
	block = &ctx->groups[R600_GROUP_RESOURCE].blocks[id];
	block->pm4[0] = state->regs[0].value;
	block->pm4[1] = state->regs[1].value;
	block->pm4[2] = state->regs[2].value;
	block->pm4[3] = state->regs[3].value;
	block->pm4[4] = state->regs[4].value;
	block->pm4[5] = state->regs[5].value;
	block->pm4[6] = state->regs[6].value;
	radeon_ws_bo_reference(ctx->radeon, &block->reloc[1].bo, block->reloc[1].bo);
	radeon_ws_bo_reference(ctx->radeon , &block->reloc[2].bo, block->reloc[2].bo);
	if (state->regs[0].bo) {
		/* VERTEX RESOURCE, we preted there is 2 bo to relocate so
		 * we have single case btw VERTEX & TEXTURE resource
		 */
		radeon_ws_bo_reference(ctx->radeon, &block->reloc[1].bo, state->regs[0].bo);
		radeon_ws_bo_reference(ctx->radeon, &block->reloc[2].bo, state->regs[0].bo);
	} else {
		/* TEXTURE RESOURCE */
		radeon_ws_bo_reference(ctx->radeon, &block->reloc[1].bo, state->regs[2].bo);
		radeon_ws_bo_reference(ctx->radeon, &block->reloc[2].bo, state->regs[3].bo);
	}
	r600_context_bo_reloc(ctx, &block->pm4[block->reloc[1].bo_pm4_index[0]], block->reloc[1].bo);
	r600_context_bo_reloc(ctx, &block->pm4[block->reloc[2].bo_pm4_index[0]], block->reloc[2].bo);
	block->status |= R600_BLOCK_STATUS_ENABLED;
	block->status |= R600_BLOCK_STATUS_DIRTY;
	ctx->pm4_dirty_cdwords += 2 + block->pm4_ndwords;
}

void r600_context_pipe_state_set_ps_resource(struct r600_context *ctx, struct r600_pipe_state *state, unsigned rid)
{
	unsigned offset = R_038000_SQ_TEX_RESOURCE_WORD0_0 + 0x1C * rid;

	r600_context_pipe_state_set_resource(ctx, state, offset);
}

void r600_context_pipe_state_set_vs_resource(struct r600_context *ctx, struct r600_pipe_state *state, unsigned rid)
{
	unsigned offset = R_038000_SQ_TEX_RESOURCE_WORD0_0 + 0x1180 + 0x1C * rid;

	r600_context_pipe_state_set_resource(ctx, state, offset);
}

static inline void r600_context_pipe_state_set_sampler(struct r600_context *ctx, struct r600_pipe_state *state, unsigned offset)
{
	struct r600_group_block *block;
	unsigned id;

	offset -= ctx->groups[R600_GROUP_SAMPLER].start_offset;
	id = ctx->groups[R600_GROUP_SAMPLER].offset_block_id[offset >> 2];
	block = &ctx->groups[R600_GROUP_SAMPLER].blocks[id];
	block->pm4[0] = state->regs[0].value;
	block->pm4[1] = state->regs[1].value;
	block->pm4[2] = state->regs[2].value;
	block->status |= R600_BLOCK_STATUS_ENABLED;
	block->status |= R600_BLOCK_STATUS_DIRTY;
	ctx->pm4_dirty_cdwords += 2 + block->pm4_ndwords;
}

static inline void r600_context_pipe_state_set_sampler_border(struct r600_context *ctx, struct r600_pipe_state *state, unsigned offset)
{
	struct r600_group_block *block;
	unsigned id;

	offset -= ctx->groups[R600_GROUP_CONFIG].start_offset;
	id = ctx->groups[R600_GROUP_CONFIG].offset_block_id[offset >> 2];
	block = &ctx->groups[R600_GROUP_CONFIG].blocks[id];
	block->pm4[0] = state->regs[3].value;
	block->pm4[1] = state->regs[4].value;
	block->pm4[2] = state->regs[5].value;
	block->pm4[3] = state->regs[6].value;
	block->status |= R600_BLOCK_STATUS_ENABLED;
	block->status |= R600_BLOCK_STATUS_DIRTY;
	ctx->pm4_dirty_cdwords += 2 + block->pm4_ndwords;
}

void r600_context_pipe_state_set_ps_sampler(struct r600_context *ctx, struct r600_pipe_state *state, unsigned id)
{
	unsigned offset;

	offset = 0x0003C000 + id * 0xc;
	r600_context_pipe_state_set_sampler(ctx, state, offset);
	if (state->nregs > 3) {
		offset = 0x0000A400 + id * 0x10;
		r600_context_pipe_state_set_sampler_border(ctx, state, offset);
	}
}

void r600_context_pipe_state_set_vs_sampler(struct r600_context *ctx, struct r600_pipe_state *state, unsigned id)
{
	unsigned offset;

	offset = 0x0003C0D8 + id * 0xc;
	r600_context_pipe_state_set_sampler(ctx, state, offset);
	if (state->nregs > 3) {
		offset = 0x0000A600 + id * 0x10;
		r600_context_pipe_state_set_sampler_border(ctx, state, offset);
	}
}

static inline void r600_context_group_emit_dirty(struct r600_context *ctx, struct r600_group *group, unsigned opcode)
{
	for (int i = 0; i < group->nblocks; i++) {
		struct r600_group_block *block = &group->blocks[i];
		if (block->status & R600_BLOCK_STATUS_DIRTY) {
			ctx->pm4[ctx->pm4_cdwords++] = PKT3(opcode, block->nreg);
			ctx->pm4[ctx->pm4_cdwords++] = (block->start_offset - group->start_offset) >> 2;
			memcpy(&ctx->pm4[ctx->pm4_cdwords], block->pm4, block->pm4_ndwords * 4);
			ctx->pm4_cdwords += block->pm4_ndwords;
			block->status ^= R600_BLOCK_STATUS_DIRTY;
		}
	}
}

void r600_context_draw(struct r600_context *ctx, const struct r600_draw *draw)
{
	unsigned ndwords = 9;

	if (draw->indices) {
		ndwords = 13;
		/* make sure there is enough relocation space before scheduling draw */
		if (ctx->creloc >= (ctx->nreloc - 1)) {
			r600_context_flush(ctx);
		}
	}
	if ((ctx->pm4_dirty_cdwords + ndwords + ctx->pm4_cdwords) > ctx->pm4_ndwords) {
		/* need to flush */
		r600_context_flush(ctx);
	}
	/* at that point everythings is flushed and ctx->pm4_cdwords = 0 */
	if ((ctx->pm4_dirty_cdwords + ndwords) > ctx->pm4_ndwords) {
		R600_ERR("context is too big to be scheduled\n");
		return;
	}
	/* Ok we enough room to copy packet */
	r600_context_group_emit_dirty(ctx, &ctx->groups[R600_GROUP_CONFIG], PKT3_SET_CONFIG_REG);
	r600_context_group_emit_dirty(ctx, &ctx->groups[R600_GROUP_CONTEXT], PKT3_SET_CONTEXT_REG);
	r600_context_group_emit_dirty(ctx, &ctx->groups[R600_GROUP_ALU_CONST], PKT3_SET_ALU_CONST);
	r600_context_group_emit_dirty(ctx, &ctx->groups[R600_GROUP_SAMPLER], PKT3_SET_SAMPLER);
	r600_context_group_emit_dirty(ctx, &ctx->groups[R600_GROUP_RESOURCE], PKT3_SET_RESOURCE);
	/* draw packet */
	ctx->pm4[ctx->pm4_cdwords++] = PKT3(PKT3_INDEX_TYPE, 0);
	ctx->pm4[ctx->pm4_cdwords++] = draw->vgt_index_type;
	ctx->pm4[ctx->pm4_cdwords++] = PKT3(PKT3_NUM_INSTANCES, 0);
	ctx->pm4[ctx->pm4_cdwords++] = draw->vgt_num_instances;
	if (draw->indices) {
		ctx->pm4[ctx->pm4_cdwords++] = PKT3(PKT3_DRAW_INDEX, 3);
		ctx->pm4[ctx->pm4_cdwords++] = draw->indices_bo_offset;
		ctx->pm4[ctx->pm4_cdwords++] = 0;
		ctx->pm4[ctx->pm4_cdwords++] = draw->vgt_num_indices;
		ctx->pm4[ctx->pm4_cdwords++] = draw->vgt_draw_initiator;
		ctx->pm4[ctx->pm4_cdwords++] = PKT3(PKT3_NOP, 0);
		ctx->pm4[ctx->pm4_cdwords++] = 0;
		r600_context_bo_reloc(ctx, &ctx->pm4[ctx->pm4_cdwords - 1], draw->indices);
	} else {
		ctx->pm4[ctx->pm4_cdwords++] = PKT3(PKT3_DRAW_INDEX_AUTO, 1);
		ctx->pm4[ctx->pm4_cdwords++] = draw->vgt_num_indices;
		ctx->pm4[ctx->pm4_cdwords++] = draw->vgt_draw_initiator;
	}
	ctx->pm4[ctx->pm4_cdwords++] = PKT3(PKT3_EVENT_WRITE, 0);
	ctx->pm4[ctx->pm4_cdwords++] = EVENT_TYPE_CACHE_FLUSH_AND_INV_EVENT;
}

void r600_context_flush(struct r600_context *ctx)
{
	struct drm_radeon_cs drmib;
	struct drm_radeon_cs_chunk chunks[2];
	uint64_t chunk_array[2];
	struct r600_group_block *block;
	int r;

	if (!ctx->pm4_cdwords)
		return;

#if 1
	/* emit cs */
	drmib.num_chunks = 2;
	drmib.chunks = (uint64_t)(uintptr_t)chunk_array;
	chunks[0].chunk_id = RADEON_CHUNK_ID_IB;
	chunks[0].length_dw = ctx->pm4_cdwords;
	chunks[0].chunk_data = (uint64_t)(uintptr_t)ctx->pm4;
	chunks[1].chunk_id = RADEON_CHUNK_ID_RELOCS;
	chunks[1].length_dw = ctx->creloc * sizeof(struct r600_reloc) / 4;
	chunks[1].chunk_data = (uint64_t)(uintptr_t)ctx->reloc;
	chunk_array[0] = (uint64_t)(uintptr_t)&chunks[0];
	chunk_array[1] = (uint64_t)(uintptr_t)&chunks[1];
	r = drmCommandWriteRead(ctx->radeon->fd, DRM_RADEON_CS, &drmib,
				sizeof(struct drm_radeon_cs));
#endif
	/* restart */
	for (int i = 0; i < ctx->creloc; i++) {
		radeon_ws_bo_reference(ctx->radeon, &ctx->bo[i], NULL);
	}
	ctx->creloc = 0;
	ctx->pm4_dirty_cdwords = 0;
	ctx->pm4_cdwords = 0;
	for (int i = 0; i < ctx->ngroups; i++) {
		for (int j = 0; j < ctx->groups[i].nblocks; j++) {
			/* mark enabled block as dirty */
			block = &ctx->groups[i].blocks[j];
			if (block->status & R600_BLOCK_STATUS_ENABLED) {
				ctx->pm4_dirty_cdwords += 2 + block->pm4_ndwords;
				block->status |= R600_BLOCK_STATUS_DIRTY;
				for (int k = 1; k <= block->nbo; k++) {
					for (int l = 0; l < block->reloc[k].nreloc; l++) {
						r600_context_bo_reloc(ctx,
							&block->pm4[block->reloc[k].bo_pm4_index[l]],
							block->reloc[k].bo);
					}
				}
			}
		}
	}
}

void r600_context_dump_bof(struct r600_context *ctx, const char *file)
{
	bof_t *bcs, *blob, *array, *bo, *size, *handle, *device_id, *root;
	unsigned i;

	root = device_id = bcs = blob = array = bo = size = handle = NULL;
	root = bof_object();
	if (root == NULL)
		goto out_err;
	device_id = bof_int32(ctx->radeon->device);
	if (device_id == NULL)
		goto out_err;
	if (bof_object_set(root, "device_id", device_id))
		goto out_err;
	bof_decref(device_id);
	device_id = NULL;
	/* dump relocs */
	blob = bof_blob(ctx->creloc * 16, ctx->reloc);
	if (blob == NULL)
		goto out_err;
	if (bof_object_set(root, "reloc", blob))
		goto out_err;
	bof_decref(blob);
	blob = NULL;
	/* dump cs */
	blob = bof_blob(ctx->pm4_cdwords * 4, ctx->pm4);
	if (blob == NULL)
		goto out_err;
	if (bof_object_set(root, "pm4", blob))
		goto out_err;
	bof_decref(blob);
	blob = NULL;
	/* dump bo */
	array = bof_array();
	if (array == NULL)
		goto out_err;
	for (i = 0; i < ctx->creloc; i++) {
		struct radeon_bo *rbo = radeon_bo_pb_get_bo(ctx->bo[i]->pb);
		bo = bof_object();
		if (bo == NULL)
			goto out_err;
		size = bof_int32(rbo->size);
		if (size == NULL)
			goto out_err;
		if (bof_object_set(bo, "size", size))
			goto out_err;
		bof_decref(size);
		size = NULL;
		handle = bof_int32(rbo->handle);
		if (handle == NULL)
			goto out_err;
		if (bof_object_set(bo, "handle", handle))
			goto out_err;
		bof_decref(handle);
		handle = NULL;
		radeon_bo_map(ctx->radeon, rbo);
		blob = bof_blob(rbo->size, rbo->data);
		radeon_bo_unmap(ctx->radeon, rbo);
		if (blob == NULL)
			goto out_err;
		if (bof_object_set(bo, "data", blob))
			goto out_err;
		bof_decref(blob);
		blob = NULL;
		if (bof_array_append(array, bo))
			goto out_err;
		bof_decref(bo);
		bo = NULL;
	}
	if (bof_object_set(root, "bo", array))
		goto out_err;
	bof_dump_file(root, file);
out_err:
	bof_decref(blob);
	bof_decref(array);
	bof_decref(bo);
	bof_decref(size);
	bof_decref(handle);
	bof_decref(device_id);
	bof_decref(root);
}