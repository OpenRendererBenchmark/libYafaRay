#pragma once
/****************************************************************************
 *      This is part of the libYafaRay package
 *
 *      This library is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU Lesser General Public
 *      License as published by the Free Software Foundation; either
 *      version 2.1 of the License, or (at your option) any later version.
 *
 *      This library is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *      Lesser General Public License for more details.
 *
 *      You should have received a copy of the GNU Lesser General Public
 *      License along with this library; if not, write to the Free Software
 *      Foundation,Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#ifndef YAFARAY_MATERIAL_ROUGH_GLASS_H
#define YAFARAY_MATERIAL_ROUGH_GLASS_H

#include "constants.h"
#include "material/material_node.h"
#include "common/scene.h"

BEGIN_YAFARAY

class RoughGlassMaterial: public NodeMaterial
{
	public:
		RoughGlassMaterial(float ior, Rgb filt_c, const Rgb &srcol, bool fake_s, float alpha, float disp_pow, Visibility e_visibility = NormalVisible);
		virtual void initBsdf(const RenderState &state, SurfacePoint &sp, unsigned int &bsdf_types) const;
		virtual Rgb sample(const RenderState &state, const SurfacePoint &sp, const Vec3 &wo, Vec3 &wi, Sample &s, float &w) const;
		virtual Rgb sample(const RenderState &state, const SurfacePoint &sp, const Vec3 &wo, Vec3 *const dir, Rgb &tcol, Sample &s, float *const w) const;
		virtual Rgb eval(const RenderState &state, const SurfacePoint &sp, const Vec3 &wo, const Vec3 &wi, Bsdf_t bsdfs, bool force_eval = false) const { return 0.f; }
		virtual float pdf(const RenderState &state, const SurfacePoint &sp, const Vec3 &wo, const Vec3 &wi, Bsdf_t bsdfs) const { return 0.f; }
		virtual bool isTransparent() const { return fake_shadow_; }
		virtual Rgb getTransparency(const RenderState &state, const SurfacePoint &sp, const Vec3 &wo) const;
		virtual float getAlpha(const RenderState &state, const SurfacePoint &sp, const Vec3 &wo) const;
		virtual float getMatIor() const;
		static Material *factory(ParamMap &, std::list< ParamMap > &, RenderEnvironment &);
		virtual Rgb getGlossyColor(const RenderState &state) const;
		virtual Rgb getTransColor(const RenderState &state) const;
		virtual Rgb getMirrorColor(const RenderState &state) const;

	protected:
		ShaderNode *bump_shader_ = nullptr;
		ShaderNode *mirror_color_shader_ = nullptr;
		ShaderNode *roughness_shader_ = nullptr;
		ShaderNode *ior_shader_ = nullptr;
		ShaderNode *filter_col_shader_ = nullptr;
		ShaderNode *wireframe_shader_ = nullptr;     //!< Shader node for wireframe shading (float)
		Rgb filter_color_, specular_reflection_color_;
		Rgb beer_sigma_a_;
		float ior_;
		float a_2_;
		float a_;
		bool absorb_ = false, disperse_ = false, fake_shadow_;
		float dispersion_power_;
		float cauchy_a_, cauchy_b_;
};

END_YAFARAY

#endif // YAFARAY_MATERIAL_ROUGH_GLASS_H
