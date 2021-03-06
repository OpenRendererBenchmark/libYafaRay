/****************************************************************************
 *              sppm.cc: A stochastic progressive photon map integrator
 *              This is part of the libYafaRay package
 *              Copyright (C) 2011  Rodrigo Placencia (DarkTide)
 *
 *              This library is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU Lesser General Public
 *              License as published by the Free Software Foundation; either
 *              version 2.1 of the License, or (at your option) any later version.
 *
 *              This library is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *              Lesser General Public License for more details.
 *
 *              You should have received a copy of the GNU Lesser General Public
 *              License along with this library; if not, write to the Free Software
 *              Foundation,Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "integrator/integrator_sppm.h"
#include "common/surface.h"
#include "common/logging.h"
#include "common/session.h"
#include "volume/volume.h"
#include "common/param.h"
#include "common/scene.h"

BEGIN_YAFARAY

const int n_max_gather__ = 1000; //used to gather all the photon in the radius. seems could get a better way to do that

SppmIntegrator::SppmIntegrator(unsigned int d_photons, int passnum, bool transp_shad, int shadow_depth)
{
	type_ = Surface;
	integrator_name_ = "SPPM";
	integrator_short_name_ = "SPPM";
	n_photons_ = d_photons;
	pass_num_ = passnum;
	totaln_photons_ = 0;
	initial_factor_ = 1.f;

	s_depth_ = shadow_depth;
	tr_shad_ = transp_shad;
	b_hashgrid_ = false;

	hal_1_.setBase(2);
	hal_2_.setBase(3);
	hal_3_.setBase(5);
	hal_4_.setBase(7);

	hal_1_.setStart(0);
	hal_2_.setStart(0);
	hal_3_.setStart(0);
	hal_4_.setStart(0);
}

SppmIntegrator::~SppmIntegrator()
{

}

bool SppmIntegrator::preprocess()
{
	return true;
}

bool SppmIntegrator::render(int num_view, yafaray4::ImageFilm *image_film)
{
	std::stringstream pass_string;
	image_film_ = image_film;
	scene_->getAaParameters(aa_samples_, aa_passes_, aa_inc_samples_, aa_threshold_, aa_resampled_floor_, aa_sample_multiplier_factor_, aa_light_sample_multiplier_factor_, aa_indirect_sample_multiplier_factor_, aa_detect_color_noise_, aa_dark_detection_type_, aa_dark_threshold_factor_, aa_variance_edge_size_, aa_variance_pixels_, aa_clamp_samples_, aa_clamp_indirect_);

	std::stringstream aa_settings;
	aa_settings << " passes=" << pass_num_ << " samples=" << aa_samples_ << " inc_samples=" << aa_inc_samples_;
	aa_settings << " clamp=" << aa_clamp_samples_ << " ind.clamp=" << aa_clamp_indirect_;

	logger__.appendAaNoiseSettings(aa_settings.str());


	session__.setStatusTotalPasses(pass_num_);	//passNum is total number of passes in SPPM

	aa_sample_multiplier_ = 1.f;
	aa_light_sample_multiplier_ = 1.f;
	aa_indirect_sample_multiplier_ = 1.f;

	Y_VERBOSE << integrator_name_ << ": AA_clamp_samples: " << aa_clamp_samples_ << YENDL;
	Y_VERBOSE << integrator_name_ << ": AA_clamp_indirect: " << aa_clamp_indirect_ << YENDL;

	std::stringstream set;

	set << "SPPM  ";

	if(tr_shad_)
	{
		set << "ShadowDepth=" << s_depth_ << "  ";
	}
	set << "RayDepth=" << r_depth_ << "  ";

	logger__.appendRenderSettings(set.str());
	Y_VERBOSE << set.str() << YENDL;


	pass_string << "Rendering pass 1 of " << std::max(1, pass_num_) << "...";
	Y_INFO << integrator_name_ << ": " << pass_string.str() << YENDL;
	if(intpb_) intpb_->setTag(pass_string.str().c_str());

	g_timer__.addEvent("rendert");
	g_timer__.start("rendert");

	image_film_->resetImagesAutoSaveTimer();
	g_timer__.addEvent("imagesAutoSaveTimer");

	image_film_->resetFilmAutoSaveTimer();
	g_timer__.addEvent("filmAutoSaveTimer");

	image_film_->init(pass_num_);
	image_film_->setAaNoiseParams(aa_detect_color_noise_, aa_dark_detection_type_, aa_dark_threshold_factor_, aa_variance_edge_size_, aa_variance_pixels_, aa_clamp_samples_);

	if(session__.renderResumed())
	{
		pass_string.clear();
		pass_string << "Loading film file, skipping pass 1...";
		intpb_->setTag(pass_string.str().c_str());
	}

	Y_INFO << integrator_name_ << ": " << pass_string.str() << YENDL;

	const Camera *camera = scene_->getCamera();

	max_depth_ = 0.f;
	min_depth_ = 1e38f;

	diff_rays_enabled_ = session__.getDifferentialRaysEnabled();	//enable ray differentials for mipmap calculation if there is at least one image texture using Mipmap interpolation

	if(scene_->passEnabled(PassIntZDepthNorm) || scene_->passEnabled(PassIntMist)) precalcDepths();

	int acum_aa_samples = 1;

	initializePpm(); // seems could integrate into the preRender
	if(session__.renderResumed())
	{
		acum_aa_samples = image_film_->getSamplingOffset();
		renderPass(num_view, 0, acum_aa_samples, false, 0);
	}
	else renderPass(num_view, 1, 0, false, 0);

	std::string initial_estimate = "no";
	if(pm_ire_) initial_estimate = "yes";

	pm_ire_ = false;

	int hp_num = camera->resX() * camera->resY();
	int pass_info = 1;
	for(int i = 1; i < pass_num_; ++i) //progress pass, the offset start from 1 as it is 0 based.
	{
		if(scene_->getSignals() & Y_SIG_ABORT) break;
		pass_info = i + 1;
		image_film_->nextPass(num_view, false, integrator_name_);
		n_refined_ = 0;
		renderPass(num_view, 1, acum_aa_samples, false, i); // offset are only related to the passNum, since we alway have only one sample.
		acum_aa_samples += 1;
		Y_INFO << integrator_name_ << ": This pass refined " << n_refined_ << " of " << hp_num << " pixels." << YENDL;
	}
	max_depth_ = 0.f;
	g_timer__.stop("rendert");
	g_timer__.stop("imagesAutoSaveTimer");
	g_timer__.stop("filmAutoSaveTimer");
	session__.setStatusRenderFinished();
	Y_INFO << integrator_name_ << ": Overall rendertime: " << g_timer__.getTime("rendert") << "s." << YENDL;

	// Integrator Settings for "drawRenderSettings()" in imageFilm, SPPM has own render method, so "getSettings()"
	// in integrator.h has no effect and Integrator settings won't be printed to the parameter badge.

	set.clear();

	set << "Passes rendered: " << pass_info << "  ";

	set << "\nPhotons=" << n_photons_ << " search=" << n_search_ << " radius=" << ds_radius_ << "(init.estim=" << initial_estimate << ") total photons=" << totaln_photons_ << "  ";

	logger__.appendRenderSettings(set.str());
	Y_VERBOSE << set.str() << YENDL;

	return true;
}


bool SppmIntegrator::renderTile(int num_view, RenderArea &a, int n_samples, int offset, bool adaptive, int thread_id, int aa_pass_number)
{
	int x;
	const Camera *camera = scene_->getCamera();
	const float x_start_film = image_film_->getCx0();
	const float y_start_film = image_film_->getCy0();
	x = camera->resX();
	DiffRay c_ray;
	Ray d_ray;
	float dx = 0.5, dy = 0.5, d_1 = 1.0 / (float)n_samples;
	float lens_u = 0.5f, lens_v = 0.5f;
	float wt, wt_dummy;
	Random prng(rand() + offset * (x * a.y_ + a.x_) + 123);
	RenderState rstate(&prng);
	rstate.thread_id_ = thread_id;
	rstate.cam_ = camera;
	bool sample_lns = camera->sampleLense();
	int pass_offs = offset, end_x = a.x_ + a.w_, end_y = a.y_ + a.h_;

	int aa_max_possible_samples = aa_samples_;

	for(int i = 1; i < aa_passes_; ++i)
	{
		aa_max_possible_samples += ceilf(aa_inc_samples_ * pow(aa_sample_multiplier_factor_, i));
	}

	float inv_aa_max_possible_samples = 1.f / ((float) aa_max_possible_samples);

	ColorPasses color_passes(scene_->getRenderPasses());

	ColorPasses tmp_passes_zero(scene_->getRenderPasses());

	for(int i = a.y_; i < end_y; ++i)
	{
		for(int j = a.x_; j < end_x; ++j)
		{
			if(scene_->getSignals() & Y_SIG_ABORT) break;

			rstate.pixel_number_ = x * i + j;
			rstate.sampling_offs_ = fnv32ABuf__(i * fnv32ABuf__(j)); //fnv_32a_buf(rstate.pixelNumber);
			float toff = scrHalton__(5, pass_offs + rstate.sampling_offs_); // **shall be just the pass number...**

			for(int sample = 0; sample < n_samples; ++sample) //set n_samples = 1.
			{
				color_passes.resetColors();

				rstate.setDefaults();
				rstate.pixel_sample_ = pass_offs + sample;
				rstate.time_ = addMod1__((float) sample * d_1, toff); //(0.5+(float)sample)*d1;
				// the (1/n, Larcher&Pillichshammer-Seq.) only gives good coverage when total sample count is known
				// hence we use scrambled (Sobol, van-der-Corput) for multipass AA

				dx = riVdC__(rstate.pixel_sample_, rstate.sampling_offs_);
				dy = riS__(rstate.pixel_sample_, rstate.sampling_offs_);

				if(sample_lns)
				{
					lens_u = scrHalton__(3, rstate.pixel_sample_ + rstate.sampling_offs_);
					lens_v = scrHalton__(4, rstate.pixel_sample_ + rstate.sampling_offs_);
				}
				c_ray = camera->shootRay(j + dx, i + dy, lens_u, lens_v, wt); // wt need to be considered
				if(wt == 0.0)
				{
					image_film_->addSample(tmp_passes_zero, j, i, dx, dy, &a); //maybe not need
					continue;
				}
				if(diff_rays_enabled_)
				{
					//setup ray differentials
					d_ray = camera->shootRay(j + 1 + dx, i + dy, lens_u, lens_v, wt_dummy);
					c_ray.xfrom_ = d_ray.from_;
					c_ray.xdir_ = d_ray.dir_;
					d_ray = camera->shootRay(j + dx, i + 1 + dy, lens_u, lens_v, wt_dummy);
					c_ray.yfrom_ = d_ray.from_;
					c_ray.ydir_ = d_ray.dir_;
					c_ray.has_differentials_ = true;
					// col = T * L_o + L_v
				}

				c_ray.time_ = rstate.time_;

				//for sppm progressive
				int index = ((i - y_start_film) * camera->resX()) + (j - x_start_film);
				HitPoint_t &hp = hit_points_[index];

				GatherInfo_t g_info = traceGatherRay(rstate, c_ray, hp, color_passes);
				hp.constant_randiance_ += g_info.constant_randiance_; // accumulate the constant radiance for later usage.

				// progressive refinement
				const float alpha = 0.7f; // another common choice is 0.8, seems not changed much.

				// The author's refine formular
				if(g_info.photon_count_ > 0)
				{
					float g = std::min((hp.acc_photon_count_ + alpha * g_info.photon_count_) / (hp.acc_photon_count_ + g_info.photon_count_), 1.0f);
					hp.radius_2_ *= g;
					hp.acc_photon_count_ += g_info.photon_count_ * alpha;
					hp.acc_photon_flux_ = (hp.acc_photon_flux_ + g_info.photon_flux_) * g;
					n_refined_++; // record the pixel that has refined.
				}

				//radiance estimate
				//colorPasses.probe_mult(PASS_INT_DIFFUSE_INDIRECT, 1.f / (hp.radius2 * M_PI * totalnPhotons));
				Rgba color = color_passes.probeSet(PassIntIndirect, hp.acc_photon_flux_ / (hp.radius_2_ * M_PI * totaln_photons_));
				color += g_info.constant_randiance_;
				color.a_ = g_info.constant_randiance_.a_; //the alpha value is hold in the constantRadiance variable
				if(color_passes.enabled(PassIntIndirect)) color_passes(PassIntIndirect).a_ = g_info.constant_randiance_.a_;

				color_passes.probeSet(PassIntCombined, color);


				if(color_passes.enabled(PassIntZDepthNorm) || color_passes.enabled(PassIntZDepthAbs) || color_passes.enabled(PassIntMist))
				{
					float depth_abs = 0.f, depth_norm = 0.f;

					if(color_passes.enabled(PassIntZDepthNorm) || color_passes.enabled(PassIntMist))
					{
						if(c_ray.tmax_ > 0.f)
						{
							depth_norm = 1.f - (c_ray.tmax_ - min_depth_) * max_depth_; // Distance normalization
						}
						color_passes.probeSet(PassIntZDepthNorm, Rgba(depth_norm));
						color_passes.probeSet(PassIntMist, Rgba(1.f - depth_norm));
					}
					if(color_passes.enabled(PassIntZDepthAbs))
					{
						depth_abs = c_ray.tmax_;
						if(depth_abs <= 0.f)
						{
							depth_abs = 99999997952.f;
						}
						color_passes.probeSet(PassIntZDepthAbs, Rgba(depth_abs));
					}
				}

				for(int idx = 0; idx < color_passes.size(); ++idx)
				{
					if(color_passes(idx).a_ > 1.f) color_passes(idx).a_ = 1.f;

					int int_pass_type = color_passes.intPassTypeFromIndex(idx);

					switch(int_pass_type)
					{
						case PassIntZDepthNorm: break;
						case PassIntZDepthAbs: break;
						case PassIntMist: break;
						case PassIntNormalSmooth: break;
						case PassIntNormalGeom: break;
						case PassIntAo: break;
						case PassIntAoClay: break;
						case PassIntUv: break;
						case PassIntDebugNu: break;
						case PassIntDebugNv: break;
						case PassIntDebugDpdu: break;
						case PassIntDebugDpdv: break;
						case PassIntDebugDsdu: break;
						case PassIntDebugDsdv: break;
						case PassIntObjIndexAbs: break;
						case PassIntObjIndexNorm: break;
						case PassIntObjIndexAuto: break;
						case PassIntObjIndexAutoAbs: break;
						case PassIntMatIndexAbs: break;
						case PassIntMatIndexNorm: break;
						case PassIntMatIndexAuto: break;
						case PassIntMatIndexAutoAbs: break;
						case PassIntAaSamples: break;

						//Processing of mask render passes:
						case PassIntObjIndexMask:
						case PassIntObjIndexMaskShadow:
						case PassIntObjIndexMaskAll:
						case PassIntMatIndexMask:
						case PassIntMatIndexMaskShadow:
						case PassIntMatIndexMaskAll:

							color_passes(idx).clampRgb01();

							if(color_passes.getPassMaskInvert())
							{
								color_passes(idx) = Rgba(1.f) - color_passes(idx);
							}

							if(!color_passes.getPassMaskOnly())
							{
								Rgba col_combined = color_passes(PassIntCombined);
								col_combined.a_ = 1.f;
								color_passes(idx) *= col_combined;
							}
							break;

						default: color_passes(idx) *= wt; break;
					}
				}

				image_film_->addSample(color_passes, j, i, dx, dy, &a, sample, aa_pass_number, inv_aa_max_possible_samples);
			}
		}
	}
	return true;
}

void SppmIntegrator::photonWorker(PhotonMap *diffuse_map, PhotonMap *caustic_map, int thread_id, const Scene *scene, unsigned int n_photons, const Pdf1D *light_power_d, int num_d_lights, const std::string &integrator_name, const std::vector<Light *> &tmplights, ProgressBar *pb, int pb_step, unsigned int &total_photons_shot, int max_bounces, Random &prng)
{
	Ray ray;
	float light_num_pdf, light_pdf, s_1, s_2, s_3, s_4, s_5, s_6, s_7, s_l;
	Rgb pcol;

	//shoot photons
	bool done = false;
	unsigned int curr = 0;

	SurfacePoint sp;
	RenderState state(&prng);
	unsigned char userdata[USER_DATA_SIZE + 7];
	state.userdata_ = (void *)(&userdata[7] - (((size_t)&userdata[7]) & 7));   // pad userdata to 8 bytes
	state.cam_ = scene->getCamera();

	float f_num_lights = (float)num_d_lights;

	unsigned int n_photons_thread = 1 + ((n_photons - 1) / scene->getNumThreadsPhotons());

	std::vector<Photon> local_caustic_photons;
	local_caustic_photons.clear();
	local_caustic_photons.reserve(n_photons_thread);

	std::vector<Photon> local_diffuse_photons;
	local_diffuse_photons.clear();
	local_diffuse_photons.reserve(n_photons_thread);

	//Pregather  photons
	float inv_diff_photons = 1.f / (float)n_photons;

	unsigned int nd_photon_stored = 0;
	//	unsigned int ncPhotonStored = 0;

	while(!done)
	{
		unsigned int haltoncurr = curr + n_photons_thread * thread_id;

		state.chromatic_ = true;
		state.wavelength_ = scrHalton__(5, haltoncurr);

		// Tried LD, get bad and strange results for some stategy.
		s_1 = hal_1_.getNext();
		s_2 = hal_2_.getNext();
		s_3 = hal_3_.getNext();
		s_4 = hal_4_.getNext();

		s_l = float(haltoncurr) * inv_diff_photons; // Does sL also need more random for each pass?
		int light_num = light_power_d->dSample(s_l, &light_num_pdf);
		if(light_num >= num_d_lights)
		{
			diffuse_map->mutx_.lock();
			Y_ERROR << integrator_name << ": lightPDF sample error! " << s_l << "/" << light_num << "\n";
			diffuse_map->mutx_.unlock();
			return;
		}

		pcol = tmplights[light_num]->emitPhoton(s_1, s_2, s_3, s_4, ray, light_pdf);
		ray.tmin_ = scene->ray_min_dist_;
		ray.tmax_ = -1.0;
		pcol *= f_num_lights * light_pdf / light_num_pdf; //remember that lightPdf is the inverse of th pdf, hence *=...

		if(pcol.isBlack())
		{
			++curr;
			done = (curr >= n_photons);
			continue;
		}

		int n_bounces = 0;
		bool caustic_photon = false;
		bool direct_photon = true;
		const Material *material = nullptr;
		Bsdf_t bsdfs;

		while(scene->intersect(ray, sp))   //scatter photons.
		{
			if(std::isnan(pcol.r_) || std::isnan(pcol.g_) || std::isnan(pcol.b_))
			{
				diffuse_map->mutx_.lock();
				Y_WARNING << integrator_name << ": NaN  on photon color for light" << light_num + 1 << "." << YENDL;
				diffuse_map->mutx_.unlock();
				continue;
			}

			Rgb transm(1.f);
			Rgb vcol(0.f);
			const VolumeHandler *vol;

			if(material)
			{
				if((bsdfs & BsdfVolumetric) && (vol = material->getVolumeHandler(sp.ng_ * -ray.dir_ < 0)))
				{
					if(vol->transmittance(state, ray, vcol)) transm = vcol;
				}
			}

			Vec3 wi = -ray.dir_, wo;
			material = sp.material_;
			material->initBsdf(state, sp, bsdfs);

			//deposit photon on diffuse surface, now we only have one map for all, elimate directPhoton for we estimate it directly
			if(!direct_photon && !caustic_photon && (bsdfs & (BsdfDiffuse)))
			{
				Photon np(wi, sp.p_, pcol);// pcol used here

				if(b_hashgrid_) photon_grid_.pushPhoton(np);
				else
				{
					local_diffuse_photons.push_back(np);
				}
				nd_photon_stored++;
			}
			// add caustic photon
			if(!direct_photon && caustic_photon && (bsdfs & (BsdfDiffuse | BsdfGlossy)))
			{
				Photon np(wi, sp.p_, pcol);// pcol used here

				if(b_hashgrid_) photon_grid_.pushPhoton(np);
				else
				{
					local_caustic_photons.push_back(np);
				}
				nd_photon_stored++;
			}

			// need to break in the middle otherwise we scatter the photon and then discard it => redundant
			if(n_bounces == max_bounces) break;

			// scatter photon
			s_5 = ourRandom__(); // now should use this to see correctness
			s_6 = ourRandom__();
			s_7 = ourRandom__();

			PSample sample(s_5, s_6, s_7, BsdfAll, pcol, transm);

			bool scattered = material->scatterPhoton(state, sp, wi, wo, sample);
			if(!scattered) break; //photon was absorped.  actually based on russian roulette

			pcol = sample.color_;

			caustic_photon = ((sample.sampled_flags_ & (BsdfGlossy | BsdfSpecular | BsdfDispersive)) && direct_photon) ||
							 ((sample.sampled_flags_ & (BsdfGlossy | BsdfSpecular | BsdfFilter | BsdfDispersive)) && caustic_photon);
			direct_photon = (sample.sampled_flags_ & BsdfFilter) && direct_photon;

			if(state.chromatic_ && (sample.sampled_flags_ & BsdfDispersive))
			{
				state.chromatic_ = false;
				Rgb wl_col;
				wl2Rgb__(state.wavelength_, wl_col);
				pcol *= wl_col;
			}

			ray.from_ = sp.p_;
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
		done = (curr >= n_photons_thread);
	}
	diffuse_map->mutx_.lock();
	caustic_map->mutx_.lock();
	diffuse_map->appendVector(local_diffuse_photons, curr);
	caustic_map->appendVector(local_caustic_photons, curr);
	total_photons_shot += curr;
	caustic_map->mutx_.unlock();
	diffuse_map->mutx_.unlock();
}


//photon pass, scatter photon
void SppmIntegrator::prePass(int samples, int offset, bool adaptive)
{
	g_timer__.addEvent("prepass");
	g_timer__.start("prepass");

	Y_INFO << integrator_name_ << ": Starting Photon tracing pass..." << YENDL;

	if(b_hashgrid_) photon_grid_.clear();
	else
	{
		session__.diffuse_map_->clear();
		session__.diffuse_map_->setNumPaths(0);
		session__.diffuse_map_->reserveMemory(n_photons_);
		session__.diffuse_map_->setNumThreadsPkDtree(scene_->getNumThreadsPhotons());

		session__.caustic_map_->clear();
		session__.caustic_map_->setNumPaths(0);
		session__.caustic_map_->reserveMemory(n_photons_);
		session__.caustic_map_->setNumThreadsPkDtree(scene_->getNumThreadsPhotons());
	}

	background_ = scene_->getBackground();
	lights_ = scene_->lights_;
	std::vector<Light *> tmplights;

	//background do not emit photons, or it is merged into normal light?

	Ray ray;
	float light_num_pdf, light_pdf, s_1, s_2, s_3, s_4, s_5, s_6, s_7, s_l;
	int num_d_lights = 0;
	float f_num_lights = 0.f;
	float *energies = nullptr;
	Rgb pcol;

	tmplights.clear();

	for(int i = 0; i < (int)lights_.size(); ++i)
	{
		num_d_lights++;
		tmplights.push_back(lights_[i]);
	}

	f_num_lights = (float)num_d_lights;
	energies = new float[num_d_lights];

	for(int i = 0; i < num_d_lights; ++i) energies[i] = tmplights[i]->totalEnergy().energy();

	light_power_d_ = new Pdf1D(energies, num_d_lights);

	Y_VERBOSE << integrator_name_ << ": Light(s) photon color testing for photon map:" << YENDL;

	for(int i = 0; i < num_d_lights; ++i)
	{
		pcol = tmplights[i]->emitPhoton(.5, .5, .5, .5, ray, light_pdf);
		light_num_pdf = light_power_d_->func_[i] * light_power_d_->inv_integral_;
		pcol *= f_num_lights * light_pdf / light_num_pdf; //remember that lightPdf is the inverse of the pdf, hence *=...
		Y_VERBOSE << integrator_name_ << ": Light [" << i + 1 << "] Photon col:" << pcol << " | lnpdf: " << light_num_pdf << YENDL;
	}

	delete[] energies;

	//shoot photons
	unsigned int curr = 0;

	SurfacePoint sp;
	Random prng(rand() + offset * (4517) + 123);
	RenderState state(&prng);
	unsigned char userdata[USER_DATA_SIZE + 7];
	state.userdata_ = (void *)(&userdata[7] - (((size_t)&userdata[7]) & 7));   // pad userdata to 8 bytes
	state.cam_ = scene_->getCamera();

	ProgressBar *pb;
	std::string previous_progress_tag;
	int previous_progress_total_steps = 0;
	int pb_step;
	if(intpb_)
	{
		pb = intpb_;
		previous_progress_tag = pb->getTag();
		previous_progress_total_steps = pb->getTotalSteps();
	}
	else pb = new ConsoleProgressBar(80);

	if(b_hashgrid_) Y_INFO << integrator_name_ << ": Building photon hashgrid..." << YENDL;
	else Y_INFO << integrator_name_ << ": Building photon map..." << YENDL;

	pb->init(128);
	pb_step = std::max(1U, n_photons_ / 128);
	pb->setTag(previous_progress_tag + " - building photon map...");

	int n_threads = scene_->getNumThreadsPhotons();

	n_photons_ = std::max((unsigned int) n_threads, (n_photons_ / n_threads) * n_threads); //rounding the number of diffuse photons so it's a number divisible by the number of threads (distribute uniformly among the threads). At least 1 photon per thread

	Y_PARAMS << integrator_name_ << ": Shooting " << n_photons_ << " photons across " << n_threads << " threads (" << (n_photons_ / n_threads) << " photons/thread)" << YENDL;

	if(n_threads >= 2)
	{
		std::vector<std::thread> threads;
		for(int i = 0; i < n_threads; ++i) threads.push_back(std::thread(&SppmIntegrator::photonWorker, this, session__.diffuse_map_, session__.caustic_map_, i, scene_, n_photons_, light_power_d_, num_d_lights, std::ref(integrator_name_), tmplights, pb, pb_step, std::ref(curr), max_bounces_, std::ref(prng)));
		for(auto &t : threads) t.join();
	}
	else
	{
		bool done = false;

		//Pregather  photons
		float inv_diff_photons = 1.f / (float)n_photons_;

		unsigned int nd_photon_stored = 0;
		//	unsigned int ncPhotonStored = 0;

		while(!done)
		{
			if(scene_->getSignals() & Y_SIG_ABORT) {  pb->done(); if(!intpb_) delete pb; return; }
			state.chromatic_ = true;
			state.wavelength_ = scrHalton__(5, curr);

			// Tried LD, get bad and strange results for some stategy.
			s_1 = hal_1_.getNext();
			s_2 = hal_2_.getNext();
			s_3 = hal_3_.getNext();
			s_4 = hal_4_.getNext();

			s_l = float(curr) * inv_diff_photons; // Does sL also need more random for each pass?
			int light_num = light_power_d_->dSample(s_l, &light_num_pdf);
			if(light_num >= num_d_lights) { Y_ERROR << integrator_name_ << ": lightPDF sample error! " << s_l << "/" << light_num << "... stopping now.\n"; delete light_power_d_; return; }

			pcol = tmplights[light_num]->emitPhoton(s_1, s_2, s_3, s_4, ray, light_pdf);
			ray.tmin_ = scene_->ray_min_dist_;
			ray.tmax_ = -1.0;
			pcol *= f_num_lights * light_pdf / light_num_pdf; //remember that lightPdf is the inverse of th pdf, hence *=...

			if(pcol.isBlack())
			{
				++curr;
				done = (curr >= n_photons_);
				continue;
			}

			int n_bounces = 0;
			bool caustic_photon = false;
			bool direct_photon = true;
			const Material *material = nullptr;
			Bsdf_t bsdfs;

			while(scene_->intersect(ray, sp))   //scatter photons.
			{
				if(std::isnan(pcol.r_) || std::isnan(pcol.g_) || std::isnan(pcol.b_))
				{ Y_WARNING << integrator_name_ << ": NaN  on photon color for light" << light_num + 1 << ".\n"; continue; }

				Rgb transm(1.f);
				Rgb vcol(0.f);
				const VolumeHandler *vol;

				if(material)
				{
					if((bsdfs & BsdfVolumetric) && (vol = material->getVolumeHandler(sp.ng_ * -ray.dir_ < 0)))
					{
						if(vol->transmittance(state, ray, vcol)) transm = vcol;
					}
				}

				Vec3 wi = -ray.dir_, wo;
				material = sp.material_;
				material->initBsdf(state, sp, bsdfs);

				//deposit photon on diffuse surface, now we only have one map for all, elimate directPhoton for we estimate it directly
				if(!direct_photon && !caustic_photon && (bsdfs & (BsdfDiffuse)))
				{
					Photon np(wi, sp.p_, pcol);// pcol used here

					if(b_hashgrid_) photon_grid_.pushPhoton(np);
					else
					{
						session__.diffuse_map_->pushPhoton(np);
						session__.diffuse_map_->setNumPaths(curr);
					}
					nd_photon_stored++;
				}
				// add caustic photon
				if(!direct_photon && caustic_photon && (bsdfs & (BsdfDiffuse | BsdfGlossy)))
				{
					Photon np(wi, sp.p_, pcol);// pcol used here

					if(b_hashgrid_) photon_grid_.pushPhoton(np);
					else
					{
						session__.caustic_map_->pushPhoton(np);
						session__.caustic_map_->setNumPaths(curr);
					}
					nd_photon_stored++;
				}

				// need to break in the middle otherwise we scatter the photon and then discard it => redundant
				if(n_bounces == max_bounces_) break;

				// scatter photon
				s_5 = ourRandom__(); // now should use this to see correctness
				s_6 = ourRandom__();
				s_7 = ourRandom__();

				PSample sample(s_5, s_6, s_7, BsdfAll, pcol, transm);

				bool scattered = material->scatterPhoton(state, sp, wi, wo, sample);
				if(!scattered) break; //photon was absorped.  actually based on russian roulette

				pcol = sample.color_;

				caustic_photon = ((sample.sampled_flags_ & (BsdfGlossy | BsdfSpecular | BsdfDispersive)) && direct_photon) ||
								 ((sample.sampled_flags_ & (BsdfGlossy | BsdfSpecular | BsdfFilter | BsdfDispersive)) && caustic_photon);
				direct_photon = (sample.sampled_flags_ & BsdfFilter) && direct_photon;

				if(state.chromatic_ && (sample.sampled_flags_ & BsdfDispersive))
				{
					state.chromatic_ = false;
					Rgb wl_col;
					wl2Rgb__(state.wavelength_, wl_col);
					pcol *= wl_col;
				}

				ray.from_ = sp.p_;
				ray.dir_ = wo;
				ray.tmin_ = scene_->ray_min_dist_;
				ray.tmax_ = -1.0;
				++n_bounces;

			}
			++curr;
			if(curr % pb_step == 0) pb->update();
			done = (curr >= n_photons_);
		}
	}

	pb->done();
	pb->setTag(previous_progress_tag + " - photon map built.");
	Y_VERBOSE << integrator_name_ << ":Photon map built." << YENDL;
	Y_INFO << integrator_name_ << ": Shot " << curr << " photons from " << num_d_lights << " light(s)" << YENDL;
	delete light_power_d_;

	totaln_photons_ +=  n_photons_;	// accumulate the total photon number, not using nPath for the case of hashgrid.

	Y_VERBOSE << integrator_name_ << ": Stored photons: " << session__.diffuse_map_->nPhotons() + session__.caustic_map_->nPhotons() << YENDL;

	if(b_hashgrid_)
	{
		Y_INFO << integrator_name_ << ": Building photons hashgrid:" << YENDL;
		photon_grid_.updateGrid();
		Y_VERBOSE << integrator_name_ << ": Done." << YENDL;
	}
	else
	{
		if(session__.diffuse_map_->nPhotons() > 0)
		{
			Y_INFO << integrator_name_ << ": Building diffuse photons kd-tree:" << YENDL;
			session__.diffuse_map_->updateTree();
			Y_VERBOSE << integrator_name_ << ": Done." << YENDL;
		}
		if(session__.caustic_map_->nPhotons() > 0)
		{
			Y_INFO << integrator_name_ << ": Building caustic photons kd-tree:" << YENDL;
			session__.caustic_map_->updateTree();
			Y_VERBOSE << integrator_name_ << ": Done." << YENDL;
		}
		if(session__.diffuse_map_->nPhotons() < 50)
		{
			Y_ERROR << integrator_name_ << ": Too few photons, stopping now." << YENDL;
			return;
		}
	}

	tmplights.clear();

	if(!intpb_) delete pb;

	g_timer__.stop("prepass");

	if(b_hashgrid_)
		Y_INFO << integrator_name_ << ": PhotonGrid building time: " << g_timer__.getTime("prepass") << YENDL;
	else
		Y_INFO << integrator_name_ << ": PhotonMap building time: " << g_timer__.getTime("prepass") << YENDL;

	if(intpb_)
	{
		intpb_->setTag(previous_progress_tag);
		intpb_->init(previous_progress_total_steps);
	}

	return;
}

//now it's a dummy function
Rgba SppmIntegrator::integrate(RenderState &state, DiffRay &ray, ColorPasses &color_passes, int additional_depth /*=0*/) const
{
	return Rgba(0.f);
}


GatherInfo_t SppmIntegrator::traceGatherRay(yafaray4::RenderState &state, yafaray4::DiffRay &ray, yafaray4::HitPoint_t &hp, ColorPasses &color_passes)
{
	static int n_max = 0;
	static int calls = 0;
	++calls;
	Rgb col(0.0);
	GatherInfo_t g_info;

	float alpha;
	SurfacePoint sp;

	void *o_udat = state.userdata_;
	bool old_include_lights = state.include_lights_;

	if(transp_background_) alpha = 0.0;
	else alpha = 1.0;

	if(scene_->intersect(ray, sp))
	{
		unsigned char userdata[USER_DATA_SIZE + 7];
		state.userdata_ = (void *)(&userdata[7] - (((size_t)&userdata[7]) & 7));   // pad userdata to 8 bytes
		if(state.raylevel_ == 0)
		{
			state.chromatic_ = true;
			state.include_lights_ = true;
		}

		Bsdf_t bsdfs;
		int additional_depth = 0;

		Vec3 wo = -ray.dir_;
		const Material *material = sp.material_;
		material->initBsdf(state, sp, bsdfs);

		if(additional_depth < material->getAdditionalDepth()) additional_depth = material->getAdditionalDepth();

		g_info.constant_randiance_ += color_passes.probeAdd(PassIntEmit, material->emit(state, sp, wo), state.raylevel_ == 0); //add only once, but FG seems add twice?
		state.include_lights_ = false;
		SpDifferentials sp_diff(sp, ray);

		ColorPasses tmp_color_passes = color_passes;

		if(bsdfs & BsdfDiffuse)
		{
			g_info.constant_randiance_ += estimateAllDirectLight(state, sp, wo, color_passes);
		}

		// estimate radiance using photon map
		FoundPhoton *gathered = new FoundPhoton[n_max_gather__];

		//if PM_IRE is on. we should estimate the initial radius using the photonMaps. (PM_IRE is only for the first pass, so not consume much time)
		if(pm_ire_ && !hp.radius_setted_) // "waste" two gather here as it has two maps now. This make the logic simple.
		{
			float radius_1 = ds_radius_ * ds_radius_;
			float radius_2 = radius_1;
			int n_gathered_1 = 0, n_gathered_2 = 0;

			if(session__.diffuse_map_->nPhotons() > 0)
				n_gathered_1 = session__.diffuse_map_->gather(sp.p_, gathered, n_search_, radius_1);
			if(session__.caustic_map_->nPhotons() > 0)
				n_gathered_2 = session__.caustic_map_->gather(sp.p_, gathered, n_search_, radius_2);
			if(n_gathered_1 > 0 || n_gathered_2 > 0) // it none photon gathered, we just skip.
			{
				if(radius_1 < radius_2) // we choose the smaller one to be the initial radius.
					hp.radius_2_ = radius_1;
				else
					hp.radius_2_ = radius_2;

				hp.radius_setted_ = true;
			}
		}

		int n_gathered = 0;
		float radius_2 = hp.radius_2_;

		if(b_hashgrid_)
			n_gathered = photon_grid_.gather(sp.p_, gathered, n_max_gather__, radius_2); // disable now
		else
		{
			if(session__.diffuse_map_->nPhotons() > 0) // this is needed to avoid a runtime error.
			{
				n_gathered = session__.diffuse_map_->gather(sp.p_, gathered, n_max_gather__, radius_2); //we always collected all the photon inside the radius
			}

			if(n_gathered > 0)
			{
				if(n_gathered > n_max)
				{
					n_max = n_gathered;
					Y_DEBUG << "maximum Photons: " << n_max << ", radius2: " << radius_2 << "\n";
					if(n_max == 10) for(int j = 0; j < n_gathered; ++j) Y_DEBUG << "col:" << gathered[j].photon_->color() << "\n";
				}
				for(int i = 0; i < n_gathered; ++i)
				{
					////test if the photon is in the ellipsoid
					//vector3d_t scale  = sp.P - gathered[i].photon->pos;
					//vector3d_t temp;
					//temp.x = scale VDOT sp.NU;
					//temp.y = scale VDOT sp.NV;
					//temp.z = scale VDOT sp.N;

					//double inv_radi = 1 / sqrt(radius2);
					//temp.x  *= inv_radi; temp.y *= inv_radi; temp.z *=  1. / (2.f * scene->rayMinDist);
					//if(temp.lengthSqr() > 1.)continue;

					g_info.photon_count_++;
					Vec3 pdir = gathered[i].photon_->direction();
					Rgb surf_col = material->eval(state, sp, wo, pdir, BsdfDiffuse); // seems could speed up using rho, (something pbrt made)
					g_info.photon_flux_ += surf_col * gathered[i].photon_->color();// * std::fabs(sp.N*pdir); //< wrong!?
					//Rgb  flux= surfCol * gathered[i].photon->color();// * std::fabs(sp.N*pdir); //< wrong!?

					////start refine here
					//double ALPHA = 0.7;
					//double g = (hp.accPhotonCount*ALPHA+ALPHA) / (hp.accPhotonCount*ALPHA+1.0);
					//hp.radius2 *= g;
					//hp.accPhotonCount++;
					//hp.accPhotonFlux=((Rgb)hp.accPhotonFlux+flux)*g;
				}
			}

			// gather caustics photons
			if(bsdfs & BsdfDiffuse && session__.caustic_map_->ready())
			{

				radius_2 = hp.radius_2_; //reset radius2 & nGathered
				n_gathered = session__.caustic_map_->gather(sp.p_, gathered, n_max_gather__, radius_2);
				if(n_gathered > 0)
				{
					Rgb surf_col(0.f);
					for(int i = 0; i < n_gathered; ++i)
					{
						Vec3 pdir = gathered[i].photon_->direction();
						g_info.photon_count_++;
						surf_col = material->eval(state, sp, wo, pdir, BsdfAll); // seems could speed up using rho, (something pbrt made)
						g_info.photon_flux_ += surf_col * gathered[i].photon_->color();// * std::fabs(sp.N*pdir); //< wrong!?//gInfo.photonFlux += colorPasses.probe_add(PASS_INT_DIFFUSE_INDIRECT, surfCol * gathered[i].photon->color(), state.raylevel == 0);// * std::fabs(sp.N*pdir); //< wrong!?
						//Rgb  flux= surfCol * gathered[i].photon->color();// * std::fabs(sp.N*pdir); //< wrong!?

						////start refine here
						//double ALPHA = 0.7;
						//double g = (hp.accPhotonCount*ALPHA+ALPHA) / (hp.accPhotonCount*ALPHA+1.0);
						//hp.radius2 *= g;
						//hp.accPhotonCount++;
						//hp.accPhotonFlux=((Rgb)hp.accPhotonFlux+flux)*g;
					}
				}
			}
		}
		delete [] gathered;

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
				GatherInfo_t cing, t_cing; //Dispersive is different handled, not same as GLOSSY, at the BSDF_VOLUMETRIC part

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
						t_cing = traceGatherRay(state, ref_ray, hp, tmp_color_passes);
						t_cing.photon_flux_ *= mcol * wl_col * w;
						t_cing.constant_randiance_ *= mcol * wl_col * w;

						tmp_color_passes.probeAdd(PassIntTrans, t_cing.constant_randiance_, state.raylevel_ == 1);

						state.chromatic_ = true;
					}
					cing += t_cing;
				}
				if((bsdfs & BsdfVolumetric) && (vol = material->getVolumeHandler(sp.ng_ * ref_ray.dir_ < 0)))
				{
					vol->transmittance(state, ref_ray, vcol);
					cing.photon_flux_ *= vcol;
					cing.constant_randiance_ *= vcol;
				}

				g_info.constant_randiance_ += cing.constant_randiance_ * d_1;
				g_info.photon_flux_ += cing.photon_flux_ * d_1;
				g_info.photon_count_ += cing.photon_count_ * d_1;

				if(tmp_color_passes.size() > 1)
				{
					tmp_color_passes *= d_1;
					color_passes += tmp_color_passes;
				}

				state.ray_division_ = old_division;
				state.ray_offset_ = old_offset;
				state.dc_1_ = old_dc_1; state.dc_2_ = old_dc_2;
			}

			// glossy reflection with recursive raytracing:  Pure GLOSSY material doesn't hold photons?

			if(bsdfs & BsdfGlossy)
			{
				state.include_lights_ = false;
				int gsam = 8;
				int old_division = state.ray_division_;
				int old_offset = state.ray_offset_;
				float old_dc_1 = state.dc_1_, old_dc_2 = state.dc_2_;
				if(state.ray_division_ > 1) gsam = std::max(1, gsam / old_division);
				state.ray_division_ *= gsam;
				int branch = state.ray_division_ * old_offset;
				unsigned int offs = gsam * state.pixel_sample_ + state.sampling_offs_;
				float d_1 = 1.f / (float)gsam;
				Rgb vcol(1.f);
				Vec3 wi;
				const VolumeHandler *vol;
				DiffRay ref_ray;

				GatherInfo_t ging, t_ging;

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

					float W = 0.f;

					Sample s(s_1, s_2, BsdfAllGlossy);
					Rgb mcol = material->sample(state, sp, wo, wi, s, W);

					if((material->getFlags() & BsdfReflect) && !(material->getFlags() & BsdfTransmit))
					{
						float w = 0.f;

						Sample s(s_1, s_2, BsdfGlossy | BsdfReflect);
						Rgb mcol = material->sample(state, sp, wo, wi, s, w);
						Rgba integ = 0.f;
						ref_ray = DiffRay(sp.p_, wi, scene_->ray_min_dist_);
						if(s.sampled_flags_ & BsdfReflect) sp_diff.reflectedRay(ray, ref_ray);
						else if(s.sampled_flags_ & BsdfTransmit) sp_diff.refractedRay(ray, ref_ray, material->getMatIor());
						integ = (Rgb)integrate(state, ref_ray, tmp_color_passes, additional_depth);

						if((bsdfs & BsdfVolumetric) && (vol = material->getVolumeHandler(sp.ng_ * ref_ray.dir_ < 0)))
						{
							if(vol->transmittance(state, ref_ray, vcol)) integ *= vcol;
						}

						//gcol += tmpColorPasses.probe_add(PASS_INT_GLOSSY_INDIRECT, (Rgb)integ * mcol * W, state.raylevel == 1);
						t_ging = traceGatherRay(state, ref_ray, hp, tmp_color_passes);
						t_ging.photon_flux_ *= mcol * w;
						t_ging.constant_randiance_ *= mcol * w;
						ging += t_ging;
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
							sp_diff.reflectedRay(ray, ref_ray);
							integ = integrate(state, ref_ray, tmp_color_passes, additional_depth);
							if((bsdfs & BsdfVolumetric) && (vol = material->getVolumeHandler(sp.ng_ * ref_ray.dir_ < 0)))
							{
								if(vol->transmittance(state, ref_ray, vcol)) integ *= vcol;
							}
							Rgb col_reflect_factor = mcol[0] * w[0];

							t_ging = traceGatherRay(state, ref_ray, hp, tmp_color_passes);
							t_ging.photon_flux_ *= col_reflect_factor;
							t_ging.constant_randiance_ *= col_reflect_factor;

							tmp_color_passes.probeAdd(PassIntTrans, (Rgb) t_ging.constant_randiance_, state.raylevel_ == 1);
							ging += t_ging;
						}

						if(s.sampled_flags_ & BsdfTransmit)
						{
							ref_ray = DiffRay(sp.p_, dir[1], scene_->ray_min_dist_);
							sp_diff.refractedRay(ray, ref_ray, material->getMatIor());
							integ = integrate(state, ref_ray, tmp_color_passes, additional_depth);
							if((bsdfs & BsdfVolumetric) && (vol = material->getVolumeHandler(sp.ng_ * ref_ray.dir_ < 0)))
							{
								if(vol->transmittance(state, ref_ray, vcol)) integ *= vcol;
							}

							Rgb col_transmit_factor = mcol[1] * w[1];
							alpha = integ.a_;
							t_ging = traceGatherRay(state, ref_ray, hp, tmp_color_passes);
							t_ging.photon_flux_ *= col_transmit_factor;
							t_ging.constant_randiance_ *= col_transmit_factor;
							tmp_color_passes.probeAdd(PassIntGlossyIndirect, (Rgb) t_ging.constant_randiance_, state.raylevel_ == 1);
							ging += t_ging;
						}
					}

					else if(s.sampled_flags_ & BsdfGlossy)
					{
						ref_ray = DiffRay(sp.p_, wi, scene_->ray_min_dist_);
						if(diff_rays_enabled_)
						{
							if(s.sampled_flags_ & BsdfReflect) sp_diff.reflectedRay(ray, ref_ray);
							else if(s.sampled_flags_ & BsdfTransmit) sp_diff.refractedRay(ray, ref_ray, material->getMatIor());
						}

						t_ging = traceGatherRay(state, ref_ray, hp, tmp_color_passes);
						t_ging.photon_flux_ *= mcol * W;
						t_ging.constant_randiance_ *= mcol * W;
						tmp_color_passes.probeAdd(PassIntGlossyIndirect, t_ging.constant_randiance_, state.raylevel_ == 1);
						ging += t_ging;
					}

					if((bsdfs & BsdfVolumetric) && (vol = material->getVolumeHandler(sp.ng_ * ref_ray.dir_ < 0)))
					{
						if(vol->transmittance(state, ref_ray, vcol))
						{
							ging.photon_flux_ *= vcol;
							ging.constant_randiance_ *= vcol;
							//tmpColorPasses.probe_add(PASS_INT_GLOSSY_INDIRECT, t_ging.constantRandiance, state.raylevel == 1);
						}
					}

				}

				g_info.constant_randiance_ += ging.constant_randiance_ * d_1;
				g_info.photon_flux_ += ging.photon_flux_ * d_1;
				g_info.photon_count_ += ging.photon_count_ * d_1;

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
			if(bsdfs & (BsdfSpecular | BsdfFilter))
			{
				state.include_lights_ = true;
				bool reflect = false, refract = false;
				Vec3 dir[2];
				Rgb rcol[2], vcol;
				material->getSpecular(state, sp, wo, reflect, refract, dir, rcol);
				const VolumeHandler *vol;

				if(reflect)
				{
					DiffRay ref_ray(sp.p_, dir[0], scene_->ray_min_dist_);
					if(diff_rays_enabled_) sp_diff.reflectedRay(ray, ref_ray); // compute the ray differentaitl
					GatherInfo_t refg = traceGatherRay(state, ref_ray, hp, tmp_color_passes);
					if((bsdfs & BsdfVolumetric) && (vol = material->getVolumeHandler(sp.ng_ * ref_ray.dir_ < 0)))
					{
						if(vol->transmittance(state, ref_ray, vcol))
						{
							refg.constant_randiance_ *= vcol;
							refg.photon_flux_ *= vcol;
						}
					}
					g_info.constant_randiance_ += color_passes.probeAdd(PassIntReflectPerfect, refg.constant_randiance_ * Rgba(rcol[0]), state.raylevel_ == 1);
					g_info.photon_flux_ += refg.photon_flux_ * Rgba(rcol[0]);
					g_info.photon_count_ += refg.photon_count_;
				}
				if(refract)
				{
					DiffRay ref_ray(sp.p_, dir[1], scene_->ray_min_dist_);
					if(diff_rays_enabled_) sp_diff.refractedRay(ray, ref_ray, material->getMatIor());
					GatherInfo_t refg = traceGatherRay(state, ref_ray, hp, tmp_color_passes);
					if((bsdfs & BsdfVolumetric) && (vol = material->getVolumeHandler(sp.ng_ * ref_ray.dir_ < 0)))
					{
						if(vol->transmittance(state, ref_ray, vcol))
						{
							refg.constant_randiance_ *= vcol;
							refg.photon_flux_ *= vcol;
						}
					}
					g_info.constant_randiance_ += color_passes.probeAdd(PassIntRefractPerfect, refg.constant_randiance_ * Rgba(rcol[1]), state.raylevel_ == 1);
					g_info.photon_flux_ += refg.photon_flux_ * Rgba(rcol[1]);
					g_info.photon_count_ += refg.photon_count_;
					alpha = refg.constant_randiance_.a_;
				}
			}
		}
		--state.raylevel_;

		if(color_passes.size() > 1 && state.raylevel_ == 0)
		{
			generateCommonRenderPasses(color_passes, state, sp, ray);

			if(color_passes.enabled(PassIntAo))
			{
				color_passes(PassIntAo) = sampleAmbientOcclusionPass(state, sp, wo);
			}

			if(color_passes.enabled(PassIntAoClay))
			{
				color_passes(PassIntAoClay) = sampleAmbientOcclusionPassClay(state, sp, wo);
			}
		}

		if(transp_refracted_background_)
		{
			float m_alpha = material->getAlpha(state, sp, wo);
			alpha = m_alpha + (1.f - m_alpha) * alpha;
		}
		else alpha = 1.0;
	}

	else //nothing hit, return background
	{
		if(background_ && !transp_refracted_background_)
		{
			g_info.constant_randiance_ += color_passes.probeSet(PassIntEnv, (*background_)(ray, state), state.raylevel_ == 0);
		}
	}

	state.userdata_ = o_udat;
	state.include_lights_ = old_include_lights;

	Rgba col_vol_transmittance = scene_->vol_integrator_->transmittance(state, ray);
	Rgba col_vol_integration = scene_->vol_integrator_->integrate(state, ray, color_passes);

	if(transp_background_) alpha = std::max(alpha, 1.f - col_vol_transmittance.r_);

	color_passes.probeSet(PassIntVolumeTransmittance, col_vol_transmittance);
	color_passes.probeSet(PassIntVolumeIntegration, col_vol_integration);

	g_info.constant_randiance_ = (g_info.constant_randiance_ * col_vol_transmittance) + col_vol_integration;

	g_info.constant_randiance_.a_ = alpha; // a small trick for just hold the alpha value.

	return g_info;
}

void SppmIntegrator::initializePpm()
{
	const Camera *camera = scene_->getCamera();
	unsigned int resolution = camera->resX() * camera->resY();

	hit_points_.reserve(resolution);
	Bound b_box = scene_->getSceneBound(); // Now using Scene Bound, this could get a bigger initial radius, and need more tests

	// initialize SPPM statistics
	float initial_radius = ((b_box.longX() + b_box.longY() + b_box.longZ()) / 3.f) / ((camera->resX() + camera->resY()) / 2.0f) * 2.f ;
	initial_radius = std::min(initial_radius, 1.f); //Fix the overflow bug
	for(unsigned int i = 0; i < resolution; i++)
	{
		HitPoint_t hp;
		hp.acc_photon_flux_  = Rgba(0.f);
		hp.acc_photon_count_ = 0;
		hp.radius_2_ = (initial_radius * initial_factor_) * (initial_radius * initial_factor_);
		hp.constant_randiance_ = Rgba(0.f);
		hp.radius_setted_ = false;	   // the flag used for IRE

		hit_points_.push_back(hp);
	}

	if(b_hashgrid_) photon_grid_.setParm(initial_radius * 2.f, n_photons_, b_box);

}

Integrator *SppmIntegrator::factory(ParamMap &params, RenderEnvironment &render)
{
	bool transp_shad = false;
	bool pm_ire = false;
	int shadow_depth = 5; //may used when integrate Direct Light
	int raydepth = 5;
	int pass_num = 1000;
	int num_photons = 500000;
	int bounces = 5;
	float times = 1.f;
	int search_num = 100;
	float ds_rad = 1.0f;
	bool do_ao = false;
	int ao_samples = 32;
	double ao_dist = 1.0;
	Rgb ao_col(1.f);
	bool bg_transp = false;
	bool bg_transp_refract = false;

	params.getParam("transpShad", transp_shad);
	params.getParam("shadowDepth", shadow_depth);
	params.getParam("raydepth", raydepth);
	params.getParam("photons", num_photons);
	params.getParam("passNums", pass_num);
	params.getParam("bounces", bounces);
	params.getParam("times", times); // initial radius times

	params.getParam("photonRadius", ds_rad);
	params.getParam("searchNum", search_num);
	params.getParam("pmIRE", pm_ire);

	params.getParam("bg_transp", bg_transp);
	params.getParam("bg_transp_refract", bg_transp_refract);
	params.getParam("do_AO", do_ao);
	params.getParam("AO_samples", ao_samples);
	params.getParam("AO_distance", ao_dist);
	params.getParam("AO_color", ao_col);

	SppmIntegrator *ite = new SppmIntegrator(num_photons, pass_num, transp_shad, shadow_depth);
	ite->r_depth_ = raydepth;
	ite->max_bounces_ = bounces;
	ite->initial_factor_ = times;

	ite->ds_radius_ = ds_rad; // under tests enable now
	ite->n_search_ = search_num;
	ite->pm_ire_ = pm_ire;
	// Background settings
	ite->transp_background_ = bg_transp;
	ite->transp_refracted_background_ = bg_transp_refract;
	// AO settings
	ite->use_ambient_occlusion_ = do_ao;
	ite->ao_samples_ = ao_samples;
	ite->ao_dist_ = ao_dist;
	ite->ao_col_ = ao_col;

	return ite;
}

END_YAFARAY
