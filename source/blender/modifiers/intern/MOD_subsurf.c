/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software  Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2005 by the Blender Foundation.
 * All rights reserved.
 *
 * Contributor(s): Daniel Dunbar
 *                 Ton Roosendaal,
 *                 Ben Batt,
 *                 Brecht Van Lommel,
 *                 Campbell Barton
 *
 * ***** END GPL LICENSE BLOCK *****
 *
 */

/** \file blender/modifiers/intern/MOD_subsurf.c
 *  \ingroup modifiers
 */


#include <stddef.h>

#include "DNA_scene_types.h"
#include "DNA_object_types.h"

#ifdef WITH_OPENSUBDIV
#  include "DNA_userdef_types.h"
#endif

#include "BLI_utildefines.h"


#include "BKE_cdderivedmesh.h"
#include "BKE_scene.h"
#include "BKE_subsurf.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "MOD_modifiertypes.h"

#include "intern/CCGSubSurf.h"

static void initData(ModifierData *md)
{
	SubsurfModifierData *smd = (SubsurfModifierData *) md;

	smd->levels = 1;
	smd->renderLevels = 2;
	smd->flags |= eSubsurfModifierFlag_SubsurfUv;
}

static void copyData(const ModifierData *md, ModifierData *target)
{
#if 0
	const SubsurfModifierData *smd = (const SubsurfModifierData *) md;
#endif
	SubsurfModifierData *tsmd = (SubsurfModifierData *) target;

	modifier_copyData_generic(md, target);

	tsmd->emCache = tsmd->mCache = NULL;
}

static void freeData(ModifierData *md)
{
	SubsurfModifierData *smd = (SubsurfModifierData *) md;

	if (smd->mCache) {
		ccgSubSurf_free(smd->mCache);
		smd->mCache = NULL;
	}
	if (smd->emCache) {
		ccgSubSurf_free(smd->emCache);
		smd->emCache = NULL;
	}
}

static bool isDisabled(const Scene *scene, ModifierData *md, int useRenderParams)
{
	SubsurfModifierData *smd = (SubsurfModifierData *) md;
	int levels = (useRenderParams) ? smd->renderLevels : smd->levels;

	return get_render_subsurf_level(&scene->r, levels, useRenderParams != 0) == 0;
}

static DerivedMesh *applyModifier(
        ModifierData *md, const ModifierEvalContext *ctx,
        DerivedMesh *derivedData)
{
	SubsurfModifierData *smd = (SubsurfModifierData *) md;
	struct Scene *scene = DEG_get_evaluated_scene(ctx->depsgraph);
	SubsurfFlags subsurf_flags = 0;
	DerivedMesh *result;
	const bool useRenderParams = (ctx->flag & MOD_APPLY_RENDER) != 0;
	const bool isFinalCalc = (ctx->flag & MOD_APPLY_USECACHE) != 0;

#ifdef WITH_OPENSUBDIV
	const bool allow_gpu = (ctx->flag & MOD_APPLY_ALLOW_GPU) != 0;
#endif
	bool do_cddm_convert = useRenderParams || !isFinalCalc;

	if (useRenderParams)
		subsurf_flags |= SUBSURF_USE_RENDER_PARAMS;
	if (isFinalCalc)
		subsurf_flags |= SUBSURF_IS_FINAL_CALC;
	if (ctx->object->mode & OB_MODE_EDIT)
		subsurf_flags |= SUBSURF_IN_EDIT_MODE;

#ifdef WITH_OPENSUBDIV
	/* TODO(sergey): Not entirely correct, modifiers on top of subsurf
	 * could be disabled.
	 */
	if (md->next == NULL &&
	    allow_gpu &&
	    do_cddm_convert == false &&
	    smd->use_opensubdiv)
	{
		if (U.opensubdiv_compute_type == USER_OPENSUBDIV_COMPUTE_NONE) {
			modifier_setError(md, "OpenSubdiv is disabled in User Preferences");
		}
		else if ((ctx->object->mode & (OB_MODE_VERTEX_PAINT | OB_MODE_WEIGHT_PAINT | OB_MODE_TEXTURE_PAINT)) != 0) {
			modifier_setError(md, "OpenSubdiv is not supported in paint modes");
		}
		else if ((DEG_get_eval_flags_for_id(ctx->depsgraph, &ctx->object->id) & DAG_EVAL_NEED_CPU) == 0) {
			subsurf_flags |= SUBSURF_USE_GPU_BACKEND;
			do_cddm_convert = false;
		}
		else {
			modifier_setError(md, "OpenSubdiv is disabled due to dependencies");
		}
	}
#endif

	result = subsurf_make_derived_from_derived(derivedData, smd, scene, NULL, subsurf_flags);
	result->cd_flag = derivedData->cd_flag;

	{
		DerivedMesh *cddm = CDDM_copy(result);
		result->release(result);
		result = cddm;
	}

#ifndef WITH_OPESUBDIV
	(void) do_cddm_convert;
#endif

	return result;
}

static DerivedMesh *applyModifierEM(
        ModifierData *md, const ModifierEvalContext *ctx,
        struct BMEditMesh *UNUSED(editData),
        DerivedMesh *derivedData)
{
	SubsurfModifierData *smd = (SubsurfModifierData *) md;
	struct Scene *scene = DEG_get_evaluated_scene(ctx->depsgraph);
	DerivedMesh *result;
	/* 'orco' using editmode flags would cause cache to be used twice in editbmesh_calc_modifiers */
	SubsurfFlags ss_flags = (ctx->flag & MOD_APPLY_ORCO) ? 0 : (SUBSURF_FOR_EDIT_MODE | SUBSURF_IN_EDIT_MODE);
#ifdef WITH_OPENSUBDIV
	const bool allow_gpu = (ctx->flag & MOD_APPLY_ALLOW_GPU) != 0;
	if (md->next == NULL && allow_gpu && smd->use_opensubdiv) {
		modifier_setError(md, "OpenSubdiv is not supported in edit mode");
	}
#endif

	result = subsurf_make_derived_from_derived(derivedData, smd, scene, NULL, ss_flags);

	return result;
}

static bool dependsOnNormals(ModifierData *md)
{
#ifdef WITH_OPENSUBDIV
	SubsurfModifierData *smd = (SubsurfModifierData *) md;
	if (smd->use_opensubdiv && md->next == NULL) {
		return true;
	}
#else
	UNUSED_VARS(md);
#endif
	return false;
}

ModifierTypeInfo modifierType_Subsurf = {
	/* name */              "Subsurf",
	/* structName */        "SubsurfModifierData",
	/* structSize */        sizeof(SubsurfModifierData),
	/* type */              eModifierTypeType_Constructive,
	/* flags */             eModifierTypeFlag_AcceptsMesh |
	                        eModifierTypeFlag_SupportsMapping |
	                        eModifierTypeFlag_SupportsEditmode |
	                        eModifierTypeFlag_EnableInEditmode |
	                        eModifierTypeFlag_AcceptsCVs,

	/* copyData */          copyData,

	/* deformVerts_DM */    NULL,
	/* deformMatrices_DM */ NULL,
	/* deformVertsEM_DM */  NULL,
	/* deformMatricesEM_DM*/NULL,
	/* applyModifier_DM */  applyModifier,
	/* applyModifierEM_DM */applyModifierEM,

	/* deformVerts */       NULL,
	/* deformMatrices */    NULL,
	/* deformVertsEM */     NULL,
	/* deformMatricesEM */  NULL,
	/* applyModifier */     NULL,
	/* applyModifierEM */   NULL,

	/* initData */          initData,
	/* requiredDataMask */  NULL,
	/* freeData */          freeData,
	/* isDisabled */        isDisabled,
	/* updateDepsgraph */   NULL,
	/* dependsOnTime */     NULL,
	/* dependsOnNormals */	dependsOnNormals,
	/* foreachObjectLink */ NULL,
	/* foreachIDLink */     NULL,
	/* foreachTexLink */    NULL,
};

