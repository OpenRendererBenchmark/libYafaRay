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

#include "volume/volumehandler_sss.h"
#include "common/scene.h"
#include "material/material.h"
#include "common/environment.h"
#include "common/param.h"
#include "utility/util_sample.h"

BEGIN_YAFARAY

SssVolumeHandler::SssVolumeHandler(const Rgb &a_col, const Rgb &s_col, double dist):
		BeerVolumeHandler(a_col, dist), dist_s_(dist), scatter_col_(s_col)
{}

bool SssVolumeHandler::scatter(const RenderState &state, const Ray &ray, Ray &s_ray, PSample &s) const
{
	float dist = -dist_s_ * log(s.s_1_);
	if(dist >= ray.tmax_) return false;
	s_ray.from_ = ray.from_ + dist * ray.dir_;
	s_ray.dir_ = sampleSphere__(s.s_2_, s.s_3_);
	s.color_ = scatter_col_;
	return true;
}

VolumeHandler *SssVolumeHandler::factory(const ParamMap &params, RenderEnvironment &env)
{
	Rgb a_col(0.5f), s_col(0.8f);
	double dist = 1.f;
	params.getParam("absorption_col", a_col);
	params.getParam("absorption_dist", dist);
	params.getParam("scatter_col", s_col);
	return new SssVolumeHandler(a_col, s_col, dist);
}

END_YAFARAY
