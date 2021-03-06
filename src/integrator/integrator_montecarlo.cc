/****************************************************************************
 *      mcintegrator.h: A basic abstract integrator for MC sampling
 *      This is part of the libYafaRay package
 *      Copyright (C) 2010  Rodrigo Placencia (DarkTide)
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

#include "integrator/integrator_montecarlo.h"
#include "common/surface.h"
#include "common/logging.h"
#include "common/renderpasses.h"
#include "material/material.h"
#include "common/scene.h"
#include "volume/volume.h"
#include "common/session.h"
#include "light/light.h"
#include "common/scr_halton.h"
#include "common/spectrum.h"
#include "utility/util_mcqmc.h"
#include "common/imagefilm.h"
#include "common/monitor.h"
#include "common/photon.h"
#include "utility/util_sample.h"

#ifdef __clang__
#define inline  // aka inline removal
#endif

BEGIN_YAFARAY

#define ALL_BSDF_INTERSECT (BSDF_GLOSSY | BSDF_DIFFUSE | BSDF_DISPERSIVE | BSDF_REFLECT | BSDF_TRANSMIT);
#define LOFFS_DELTA 4567 //just some number to have different sequences per light...and it's a prime even...

inline Rgb MonteCarloIntegrator::estimateAllDirectLight(RenderState &state, const SurfacePoint &sp, const Vec3 &wo, ColorPasses &color_passes) const
{
	Rgb col;
	unsigned int loffs = 0;
	for(auto l = lights_.begin(); l != lights_.end(); ++l)
	{
		col += doLightEstimation(state, (*l), sp, wo, loffs, color_passes);
		loffs++;
	}

	color_passes.probeMult(PassIntShadow, 1.f / (float) loffs);

	return col;
}

inline Rgb MonteCarloIntegrator::estimateOneDirectLight(RenderState &state, const SurfacePoint &sp, Vec3 wo, int n, ColorPasses &color_passes) const
{
	int light_num = lights_.size();

	if(light_num == 0) return Rgb(0.f); //??? if you get this far the lights must be >= 1 but, what the hell... :)

	Halton hal_2(2);

	hal_2.setStart(image_film_->getBaseSamplingOffset() + correlative_sample_number_[state.thread_id_] - 1); //Probably with this change the parameter "n" is no longer necessary, but I will keep it just in case I have to revert back this change!
	int lnum = std::min((int)(hal_2.getNext() * (float)light_num), light_num - 1);

	++correlative_sample_number_[state.thread_id_];

	return doLightEstimation(state, lights_[lnum], sp, wo, lnum, color_passes) * light_num;
}

inline Rgb MonteCarloIntegrator::doLightEstimation(RenderState &state, Light *light, const SurfacePoint &sp, const Vec3 &wo, const unsigned int  &loffs, ColorPasses &color_passes) const
{
	Rgb col(0.f);
	Rgba col_shadow(0.f), col_shadow_obj_mask(0.f), col_shadow_mat_mask(0.f), col_diff_dir(0.f), col_diff_no_shadow(0.f), col_glossy_dir(0.f);
	bool shadowed;
	unsigned int l_offs = loffs * LOFFS_DELTA;
	const Material *material = sp.material_;
	Ray light_ray;
	light_ray.from_ = sp.p_;
	Rgb lcol(0.f), scol;
	float light_pdf;
	float mask_obj_index = 0.f, mask_mat_index = 0.f;

	bool cast_shadows = light->castShadows() && material->getReceiveShadows();

	// handle lights with delta distribution, e.g. point and directional lights
	if(light->diracLight())
	{
		if(light->illuminate(sp, lcol, light_ray))
		{
			// ...shadowed...
			if(scene_->shadow_bias_auto_) light_ray.tmin_ = scene_->shadow_bias_ * std::max(1.f, Vec3(sp.p_).length());
			else light_ray.tmin_ = scene_->shadow_bias_;

			if(cast_shadows) shadowed = (tr_shad_) ? scene_->isShadowed(state, light_ray, s_depth_, scol, mask_obj_index, mask_mat_index) : scene_->isShadowed(state, light_ray, mask_obj_index, mask_mat_index);
			else shadowed = false;

			float angle_light_normal = (material->isFlat() ? 1.f : std::fabs(sp.n_ * light_ray.dir_));	//If the material has the special attribute "isFlat()" then we will not multiply the surface reflection by the cosine of the angle between light and normal

			if(!shadowed || color_passes.enabled(PassIntDiffuseNoShadow))
			{
				if(!shadowed && color_passes.enabled(PassIntShadow)) col_shadow += Rgb(1.f);

				Rgb surf_col = material->eval(state, sp, wo, light_ray.dir_, BsdfAll);
				Rgb transmit_col = scene_->vol_integrator_->transmittance(state, light_ray);
				Rgba tmp_col_no_shadow = surf_col * lcol * angle_light_normal * transmit_col;
				if(tr_shad_ && cast_shadows) lcol *= scol;

				if(color_passes.enabled(PassIntDiffuse) || color_passes.enabled(PassIntDiffuseNoShadow))
				{
					Rgb tmp_col = material->eval(state, sp, wo, light_ray.dir_, BsdfDiffuse) * lcol * angle_light_normal * transmit_col;
					col_diff_no_shadow += tmp_col_no_shadow;
					if(!shadowed) col_diff_dir += tmp_col;
				}

				if(color_passes.enabled(PassIntGlossy) && state.raylevel_ == 0)
				{
					Rgb tmp_col = material->eval(state, sp, wo, light_ray.dir_, BsdfGlossy, true) * lcol * angle_light_normal * transmit_col;
					if(!shadowed) col_glossy_dir += tmp_col;
				}

				if(!shadowed) col += surf_col * lcol * angle_light_normal * transmit_col;
			}

			if(shadowed)
			{
				if((color_passes.enabled(PassIntMatIndexMaskShadow))
				        && mask_mat_index == color_passes.getPassMaskMatIndex()) col_shadow_mat_mask += Rgb(1.f);

				if((color_passes.enabled(PassIntObjIndexMaskShadow))
				        && mask_obj_index == color_passes.getPassMaskObjIndex()) col_shadow_obj_mask += Rgb(1.f);
			}
		}
		color_passes.probeAdd(PassIntShadow, col_shadow, state.raylevel_ == 0);
		color_passes.probeAdd(PassIntMatIndexMaskShadow, col_shadow_mat_mask, state.raylevel_ == 0);
		color_passes.probeAdd(PassIntObjIndexMaskShadow, col_shadow_obj_mask, state.raylevel_ == 0);
		color_passes.probeAdd(PassIntDiffuse, col_diff_dir, state.raylevel_ == 0);
		color_passes.probeAdd(PassIntDiffuseNoShadow, col_diff_no_shadow, state.raylevel_ == 0);
		color_passes.probeAdd(PassIntGlossy, col_glossy_dir, state.raylevel_ == 0);
		color_passes.probeAdd(PassIntDebugLightEstimationLightDirac, col, state.raylevel_ == 0);
	}
	else // area light and suchlike
	{
		Halton hal_2(2);
		Halton hal_3(3);
		int n = (int) ceilf(light->nSamples() * aa_light_sample_multiplier_);
		if(state.ray_division_ > 1) n = std::max(1, n / state.ray_division_);
		float inv_ns = 1.f / (float)n;
		unsigned int offs = n * state.pixel_sample_ + state.sampling_offs_ + l_offs;
		bool can_intersect = light->canIntersect();
		Rgb ccol(0.0);
		LSample ls;

		hal_2.setStart(offs - 1);
		hal_3.setStart(offs - 1);

		for(int i = 0; i < n; ++i)
		{
			// ...get sample val...
			ls.s_1_ = hal_2.getNext();
			ls.s_2_ = hal_3.getNext();

			if(light->illumSample(sp, ls, light_ray))
			{
				// ...shadowed...
				if(scene_->shadow_bias_auto_) light_ray.tmin_ = scene_->shadow_bias_ * std::max(1.f, Vec3(sp.p_).length());
				else light_ray.tmin_ = scene_->shadow_bias_;

				if(cast_shadows) shadowed = (tr_shad_) ? scene_->isShadowed(state, light_ray, s_depth_, scol, mask_obj_index, mask_mat_index) : scene_->isShadowed(state, light_ray, mask_obj_index, mask_mat_index);
				else shadowed = false;

				if((!shadowed && ls.pdf_ > 1e-6f) || color_passes.enabled(PassIntDiffuseNoShadow))
				{
					Rgb ls_col_no_shadow = ls.col_;
					if(tr_shad_ && cast_shadows) ls.col_ *= scol;
					Rgb transmit_col = scene_->vol_integrator_->transmittance(state, light_ray);
					ls.col_ *= transmit_col;
					Rgb surf_col = material->eval(state, sp, wo, light_ray.dir_, BsdfAll);

					if((!shadowed && ls.pdf_ > 1e-6f) && color_passes.enabled(PassIntShadow)) col_shadow += Rgb(1.f);

					float angle_light_normal = (material->isFlat() ? 1.f : std::fabs(sp.n_ * light_ray.dir_));	//If the material has the special attribute "isFlat()" then we will not multiply the surface reflection by the cosine of the angle between light and normal

					if(can_intersect)
					{
						float m_pdf = material->pdf(state, sp, wo, light_ray.dir_, BsdfGlossy | BsdfDiffuse | BsdfDispersive | BsdfReflect | BsdfTransmit);
						if(m_pdf > 1e-6f)
						{
							float l_2 = ls.pdf_ * ls.pdf_;
							float m_2 = m_pdf * m_pdf;
							float w = l_2 / (l_2 + m_2);

							if(color_passes.enabled(PassIntDiffuse) || color_passes.enabled(PassIntDiffuseNoShadow))
							{
								Rgb tmp_col_no_light_color = material->eval(state, sp, wo, light_ray.dir_, BsdfDiffuse) * angle_light_normal * w / ls.pdf_;
								col_diff_no_shadow += tmp_col_no_light_color * ls_col_no_shadow;
								if((!shadowed && ls.pdf_ > 1e-6f)) col_diff_dir += tmp_col_no_light_color * ls.col_;
							}
							if(color_passes.enabled(PassIntGlossy) && state.raylevel_ == 0)
							{
								Rgb tmp_col = material->eval(state, sp, wo, light_ray.dir_, BsdfGlossy, true) * ls.col_ * angle_light_normal * w / ls.pdf_;
								if((!shadowed && ls.pdf_ > 1e-6f)) col_glossy_dir += tmp_col;
							}

							if((!shadowed && ls.pdf_ > 1e-6f)) ccol += surf_col * ls.col_ * angle_light_normal * w / ls.pdf_;
						}
						else
						{
							if(color_passes.enabled(PassIntDiffuse) || color_passes.enabled(PassIntDiffuseNoShadow))
							{
								Rgb tmp_col_no_light_color = material->eval(state, sp, wo, light_ray.dir_, BsdfDiffuse) * angle_light_normal / ls.pdf_;
								col_diff_no_shadow += tmp_col_no_light_color * ls_col_no_shadow;
								if((!shadowed && ls.pdf_ > 1e-6f)) col_diff_dir += tmp_col_no_light_color * ls.col_;
							}

							if(color_passes.enabled(PassIntGlossy) && state.raylevel_ == 0)
							{
								Rgb tmp_col = material->eval(state, sp, wo, light_ray.dir_, BsdfGlossy, true) * ls.col_ * angle_light_normal / ls.pdf_;
								if((!shadowed && ls.pdf_ > 1e-6f)) col_glossy_dir += tmp_col;
							}

							if((!shadowed && ls.pdf_ > 1e-6f)) ccol += surf_col * ls.col_ * angle_light_normal / ls.pdf_;
						}
					}
					else
					{
						if(color_passes.enabled(PassIntDiffuse) || color_passes.enabled(PassIntDiffuseNoShadow))
						{
							Rgb tmp_col_no_light_color = material->eval(state, sp, wo, light_ray.dir_, BsdfDiffuse) * angle_light_normal / ls.pdf_;
							col_diff_no_shadow += tmp_col_no_light_color * ls_col_no_shadow;
							if((!shadowed && ls.pdf_ > 1e-6f)) col_diff_dir += tmp_col_no_light_color * ls.col_;
						}

						if(color_passes.enabled(PassIntGlossy) && state.raylevel_ == 0)
						{
							Rgb tmp_col = material->eval(state, sp, wo, light_ray.dir_, BsdfGlossy, true) * ls.col_ * angle_light_normal / ls.pdf_;
							if((!shadowed && ls.pdf_ > 1e-6f)) col_glossy_dir += tmp_col;
						}

						if((!shadowed && ls.pdf_ > 1e-6f)) ccol += surf_col * ls.col_ * angle_light_normal / ls.pdf_;
					}
				}

				if(shadowed || ls.pdf_ <= 1e-6f)
				{
					if((color_passes.enabled(PassIntMatIndexMaskShadow))
					        && mask_mat_index == color_passes.getPassMaskMatIndex()) col_shadow_mat_mask += Rgb(1.f);

					if((color_passes.enabled(PassIntObjIndexMaskShadow))
					        && mask_obj_index == color_passes.getPassMaskObjIndex()) col_shadow_obj_mask += Rgb(1.f);
				}
			}
		}

		col += ccol * inv_ns;

		color_passes.probeAdd(PassIntDebugLightEstimationLightSampling, ccol * inv_ns, state.raylevel_ == 0);

		color_passes.probeAdd(PassIntShadow, col_shadow * inv_ns, state.raylevel_ == 0);
		color_passes.probeAdd(PassIntMatIndexMaskShadow, col_shadow_mat_mask * inv_ns, state.raylevel_ == 0);
		color_passes.probeAdd(PassIntObjIndexMaskShadow, col_shadow_obj_mask * inv_ns, state.raylevel_ == 0);
		color_passes.probeAdd(PassIntDiffuse, col_diff_dir * inv_ns, state.raylevel_ == 0);
		color_passes.probeAdd(PassIntDiffuseNoShadow, col_diff_no_shadow * inv_ns, state.raylevel_ == 0);
		color_passes.probeAdd(PassIntGlossy, col_glossy_dir * inv_ns, state.raylevel_ == 0);

		if(can_intersect) // sample from BSDF to complete MIS
		{
			Rgb ccol_2(0.f);

			if(color_passes.enabled(PassIntDiffuse) || color_passes.enabled(PassIntDiffuseNoShadow))
			{
				col_diff_no_shadow = Rgba(0.f);
				col_diff_dir = Rgba(0.f);
			}

			if(color_passes.enabled(PassIntGlossy) && state.raylevel_ == 0) col_glossy_dir = Rgba(0.f);

			hal_2.setStart(offs - 1);
			hal_3.setStart(offs - 1);

			for(int i = 0; i < n; ++i)
			{
				Ray b_ray;
				if(scene_->ray_min_dist_auto_) b_ray.tmin_ = scene_->ray_min_dist_ * std::max(1.f, Vec3(sp.p_).length());
				else b_ray.tmin_ = scene_->ray_min_dist_;

				b_ray.from_ = sp.p_;

				float s_1 = hal_2.getNext();
				float s_2 = hal_3.getNext();
				float W = 0.f;

				Sample s(s_1, s_2, BsdfGlossy | BsdfDiffuse | BsdfDispersive | BsdfReflect | BsdfTransmit);
				Rgb surf_col = material->sample(state, sp, wo, b_ray.dir_, s, W);
				if(s.pdf_ > 1e-6f && light->intersect(b_ray, b_ray.tmax_, lcol, light_pdf))
				{
					if(cast_shadows) shadowed = (tr_shad_) ? scene_->isShadowed(state, b_ray, s_depth_, scol, mask_obj_index, mask_mat_index) : scene_->isShadowed(state, b_ray, mask_obj_index, mask_mat_index);
					else shadowed = false;

					if((!shadowed && light_pdf > 1e-6f) || color_passes.enabled(PassIntDiffuseNoShadow))
					{
						if(tr_shad_ && cast_shadows) lcol *= scol;
						Rgb transmit_col = scene_->vol_integrator_->transmittance(state, light_ray);
						lcol *= transmit_col;
						float l_pdf = 1.f / light_pdf;
						float l_2 = l_pdf * l_pdf;
						float m_2 = s.pdf_ * s.pdf_;
						float w = m_2 / (l_2 + m_2);

						if(color_passes.enabled(PassIntDiffuse) || color_passes.enabled(PassIntDiffuseNoShadow))
						{
							Rgb tmp_col = material->sample(state, sp, wo, b_ray.dir_, s, W) * lcol * w * W;
							col_diff_no_shadow += tmp_col;
							if((!shadowed && light_pdf > 1e-6f) && ((s.sampled_flags_ & BsdfDiffuse) == BsdfDiffuse)) col_diff_dir += tmp_col;
						}

						if(color_passes.enabled(PassIntGlossy) && state.raylevel_ == 0)
						{
							Rgb tmp_col = material->sample(state, sp, wo, b_ray.dir_, s, W) * lcol * w * W;
							if((!shadowed && light_pdf > 1e-6f) && ((s.sampled_flags_ & BsdfGlossy) == BsdfGlossy)) col_glossy_dir += tmp_col;
						}

						if((!shadowed && light_pdf > 1e-6f)) ccol_2 += surf_col * lcol * w * W;
					}
				}
			}

			col += ccol_2 * inv_ns;

			color_passes.probeAdd(PassIntDebugLightEstimationMatSampling, ccol_2 * inv_ns, state.raylevel_ == 0);
			color_passes.probeAdd(PassIntDiffuse, col_diff_dir * inv_ns, state.raylevel_ == 0);
			color_passes.probeAdd(PassIntDiffuseNoShadow, col_diff_no_shadow * inv_ns, state.raylevel_ == 0);
			color_passes.probeAdd(PassIntGlossy, col_glossy_dir * inv_ns, state.raylevel_ == 0);
		}
	}

	return col;
}

void MonteCarloIntegrator::causticWorker(PhotonMap *caustic_map, int thread_id, const Scene *scene, unsigned int n_caus_photons, Pdf1D *light_power_d, int num_lights, const std::string &integrator_name, const std::vector<Light *> &caus_lights, int caus_depth, ProgressBar *pb, int pb_step, unsigned int &total_photons_shot)
{
	bool done = false;
	float s_1, s_2, s_3, s_4, s_5, s_6, s_7, s_l;
	float f_num_lights = (float)num_lights;
	float light_num_pdf, light_pdf;

	unsigned int curr = 0;
	unsigned int n_caus_photons_thread = 1 + ((n_caus_photons - 1) / scene->getNumThreadsPhotons());

	std::vector<Photon> local_caustic_photons;

	SurfacePoint sp_1, sp_2;
	SurfacePoint *hit = &sp_1, *hit_2 = &sp_2;
	Ray ray;

	RenderState state;
	state.cam_ = scene->getCamera();
	unsigned char userdata[USER_DATA_SIZE + 7];
	state.userdata_ = (void *)(&userdata[7] - (((size_t)&userdata[7]) & 7));   // pad userdata to 8 bytes

	local_caustic_photons.clear();
	local_caustic_photons.reserve(n_caus_photons_thread);

	while(!done)
	{
		unsigned int haltoncurr = curr + n_caus_photons_thread * thread_id;

		state.chromatic_ = true;
		state.wavelength_ = riS__(haltoncurr);

		s_1 = riVdC__(haltoncurr);
		s_2 = scrHalton__(2, haltoncurr);
		s_3 = scrHalton__(3, haltoncurr);
		s_4 = scrHalton__(4, haltoncurr);

		s_l = float(haltoncurr) / float(n_caus_photons);

		int light_num = light_power_d->dSample(s_l, &light_num_pdf);

		if(light_num >= num_lights)
		{
			caustic_map->mutx_.lock();
			Y_ERROR << integrator_name << ": lightPDF sample error! " << s_l << "/" << light_num << YENDL;
			caustic_map->mutx_.unlock();
			return;
		}

		Rgb pcol = caus_lights[light_num]->emitPhoton(s_1, s_2, s_3, s_4, ray, light_pdf);
		ray.tmin_ = scene->ray_min_dist_;
		ray.tmax_ = -1.0;
		pcol *= f_num_lights * light_pdf / light_num_pdf; //remember that lightPdf is the inverse of th pdf, hence *=...
		if(pcol.isBlack())
		{
			++curr;
			done = (curr >= n_caus_photons_thread);
			continue;
		}
		Bsdf_t bsdfs = BsdfNone;
		int n_bounces = 0;
		bool caustic_photon = false;
		bool direct_photon = true;
		const Material *material = nullptr;
		const VolumeHandler *vol = nullptr;

		while(scene->intersect(ray, *hit_2))
		{
			if(std::isnan(pcol.r_) || std::isnan(pcol.g_) || std::isnan(pcol.b_))
			{
				caustic_map->mutx_.lock();
				Y_WARNING << integrator_name << ": NaN (photon color)" << YENDL;
				caustic_map->mutx_.unlock();
				break;
			}
			Rgb transm(1.f), vcol;
			// check for volumetric effects
			if(material)
			{
				if((bsdfs & BsdfVolumetric) && (vol = material->getVolumeHandler(hit->ng_ * ray.dir_ < 0)))
				{
					vol->transmittance(state, ray, vcol);
					transm = vcol;
				}
			}
			std::swap(hit, hit_2);
			Vec3 wi = -ray.dir_, wo;
			material = hit->material_;
			material->initBsdf(state, *hit, bsdfs);
			if(bsdfs & (BsdfDiffuse | BsdfGlossy))
			{
				//deposit caustic photon on surface
				if(caustic_photon)
				{
					Photon np(wi, hit->p_, pcol);
					local_caustic_photons.push_back(np);
				}
			}
			// need to break in the middle otherwise we scatter the photon and then discard it => redundant
			if(n_bounces == caus_depth) break;
			// scatter photon
			int d_5 = 3 * n_bounces + 5;
			//int d6 = d5 + 1;

			s_5 = scrHalton__(d_5, haltoncurr);
			s_6 = scrHalton__(d_5 + 1, haltoncurr);
			s_7 = scrHalton__(d_5 + 2, haltoncurr);

			PSample sample(s_5, s_6, s_7, BsdfAllSpecular | BsdfGlossy | BsdfFilter | BsdfDispersive, pcol, transm);
			bool scattered = material->scatterPhoton(state, *hit, wi, wo, sample);
			if(!scattered) break; //photon was absorped.
			pcol = sample.color_;
			// hm...dispersive is not really a scattering qualifier like specular/glossy/diffuse or the special case filter...
			caustic_photon = ((sample.sampled_flags_ & (BsdfGlossy | BsdfSpecular | BsdfDispersive)) && direct_photon) ||
							 ((sample.sampled_flags_ & (BsdfGlossy | BsdfSpecular | BsdfFilter | BsdfDispersive)) && caustic_photon);
			// light through transparent materials can be calculated by direct lighting, so still consider them direct!
			direct_photon = (sample.sampled_flags_ & BsdfFilter) && direct_photon;
			// caustic-only calculation can be stopped if:
			if(!(caustic_photon || direct_photon)) break;

			if(state.chromatic_ && (sample.sampled_flags_ & BsdfDispersive))
			{
				state.chromatic_ = false;
				Rgb wl_col;
				wl2Rgb__(state.wavelength_, wl_col);
				pcol *= wl_col;
			}
			ray.from_ = hit->p_;
			ray.dir_ = wo;
			ray.tmin_ = scene->ray_min_dist_;
			ray.tmax_ = -1.0;
			++n_bounces;
		}
		++curr;
		if(curr % pb_step == 0)
		{
			pb->mutx_.lock();
			pb->update();
			pb->mutx_.unlock();
			if(scene->getSignals() & Y_SIG_ABORT) { return; }
		}
		done = (curr >= n_caus_photons_thread);
	}
	caustic_map->mutx_.lock();
	caustic_map->appendVector(local_caustic_photons, curr);
	total_photons_shot += curr;
	caustic_map->mutx_.unlock();
}

bool MonteCarloIntegrator::createCausticMap()
{
	ProgressBar *pb;
	if(intpb_) pb = intpb_;
	else pb = new ConsoleProgressBar(80);

	if(photon_map_processing_ == PhotonsLoad)
	{
		pb->setTag("Loading caustic photon map from file...");
		const std::string filename = session__.getPathImageOutput() + "_caustic.photonmap";
		Y_INFO << integrator_name_ << ": Loading caustic photon map from: " << filename << ". If it does not match the scene you could have crashes and/or incorrect renders, USE WITH CARE!" << YENDL;
		if(session__.caustic_map_->load(filename))
		{
			Y_VERBOSE << integrator_name_ << ": Caustic map loaded." << YENDL;
			return true;
		}
		else
		{
			photon_map_processing_ = PhotonsGenerateAndSave;
			Y_WARNING << integrator_name_ << ": photon map loading failed, changing to Generate and Save mode." << YENDL;
		}
	}

	if(photon_map_processing_ == PhotonsReuse)
	{
		Y_INFO << integrator_name_ << ": Reusing caustics photon map from memory. If it does not match the scene you could have crashes and/or incorrect renders, USE WITH CARE!" << YENDL;
		if(session__.caustic_map_->nPhotons() == 0)
		{
			photon_map_processing_ = PhotonsGenerateOnly;
			Y_WARNING << integrator_name_ << ": One of the photon maps in memory was empty, they cannot be reused: changing to Generate mode." << YENDL;
		}
		else return true;
	}

	session__.caustic_map_->clear();
	session__.caustic_map_->setNumPaths(0);
	session__.caustic_map_->reserveMemory(n_caus_photons_);
	session__.caustic_map_->setNumThreadsPkDtree(scene_->getNumThreadsPhotons());

	Ray ray;
	std::vector<Light *> caus_lights;

	for(unsigned int i = 0; i < lights_.size(); ++i)
	{
		if(lights_[i]->shootsCausticP())
		{
			caus_lights.push_back(lights_[i]);
		}
	}

	int num_lights = caus_lights.size();

	if(num_lights > 0)
	{
		float light_num_pdf, light_pdf;
		float f_num_lights = (float)num_lights;
		float *energies = new float[num_lights];
		for(int i = 0; i < num_lights; ++i) energies[i] = caus_lights[i]->totalEnergy().energy();
		Pdf1D *light_power_d = new Pdf1D(energies, num_lights);

		Y_VERBOSE << integrator_name_ << ": Light(s) photon color testing for caustics map:" << YENDL;
		Rgb pcol(0.f);

		for(int i = 0; i < num_lights; ++i)
		{
			pcol = caus_lights[i]->emitPhoton(.5, .5, .5, .5, ray, light_pdf);
			light_num_pdf = light_power_d->func_[i] * light_power_d->inv_integral_;
			pcol *= f_num_lights * light_pdf / light_num_pdf; //remember that lightPdf is the inverse of the pdf, hence *=...
			Y_VERBOSE << integrator_name_ << ": Light [" << i + 1 << "] Photon col:" << pcol << " | lnpdf: " << light_num_pdf << YENDL;
		}

		delete[] energies;

		int pb_step;
		Y_INFO << integrator_name_ << ": Building caustics photon map..." << YENDL;
		pb->init(128);
		pb_step = std::max(1U, n_caus_photons_ / 128);
		pb->setTag("Building caustics photon map...");

		unsigned int curr = 0;

		int n_threads = scene_->getNumThreadsPhotons();

		n_caus_photons_ = std::max((unsigned int) n_threads, (n_caus_photons_ / n_threads) * n_threads); //rounding the number of diffuse photons so it's a number divisible by the number of threads (distribute uniformly among the threads). At least 1 photon per thread

		Y_PARAMS << integrator_name_ << ": Shooting " << n_caus_photons_ << " photons across " << n_threads << " threads (" << (n_caus_photons_ / n_threads) << " photons/thread)" << YENDL;

		if(n_threads >= 2)
		{
			std::vector<std::thread> threads;
			for(int i = 0; i < n_threads; ++i) threads.push_back(std::thread(&MonteCarloIntegrator::causticWorker, this, session__.caustic_map_, i, scene_, n_caus_photons_, light_power_d, num_lights, std::ref(integrator_name_), caus_lights, caus_depth_, pb, pb_step, std::ref(curr)));
			for(auto &t : threads) t.join();
		}
		else
		{
			bool done = false;
			float s_1, s_2, s_3, s_4, s_5, s_6, s_7, s_l;
			SurfacePoint sp_1, sp_2;
			SurfacePoint *hit = &sp_1, *hit_2 = &sp_2;

			RenderState state;
			state.cam_ = scene_->getCamera();
			unsigned char userdata[USER_DATA_SIZE + 7];
			state.userdata_ = (void *)(&userdata[7] - (((size_t)&userdata[7]) & 7));   // pad userdata to 8 bytes

			while(!done)
			{
				if(scene_->getSignals() & Y_SIG_ABORT) { pb->done(); if(!intpb_) delete pb; return false; }
				state.chromatic_ = true;
				state.wavelength_ = riS__(curr);
				s_1 = riVdC__(curr);
				s_2 = scrHalton__(2, curr);
				s_3 = scrHalton__(3, curr);
				s_4 = scrHalton__(4, curr);

				s_l = float(curr) / float(n_caus_photons_);

				int light_num = light_power_d->dSample(s_l, &light_num_pdf);

				if(light_num >= num_lights)
				{
					Y_ERROR << integrator_name_ << ": lightPDF sample error! " << s_l << "/" << light_num << YENDL;
					delete light_power_d;
					return false;
				}

				Rgb pcol = caus_lights[light_num]->emitPhoton(s_1, s_2, s_3, s_4, ray, light_pdf);
				ray.tmin_ = scene_->ray_min_dist_;
				ray.tmax_ = -1.0;
				pcol *= f_num_lights * light_pdf / light_num_pdf; //remember that lightPdf is the inverse of th pdf, hence *=...
				if(pcol.isBlack())
				{
					++curr;
					done = (curr >= n_caus_photons_);
					continue;
				}
				Bsdf_t bsdfs = BsdfNone;
				int n_bounces = 0;
				bool caustic_photon = false;
				bool direct_photon = true;
				const Material *material = nullptr;
				const VolumeHandler *vol = nullptr;

				while(scene_->intersect(ray, *hit_2))
				{
					if(std::isnan(pcol.r_) || std::isnan(pcol.g_) || std::isnan(pcol.b_))
					{
						Y_WARNING << integrator_name_ << ": NaN (photon color)" << YENDL;
						break;
					}
					Rgb transm(1.f), vcol;
					// check for volumetric effects
					if(material)
					{
						if((bsdfs & BsdfVolumetric) && (vol = material->getVolumeHandler(hit->ng_ * ray.dir_ < 0)))
						{
							vol->transmittance(state, ray, vcol);
							transm = vcol;
						}

					}
					std::swap(hit, hit_2);
					Vec3 wi = -ray.dir_, wo;
					material = hit->material_;
					material->initBsdf(state, *hit, bsdfs);
					if(bsdfs & (BsdfDiffuse | BsdfGlossy))
					{
						//deposit caustic photon on surface
						if(caustic_photon)
						{
							Photon np(wi, hit->p_, pcol);
							session__.caustic_map_->pushPhoton(np);
							session__.caustic_map_->setNumPaths(curr);
						}
					}
					// need to break in the middle otherwise we scatter the photon and then discard it => redundant
					if(n_bounces == caus_depth_) break;
					// scatter photon
					int d_5 = 3 * n_bounces + 5;
					//int d6 = d5 + 1;

					s_5 = scrHalton__(d_5, curr);
					s_6 = scrHalton__(d_5 + 1, curr);
					s_7 = scrHalton__(d_5 + 2, curr);

					PSample sample(s_5, s_6, s_7, BsdfAllSpecular | BsdfGlossy | BsdfFilter | BsdfDispersive, pcol, transm);
					bool scattered = material->scatterPhoton(state, *hit, wi, wo, sample);
					if(!scattered) break; //photon was absorped.
					pcol = sample.color_;
					// hm...dispersive is not really a scattering qualifier like specular/glossy/diffuse or the special case filter...
					caustic_photon = ((sample.sampled_flags_ & (BsdfGlossy | BsdfSpecular | BsdfDispersive)) && direct_photon) ||
									 ((sample.sampled_flags_ & (BsdfGlossy | BsdfSpecular | BsdfFilter | BsdfDispersive)) && caustic_photon);
					// light through transparent materials can be calculated by direct lighting, so still consider them direct!
					direct_photon = (sample.sampled_flags_ & BsdfFilter) && direct_photon;
					// caustic-only calculation can be stopped if:
					if(!(caustic_photon || direct_photon)) break;

					if(state.chromatic_ && (sample.sampled_flags_ & BsdfDispersive))
					{
						state.chromatic_ = false;
						Rgb wl_col;
						wl2Rgb__(state.wavelength_, wl_col);
						pcol *= wl_col;
					}
					ray.from_ = hit->p_;
					ray.dir_ = wo;
					ray.tmin_ = scene_->ray_min_dist_;
					ray.tmax_ = -1.0;
					++n_bounces;
				}
				++curr;
				if(curr % pb_step == 0) pb->update();
				done = (curr >= n_caus_photons_);
			}
		}

		pb->done();
		pb->setTag("Caustic photon map built.");
		Y_VERBOSE << integrator_name_ << ": Done." << YENDL;
		Y_INFO << integrator_name_ << ": Shot " << curr << " caustic photons from " << num_lights << " light(s)." << YENDL;
		Y_VERBOSE << integrator_name_ << ": Stored caustic photons: " << session__.caustic_map_->nPhotons() << YENDL;

		delete light_power_d;

		if(session__.caustic_map_->nPhotons() > 0)
		{
			pb->setTag("Building caustic photons kd-tree...");
			session__.caustic_map_->updateTree();
			Y_VERBOSE << integrator_name_ << ": Done." << YENDL;
		}

		if(photon_map_processing_ == PhotonsGenerateAndSave)
		{
			pb->setTag("Saving caustic photon map to file...");
			std::string filename = session__.getPathImageOutput() + "_caustic.photonmap";
			Y_INFO << integrator_name_ << ": Saving caustic photon map to: " << filename << YENDL;
			if(session__.caustic_map_->save(filename)) Y_VERBOSE << integrator_name_ << ": Caustic map saved." << YENDL;
		}

		if(!intpb_) delete pb;

	}
	else
	{
		Y_VERBOSE << integrator_name_ << ": No caustic source lights found, skiping caustic map building..." << YENDL;
	}

	return true;
}

inline Rgb MonteCarloIntegrator::estimateCausticPhotons(RenderState &state, const SurfacePoint &sp, const Vec3 &wo) const
{
	if(!session__.caustic_map_->ready()) return Rgb(0.f);

	FoundPhoton *gathered = new FoundPhoton[n_caus_search_];//(foundPhoton_t *)alloca(nCausSearch * sizeof(foundPhoton_t));
	int n_gathered = 0;

	float g_radius_square = caus_radius_ * caus_radius_;

	n_gathered = session__.caustic_map_->gather(sp.p_, gathered, n_caus_search_, g_radius_square);

	g_radius_square = 1.f / g_radius_square;

	Rgb sum(0.f);

	if(n_gathered > 0)
	{
		const Material *material = sp.material_;
		Rgb surf_col(0.f);
		float k = 0.f;
		const Photon *photon;

		for(int i = 0; i < n_gathered; ++i)
		{
			photon = gathered[i].photon_;
			surf_col = material->eval(state, sp, wo, photon->direction(), BsdfAll);
			k = kernel__(gathered[i].dist_square_, g_radius_square);
			sum += surf_col * k * photon->color();
		}
		sum *= 1.f / (float(session__.caustic_map_->nPaths()));
	}

	delete [] gathered;

	return sum;
}

inline void MonteCarloIntegrator::recursiveRaytrace(RenderState &state, DiffRay &ray, Bsdf_t bsdfs, SurfacePoint &sp, Vec3 &wo, Rgb &col, float &alpha, ColorPasses &color_passes, int additional_depth) const
{
	const Material *material = sp.material_;
	SpDifferentials sp_diff(sp, ray);

	ColorPasses tmp_color_passes = color_passes;

	state.raylevel_++;

	if(state.raylevel_ <= (r_depth_ + additional_depth))
	{
		Halton hal_2(2);
		Halton hal_3(3);

		// dispersive effects with recursive raytracing:
		if((bsdfs & BsdfDispersive) && state.chromatic_)
		{
			state.include_lights_ = false; //debatable...
			int dsam = 8;
			int old_division = state.ray_division_;
			int old_offset = state.ray_offset_;
			float old_dc_1 = state.dc_1_, old_dc_2 = state.dc_2_;
			if(state.ray_division_ > 1) dsam = std::max(1, dsam / old_division);
			state.ray_division_ *= dsam;
			int branch = state.ray_division_ * old_offset;
			float d_1 = 1.f / (float)dsam;
			float ss_1 = riS__(state.pixel_sample_ + state.sampling_offs_);
			Rgb dcol(0.f), vcol(1.f);
			Vec3 wi;
			const VolumeHandler *vol;
			DiffRay ref_ray;
			float w = 0.f;

			if(tmp_color_passes.size() > 1) tmp_color_passes.resetColors();

			for(int ns = 0; ns < dsam; ++ns)
			{
				state.wavelength_ = (ns + ss_1) * d_1;
				state.dc_1_ = scrHalton__(2 * state.raylevel_ + 1, branch + state.sampling_offs_);
				state.dc_2_ = scrHalton__(2 * state.raylevel_ + 2, branch + state.sampling_offs_);
				if(old_division > 1) state.wavelength_ = addMod1__(state.wavelength_, old_dc_1);
				state.ray_offset_ = branch;
				++branch;
				Sample s(0.5f, 0.5f, BsdfReflect | BsdfTransmit | BsdfDispersive);
				Rgb mcol = material->sample(state, sp, wo, wi, s, w);

				if(s.pdf_ > 1.0e-6f && (s.sampled_flags_ & BsdfDispersive))
				{
					state.chromatic_ = false;
					Rgb wl_col;
					wl2Rgb__(state.wavelength_, wl_col);
					ref_ray = DiffRay(sp.p_, wi, scene_->ray_min_dist_);

					dcol += tmp_color_passes.probeAdd(PassIntTrans, (Rgb) integrate(state, ref_ray, tmp_color_passes, additional_depth) * mcol * wl_col * w, state.raylevel_ == 1);

					state.chromatic_ = true;
				}
			}

			if((bsdfs & BsdfVolumetric) && (vol = material->getVolumeHandler(sp.ng_ * ref_ray.dir_ < 0)))
			{
				vol->transmittance(state, ref_ray, vcol);
				dcol *= vcol;
				//if(tmpColorPasses.size() > 1) tmpColorPasses *= vcol; //FIXME DAVID??
			}

			col += dcol * d_1;
			if(tmp_color_passes.size() > 1)
			{
				tmp_color_passes *= d_1;
				color_passes += tmp_color_passes;
			}

			state.ray_division_ = old_division;
			state.ray_offset_ = old_offset;
			state.dc_1_ = old_dc_1;
			state.dc_2_ = old_dc_2;
		}

		// glossy reflection with recursive raytracing:
		if(bsdfs & BsdfGlossy && state.raylevel_ < 20)
		{
			state.include_lights_ = true;
			int gsam = 8;
			int old_division = state.ray_division_;
			int old_offset = state.ray_offset_;
			float old_dc_1 = state.dc_1_, old_dc_2 = state.dc_2_;
			if(state.ray_division_ > 1) gsam = std::max(1, gsam / old_division);
			state.ray_division_ *= gsam;
			int branch = state.ray_division_ * old_offset;
			unsigned int offs = gsam * state.pixel_sample_ + state.sampling_offs_;
			float d_1 = 1.f / (float)gsam;
			Rgb gcol(0.f), vcol(1.f);
			Vec3 wi;
			const VolumeHandler *vol;
			DiffRay ref_ray;

			hal_2.setStart(offs);
			hal_3.setStart(offs);

			if(tmp_color_passes.size() > 1) tmp_color_passes.resetColors();

			for(int ns = 0; ns < gsam; ++ns)
			{
				state.dc_1_ = scrHalton__(2 * state.raylevel_ + 1, branch + state.sampling_offs_);
				state.dc_2_ = scrHalton__(2 * state.raylevel_ + 2, branch + state.sampling_offs_);
				state.ray_offset_ = branch;
				++offs;
				++branch;

				float s_1 = hal_2.getNext();
				float s_2 = hal_3.getNext();

				if(material->getFlags() & BsdfGlossy)
				{
					if((material->getFlags() & BsdfReflect) && !(material->getFlags() & BsdfTransmit))
					{
						float w = 0.f;

						Sample s(s_1, s_2, BsdfGlossy | BsdfReflect);
						Rgb mcol = material->sample(state, sp, wo, wi, s, w);
						Rgba integ = 0.f;
						ref_ray = DiffRay(sp.p_, wi, scene_->ray_min_dist_);
						if(diff_rays_enabled_)
						{
							if(s.sampled_flags_ & BsdfReflect) sp_diff.reflectedRay(ray, ref_ray);
							else if(s.sampled_flags_ & BsdfTransmit) sp_diff.refractedRay(ray, ref_ray, material->getMatIor());
						}
						integ = (Rgb)integrate(state, ref_ray, tmp_color_passes, additional_depth);

						if((bsdfs & BsdfVolumetric) && (vol = material->getVolumeHandler(sp.ng_ * ref_ray.dir_ < 0)))
						{
							if(vol->transmittance(state, ref_ray, vcol)) integ *= vcol;
						}

						gcol += tmp_color_passes.probeAdd(PassIntGlossyIndirect, (Rgb) integ * mcol * w, state.raylevel_ == 1);
					}
					else if((material->getFlags() & BsdfReflect) && (material->getFlags() & BsdfTransmit))
					{
						Sample s(s_1, s_2, BsdfGlossy | BsdfAllGlossy);
						Rgb mcol[2];
						float w[2];
						Vec3 dir[2];

						mcol[0] = material->sample(state, sp, wo, dir, mcol[1], s, w);
						Rgba integ = 0.f;

						if(s.sampled_flags_ & BsdfReflect && !(s.sampled_flags_ & BsdfDispersive))
						{
							ref_ray = DiffRay(sp.p_, dir[0], scene_->ray_min_dist_);
							if(diff_rays_enabled_) sp_diff.reflectedRay(ray, ref_ray);
							integ = integrate(state, ref_ray, tmp_color_passes, additional_depth);
							if((bsdfs & BsdfVolumetric) && (vol = material->getVolumeHandler(sp.ng_ * ref_ray.dir_ < 0)))
							{
								if(vol->transmittance(state, ref_ray, vcol)) integ *= vcol;
							}
							Rgb col_reflect_factor = mcol[0] * w[0];
							gcol += tmp_color_passes.probeAdd(PassIntTrans, (Rgb) integ * col_reflect_factor, state.raylevel_ == 1);
						}

						if(s.sampled_flags_ & BsdfTransmit)
						{
							ref_ray = DiffRay(sp.p_, dir[1], scene_->ray_min_dist_);
							if(diff_rays_enabled_) sp_diff.refractedRay(ray, ref_ray, material->getMatIor());
							integ = integrate(state, ref_ray, tmp_color_passes, additional_depth);
							if((bsdfs & BsdfVolumetric) && (vol = material->getVolumeHandler(sp.ng_ * ref_ray.dir_ < 0)))
							{
								if(vol->transmittance(state, ref_ray, vcol)) integ *= vcol;
							}

							Rgb col_transmit_factor = mcol[1] * w[1];
							gcol += tmp_color_passes.probeAdd(PassIntGlossyIndirect, (Rgb) integ * col_transmit_factor, state.raylevel_ == 1);
							alpha = integ.a_;
						}
					}
				}
			}

			col += gcol * d_1;

			if(tmp_color_passes.size() > 1)
			{
				tmp_color_passes *= d_1;
				color_passes += tmp_color_passes;
			}

			state.ray_division_ = old_division;
			state.ray_offset_ = old_offset;
			state.dc_1_ = old_dc_1;
			state.dc_2_ = old_dc_2;
		}

		//...perfect specular reflection/refraction with recursive raytracing...
		if(bsdfs & (BsdfSpecular | BsdfFilter) && state.raylevel_ < 20)
		{
			state.include_lights_ = true;
			bool reflect = false, refract = false;
			Vec3 dir[2];
			Rgb rcol[2], vcol;
			material->getSpecular(state, sp, wo, reflect, refract, dir, rcol);
			const VolumeHandler *vol;
			if(reflect)
			{
				if(tmp_color_passes.size() > 1) tmp_color_passes.resetColors();

				DiffRay ref_ray(sp.p_, dir[0], scene_->ray_min_dist_);
				if(diff_rays_enabled_) sp_diff.reflectedRay(ray, ref_ray);
				Rgb integ = integrate(state, ref_ray, tmp_color_passes, additional_depth);

				if((bsdfs & BsdfVolumetric) && (vol = material->getVolumeHandler(sp.ng_ * ref_ray.dir_ < 0)))
				{
					if(vol->transmittance(state, ref_ray, vcol)) integ *= vcol;
				}

				col += color_passes.probeAdd(PassIntReflectPerfect, (Rgb) integ * rcol[0], state.raylevel_ == 1);
			}
			if(refract)
			{
				if(tmp_color_passes.size() > 1) tmp_color_passes.resetColors();

				DiffRay ref_ray;
				float transp_bias_factor = material->getTransparentBiasFactor();

				if(transp_bias_factor > 0.f)
				{
					bool transpbias_multiply_raydepth = material->getTransparentBiasMultiplyRayDepth();
					if(transpbias_multiply_raydepth) transp_bias_factor *= state.raylevel_;
					ref_ray = DiffRay(sp.p_ + dir[1] * transp_bias_factor, dir[1], scene_->ray_min_dist_);
				}
				else ref_ray = DiffRay(sp.p_, dir[1], scene_->ray_min_dist_);

				if(diff_rays_enabled_) sp_diff.refractedRay(ray, ref_ray, material->getMatIor());
				Rgba integ = integrate(state, ref_ray, tmp_color_passes, additional_depth);

				if((bsdfs & BsdfVolumetric) && (vol = material->getVolumeHandler(sp.ng_ * ref_ray.dir_ < 0)))
				{
					if(vol->transmittance(state, ref_ray, vcol)) integ *= vcol;
				}

				col += color_passes.probeAdd(PassIntRefractPerfect, (Rgb) integ * rcol[1], state.raylevel_ == 1);

				alpha = integ.a_;
			}
		}
	}
	--state.raylevel_;
}

Rgb MonteCarloIntegrator::sampleAmbientOcclusion(RenderState &state, const SurfacePoint &sp, const Vec3 &wo) const
{
	Rgb col(0.f), surf_col(0.f), scol(0.f);
	bool shadowed;
	const Material *material = sp.material_;
	Ray light_ray;
	light_ray.from_ = sp.p_;
	light_ray.dir_ = Vec3(0.f);
	float mask_obj_index = 0.f, mask_mat_index = 0.f;

	int n = ao_samples_;//(int) ceilf(aoSamples*getSampleMultiplier());
	if(state.ray_division_ > 1) n = std::max(1, n / state.ray_division_);

	unsigned int offs = n * state.pixel_sample_ + state.sampling_offs_;

	Halton hal_2(2);
	Halton hal_3(3);

	hal_2.setStart(offs - 1);
	hal_3.setStart(offs - 1);

	for(int i = 0; i < n; ++i)
	{
		float s_1 = hal_2.getNext();
		float s_2 = hal_3.getNext();

		if(state.ray_division_ > 1)
		{
			s_1 = addMod1__(s_1, state.dc_1_);
			s_2 = addMod1__(s_2, state.dc_2_);
		}

		if(scene_->shadow_bias_auto_) light_ray.tmin_ = scene_->shadow_bias_ * std::max(1.f, Vec3(sp.p_).length());
		else light_ray.tmin_ = scene_->shadow_bias_;

		light_ray.tmax_ = ao_dist_;

		float w = 0.f;

		Sample s(s_1, s_2, BsdfGlossy | BsdfDiffuse | BsdfReflect);
		surf_col = material->sample(state, sp, wo, light_ray.dir_, s, w);

		if(material->getFlags() & BsdfEmit)
		{
			col += material->emit(state, sp, wo) * s.pdf_;
		}

		shadowed = (tr_shad_) ? scene_->isShadowed(state, light_ray, s_depth_, scol, mask_obj_index, mask_mat_index) : scene_->isShadowed(state, light_ray, mask_obj_index, mask_mat_index);

		if(!shadowed)
		{
			float cos = std::fabs(sp.n_ * light_ray.dir_);
			if(tr_shad_) col += ao_col_ * scol * surf_col * cos * w;
			else col += ao_col_ * surf_col * cos * w;
		}
	}

	return col / (float)n;
}

Rgb MonteCarloIntegrator::sampleAmbientOcclusionPass(RenderState &state, const SurfacePoint &sp, const Vec3 &wo) const
{
	Rgb col(0.f), surf_col(0.f), scol(0.f);
	bool shadowed;
	const Material *material = sp.material_;
	Ray light_ray;
	light_ray.from_ = sp.p_;
	light_ray.dir_ = Vec3(0.f);
	float mask_obj_index = 0.f, mask_mat_index = 0.f;

	int n = ao_samples_;//(int) ceilf(aoSamples*getSampleMultiplier());
	if(state.ray_division_ > 1) n = std::max(1, n / state.ray_division_);

	unsigned int offs = n * state.pixel_sample_ + state.sampling_offs_;

	Halton hal_2(2);
	Halton hal_3(3);

	hal_2.setStart(offs - 1);
	hal_3.setStart(offs - 1);

	for(int i = 0; i < n; ++i)
	{
		float s_1 = hal_2.getNext();
		float s_2 = hal_3.getNext();

		if(state.ray_division_ > 1)
		{
			s_1 = addMod1__(s_1, state.dc_1_);
			s_2 = addMod1__(s_2, state.dc_2_);
		}

		if(scene_->shadow_bias_auto_) light_ray.tmin_ = scene_->shadow_bias_ * std::max(1.f, Vec3(sp.p_).length());
		else light_ray.tmin_ = scene_->shadow_bias_;

		light_ray.tmax_ = ao_dist_;

		float w = 0.f;

		Sample s(s_1, s_2, BsdfGlossy | BsdfDiffuse | BsdfReflect);
		surf_col = material->sample(state, sp, wo, light_ray.dir_, s, w);

		if(material->getFlags() & BsdfEmit)
		{
			col += material->emit(state, sp, wo) * s.pdf_;
		}

		shadowed = scene_->isShadowed(state, light_ray, mask_obj_index, mask_mat_index);

		if(!shadowed)
		{
			float cos = std::fabs(sp.n_ * light_ray.dir_);
			//if(trShad) col += aoCol * scol * surfCol * cos * W;
			col += ao_col_ * surf_col * cos * w;
		}
	}

	return col / (float)n;
}


Rgb MonteCarloIntegrator::sampleAmbientOcclusionPassClay(RenderState &state, const SurfacePoint &sp, const Vec3 &wo) const
{
	Rgb col(0.f), surf_col(0.f), scol(0.f);
	bool shadowed;
	const Material *material = sp.material_;
	Ray light_ray;
	light_ray.from_ = sp.p_;
	light_ray.dir_ = Vec3(0.f);
	float mask_obj_index = 0.f, mask_mat_index = 0.f;

	int n = ao_samples_;
	if(state.ray_division_ > 1) n = std::max(1, n / state.ray_division_);

	unsigned int offs = n * state.pixel_sample_ + state.sampling_offs_;

	Halton hal_2(2);
	Halton hal_3(3);

	hal_2.setStart(offs - 1);
	hal_3.setStart(offs - 1);

	for(int i = 0; i < n; ++i)
	{
		float s_1 = hal_2.getNext();
		float s_2 = hal_3.getNext();

		if(state.ray_division_ > 1)
		{
			s_1 = addMod1__(s_1, state.dc_1_);
			s_2 = addMod1__(s_2, state.dc_2_);
		}

		if(scene_->shadow_bias_auto_) light_ray.tmin_ = scene_->shadow_bias_ * std::max(1.f, Vec3(sp.p_).length());
		else light_ray.tmin_ = scene_->shadow_bias_;

		light_ray.tmax_ = ao_dist_;

		float w = 0.f;

		Sample s(s_1, s_2, BsdfAll);
		surf_col = material->sampleClay(state, sp, wo, light_ray.dir_, s, w);
		s.pdf_ = 1.f;

		if(material->getFlags() & BsdfEmit)
		{
			col += material->emit(state, sp, wo) * s.pdf_;
		}

		shadowed = scene_->isShadowed(state, light_ray, mask_obj_index, mask_mat_index);

		if(!shadowed)
		{
			float cos = std::fabs(sp.n_ * light_ray.dir_);
			//if(trShad) col += aoCol * scol * surfCol * cos * W;
			col += ao_col_ * surf_col * cos * w;
		}
	}

	return col / (float)n;
}

END_YAFARAY
