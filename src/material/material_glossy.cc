/****************************************************************************
 *      glossy_mat.cc: a glossy material based on Ashikhmin&Shirley's Paper
 *      This is part of the libYafaRay package
 *      Copyright (C) 2006  Mathias Wein
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

#include "material/material_glossy.h"
#include "shader/shader_node.h"
#include "common/environment.h"
#include "utility/util_sample.h"
#include "material/material_utils_microfacet.h"
#include "common/color_ramp.h"
#include "common/param.h"
#include "common/scene.h"

BEGIN_YAFARAY

GlossyMaterial::GlossyMaterial(const Rgb &col, const Rgb &dcol, float reflect, float diff, float expo, bool as_diff, Visibility e_visibility):
		gloss_color_(col), diff_color_(dcol), exponent_(expo), reflectivity_(reflect), diffuse_(diff), as_diffuse_(as_diff)
{
	visibility_ = e_visibility;
	bsdf_flags_ = BsdfNone;

	if(diff > 0)
	{
		bsdf_flags_ = BsdfDiffuse | BsdfReflect;
		with_diffuse_ = true;
	}

	oren_nayar_ = false;

	bsdf_flags_ |= as_diffuse_ ? (BsdfDiffuse | BsdfReflect) : (BsdfGlossy | BsdfReflect);

	visibility_ = e_visibility;
}

void GlossyMaterial::initBsdf(const RenderState &state, SurfacePoint &sp, Bsdf_t &bsdf_types) const
{
	MDatT *dat = (MDatT *)state.userdata_;
	dat->stack_ = (char *)state.userdata_ + sizeof(MDatT);
	NodeStack stack(dat->stack_);
	if(bump_shader_) evalBump(stack, state, sp, bump_shader_);

	auto end = all_viewindep_.end();
	for(auto iter = all_viewindep_.begin(); iter != end; ++iter)(*iter)->eval(stack, state, sp);
	bsdf_types = bsdf_flags_;
	dat->m_diffuse_ = diffuse_;
	dat->m_glossy_ = glossy_reflection_shader_ ? glossy_reflection_shader_->getScalar(stack) : reflectivity_;
	dat->p_diffuse_ = std::min(0.6f, 1.f - (dat->m_glossy_ / (dat->m_glossy_ + (1.f - dat->m_glossy_) * dat->m_diffuse_)));
}

void GlossyMaterial::initOrenNayar(double sigma)
{
	double sigma_2 = sigma * sigma;
	oren_a_ = 1.0 - 0.5 * (sigma_2 / (sigma_2 + 0.33));
	oren_b_ = 0.45 * sigma_2 / (sigma_2 + 0.09);
	oren_nayar_ = true;
}

float GlossyMaterial::orenNayar(const Vec3 &wi, const Vec3 &wo, const Vec3 &n, bool use_texture_sigma, double texture_sigma) const
{
	float cos_ti = std::max(-1.f, std::min(1.f, n * wi));
	float cos_to = std::max(-1.f, std::min(1.f, n * wo));
	float maxcos_f = 0.f;

	if(cos_ti < 0.9999f && cos_to < 0.9999f)
	{
		Vec3 v_1 = (wi - n * cos_ti).normalize();
		Vec3 v_2 = (wo - n * cos_to).normalize();
		maxcos_f = std::max(0.f, v_1 * v_2);
	}

	float sin_alpha, tan_beta;

	if(cos_to >= cos_ti)
	{
		sin_alpha = fSqrt__(1.f - cos_ti * cos_ti);
		tan_beta = fSqrt__(1.f - cos_to * cos_to) / ((cos_to == 0.f) ? 1e-8f : cos_to); // white (black on windows) dots fix for oren-nayar, could happen with bad normals
	}
	else
	{
		sin_alpha = fSqrt__(1.f - cos_to * cos_to);
		tan_beta = fSqrt__(1.f - cos_ti * cos_ti) / ((cos_ti == 0.f) ? 1e-8f : cos_ti); // white (black on windows) dots fix for oren-nayar, could happen with bad normals
	}

	if(use_texture_sigma)
	{
		double sigma_squared = texture_sigma * texture_sigma;
		double m_oren_nayar_texture_a = 1.0 - 0.5 * (sigma_squared / (sigma_squared + 0.33));
		double m_oren_nayar_texture_b = 0.45 * sigma_squared / (sigma_squared + 0.09);
		return std::min(1.f, std::max(0.f, (float)(m_oren_nayar_texture_a + m_oren_nayar_texture_b * maxcos_f * sin_alpha * tan_beta)));
	}
	else
	{
		return std::min(1.f, std::max(0.f, (float)(oren_a_ + oren_b_ * maxcos_f * sin_alpha * tan_beta)));
	}
}

Rgb GlossyMaterial::eval(const RenderState &state, const SurfacePoint &sp, const Vec3 &wo, const Vec3 &wi, Bsdf_t bsdfs, bool force_eval) const
{
	if(!force_eval)	//If the flag force_eval = true then the next line will be skipped, necessary for the Glossy Direct render pass
	{
		if(!(bsdfs & BsdfDiffuse) || ((sp.ng_ * wi) * (sp.ng_ * wo)) < 0.f) return Rgb(0.f);
	}

	MDatT *dat = (MDatT *)state.userdata_;
	Rgb col(0.f);
	bool diffuse_flag = bsdfs & BsdfDiffuse;

	NodeStack stack(dat->stack_);
	Vec3 n = FACE_FORWARD(sp.ng_, sp.n_, wo);

	float wi_n = std::fabs(wi * n);
	float wo_n = std::fabs(wo * n);


	if((as_diffuse_ && diffuse_flag) || (!as_diffuse_ && (bsdfs & BsdfGlossy)))
	{
		Vec3 h = (wo + wi).normalize(); // half-angle
		float cos_wi_h = std::max(0.f, wi * h);
		float glossy;

		if(anisotropic_)
		{
			Vec3 hs(h * sp.nu_, h * sp.nv_, h * n);
			glossy = asAnisoD__(hs, exp_u_, exp_v_) * schlickFresnel__(cos_wi_h, dat->m_glossy_) / AS_DIVISOR(cos_wi_h, wo_n, wi_n);
		}
		else
		{
			glossy = blinnD__(h * n, (exponent_shader_ ? exponent_shader_->getScalar(stack) : exponent_)) * schlickFresnel__(cos_wi_h, dat->m_glossy_) / AS_DIVISOR(cos_wi_h, wo_n, wi_n);

		}

		col = glossy * (glossy_shader_ ? glossy_shader_->getColor(stack) : gloss_color_);
	}

	if(with_diffuse_ && diffuse_flag)
	{
		Rgb add_col = dat->m_diffuse_ * (1.f - dat->m_glossy_) * (diffuse_shader_ ? diffuse_shader_->getColor(stack) : diff_color_);

		if(diffuse_reflection_shader_) add_col *= diffuse_reflection_shader_->getScalar(stack);

		if(oren_nayar_)
		{
			double texture_sigma = (sigma_oren_shader_ ? sigma_oren_shader_->getScalar(stack) : 0.f);
			bool use_texture_sigma = (sigma_oren_shader_ ? true : false);

			add_col *= orenNayar(wi, wo, n, use_texture_sigma, texture_sigma);
		}

		col += add_col;

		//diffuseReflect(wiN, woN, dat->mGlossy, dat->mDiffuse, (diffuseS ? diffuseS->getColor(stack) : diff_color)) * ((orenNayar)?OrenNayar(wi, wo, N):1.f);
	}

	float wire_frame_amount = (wireframe_shader_ ? wireframe_shader_->getScalar(stack) * wireframe_amount_ : wireframe_amount_);
	applyWireFrame(col, wire_frame_amount, sp);
	return col;
}


Rgb GlossyMaterial::sample(const RenderState &state, const SurfacePoint &sp, const Vec3 &wo, Vec3 &wi, Sample &s, float &w) const
{
	MDatT *dat = (MDatT *)state.userdata_;
	float cos_ng_wo = sp.ng_ * wo;
	float cos_ng_wi;
	Vec3 n = FACE_FORWARD(sp.ng_, sp.n_, wo);//(cos_Ng_wo < 0) ? -sp.N : sp.N;
	Vec3 Hs;
	s.pdf_ = 0.f;
	float wi_n = 0.f;
	float wo_n = std::fabs(wo * n);
	float cos_wo_h = 0.f;

	Rgb scolor(0.f);

	float s_1 = s.s_1_;
	float cur_p_diffuse = dat->p_diffuse_;
	bool use_glossy = as_diffuse_ ? (s.flags_ & BsdfDiffuse) : (s.flags_ & BsdfGlossy);
	bool use_diffuse = with_diffuse_ && (s.flags_ & BsdfDiffuse);
	NodeStack stack(dat->stack_);
	float glossy = 0.f;

	if(use_diffuse)
	{
		float s_p_diffuse = use_glossy ? cur_p_diffuse : 1.f;
		if(s_1 < s_p_diffuse)
		{
			s_1 /= s_p_diffuse;
			wi = sampleCosHemisphere__(n, sp.nu_, sp.nv_, s_1, s.s_2_);

			cos_ng_wi = sp.ng_ * wi;

			if(cos_ng_wi * cos_ng_wo < 0.f)
			{
				float wire_frame_amount = (wireframe_shader_ ? wireframe_shader_->getScalar(stack) * wireframe_amount_ : wireframe_amount_);
				applyWireFrame(scolor, wire_frame_amount, sp);
				return scolor;
			}

			wi_n = std::fabs(wi * n);

			s.pdf_ = wi_n;

			if(use_glossy)
			{
				Vec3 h = (wi + wo).normalize();
				cos_wo_h = wo * h;
				float cos_wi_h = std::fabs(wi * h);
				float cos_n_h = n * h;
				if(anisotropic_)
				{
					Vec3 hs(h * sp.nu_, h * sp.nv_, cos_n_h);
					s.pdf_ = s.pdf_ * cur_p_diffuse + asAnisoPdf__(hs, cos_wo_h, exp_u_, exp_v_) * (1.f - cur_p_diffuse);
					glossy = asAnisoD__(hs, exp_u_, exp_v_) * schlickFresnel__(cos_wi_h, dat->m_glossy_) / AS_DIVISOR(cos_wi_h, wo_n, wi_n);
				}
				else
				{
					s.pdf_ = s.pdf_ * cur_p_diffuse + blinnPdf__(cos_n_h, cos_wo_h, (exponent_shader_ ? exponent_shader_->getScalar(stack) : exponent_)) * (1.f - cur_p_diffuse);
					glossy = blinnD__(cos_n_h, (exponent_shader_ ? exponent_shader_->getScalar(stack) : exponent_)) * schlickFresnel__(cos_wi_h, dat->m_glossy_) / AS_DIVISOR(cos_wi_h, wo_n, wi_n);
				}
			}
			s.sampled_flags_ = BsdfDiffuse | BsdfReflect;

			if(!(s.flags_ & BsdfReflect))
			{
				scolor = Rgb(0.f);
				float wire_frame_amount = (wireframe_shader_ ? wireframe_shader_->getScalar(stack) * wireframe_amount_ : wireframe_amount_);
				applyWireFrame(scolor, wire_frame_amount, sp);
				return scolor;
			}

			scolor = glossy * (glossy_shader_ ? glossy_shader_->getColor(stack) : gloss_color_);

			if(use_diffuse)
			{
				Rgb add_col = diffuseReflect__(wi_n, wo_n, dat->m_glossy_, dat->m_diffuse_, (diffuse_shader_ ? diffuse_shader_->getColor(stack) : diff_color_));

				if(diffuse_reflection_shader_) add_col *= diffuse_reflection_shader_->getScalar(stack);

				if(oren_nayar_)
				{
					double texture_sigma = (sigma_oren_shader_ ? sigma_oren_shader_->getScalar(stack) : 0.f);
					bool use_texture_sigma = (sigma_oren_shader_ ? true : false);

					add_col *= orenNayar(wi, wo, n, use_texture_sigma, texture_sigma);
				}
				scolor += add_col;
			}
			w = wi_n / (s.pdf_ * 0.99f + 0.01f);

			float wire_frame_amount = (wireframe_shader_ ? wireframe_shader_->getScalar(stack) * wireframe_amount_ : wireframe_amount_);
			applyWireFrame(scolor, wire_frame_amount, sp);
			return scolor;
		}
		s_1 -= cur_p_diffuse;
		s_1 /= (1.f - cur_p_diffuse);
	}

	if(use_glossy)
	{
		if(anisotropic_)
		{
			asAnisoSample__(Hs, s_1, s.s_2_, exp_u_, exp_v_);
			Vec3 h = Hs.x_ * sp.nu_ + Hs.y_ * sp.nv_ + Hs.z_ * n;
			cos_wo_h = wo * h;
			if(cos_wo_h < 0.f)
			{
				h.reflect(n);
				cos_wo_h = wo * h;
			}
			// Compute incident direction by reflecting wo about H
			wi = reflectDir__(h, wo);
			cos_ng_wi = sp.ng_ * wi;

			if(cos_ng_wo * cos_ng_wi < 0.f)
			{
				scolor = Rgb(0.f);
				float wire_frame_amount = (wireframe_shader_ ? wireframe_shader_->getScalar(stack) * wireframe_amount_ : wireframe_amount_);
				applyWireFrame(scolor, wire_frame_amount, sp);
				return scolor;
			}

			wi_n = std::fabs(wi * n);

			s.pdf_ = asAnisoPdf__(Hs, cos_wo_h, exp_u_, exp_v_);
			glossy = asAnisoD__(Hs, exp_u_, exp_v_) * schlickFresnel__(cos_wo_h, dat->m_glossy_) / AS_DIVISOR(cos_wo_h, wo_n, wi_n);
		}
		else
		{
			blinnSample__(Hs, s_1, s.s_2_, (exponent_shader_ ? exponent_shader_->getScalar(stack) : exponent_));
			Vec3 h = Hs.x_ * sp.nu_ + Hs.y_ * sp.nv_ + Hs.z_ * n;
			cos_wo_h = wo * h;
			if(cos_wo_h < 0.f)
			{
				h.reflect(n);
				cos_wo_h = wo * h;
			}
			// Compute incident direction by reflecting wo about H
			wi = reflectDir__(h, wo);
			cos_ng_wi = sp.ng_ * wi;

			if(cos_ng_wo * cos_ng_wi < 0.f)
			{
				scolor = Rgb(0.f);
				float wire_frame_amount = (wireframe_shader_ ? wireframe_shader_->getScalar(stack) * wireframe_amount_ : wireframe_amount_);
				applyWireFrame(scolor, wire_frame_amount, sp);
				return scolor;
			}

			wi_n = std::fabs(wi * n);
			float cos_hn = h * n;

			s.pdf_ = blinnPdf__(cos_hn, cos_wo_h, (exponent_shader_ ? exponent_shader_->getScalar(stack) : exponent_));
			glossy = blinnD__(cos_hn, (exponent_shader_ ? exponent_shader_->getScalar(stack) : exponent_)) * schlickFresnel__(cos_wo_h, dat->m_glossy_) / AS_DIVISOR(cos_wo_h, wo_n, wi_n);
		}

		scolor = glossy * (glossy_shader_ ? glossy_shader_->getColor(stack) : gloss_color_);
		s.sampled_flags_ = as_diffuse_ ? BsdfDiffuse | BsdfReflect : BsdfGlossy | BsdfReflect;
	}

	if(use_diffuse)
	{
		Rgb add_col = diffuseReflect__(wi_n, wo_n, dat->m_glossy_, dat->m_diffuse_, (diffuse_shader_ ? diffuse_shader_->getColor(stack) : diff_color_));

		if(diffuse_reflection_shader_) add_col *= diffuse_reflection_shader_->getScalar(stack);

		if(oren_nayar_)
		{
			double texture_sigma = (sigma_oren_shader_ ? sigma_oren_shader_->getScalar(stack) : 0.f);
			bool use_texture_sigma = (sigma_oren_shader_ ? true : false);

			add_col *= orenNayar(wi, wo, n, use_texture_sigma, texture_sigma);
		}
		s.pdf_ = wi_n * cur_p_diffuse + s.pdf_ * (1.f - cur_p_diffuse);
		scolor += add_col;
	}

	w = wi_n / (s.pdf_ * 0.99f + 0.01f);

	float wire_frame_amount = (wireframe_shader_ ? wireframe_shader_->getScalar(stack) * wireframe_amount_ : wireframe_amount_);
	applyWireFrame(scolor, wire_frame_amount, sp);
	return scolor;
}

float GlossyMaterial::pdf(const RenderState &state, const SurfacePoint &sp, const Vec3 &wo, const Vec3 &wi, Bsdf_t flags) const
{
	MDatT *dat = (MDatT *)state.userdata_;
	NodeStack stack(dat->stack_);

	if((sp.ng_ * wo) * (sp.ng_ * wi) < 0.f) return 0.f;
	Vec3 n = FACE_FORWARD(sp.ng_, sp.n_, wo);
	float pdf = 0.f;
	float cos_wo_h = 0.f;
	float cos_n_h = 0.f;

	float cur_p_diffuse = dat->p_diffuse_;
	bool use_glossy = as_diffuse_ ? (flags & BsdfDiffuse) : (flags & BsdfGlossy);
	bool use_diffuse = with_diffuse_ && (flags & BsdfDiffuse);

	if(use_diffuse)
	{
		pdf = std::fabs(wi * n);
		if(use_glossy)
		{
			Vec3 h = (wi + wo).normalize();
			cos_wo_h = wo * h;
			cos_n_h = n * h;
			if(anisotropic_)
			{
				Vec3 hs(h * sp.nu_, h * sp.nv_, cos_n_h);
				pdf = pdf * cur_p_diffuse + asAnisoPdf__(hs, cos_wo_h, exp_u_, exp_v_) * (1.f - cur_p_diffuse);
			}
			else pdf = pdf * cur_p_diffuse + blinnPdf__(cos_n_h, cos_wo_h, (exponent_shader_ ? exponent_shader_->getScalar(stack) : exponent_)) * (1.f - cur_p_diffuse);
		}
		return pdf;
	}

	if(use_glossy)
	{
		Vec3 h = (wi + wo).normalize();
		cos_wo_h = wo * h;
		cos_n_h = n * h;
		if(anisotropic_)
		{
			Vec3 hs(h * sp.nu_, h * sp.nv_, cos_n_h);
			pdf = asAnisoPdf__(hs, cos_wo_h, exp_u_, exp_v_);
		}
		else pdf = blinnPdf__(cos_n_h, cos_wo_h, (exponent_shader_ ? exponent_shader_->getScalar(stack) : exponent_));
	}
	return pdf;
}

Material *GlossyMaterial::factory(ParamMap &params, std::list< ParamMap > &param_list, RenderEnvironment &render)
{
	Rgb col(1.f), dcol(1.f);
	float refl = 1.f;
	float diff = 0.f;
	float exponent = 50.f; //wild guess, do sth better
	bool as_diff = true;
	bool aniso = false;
	std::string s_visibility = "normal";
	Visibility visibility = NormalVisible;
	int mat_pass_index = 0;
	bool receive_shadows = true;
	int additionaldepth = 0;
	float samplingfactor = 1.f;
	float wire_frame_amount = 0.f;           //!< Wireframe shading amount
	float wire_frame_thickness = 0.01f;      //!< Wireframe thickness
	float wire_frame_exponent = 0.f;         //!< Wireframe exponent (0.f = solid, 1.f=linearly gradual, etc)
	Rgb wire_frame_color = Rgb(1.f); //!< Wireframe shading color

	std::string name;
	params.getParam("color", col);
	params.getParam("diffuse_color", dcol);
	params.getParam("diffuse_reflect", diff);
	params.getParam("glossy_reflect", refl);
	params.getParam("as_diffuse", as_diff);
	params.getParam("exponent", exponent);
	params.getParam("anisotropic", aniso);

	params.getParam("receive_shadows", receive_shadows);
	params.getParam("visibility", s_visibility);
	params.getParam("mat_pass_index",   mat_pass_index);
	params.getParam("additionaldepth",   additionaldepth);
	params.getParam("samplingfactor",   samplingfactor);

	params.getParam("wireframe_amount", wire_frame_amount);
	params.getParam("wireframe_thickness", wire_frame_thickness);
	params.getParam("wireframe_exponent", wire_frame_exponent);
	params.getParam("wireframe_color", wire_frame_color);

	if(s_visibility == "normal") visibility = NormalVisible;
	else if(s_visibility == "no_shadows") visibility = VisibleNoShadows;
	else if(s_visibility == "shadow_only") visibility = InvisibleShadowsOnly;
	else if(s_visibility == "invisible") visibility = Invisible;
	else visibility = NormalVisible;

	GlossyMaterial *mat = new GlossyMaterial(col, dcol, refl, diff, exponent, as_diff, visibility);

	mat->setMaterialIndex(mat_pass_index);
	mat->receive_shadows_ = receive_shadows;
	mat->additional_depth_ = additionaldepth;

	mat->wireframe_amount_ = wire_frame_amount;
	mat->wireframe_thickness_ = wire_frame_thickness;
	mat->wireframe_exponent_ = wire_frame_exponent;
	mat->wireframe_color_ = wire_frame_color;

	mat->setSamplingFactor(samplingfactor);

	if(aniso)
	{
		float e_u = 50.0, e_v = 50.0;
		params.getParam("exp_u", e_u);
		params.getParam("exp_v", e_v);
		mat->anisotropic_ = true;
		mat->exp_u_ = e_u;
		mat->exp_v_ = e_v;
	}

	if(params.getParam("diffuse_brdf", name))
	{
		if(name == "Oren-Nayar")
		{
			double sigma = 0.1;
			params.getParam("sigma", sigma);
			mat->initOrenNayar(sigma);
		}
	}

	std::vector<ShaderNode *> roots;
	std::map<std::string, ShaderNode *> node_list;

	// Prepare our node list
	node_list["diffuse_shader"] = nullptr;
	node_list["glossy_shader"] = nullptr;
	node_list["glossy_reflect_shader"] = nullptr;
	node_list["bump_shader"] = nullptr;
	node_list["sigma_oren_shader"]   = nullptr;
	node_list["exponent_shader"] = nullptr;
	node_list["wireframe_shader"]    = nullptr;
	node_list["diffuse_refl_shader"] = nullptr;

	if(mat->loadNodes(param_list, render))
	{
		mat->parseNodes(params, roots, node_list);
	}
	else Y_ERROR << "Glossy: loadNodes() failed!" << YENDL;

	mat->diffuse_shader_ = node_list["diffuse_shader"];
	mat->glossy_shader_ = node_list["glossy_shader"];
	mat->glossy_reflection_shader_ = node_list["glossy_reflect_shader"];
	mat->bump_shader_ = node_list["bump_shader"];
	mat->sigma_oren_shader_ = node_list["sigma_oren_shader"];
	mat->exponent_shader_ = node_list["exponent_shader"];
	mat->wireframe_shader_    = node_list["wireframe_shader"];
	mat->diffuse_reflection_shader_  = node_list["diffuse_refl_shader"];

	// solve nodes order
	if(!roots.empty())
	{
		std::vector<ShaderNode *> color_nodes;

		mat->solveNodesOrder(roots);

		if(mat->diffuse_shader_) mat->getNodeList(mat->diffuse_shader_, color_nodes);
		if(mat->glossy_shader_) mat->getNodeList(mat->glossy_shader_, color_nodes);
		if(mat->glossy_reflection_shader_) mat->getNodeList(mat->glossy_reflection_shader_, color_nodes);
		if(mat->sigma_oren_shader_)    mat->getNodeList(mat->sigma_oren_shader_, color_nodes);
		if(mat->exponent_shader_) mat->getNodeList(mat->exponent_shader_, color_nodes);
		if(mat->wireframe_shader_)    mat->getNodeList(mat->wireframe_shader_, color_nodes);
		if(mat->diffuse_reflection_shader_)  mat->getNodeList(mat->diffuse_reflection_shader_, color_nodes);
		mat->filterNodes(color_nodes, mat->all_viewdep_, ViewDep);
		mat->filterNodes(color_nodes, mat->all_viewindep_, ViewIndep);
		if(mat->bump_shader_) mat->getNodeList(mat->bump_shader_, mat->bump_nodes_);
	}

	mat->req_mem_ = mat->req_node_mem_ + sizeof(MDatT);

	return mat;
}

Rgb GlossyMaterial::getDiffuseColor(const RenderState &state) const {
	MDatT *dat = (MDatT *)state.userdata_;
	NodeStack stack(dat->stack_);

	if(as_diffuse_ || with_diffuse_) return (diffuse_reflection_shader_ ? diffuse_reflection_shader_->getScalar(stack) : 1.f) * (diffuse_shader_ ? diffuse_shader_->getColor(stack) : diff_color_);
	else return Rgb(0.f);
}

Rgb GlossyMaterial::getGlossyColor(const RenderState &state) const {
	MDatT *dat = (MDatT *)state.userdata_;
	NodeStack stack(dat->stack_);

	return (glossy_reflection_shader_ ? glossy_reflection_shader_->getScalar(stack) : reflectivity_) * (glossy_shader_ ? glossy_shader_->getColor(stack) : gloss_color_);
}

END_YAFARAY
