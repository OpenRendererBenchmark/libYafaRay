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

#include "integrator/integrator_empty_volume.h"
#include "common/environment.h"
#include "material/material.h"
#include "background/background.h"
#include "light/light.h"
#include "integrator/integrator_utils.h"
#include "common/photon.h"
#include "utility/util_mcqmc.h"
#include "common/scr_halton.h"
#include <vector>

BEGIN_YAFARAY

Rgba EmptyVolumeIntegrator::transmittance(RenderState &state, Ray &ray) const {
	return Rgb(1.f);
}

Rgba EmptyVolumeIntegrator::integrate(RenderState &state, Ray &ray, ColorPasses &color_passes, int additional_depth) const {
	return Rgba(0.f);
}

Integrator *EmptyVolumeIntegrator::factory(ParamMap &params, RenderEnvironment &render) {
	return new EmptyVolumeIntegrator();
}

END_YAFARAY
