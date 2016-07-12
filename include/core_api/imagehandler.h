/****************************************************************************
 *
 * 		imagehandler.h: image load and save abstract class
 *      This is part of the yafray package
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
 
#ifndef Y_IMAGE_HANDLER_H
#define Y_IMAGE_HANDLER_H

#include "yafray_constants.h"
#include <utilities/image_buffers.h>

#include <fstream>
#include <sstream>
#include <string>
#include <core_api/renderpasses.h>

__BEGIN_YAFRAY

typedef unsigned char yByte;
typedef unsigned short yWord;

enum textureOptimization_t
{
	TEX_OPTIMIZATION_NONE				= 1,
	TEX_OPTIMIZATION_OPTIMIZED			= 2,
	TEX_OPTIMIZATION_COMPRESSED			= 3
};

class YAFRAYCORE_EXPORT imageHandler_t
{
public:
	imageHandler_t():m_width(0), m_height(0), m_hasAlpha(false), m_textureOptimization(TEX_OPTIMIZATION_OPTIMIZED), rgbaOptimizedBuffer(nullptr), rgbaCompressedBuffer(nullptr), rgbOptimizedBuffer(nullptr), rgbCompressedBuffer(nullptr), m_MultiLayer(false) {};

	virtual void initForOutput(int width, int height, const renderPasses_t *renderPasses, bool denoiseEnabled, int denoiseHLum, int denoiseHCol, float denoiseMix, bool withAlpha = false, bool multi_layer = false) = 0;
	virtual ~imageHandler_t() {};
	virtual bool loadFromFile(const std::string &name) = 0;
	virtual bool loadFromMemory(const yByte *data, size_t size) {return false; }
	virtual bool saveToFile(const std::string &name, int imagePassNumber = 0) = 0;
	virtual bool saveToFileMultiChannel(const std::string &name, const renderPasses_t *renderPasses) { return false; };
	virtual void putPixel(int x, int y, const colorA_t &rgba, int imagePassNumber = 0) = 0;
	virtual colorA_t getPixel(int x, int y, int imagePassNumber = 0) = 0;
	virtual int getWidth() { return m_width; }
	virtual int getHeight() { return m_height; }
	virtual bool isHDR() { return false; }
	virtual bool isMultiLayer() { return m_MultiLayer; }
	int getTextureOptimization() { return m_textureOptimization; }
	void setTextureOptimization(int texture_optimization) { m_textureOptimization = texture_optimization; }
	virtual bool denoiseEnabled() { return m_Denoise; }
	std::string getDenoiseParams() const
	{
		if(!m_Denoise) return "";
		std::stringstream paramString;
		paramString << "| Image file denoise enabled [mix=" << m_DenoiseMix << ", h(Luminance)=" << m_DenoiseHLum << ", h(Chrominance)=" <<  m_DenoiseHCol << "]" << yendl;
		return paramString.str();
	}
	
protected:
	std::string handlerName;
	int m_width;
	int m_height;
	bool m_hasAlpha;
	int m_textureOptimization;
	std::vector<rgba2DImage_nw_t*> imagePasses; //!< rgba color buffers for the additional render passes
	rgbaOptimizedImage_nw_t *rgbaOptimizedBuffer;	//!< optimized RGBA (32bit/pixel) with alpha buffer
	rgbaCompressedImage_nw_t *rgbaCompressedBuffer;	//!< compressed RGBA (24bit/pixel) LOSSY! with alpha buffer
	rgbOptimizedImage_nw_t *rgbOptimizedBuffer;	//!< optimized RGB (24bit/pixel) without alpha buffer
	rgbCompressedImage_nw_t *rgbCompressedBuffer;	//!< compressed RGB (16bit/pixel) LOSSY! without alpha buffer
	bool m_MultiLayer = false;
	bool m_Denoise = false;
	int m_DenoiseHLum = 3;
	int m_DenoiseHCol = 3;
	float m_DenoiseMix = 0.8f;	//!< Mix factor between the de-noised image and the original "noisy" image to avoid banding artifacts in images with all noise removed.
};


__END_YAFRAY

#endif
