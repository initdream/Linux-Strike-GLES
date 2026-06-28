//============ Copyright (c) Valve Corporation, All rights reserved. ============
//
// cglmtex.cpp
//
//===============================================================================

#include <vprof.h>
#include "togl/rendermechanism.h"
#include "tier0/icommandline.h"
#include "glmtexinlines.h"

extern "C" {
	#include "decompress.h"
}

// memdbgon -must- be the last include file in a .cpp file.
#include "tier0/memdbgon.h"

#if defined(OSX)
#include "appframework/ilaunchermgr.h"
extern ILauncherMgr *g_pLauncherMgr;
#endif

//===============================================================================

#if GLMDEBUG
CGLMTex *g_pFirstCGMLTex;
#endif
#define TEXSPACE_LOGGING 0

bool pwroftwo (int val )
{
	return (val & (val-1)) == 0;
}

int	sEncodeLayoutAsIndex( GLMTexLayoutKey *key )
{
	int index = 0;
	if (key->m_texFlags & kGLMTexMipped) index |= 1;
	if ( ! ( pwroftwo(key->m_xSize) && pwroftwo(key->m_ySize) && pwroftwo(key->m_zSize) ) ) index |= 2;
	if (GetFormatDesc( key->m_texFormat )->m_chunkSize >1 ) index |= 4;
	return index;
}

static unsigned long g_texGlobalBytes[8];

//===============================================================================

const GLMTexFormatDesc g_formatDescTable[] =
{
	{ "_D16",			D3DFMT_D16,				GL_DEPTH_COMPONENT16,				0,									GL_DEPTH_COMPONENT,		GL_UNSIGNED_SHORT,				1, 2 },
	{ "_D24X8",			D3DFMT_D24X8,			GL_DEPTH_COMPONENT24,				0,									GL_DEPTH_COMPONENT,		GL_UNSIGNED_INT,				1, 4 },
	{ "_D24S8",			D3DFMT_D24S8,			GL_DEPTH24_STENCIL8,				0,									GL_DEPTH_STENCIL,		GL_UNSIGNED_INT_24_8,			1, 4 },

	{ "_A8R8G8B8",		D3DFMT_A8R8G8B8,		GL_RGBA8,							GL_SRGB8_ALPHA8,					GL_BGRA,				GL_UNSIGNED_INT_8_8_8_8_REV,	1, 4 },
	{ "_A4R4G4B4",		D3DFMT_A4R4G4B4,		GL_RGBA4,							0,									GL_BGRA,				GL_UNSIGNED_SHORT_4_4_4_4_REV,	1, 2 },
	{ "_X8R8G8B8",		D3DFMT_X8R8G8B8,		GL_RGB8,							GL_SRGB8,							GL_BGRA,				GL_UNSIGNED_INT_8_8_8_8_REV,	1, 4 },

	{ "_X1R5G5B5",		D3DFMT_X1R5G5B5,		GL_RGB5_A1,							0,									GL_BGRA,				GL_UNSIGNED_SHORT_1_5_5_5_REV,	1, 2 },
	{ "_A1R5G5B5",		D3DFMT_A1R5G5B5,		GL_RGB5_A1,							0,									GL_BGRA,				GL_UNSIGNED_SHORT_1_5_5_5_REV,	1, 2 },

	{ "_L8",			D3DFMT_L8,				GL_LUMINANCE,						GL_SLUMINANCE_EXT,					GL_LUMINANCE,			GL_UNSIGNED_BYTE,				1, 1 },
	{ "_A8L8",			D3DFMT_A8L8,			GL_LUMINANCE_ALPHA,					GL_SLUMINANCE_ALPHA_EXT,			GL_LUMINANCE_ALPHA,		GL_UNSIGNED_BYTE,				1, 2 },

	{ "_DXT1",			D3DFMT_DXT1,			GL_COMPRESSED_RGB_S3TC_DXT1_EXT,	GL_COMPRESSED_SRGB_S3TC_DXT1_EXT,		GL_RGB,				GL_UNSIGNED_BYTE,				4, 8 },
	{ "_DXT3",			D3DFMT_DXT3,			GL_COMPRESSED_RGBA_S3TC_DXT3_EXT,	GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT,	GL_RGBA,			GL_UNSIGNED_BYTE,				4, 16 },
	{ "_DXT5",			D3DFMT_DXT5,			GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,	GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT,	GL_RGBA,			GL_UNSIGNED_BYTE,				4, 16 },

	{ "_A16B16G16R16F",	D3DFMT_A16B16G16R16F,	GL_RGBA16F,							0,									GL_RGBA,				GL_HALF_FLOAT,					1, 8 },
	{ "_A16B16G16R16",	D3DFMT_A16B16G16R16,	GL_RGBA16UI,						0,									GL_RGBA_INTEGER,		GL_UNSIGNED_SHORT,				1, 8 },

	{ "_A32B32G32R32F",	D3DFMT_A32B32G32R32F,	GL_RGBA32F,							0,									GL_RGBA,				GL_FLOAT,						1, 16 },
	{ "_R8G8B8",		D3DFMT_R8G8B8,			GL_RGB8,							GL_SRGB8,							GL_BGR,					GL_UNSIGNED_BYTE,				1, 3 },
	{ "_A8",			D3DFMT_A8,				GL_ALPHA,							0,									GL_ALPHA,				GL_UNSIGNED_BYTE,				1, 1 },
	{ "_R5G6B5",		D3DFMT_R5G6B5,			GL_RGB565,							GL_SRGB8,							GL_RGB,					GL_UNSIGNED_SHORT_5_6_5,		1, 2 },
	{ "_Q8W8V8U8",		D3DFMT_Q8W8V8U8,		GL_RGBA8,							0,									GL_BGRA,				GL_UNSIGNED_INT_8_8_8_8_REV,	1, 4 },
	{ "_V8U8",			D3DFMT_V8U8,			GL_RG8,								0,									GL_RG,					GL_BYTE,						1, 2 },
	{ "_R32F",			D3DFMT_R32F,			GL_R32F,							GL_R32F,							GL_RED,					GL_FLOAT,						1, 4 },
	{ "_A2R10G10B10",	D3DFMT_A2R10G10B10,		GL_RGB10_A2,						GL_RGB10_A2,						GL_RGBA,				GL_UNSIGNED_INT_2_10_10_10_REV,	1, 4 },
	{ "_A2B10G10R10",	D3DFMT_A2B10G10R10,		GL_RGB10_A2,						GL_RGB10_A2,						GL_BGRA,				GL_UNSIGNED_INT_2_10_10_10_REV,	1, 4 },
};

int	g_formatDescTableCount = sizeof(g_formatDescTable) / sizeof( g_formatDescTable[0] );

const GLMTexFormatDesc *GetFormatDesc( D3DFORMAT format )
{
	for( int i=0; i<g_formatDescTableCount; i++)
	{
		if (g_formatDescTable[i].m_d3dFormat == format)
			return &g_formatDescTable[i];
	}
	return (const GLMTexFormatDesc *)NULL;
}

//===============================================================================

void convert_texture( GLenum &internalformat, GLsizei width, GLsizei height, GLenum &format, GLenum &type, void *data )
{
	// Convert unsupported D3D native BGRA formats to standard RGBA for GLES 3.0
	if( format == GL_BGRA ) format = GL_RGBA;
	if( format == GL_BGR ) format = GL_RGB;

	if( internalformat == GL_SRGB8 && format == GL_RGBA )
		internalformat = GL_SRGB8_ALPHA8;

	// Note: GLES 3.0 natively uses RED instead of LUMINANCE but we maintain backwards compatibility macros where needed
	if( format == GL_LUMINANCE || format == GL_LUMINANCE_ALPHA )
		internalformat = format;

	if( data )
	{
		// Manually downsample 16-bit texture to 8-bit if EXT_texture_norm16 is missing
		if( internalformat == GL_RGBA16 && !gGL->m_bHave_GL_EXT_texture_norm16 )
		{
			uint16_t *_data = (uint16_t*)data;
			uint8_t *new_data = (uint8_t*)data;

			for( int i = 0; i < width*height*4; i+=4 )
			{
				new_data[i] = _data[i] >> 8;
				new_data[i+1] = _data[i+1] >> 8;
				new_data[i+2] = _data[i+2] >> 8;
				new_data[i+3] = _data[i+3] >> 8;
			}
		}
	}

	if( internalformat == GL_RGBA16 && !gGL->m_bHave_GL_EXT_texture_norm16 )
	{
		internalformat = GL_RGBA8;
		format = GL_RGBA;
		type = GL_UNSIGNED_BYTE;
	}

	if( type == GL_UNSIGNED_INT_8_8_8_8_REV )
		type = GL_UNSIGNED_BYTE;
}

GLboolean isDXTc(GLenum format) {
	switch (format) {
		case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
		case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
		case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
		case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
		case GL_COMPRESSED_SRGB_S3TC_DXT1_EXT:
		case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT:
		case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT:
		case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:
			return 1;
	}
	return 0;
}

GLboolean isDXTcSRGB(GLenum format) {
	switch (format) {
		case GL_COMPRESSED_SRGB_S3TC_DXT1_EXT:
		case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT:
		case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT:
		case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:
			return 1;
	}
	return 0;
}

static GLboolean isDXTcAlpha(GLenum format) {
	switch (format) {
		case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
		case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
		case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
		case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT:
		case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT:
		case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:
			return 1;
	}
	return 0;
}

GLvoid *uncompressDXTc(GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, int transparent0, int* simpleAlpha, int* complexAlpha, const GLvoid *data) {
	int pixelsize = 4;
	if (format == GL_COMPRESSED_RGB_S3TC_DXT1_EXT || format == GL_COMPRESSED_SRGB_S3TC_DXT1_EXT)
		pixelsize = 3;

	if (imageSize == width*height*pixelsize || data==NULL) {
		return (GLvoid*)data;
	}

	GLvoid *pixels = malloc(((width+3)&~3)*((height+3)&~3)*pixelsize);

	int blocksize;
	switch (format) {
		case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
		case GL_COMPRESSED_SRGB_S3TC_DXT1_EXT:
		case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
		case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT:
			blocksize = 8;
			break;
		case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
		case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
		case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT:
		case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:
			blocksize = 16;
			break;
	}
	uintptr_t src = (uintptr_t) data;
	for (int y=0; y<height; y+=4) {
		for (int x=0; x<width; x+=4) {
			switch(format) {
				case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
				case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
				case GL_COMPRESSED_SRGB_S3TC_DXT1_EXT:
				case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT:
					DecompressBlockDXT1(x, y, width, (uint8_t*)src, transparent0, simpleAlpha, complexAlpha, (uint32_t*)pixels);
					break;
				case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
				case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT:
					DecompressBlockDXT3(x, y, width, (uint8_t*)src, transparent0, simpleAlpha, complexAlpha, (uint32_t*)pixels);
					break;
				case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
				case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:
					DecompressBlockDXT5(x, y, width, (uint8_t*)src, transparent0, simpleAlpha, complexAlpha, (uint32_t*)pixels);
					break;
			}
			src+=blocksize;
		}
	}
	return pixels;
}

void CompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const GLvoid *data)
{
	if (internalformat==GL_RGBA8)
		internalformat = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;

	if ((width<=0) || (height<=0)) return;

	bool hasAlpha = (internalformat != GL_COMPRESSED_RGB_S3TC_DXT1_EXT) && (internalformat != GL_COMPRESSED_SRGB_S3TC_DXT1_EXT);

	GLenum format = hasAlpha ? GL_RGBA : GL_RGB;
	GLenum intformat = hasAlpha ? GL_RGBA8 : GL_RGB8;
	GLenum type = GL_UNSIGNED_BYTE;
	GLvoid *pixels = NULL;

	if (isDXTc(internalformat))
	{
		int srgb = isDXTcSRGB(internalformat);
		int simpleAlpha = 0;
		int complexAlpha = 0;
		int transparent0 = (internalformat==GL_COMPRESSED_RGBA_S3TC_DXT1_EXT || internalformat==GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT)?1:0;

		if (data) {
			pixels = uncompressDXTc(width, height, internalformat, imageSize, transparent0, &simpleAlpha, &complexAlpha, data);
		} else {
			if(isDXTcAlpha(internalformat)) simpleAlpha = complexAlpha = 1;
		}

		if( srgb ) intformat = hasAlpha ? GL_SRGB8_ALPHA8 : GL_SRGB8;
	}

	gGL->glTexImage2D(target, level, intformat, width, height, border, format, type, pixels);
	if( data != pixels && pixels != NULL )
		free(pixels);
}

// ... The rest of the file (CGLMTexLayoutTable mapping, CGLMTex::WriteTexels, CGLMTex::Lock/Unlock) is logically identical
// to your existing file, just call CompressedTexImage2D where compression extensions are requested.
