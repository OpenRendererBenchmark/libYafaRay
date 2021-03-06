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

#include "shader/shader_node_layer.h"
#include "common/param.h"

BEGIN_YAFARAY

LayerNode::LayerNode(unsigned tflag, float col_fac, float val_fac, float def_val, Rgba def_col, MixModes mmod):
		input_(0), upper_layer_(0), texflag_(tflag), colfac_(col_fac), valfac_(val_fac), default_val_(def_val),
		default_col_(def_col), mode_(mmod), do_color_(false), do_scalar_(false), color_input_(false)
{}

void LayerNode::eval(NodeStack &stack, const RenderState &state, const SurfacePoint &sp) const
{
	Rgba rcol, texcolor;
	float rval, tin = 0.f, ta = 1.f, stencil_tin = 1.f;
	// == get result of upper layer (or base values) ==
	rcol = (upper_layer_) ? upper_layer_->getColor(stack) : upper_col_;
	rval = (upper_layer_) ? upper_layer_->getScalar(stack) : upper_val_;
	stencil_tin = rcol.a_;

	// == get texture input color ==
	bool tex_rgb = color_input_;

	if(color_input_)
	{
		texcolor = input_->getColor(stack);
		ta = texcolor.a_;
	}
	else tin = input_->getScalar(stack);

	if(texflag_ & TXF_RGBTOINT)
	{
		tin = texcolor.col2Bri();
		tex_rgb = false;
	}

	if(texflag_ & TXF_NEGATIVE)
	{
		if(tex_rgb) texcolor = Rgba(1.f) - texcolor;
		tin = 1.f - tin;
	}

	float fact;

	if(texflag_ & TXF_STENCIL)
	{
		if(tex_rgb) // only scalar input affects stencil...?
		{
			fact = ta;
			ta *= stencil_tin;
			stencil_tin *= fact;
		}
		else
		{
			fact = tin;
			tin *= stencil_tin;
			stencil_tin *= fact;
		}
	}

	// color type modulation
	if(do_color_)
	{
		if(!tex_rgb) texcolor = default_col_;
		else tin = ta;

		float tin_truncated_range;

		if(tin > 1.f) tin_truncated_range = 1.f;
		else if(tin < 0.f) tin_truncated_range = 0.f;
		else tin_truncated_range = tin;

		rcol = textureRgbBlend__(texcolor, rcol, tin_truncated_range, stencil_tin * colfac_, mode_);
		rcol.clampRgb0();
	}

	// intensity type modulation
	if(do_scalar_)
	{
		if(tex_rgb)
		{
			if(use_alpha_)
			{
				tin = ta;
				if(texflag_ & TXF_NEGATIVE) tin = 1.f - tin;
			}
			else
			{
				tin = texcolor.col2Bri();
			}
		}

		rval = textureValueBlend__(default_val_, rval, tin, stencil_tin * valfac_, mode_);
		if(rval < 0.f) rval = 0.f;
	}
	rcol.a_ = stencil_tin;
	stack[this->id_] = NodeResult(rcol, rval);
}

void LayerNode::eval(NodeStack &stack, const RenderState &state, const SurfacePoint &sp, const Vec3 &wo, const Vec3 &wi) const
{
	eval(stack, state, sp);
}

void LayerNode::evalDerivative(NodeStack &stack, const RenderState &state, const SurfacePoint &sp) const
{
	Rgba texcolor;
	float rdu = 0.f, rdv = 0.f, tdu, tdv;
	float stencil_tin = 1.f;

	// == get result of upper layer (or base values) ==
	if(upper_layer_)
	{
		Rgba ucol = upper_layer_->getColor(stack);
		rdu = ucol.r_, rdv = ucol.g_;
		stencil_tin = ucol.a_;
	}

	// == get texture input derivative ==
	texcolor = input_->getColor(stack);
	tdu = texcolor.r_;
	tdv = texcolor.g_;

	if(texflag_ & TXF_NEGATIVE)
	{
		tdu = -tdu;
		tdv = -tdv;
	}
	// derivative modulation

	rdu += tdu;
	rdv += tdv;

	stack[this->id_] = NodeResult(Rgba(rdu, rdv, 0.f, stencil_tin), 0.f);
}

bool LayerNode::isViewDependant() const
{
	bool view_dep = false;
	if(input_) view_dep = view_dep || input_->isViewDependant();
	if(upper_layer_) view_dep = view_dep || upper_layer_->isViewDependant();
	return view_dep;
}

bool LayerNode::configInputs(const ParamMap &params, const NodeFinder &find)
{
	std::string name;
	if(params.getParam("input", name))
	{
		input_ = find(name);
		if(!input_)
		{
			Y_WARNING << "LayerNode: Couldn't get input " << name << YENDL;
			return false;
		}
	}
	else
	{
		Y_WARNING << "LayerNode: input not set" << YENDL;
		return false;
	}

	if(params.getParam("upper_layer", name))
	{
		upper_layer_ = find(name);
		if(!upper_layer_)
		{
			Y_VERBOSE << "LayerNode: Couldn't get upper_layer " << name << YENDL;
			return false;
		}
	}
	else
	{
		if(!params.getParam("upper_color", upper_col_))
		{
			upper_col_ = Rgb(0.f);
		}
		if(!params.getParam("upper_value", upper_val_))
		{
			upper_val_ = 0.f;
		}
	}
	return true;
}

bool LayerNode::getDependencies(std::vector<const ShaderNode *> &dep) const
{
	// input actually needs to exist, but well...
	if(input_) dep.push_back(input_);
	if(upper_layer_) dep.push_back(upper_layer_);
	return !dep.empty();
}

ShaderNode *LayerNode::factory(const ParamMap &params, RenderEnvironment &render)
{
	Rgb def_col(1.f);
	bool do_color = true, do_scalar = false, color_input = true, use_alpha = false;
	bool stencil = false, no_rgb = false, negative = false;
	double def_val = 1.0, colfac = 1.0, valfac = 1.0;
	int mode = 0;

	params.getParam("mode", mode);
	params.getParam("def_col", def_col);
	params.getParam("colfac", colfac);
	params.getParam("def_val", def_val);
	params.getParam("valfac", valfac);
	params.getParam("do_color", do_color);
	params.getParam("do_scalar", do_scalar);
	params.getParam("color_input", color_input);
	params.getParam("use_alpha", use_alpha);
	params.getParam("noRGB", no_rgb);
	params.getParam("stencil", stencil);
	params.getParam("negative", negative);

	unsigned int flags = 0;
	if(no_rgb) flags |= TXF_RGBTOINT;
	if(stencil) flags |= TXF_STENCIL;
	if(negative) flags |= TXF_NEGATIVE;
	if(use_alpha) flags |= TXF_ALPHAMIX;

	LayerNode *node = new LayerNode(flags, colfac, valfac, def_val, def_col, (MixModes)mode);
	node->do_color_ = do_color;
	node->do_scalar_ = do_scalar;
	node->color_input_ = color_input;
	node->use_alpha_ = use_alpha;

	return node;
}

END_YAFARAY
