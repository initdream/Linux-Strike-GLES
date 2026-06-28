//============ Copyright (c) Valve Corporation, All rights reserved. ============
//
// cglmfbo.cpp
//
//===============================================================================

#include "togl/rendermechanism.h"
#include "tier0/memdbgon.h"

CGLMFBO::CGLMFBO( GLMContext *ctx )
{
	m_ctx = ctx;
	m_ctx->CheckCurrent();

	gGL->glGenFramebuffers( 1, &m_name );

	memset( m_attach, 0, sizeof( m_attach ) );
}

CGLMFBO::~CGLMFBO( )
{
	m_ctx->CheckCurrent();

	// detach all known attached textures first... necessary ?
	for( int index = 0; index < kAttCount; index++)
	{
		if (m_attach[ index ].m_tex)
		{
			TexDetach( (EGLMFBOAttachment)index, GL_DRAW_FRAMEBUFFER );
		}
	}

	gGL->glDeleteFramebuffers( 1, &m_name );

	m_name = 0;
	m_ctx = NULL;
}

static GLenum EncodeAttachmentFBO( EGLMFBOAttachment index )
{
	if (index < kAttDepth)
	{
		return GL_COLOR_ATTACHMENT0 + (int) index;
	}
	else
	{
		switch( index )
		{
			case kAttDepth:			return GL_DEPTH_ATTACHMENT;
			case kAttStencil:		return GL_STENCIL_ATTACHMENT;
			case kAttDepthStencil:	return GL_DEPTH_STENCIL_ATTACHMENT;
			default:
				GLMStop(); // bad news
				break;
		}
	}

	GLMStop(); // bad news
	return GL_COLOR_ATTACHMENT0;
}

void CGLMFBO::TexAttach( GLMFBOTexAttachParams *params, EGLMFBOAttachment attachIndex, GLenum fboBindPoint )
{
	m_ctx->MakeCurrent();
	m_ctx->BindFBOToCtx( this, fboBindPoint );

	CGLMTex *tex = params->m_tex;

	this->TexDetach( attachIndex, fboBindPoint );

	if (!tex)
		return;

	GLMTexLayout	*layout = tex->m_layout;
	GLenum			target = tex->m_layout->m_key.m_texGLTarget;
	GLenum			attachIndexGL = EncodeAttachmentFBO( attachIndex );

	switch( target )
	{
		case GL_TEXTURE_2D:
		{
			bool useRBO = false;

			if (layout->m_key.m_texFlags & kGLMTexMultisampled)
			{
				if (fboBindPoint == GL_READ_FRAMEBUFFER)
					Assert( tex->IsRBODirty() == false );
				else
					useRBO = true;
			}

			if (useRBO)
			{
				if (attachIndexGL==GL_DEPTH_STENCIL_ATTACHMENT)
				{
					gGL->glBindRenderbuffer( GL_RENDERBUFFER, tex->m_rboName );
					gGL->glFramebufferRenderbuffer( fboBindPoint, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, tex->m_rboName);
					gGL->glFramebufferRenderbuffer( fboBindPoint, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, tex->m_rboName);
					gGL->glBindRenderbuffer( GL_RENDERBUFFER, 0 );
				}
				else
				{
					gGL->glBindRenderbuffer( GL_RENDERBUFFER, tex->m_rboName );
					gGL->glFramebufferRenderbuffer( fboBindPoint, attachIndexGL, GL_RENDERBUFFER, tex->m_rboName);
					gGL->glBindRenderbuffer( GL_RENDERBUFFER, 0 );
				}
				tex->ForceRBODirty();
			}
			else
			{
				if (attachIndexGL==GL_DEPTH_STENCIL_ATTACHMENT)
				{
					gGL->glFramebufferTexture2D( fboBindPoint, GL_DEPTH_ATTACHMENT, target, tex->m_texName, params->m_mip );
					gGL->glFramebufferTexture2D( fboBindPoint, GL_STENCIL_ATTACHMENT, target, tex->m_texName, params->m_mip );
				}
				else
				{
					gGL->glFramebufferTexture2D( fboBindPoint, attachIndexGL, target, tex->m_texName, params->m_mip );
				}
			}
		}
		break;

		case GL_TEXTURE_3D:
		{
			// GLES 3.0 uses FramebufferTextureLayer for 3D textures
			if (gGL->glFramebufferTextureLayer)
				gGL->glFramebufferTextureLayer( fboBindPoint, attachIndexGL, tex->m_texName, params->m_mip, params->m_zslice );
			else
				gGL->glFramebufferTexture3D( fboBindPoint, attachIndexGL, target, tex->m_texName, params->m_mip, params->m_zslice );
		}
		break;

		case GL_TEXTURE_CUBE_MAP:
		{
			target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + params->m_face;
			gGL->glFramebufferTexture2D( fboBindPoint, attachIndexGL, target, tex->m_texName, params->m_mip );
		}
		break;
	}

	m_attach[ attachIndex ] = *params;
	tex->m_rtAttachCount++;
}

void CGLMFBO::TexDetach( EGLMFBOAttachment attachIndex, GLenum fboBindPoint )
{
	m_ctx->MakeCurrent();
	m_ctx->BindFBOToCtx( this, fboBindPoint );

	if (m_attach[ attachIndex ].m_tex)
	{
		CGLMTex			*tex = m_attach[ attachIndex ].m_tex;
		GLMTexLayout	*layout = tex->m_layout;
		GLenum			target = tex->m_layout->m_key.m_texGLTarget;
		GLenum			attachIndexGL = EncodeAttachmentFBO( attachIndex );

		switch( target )
		{
			case GL_TEXTURE_2D:
			{
				if (layout->m_key.m_texFlags & kGLMTexMultisampled)
				{
					gGL->glBindRenderbuffer( GL_RENDERBUFFER, 0 );

					if (attachIndexGL==GL_DEPTH_STENCIL_ATTACHMENT)
					{
						gGL->glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
						gGL->glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
					}
					else
					{
						gGL->glFramebufferRenderbuffer( GL_FRAMEBUFFER, attachIndexGL, GL_RENDERBUFFER, 0);
					}
				}
				else
				{
					if (attachIndexGL==GL_DEPTH_STENCIL_ATTACHMENT)
					{
						gGL->glFramebufferTexture2D( fboBindPoint, GL_DEPTH_ATTACHMENT, target, 0, 0 );
						gGL->glFramebufferTexture2D( fboBindPoint, GL_STENCIL_ATTACHMENT, target, 0, 0 );
					}
					else
					{
						gGL->glFramebufferTexture2D( fboBindPoint, attachIndexGL, target, 0, 0 );
					}
				}
			}
			break;

			case GL_TEXTURE_3D:
			{
				if (gGL->glFramebufferTextureLayer)
					gGL->glFramebufferTextureLayer( fboBindPoint, attachIndexGL, 0, 0, 0 );
				else
					gGL->glFramebufferTexture3D( fboBindPoint, attachIndexGL, target, 0, 0, 0 );
			}
			break;

			case GL_TEXTURE_CUBE_MAP:
			{
				gGL->glFramebufferTexture2D( fboBindPoint, attachIndexGL, target, 0, 0 );
			}
			break;
		}

		memset( &m_attach[ attachIndex ], 0, sizeof( m_attach[0] ) );
		tex->m_rtAttachCount--;
	}
}

void CGLMFBO::TexScrub( CGLMTex *tex )
{
	for( int attachIndex = 0; attachIndex < kAttCount; attachIndex++ )
	{
		if (m_attach[ attachIndex ].m_tex == tex)
		{
			TexDetach( (EGLMFBOAttachment)attachIndex, GL_DRAW_FRAMEBUFFER );
		}
	}
}

bool CGLMFBO::IsReady( void )
{
	bool result = false;
	m_ctx->CheckCurrent();
	m_ctx->BindFBOToCtx( this );

	GLenum status = gGL->glCheckFramebufferStatus(GL_FRAMEBUFFER);
	switch(status)
	{
		case GL_FRAMEBUFFER_COMPLETE:
			result = true;
			break;
		case GL_FRAMEBUFFER_UNSUPPORTED:
			result = false;
			DebuggerBreak();
			break;
		default:
			result = false;
			DebuggerBreak();
			break;
	}
	return result;
}
