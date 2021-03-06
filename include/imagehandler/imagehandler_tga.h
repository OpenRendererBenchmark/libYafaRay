#pragma once
/****************************************************************************
 *
 *      imagehandler_tga.h: Truevision TGA format handler
 *      This is part of the libYafaRay package
 *      Copyright (C) 2010 Rodrigo Placencia Vazquez
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
 *
 */

#ifndef YAFARAY_IMAGEHANDLER_TGA_H
#define YAFARAY_IMAGEHANDLER_TGA_H

#include "imagehandler/imagehandler.h"

BEGIN_YAFARAY

class TgaHandler;
class TgaHeader;

typedef Rgba (TgaHandler::*ColorProcessor_t)(void *data);

class TgaHandler final : public ImageHandler
{
	public:
		static ImageHandler *factory(ParamMap &params, RenderEnvironment &render);

	private:
		TgaHandler();
		virtual ~TgaHandler() override;
		virtual bool loadFromFile(const std::string &name) override;
		virtual bool saveToFile(const std::string &name, int img_index = 0) override;
		void initForInput();

		/*! Image data reading template functions */
		template <class ColorType> void readColorMap(FILE *fp, TgaHeader &header, ColorProcessor_t cp);
		template <class ColorType> void readRleImage(FILE *fp, ColorProcessor_t cp);
		template <class ColorType> void readDirectImage(FILE *fp, ColorProcessor_t cp);

		/*! colorProcesors definitions with signature Rgba (void *)
		to be passed as pointer-to-non-static-member-functions */
		Rgba processGray8(void *data);
		Rgba processGray16(void *data);
		Rgba processColor8(void *data);
		Rgba processColor15(void *data);
		Rgba processColor16(void *data);
		Rgba processColor24(void *data);
		Rgba processColor32(void *data);

		bool precheckFile(TgaHeader &header, const std::string &name, bool &is_gray, bool &is_rle, bool &has_color_map, YByte_t &alpha_bit_depth);

		Rgba2DImage_t *color_map_;
		size_t tot_pixels_;
		size_t min_x_, max_x_, step_x_;
		size_t min_y_, max_y_, step_y_;

};

END_YAFARAY

#endif // YAFARAY_IMAGEHANDLER_TGA_H