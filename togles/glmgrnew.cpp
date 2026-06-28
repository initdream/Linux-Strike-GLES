//============ Copyright (c) Valve Corporation, All rights reserved. ============
//
// glmgr.cpp
//
//===============================================================================
#include "togl/rendermechanism.h"
#include "tier0/icommandline.h"
#include "tier0/vprof.h"
#include "glmtexinlines.h"
#include "materialsystem/IShader.h"
#include "appframework/ilaunchermgr.h"
#include "convar.h"
#include "glmgr_flush.inl"
#include "tier0/memdbgon.h"

ConVar gl_debug_output( "gl_debug_output", "1" );
ConVar gl_swap_limit( "gl_swap_limit", "1", FCVAR_RELEASE );

uint g_nTotalDrawsOrClears, g_nTotalVBLockBytes, g_nTotalIBLockBytes;

#if GL_TELEMETRY_GPU_ZONES
TelemetryGPUStats_t g_TelemetryGPUStats;
#endif

// Valid GLES 3.0 Dummy Shaders
char g_nullFragmentProgramText [] =
{
	"#version 300 es\n"
	"precision mediump float;\n"
	"out vec4 _gl_FragColor;\n"
	"void main()\n"
	"{\n"
	"_gl_FragColor = vec4( 0.0, 0.0, 0.0, 1.0 );\n"
	"}\n"
};

char g_preloadTexVertexProgramText[] =
{
	"#version 300 es\n"
	"in vec4 vertex;\n"
	"out vec4 otex;  \n"
	"void main()  \n"
	"{  \n"
	"gl_Position = vertex;  \n"
	"otex = vec4(0.0);  \n"
	"}  \n"
};

char g_preload2DTexFragmentProgramText[] =
{
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec4 otex;  \n"
	"out vec4 _gl_FragColor;\n"
	"uniform sampler2D sampler15;  \n"
	"void main()  \n"
	"{  \n"
	"_gl_FragColor = texture( sampler15, otex.xy );  \n"
	"}  \n"
};

char g_preload3DTexFragmentProgramText[] =
{
	"#version 300 es\n"
	"precision mediump float;\n"
	"precision mediump sampler3D;\n"
	"in vec4 otex;  \n"
	"out vec4 _gl_FragColor;\n"
	"uniform sampler3D sampler15;  \n"
	"void main()  \n"
	"{  \n"
	"_gl_FragColor = texture( sampler15, otex.xyz );  \n"
	"}  \n"
};

char g_preloadCubeTexFragmentProgramText[] =
{
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec4 otex;  \n"
	"out vec4 _gl_FragColor;\n"
	"uniform samplerCube sampler15;  \n"
	"void main()  \n"
	"{  \n"
	"_gl_FragColor = texture( sampler15, otex.xyz );  \n"
	"}  \n"
};

const char* glSourceToString(GLenum source) { return "UNKNOWN"; }
const char* glTypeToString(GLenum type) { return "UNKNOWN"; }
const char* glSeverityToString(GLenum severity) { return "UNKNOWN"; }

bool g_bDebugOutputBreakpoints = true;

void APIENTRY GL_Debug_Output_Callback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, GLvoid* userParam)
{
	if (  ( type == GL_DEBUG_TYPE_ERROR ) && strstr( message, "base level inconsistent" ) ) return;
	if ( gl_debug_output.GetBool() || type == GL_DEBUG_TYPE_ERROR )
	{
		Msg("GL: [%d]: %s\n", id, message);
	}
}

void GLMDebugPrintf( const char *pMsg, ... )
{
	va_list args;
	va_start( args, pMsg );
	char buf[1024];
	V_vsnprintf( buf, sizeof( buf ), pMsg, args );
	va_end( args );
	Plat_DebugString( buf );
}

inline bool MakeContextCurrent( PseudoGLContextPtr hContext ) { return g_pLauncherMgr->MakeContextCurrent( hContext ); }
inline PseudoGLContextPtr GetMainContext() { return g_pLauncherMgr->GetMainContext(); }
inline PseudoGLContextPtr GetGLContextForWindow( void* windowref ) { return g_pLauncherMgr->GetGLContextForWindow( windowref ); }
inline void IncrementWindowRefCount() {}
inline void DecrementWindowRefCount() {}
inline void ShowPixels( CShowPixelsParams *params ) { g_pLauncherMgr->ShowPixels(params); }
inline void DisplayedSize( uint &width, uint &height ) { g_pLauncherMgr->DisplayedSize( width, height ); }
inline void GetDesiredPixelFormatAttribsAndRendererInfo( uint **ptrOut, uint *countOut, GLMRendererInfoFields *rendInfoOut ) { g_pLauncherMgr->GetDesiredPixelFormatAttribsAndRendererInfo( ptrOut, countOut, rendInfoOut ); }
inline void GetStackCrawl( CStackCrawlParams *params ) { g_pLauncherMgr->GetStackCrawl(params); }

static bool hasnonzeros( float *values, int count )
{
	for( int i=0; i<count; i++) if (values[i] != 0.0) return true;
	return false;
}

static void printmat( char *label, int baseSlotNumber, int slots, float *m00 ) { }
static void transform_dp4( float *in4, float *m00, int slots, float *out4 ) { }

GLMgr	*g_glmgr = NULL;
void GLMgr::NewGLMgr( void ) { if (!g_glmgr) g_glmgr = new GLMgr; }
GLMgr *GLMgr::aGLMgr( void ) { assert( g_glmgr != NULL); return g_glmgr; }
void GLMgr::DelGLMgr( void ) { if (g_glmgr) { delete g_glmgr; g_glmgr = NULL; } }
GLMgr::GLMgr() {}	
GLMgr::~GLMgr() {}

GLMContext *GLMgr::NewContext( IDirect3DDevice9 *pDevice, GLMDisplayParams *params ) { return new GLMContext( pDevice, params ); }
void GLMgr::DelContext( GLMContext *context ) { delete context; }

void GLMgr::SetCurrentContext( GLMContext *context )
{
#if defined( USE_SDL )
	context->m_nCurOwnerThreadId = ThreadGetCurrentId();
	MakeContextCurrent( context->m_ctx );
#endif
}

GLMContext *GLMgr::GetCurrentContext( void )
{
#if defined( USE_SDL )
	PseudoGLContextPtr context = GetMainContext();
	return (GLMContext*) context;
#endif
	return NULL;
}
	
void GLMContext::MakeCurrent( bool bRenderThread )
{
	TM_ZONE( TELEMETRY_LEVEL0, 0, "GLMContext::MakeCurrent" );
#if defined( USE_SDL )
	uint32 dwThreadId = ThreadGetCurrentId();
	if ( bRenderThread || dwThreadId == m_dwRenderThreadId )
	{
		m_nCurOwnerThreadId = ThreadGetCurrentId();
		m_dwRenderThreadId = dwThreadId;
		MakeContextCurrent( m_ctx );
		m_bIsThreading = true;
	}
	else if ( !m_bIsThreading )
	{
		m_nCurOwnerThreadId = ThreadGetCurrentId();
		MakeContextCurrent( m_ctx );
	}
#endif
}

void GLMContext::ReleaseCurrent( bool bRenderThread )
{
	TM_ZONE( TELEMETRY_LEVEL0, 0, "GLMContext::ReleaseCurrent" );
#if defined( USE_SDL )
	m_nCurOwnerThreadId = 0;
	m_nThreadOwnershipReleaseCounter++;
	MakeContextCurrent( NULL );
	if ( bRenderThread ) m_bIsThreading = false;
#endif
}

void GLMContext::ForceFlushStates()
{
	m_AlphaTestEnable.Flush(); m_AlphaTestFunc.Flush(); m_DepthBias.Flush();
	m_ScissorEnable.Flush(); m_ScissorBox.Flush(); m_ViewportBox.Flush();		
	m_ViewportDepthRange.Flush(); m_ColorMaskSingle.Flush(); m_BlendEnable.Flush();
	m_BlendFactor.Flush(); m_BlendEnableSRGB.Flush(); m_DepthTestEnable.Flush();
	m_DepthFunc.Flush(); m_DepthMask.Flush(); m_StencilTestEnable.Flush();
	m_StencilFunc.Flush(); m_StencilOp.Flush(); m_StencilWriteMask.Flush();
	m_ClearColor.Flush(); m_ClearDepth.Flush(); m_ClearStencil.Flush();
	m_ClipPlaneEnable.Flush(); m_ClipPlaneEquation.Flush(); m_CullFaceEnable.Flush();
	m_CullFrontFace.Flush(); m_PolygonMode.Flush(); m_AlphaToCoverageEnable.Flush();
	m_ColorMaskMultiple.Flush(); m_BlendEquation.Flush(); m_BlendColor.Flush();

	m_activeTexture = -1;
	for ( int i = 0; i < GLM_SAMPLER_COUNT; i++ )
	{
		SetSamplerTex( i, m_samplers[i].m_pBoundTex );
		SetSamplerDirty( i );
	}

	ClearCurAttribs();
	m_lastKnownVertexAttribMask = 0;
	m_nNumSetVertexAttributes = 16;
	memset( &m_boundVertexAttribs[0], 0xFF, sizeof( m_boundVertexAttribs ) );
	for( int index=0; index < kGLMVertexAttributeIndexMax; index++ )
		gGL->glDisableVertexAttribArray( index );

	NullProgram();

    // Stripped EXT suffixes
	BindFBOToCtx( m_boundReadFBO, GL_READ_FRAMEBUFFER );
	BindFBOToCtx( m_boundDrawFBO, GL_DRAW_FRAMEBUFFER );

	gGL->glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, m_nBoundGLBuffer[ kGLMIndexBuffer] );
	gGL->glBindBuffer( GL_ARRAY_BUFFER, m_nBoundGLBuffer[ kGLMVertexBuffer] );
}

const GLMRendererInfoFields& GLMContext::Caps( void ) { return m_caps; }
void GLMContext::DumpCaps( void ) {}

CGLMTex	*GLMContext::NewTex( GLMTexLayoutKey *key, uint levels, const char *debugLabel )
{
	GLMTexLayout *layout = m_texLayoutTable->NewLayoutRef( key );
	CGLMTex *tex = new CGLMTex( this, layout, levels, debugLabel );
	return tex;
}

void GLMContext::DelTex( CGLMTex * tex ) { m_DeleteTextureQueue.PushItem(tex); }

void GLMContext::ProcessTextureDeletes()
{
	CGLMTex* tex = nullptr;
	while ( m_DeleteTextureQueue.PopItem( &tex ) )
	{
		for( int i = 0; i < GLM_SAMPLER_COUNT; i++)
		{
			if ( m_samplers[i].m_pBoundTex == tex ) BindTexToTMU( NULL, i );
		}
			
		if ( tex->m_rtAttachCount != 0 ) GLMDebugPrintf("GLMContext::DelTex: Leaking tex %08x",tex );
		else delete tex;
	}
}

ConVar gl_radar7954721_workaround_mixed ( "gl_radar7954721_workaround_mixed", "1" );
ConVar gl_radar7954721_workaround_all ( "gl_radar7954721_workaround_all", "0" );
ConVar gl_radar7954721_workaround_maskval ( "gl_radar7954721_workaround_maskval", "0" );

enum eBlitFormatClass { eColor, eDepth, eDepthStencil };
uint glAttachFromClass[ 3 ] = { GL_COLOR_ATTACHMENT0, GL_DEPTH_ATTACHMENT, GL_DEPTH_STENCIL_ATTACHMENT };

void glScrubFBO( GLenum target )
{
	gGL->glFramebufferRenderbuffer( target, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, 0);
	gGL->glFramebufferRenderbuffer( target, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
	gGL->glFramebufferRenderbuffer( target, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
	gGL->glFramebufferTexture2D( target, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0 );
	gGL->glFramebufferTexture2D( target, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0 );
	gGL->glFramebufferTexture2D( target, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0 );
}

void glAttachRBOtoFBO( GLenum target, eBlitFormatClass formatClass, uint rboName )
{
	switch( formatClass )
	{
		case eColor: gGL->glFramebufferRenderbuffer( target, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rboName); break;
		case eDepth: gGL->glFramebufferRenderbuffer( target, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rboName); break;
		case eDepthStencil:
			gGL->glFramebufferRenderbuffer( target, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rboName);
			gGL->glFramebufferRenderbuffer( target, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rboName);
		break;
	}
}

void glAttachTex2DtoFBO( GLenum target, eBlitFormatClass formatClass, uint texName, uint texMip )
{
	switch( formatClass )
	{
		case eColor: gGL->glFramebufferTexture2D( target, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texName, texMip ); break;
		case eDepth: gGL->glFramebufferTexture2D( target, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, texName, texMip ); break;
		case eDepthStencil: gGL->glFramebufferTexture2D( target, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, texName, texMip ); break;
	}
}

ConVar gl_can_resolve_flipped("gl_can_resolve_flipped", "0" );
ConVar gl_cannot_resolve_flipped("gl_cannot_resolve_flipped", "0" );
ConVar gl_minify_resolve_mode("gl_minify_resolve_mode", "1" );		
ConVar gl_magnify_resolve_mode("gl_magnify_resolve_mode", "2" );	

void GLMContext::SaveColorMaskAndSetToDefault()
{
	m_ColorMaskSingle.Read( &m_SavedColorMask, 0 );
	GLColorMaskSingle_t newColorMask;
	newColorMask.r = newColorMask.g = newColorMask.b = newColorMask.a = -1;
	m_ColorMaskSingle.Write( &newColorMask );
}

void GLMContext::RestoreSavedColorMask()
{
	m_ColorMaskSingle.Write( &m_SavedColorMask );
}

void GLMContext::Blit2( CGLMTex *srcTex, GLMRect *srcRect, int srcFace, int srcMip, CGLMTex *dstTex, GLMRect *dstRect, int dstFace, int dstMip, uint filter )
{
	SaveColorMaskAndSetToDefault();
	Assert( srcFace == 0 ); Assert( dstFace == 0 );

	eBlitFormatClass formatClass = eColor;
	uint blitMask= 0;

	switch( srcTex->m_layout->m_format->m_glDataFormat )
	{
		case GL_RED: case GL_BGRA:	case GL_RGB:	case GL_RGBA:	case GL_ALPHA:	case GL_LUMINANCE:	case GL_LUMINANCE_ALPHA:
			formatClass = eColor; blitMask = GL_COLOR_BUFFER_BIT; break;
		case GL_DEPTH_COMPONENT:
			formatClass = eDepth; blitMask = GL_DEPTH_BUFFER_BIT; break;
		case GL_DEPTH_STENCIL:
			formatClass = eDepthStencil; blitMask = GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT; break;
		default: GLMStop(); break;
	}

	bool blitResolves	=	srcTex->m_rboName != 0;
	bool blitScales		=	((srcRect->xmax - srcRect->xmin) != (dstRect->xmax - dstRect->xmin)) || ((srcRect->ymax - srcRect->ymin) != (dstRect->ymax - dstRect->ymin));
	bool blitToBack		=	(dstTex == NULL);
	bool blitFlips		=	blitToBack;			

	bool blitTwoStep = false;		
	
	if (blitResolves && (blitFlips||blitToBack))		
	{
		if( gl_cannot_resolve_flipped.GetInt() ) blitTwoStep = true;
		else if (!gl_can_resolve_flipped.GetInt()) blitTwoStep = blitTwoStep || m_caps.m_cantResolveFlipped;	
	}

	if (!blitTwoStep)
	{
		if (blitResolves && blitScales)
		{
			if (m_caps.m_cantResolveScaled) blitTwoStep = true;
			else filter = GL_NEAREST;
		}	
	}

	GLScissorEnable_t	oldsciss,newsciss;
	m_ScissorEnable.Read( &oldsciss, 0 );

	if (oldsciss.enable)
	{
		newsciss.enable = false;
		m_ScissorEnable.Write( &newsciss );
	}

	if (blitTwoStep)
	{
		BindFBOToCtx( m_scratchFBO[0], GL_READ_FRAMEBUFFER );
		glScrubFBO( GL_READ_FRAMEBUFFER );
		glAttachRBOtoFBO( GL_READ_FRAMEBUFFER, formatClass, srcTex->m_rboName );
		
		BindFBOToCtx( m_scratchFBO[1], GL_DRAW_FRAMEBUFFER );
		glScrubFBO( GL_DRAW_FRAMEBUFFER );
		glAttachTex2DtoFBO( GL_DRAW_FRAMEBUFFER, formatClass, srcTex->m_texName, 0 );

		gGL->glReadBuffer( glAttachFromClass[formatClass] );
		gGL->glDrawBuffers( 1, &glAttachFromClass[formatClass] );
		
		gGL->glBlitFramebuffer(	0, 0,	srcTex->m_layout->m_key.m_xSize, srcTex->m_layout->m_key.m_ySize,
								0, 0,	srcTex->m_layout->m_key.m_xSize, srcTex->m_layout->m_key.m_ySize,	
								blitMask, GL_NEAREST );
								
		glScrubFBO( GL_READ_FRAMEBUFFER );	
		BindFBOToCtx( m_scratchFBO[1], GL_READ_FRAMEBUFFER );
		srcTex->ForceRBONonDirty();
	}
	else
	{
		if (srcTex->m_pBlitSrcFBO == NULL) 
		{
			srcTex->m_pBlitSrcFBO = NewFBO();
			BindFBOToCtx( srcTex->m_pBlitSrcFBO, GL_READ_FRAMEBUFFER );
			if (blitResolves) glAttachRBOtoFBO( GL_READ_FRAMEBUFFER, formatClass, srcTex->m_rboName );
			else glAttachTex2DtoFBO( GL_READ_FRAMEBUFFER, formatClass, srcTex->m_texName, srcMip );
		} 
		else 
		{
			BindFBOToCtx( srcTex->m_pBlitSrcFBO, GL_READ_FRAMEBUFFER );
		}

		if ( blitMask != GL_DEPTH_BUFFER_BIT ) gGL->glReadBuffer( glAttachFromClass[formatClass] );
		else gGL->glReadBuffer( GL_NONE );
	}
	
	bool yflip = false;
	if (blitToBack)
	{
		BindFBOToCtx( NULL, GL_DRAW_FRAMEBUFFER );
		GLenum bufs = GL_BACK;
		gGL->glDrawBuffers( 1, &bufs );
		yflip = true;
	}
	else
	{
		Assert( dstTex != NULL );
		if (dstTex->m_pBlitDstFBO == NULL) 
		{
			dstTex->m_pBlitDstFBO = NewFBO();
			BindFBOToCtx( dstTex->m_pBlitDstFBO, GL_DRAW_FRAMEBUFFER );
			if (dstTex->m_rboName) glAttachRBOtoFBO( GL_DRAW_FRAMEBUFFER, formatClass, dstTex->m_rboName );		
			else glAttachTex2DtoFBO( GL_DRAW_FRAMEBUFFER, formatClass, dstTex->m_texName, dstMip );
		} 
		else
		{
            BindFBOToCtx( dstTex->m_pBlitDstFBO, GL_DRAW_FRAMEBUFFER );
			if ( blitMask == GL_DEPTH_BUFFER_BIT )
			{
                GLenum bufs = GL_NONE;
                gGL->glDrawBuffers(1, &bufs);
            }
		}
	}

	if (!blitScales) filter = GL_NEAREST;
	
	if (yflip)
	{
		gGL->glBlitFramebuffer(	srcRect->xmin, srcRect->ymin, srcRect->xmax, srcRect->ymax,
								dstRect->xmin, dstRect->ymax, dstRect->xmax, dstRect->ymin,		
								blitMask, filter );
	}
	else
	{
		gGL->glBlitFramebuffer( srcRect->xmin, srcRect->ymin, srcRect->xmax, srcRect->ymax,
								   dstRect->xmin, dstRect->ymin, dstRect->xmax, dstRect->ymax,
								   blitMask, filter );
	}

	BindFBOToCtx( NULL, GL_READ_FRAMEBUFFER );
	if (!blitToBack) BindFBOToCtx( NULL, GL_DRAW_FRAMEBUFFER );
	BindFBOToCtx( m_drawingFBO, GL_FRAMEBUFFER );
	
	if (oldsciss.enable) m_ScissorEnable.Write( &oldsciss );

	RestoreSavedColorMask();
}

void GLMContext::BlitTex( CGLMTex *srcTex, GLMRect *srcRect, int srcFace, int srcMip, CGLMTex *dstTex, GLMRect *dstRect, int dstFace, int dstMip, GLenum filter, bool useBlitFB )
{
    // Framebuffer blits are core in GLES 3.0, bypass textured quad fallback
    useBlitFB = true; 

	SaveColorMaskAndSetToDefault();
	
	if (useBlitFB)
	{
		GLScissorEnable_t oldsciss,newsciss;
		m_ScissorEnable.Read( &oldsciss, 0 );
		newsciss.enable = false;
		m_ScissorEnable.Write( &newsciss );

		Assert( srcTex->m_layout->m_format->m_glDataFormat == dstTex->m_layout->m_format->m_glDataFormat );
		
		EGLMFBOAttachment	attachIndex = (EGLMFBOAttachment)0;
		GLenum				attachIndexGL = 0;
		GLuint				blitMask = 0;
		switch( srcTex->m_layout->m_format->m_glDataFormat )
		{
			case GL_BGRA: case GL_RGB: case GL_RGBA: case GL_ALPHA: case GL_LUMINANCE: case GL_LUMINANCE_ALPHA:
				attachIndex = kAttColor0; attachIndexGL = GL_COLOR_ATTACHMENT0; blitMask = GL_COLOR_BUFFER_BIT; break;
			case GL_DEPTH_COMPONENT:
				attachIndex = kAttDepth; attachIndexGL = GL_DEPTH_ATTACHMENT; blitMask = GL_DEPTH_BUFFER_BIT; break;
			case GL_DEPTH_STENCIL:
				attachIndex = kAttDepthStencil; attachIndexGL = GL_DEPTH_STENCIL_ATTACHMENT; blitMask = GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT; break;
			default: Assert(0); break;
		}

		BindFBOToCtx( m_blitReadFBO, GL_READ_FRAMEBUFFER );
		GLMFBOTexAttachParams attparams;
		memset( &attparams, 0, sizeof(attparams) );
		attparams.m_tex = srcTex; attparams.m_face = srcFace; attparams.m_mip = srcMip; attparams.m_zslice = 0;
		m_blitReadFBO->TexAttach( &attparams, attachIndex, GL_READ_FRAMEBUFFER );
		gGL->glReadBuffer( attachIndexGL );

		BindFBOToCtx( m_blitDrawFBO, GL_DRAW_FRAMEBUFFER );
		attparams.m_tex = dstTex; attparams.m_face = dstFace; attparams.m_mip = dstMip; attparams.m_zslice = 0;
		m_blitDrawFBO->TexAttach( &attparams, attachIndex, GL_DRAW_FRAMEBUFFER );
		gGL->glDrawBuffers( 1, &attachIndexGL );

		gGL->glBlitFramebuffer(	srcRect->xmin, srcRect->ymin, srcRect->xmax, srcRect->ymax,
								dstRect->xmin, dstRect->ymin, dstRect->xmax, dstRect->ymax,
								blitMask, filter );
							
		m_blitReadFBO->TexDetach( attachIndex, GL_READ_FRAMEBUFFER );
		m_blitDrawFBO->TexDetach( attachIndex, GL_DRAW_FRAMEBUFFER );
		BindFBOToCtx( m_drawingFBO, GL_FRAMEBUFFER );
		
		m_ScissorEnable.Write( &oldsciss );
	}
	
	RestoreSavedColorMask();
}

void GLMContext::ResolveTex( CGLMTex *tex, bool forceDirty )
{
	if ( ( tex->m_rboName ) && ( tex->IsRBODirty() || forceDirty ) )
	{
		GLScissorEnable_t	oldsciss,newsciss;
		m_ScissorEnable.Read( &oldsciss, 0 );
		newsciss.enable = false;
		m_ScissorEnable.Write( &newsciss );

		EGLMFBOAttachment	attachIndex = (EGLMFBOAttachment)0;
		GLenum				attachIndexGL = 0;
		GLuint				blitMask = 0;
		switch( tex->m_layout->m_format->m_glDataFormat )
		{
			case GL_BGRA: case GL_RGB: case GL_RGBA:
				attachIndex = kAttColor0; attachIndexGL = GL_COLOR_ATTACHMENT0; blitMask = GL_COLOR_BUFFER_BIT; break;
			case GL_DEPTH_STENCIL:
				attachIndex = kAttDepthStencil; attachIndexGL = GL_DEPTH_STENCIL_ATTACHMENT; blitMask = GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT; break;
			default: Assert(!"Unsupported format for MSAA resolve" ); break;
		}

		BindFBOToCtx( m_blitReadFBO, GL_READ_FRAMEBUFFER );
		
		if (attachIndexGL==GL_DEPTH_STENCIL_ATTACHMENT)
		{
			gGL->glFramebufferRenderbuffer( GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, tex->m_rboName);						
			gGL->glFramebufferRenderbuffer( GL_READ_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, tex->m_rboName);
		}
		else gGL->glFramebufferRenderbuffer( GL_READ_FRAMEBUFFER, attachIndexGL, GL_RENDERBUFFER, tex->m_rboName);

		gGL->glReadBuffer( attachIndexGL );
		BindFBOToCtx( m_blitDrawFBO, GL_DRAW_FRAMEBUFFER );

		if (attachIndexGL==GL_DEPTH_STENCIL_ATTACHMENT)
		{
			gGL->glFramebufferTexture2D( GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, tex->m_texName, 0 );
			gGL->glFramebufferTexture2D( GL_DRAW_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, tex->m_texName, 0 );
		}
		else gGL->glFramebufferTexture2D( GL_DRAW_FRAMEBUFFER, attachIndexGL, GL_TEXTURE_2D, tex->m_texName, 0 );

		gGL->glDrawBuffers( 1, &attachIndexGL );

		gGL->glBlitFramebuffer(	0, 0, tex->m_layout->m_key.m_xSize, tex->m_layout->m_key.m_ySize,
								0, 0, tex->m_layout->m_key.m_xSize, tex->m_layout->m_key.m_ySize,
								blitMask, GL_NEAREST );
			
		if (attachIndexGL==GL_DEPTH_STENCIL_ATTACHMENT)
		{
			gGL->glFramebufferRenderbuffer( GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);						
			gGL->glFramebufferRenderbuffer( GL_READ_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
		}
		else gGL->glFramebufferRenderbuffer( GL_READ_FRAMEBUFFER, attachIndexGL, GL_RENDERBUFFER, 0);

		if (attachIndexGL==GL_DEPTH_STENCIL_ATTACHMENT)
		{
			gGL->glFramebufferTexture2D( GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0 );
			gGL->glFramebufferTexture2D( GL_DRAW_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0 );
		}
		else gGL->glFramebufferTexture2D( GL_DRAW_FRAMEBUFFER, attachIndexGL, GL_TEXTURE_2D, 0, 0 );

		BindFBOToCtx( m_drawingFBO, GL_FRAMEBUFFER );
		m_ScissorEnable.Write( &oldsciss );
		tex->ForceRBONonDirty();
	}
}

// ... Additional context initialization removed for brevity

// Crucial BaseVertex fallback for GLES 3.0 missing glDrawElementsBaseVertex
HRESULT IDirect3DDevice9::FlushIndexBindings( void )
{
    m_ctx->SetIndexBuffer( m_indices.m_idxBuffer->m_idxBuffer );
    return S_OK;
}

HRESULT IDirect3DDevice9::FlushVertexBindings( uint baseVertexIndex )
{
    GLMVertexSetup	setup;
    memset( &setup, 0, sizeof( setup ) );
    
    IDirect3DVertexDeclaration9 *vxdecl = m_pVertDecl;
    unsigned char *vshAttribMap = m_vertexShader->m_vtxAttribMap;
    
    GLMVertexAttributeDesc *dstAttr = setup.m_attrs;
    for( int i=0; i<16; i++,dstAttr++ )
    {
        unsigned char vshattrib = vshAttribMap[ i ];
        if (vshattrib != 0xBB)
        {
            D3DVERTEXELEMENT9_GL *elem = m_pVertDecl->m_elements;
            for( int j=0; j< m_pVertDecl->m_elemCount; j++,elem++)
            {
                if ( ((vshattrib>>4) == elem->m_dxdecl.Usage) && ((vshattrib & 0x0F) == elem->m_dxdecl.UsageIndex) )
                {
                    *dstAttr = elem->m_gldecl;
                    int streamIndex = elem->m_dxdecl.Stream;
                    dstAttr->m_pBuffer = m_streams[ streamIndex ].m_vtxBuffer->m_vtxBuffer;
                    dstAttr->m_stride = m_streams[ streamIndex ].m_stride;
                    dstAttr->m_offset += m_streams[ streamIndex ].m_offset + (baseVertexIndex * dstAttr->m_stride);
                    setup.m_attrMask |= (1 << i);
                    vshattrib = 0xBB;
                    j = 999;
                }
            }
            if (vshattrib != 0xBB)
            {
                dstAttr->m_pBuffer = NULL;
                dstAttr->m_stride = 0;
                dstAttr->m_offset = 0;
                switch (vshattrib >> 4)
                {
                    case D3DDECLUSAGE_NORMAL:
                    case D3DDECLUSAGE_TEXCOORD:
                        dstAttr->m_nCompCount = 3;
                        dstAttr->m_datatype = GL_FLOAT;
                        dstAttr->m_normalized = false;
                        break;
                    case D3DDECLUSAGE_COLOR:
                        dstAttr->m_nCompCount = 4;
                        dstAttr->m_datatype = GL_UNSIGNED_BYTE;
                        dstAttr->m_normalized = true;
                        break;
                }
            }
        }
    }
    
    memcpy(&setup.m_vtxAttribMap, m_vertexShader->m_vtxAttribMap, sizeof(m_vertexShader->m_vtxAttribMap));
    m_ctx->SetVertexAttributes(&setup);
    return S_OK;
}

HRESULT IDirect3DDevice9::DrawIndexedPrimitive( D3DPRIMITIVETYPE Type,INT BaseVertexIndex,UINT MinVertexIndex,UINT NumVertices,UINT startIndex,UINT primCount )
{
    Assert( m_ctx->m_nCurOwnerThreadId == ThreadGetCurrentId() );
    
    TOGL_NULL_DEVICE_CHECK;
    if ( m_bFBODirty ) UpdateBoundFBO();
    g_nTotalDrawsOrClears++;
    
    if ( ( !m_indices.m_idxBuffer ) || ( !m_vertexShader ) ) goto draw_failed;    
    
    this->FlushIndexBindings( );
    this->FlushVertexBindings( BaseVertexIndex ); // Shifts the buffer offset to substitute glDrawElementsBaseVertex
    m_ctx->FlushDrawStates( MinVertexIndex, MinVertexIndex + NumVertices - 1, 0 );
    
    switch(Type)
    {
        case D3DPT_LINELIST:
            m_ctx->DrawRangeElements( GL_LINES, (GLuint)MinVertexIndex, (GLuint)(MinVertexIndex + NumVertices), (GLsizei)primCount*2, GL_UNSIGNED_SHORT, (const GLvoid *)(startIndex * sizeof(short)), m_indices.m_idxBuffer->m_idxBuffer );
            break;
        case D3DPT_TRIANGLELIST:
            m_ctx->DrawRangeElements( GL_TRIANGLES, (GLuint)MinVertexIndex, (GLuint)(MinVertexIndex + NumVertices), (GLsizei)primCount*3, GL_UNSIGNED_SHORT, (const GLvoid *)(startIndex * sizeof(short)), m_indices.m_idxBuffer->m_idxBuffer );
            break;
        case D3DPT_TRIANGLESTRIP:
            m_ctx->DrawRangeElements( GL_TRIANGLE_STRIP, (GLuint)MinVertexIndex, (GLuint)(MinVertexIndex + NumVertices), (GLsizei)(2+primCount), GL_UNSIGNED_SHORT, (const GLvoid *)(startIndex * sizeof(short)), m_indices.m_idxBuffer->m_idxBuffer );
            break;
    }
    return S_OK;
draw_failed:
    Assert( 0 );
    return E_FAIL;
}

// ... Note: Huge g_gl_enums Table omitted here for brevity (Keep existing table in your actual file)