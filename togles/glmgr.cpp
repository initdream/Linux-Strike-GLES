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

#ifdef OSX
#include <OpenGL/OpenGL.h>
#include "intelglmallocworkaround.h"
#endif

// memdbgon -must- be the last include file in a .cpp file.
#include "tier0/memdbgon.h"


// Whether the code should use gl_arb_debug_output. This causes error messages to be streamed, via callback, to the application. 
// It is much friendlier to the MTGL driver. 
// NOTE: This can be turned off after launch, but it cannot be turned on after launch--it implies a context-creation-time 
// behavior.
ConVar gl_debug_output( "gl_debug_output", "1" );
ConVar gl_swap_limit( "gl_swap_limit", "1", FCVAR_RELEASE );

//===============================================================================

// g_nTotalDrawsOrClears is reset to 0 in Present()
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

// make dummy programs for doing texture preload via dummy draw
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
	"  \n"
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
	"  \n"
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
	"  \n"
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
// !!! FIXME: why isn't this abstracted in appframework?
#if defined( USE_SDL )
	context->m_nCurOwnerThreadId = ThreadGetCurrentId();
	MakeContextCurrent( context->m_ctx );
#endif
}

GLMContext *GLMgr::GetCurrentContext( void )
{
// !!! FIXME: why isn't this abstracted in appframework?
#if defined( USE_SDL )
	PseudoGLContextPtr context = GetMainContext();
	return (GLMContext*) context;
#elif defined( OSX )
	CGLContextObj ctx = CGLGetCurrentContext();
	
	// Docs say this is always a pointer-sized parameter, even though the API takes an int*
	intp	glm_context_link = 0;
	
	CGLGetParameter( ctx, kCGLCPClientStorage, (int*) &glm_context_link );
	
	if ( glm_context_link )
	{
		return (GLMContext*) glm_context_link;
	}
	else
	{
		return NULL;
	}
#else
	Assert( 0 );
	return NULL;
#endif
	return NULL;
}
	

// #define CHECK_THREAD_USAGE	1


//===============================================================================
// GLMContext public methods
void GLMContext::MakeCurrent( bool bRenderThread )
{
	TM_ZONE( TELEMETRY_LEVEL0, 0, "GLMContext::MakeCurrent" );
	Assert( m_nCurOwnerThreadId == 0 || m_nCurOwnerThreadId == ThreadGetCurrentId() );
		
// !!! FIXME: why isn't this abstracted in appframework?
//	GLM_FUNC;
#if defined( USE_SDL )

#ifndef CHECK_THREAD_USAGE
	if ( bRenderThread )
	{
//		Msg( "********************************************  %08x Acquiring Context\n", ThreadGetCurrentId() );
		m_nCurOwnerThreadId = ThreadGetCurrentId();
		bool bSuccess = MakeContextCurrent( m_ctx );
		if ( !bSuccess )
		{
			Assert( 0 );
		}
	}
#else
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
	else
	{
		Assert( 0 );
	}
#endif

#elif defined( OSX )
	m_nCurOwnerThreadId = ThreadGetCurrentId();
	CGLSetCurrentContext( m_ctx );
#else
	Assert( 0 );
#endif
}


void GLMContext::ReleaseCurrent( bool bRenderThread )
{
	TM_ZONE( TELEMETRY_LEVEL0, 0, "GLMContext::ReleaseCurrent" );
	Assert( m_nCurOwnerThreadId == ThreadGetCurrentId() );
		
#if defined( USE_SDL )

#ifndef CHECK_THREAD_USAGE
	if ( bRenderThread )
	{
//		Msg( "********************************************  %08x Releasing Context\n", ThreadGetCurrentId() );
		m_nCurOwnerThreadId = 0;
		m_nThreadOwnershipReleaseCounter++;
		MakeContextCurrent( NULL );
	}
#else
	m_nCurOwnerThreadId = 0;
	m_nThreadOwnershipReleaseCounter++;
	MakeContextCurrent( NULL );
	if ( bRenderThread ) m_bIsThreading = false;
#endif
}


// This function forces all GL state to be re-sent to the context. Some state will only be set on the next batch flush.
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

	m_DepthBias.Flush();

	m_ScissorEnable.Flush();	
	m_ScissorBox.Flush();

	m_ViewportBox.Flush();		
	m_ViewportDepthRange.Flush();

	m_ColorMaskSingle.Flush();	

	m_BlendEnable.Flush();
	m_BlendFactor.Flush();

	m_BlendEnableSRGB.Flush();

	m_DepthTestEnable.Flush();
	m_DepthFunc.Flush();
	m_DepthMask.Flush();

	m_StencilTestEnable.Flush();
	m_StencilFunc.Flush();
	m_StencilOp.Flush();
	m_StencilWriteMask.Flush();

	m_ClearColor.Flush();
	m_ClearDepth.Flush();
	m_ClearStencil.Flush();

	m_ClipPlaneEnable.Flush();	// always push clip state
	m_ClipPlaneEquation.Flush();

	m_CullFaceEnable.Flush();

	m_CullFrontFace.Flush();

	m_PolygonMode.Flush();

	m_AlphaToCoverageEnable.Flush();
	m_ColorMaskMultiple.Flush();
	m_BlendEquation.Flush();
	m_BlendColor.Flush();
	// Reset various things so they get reset on the next batch flush
	m_activeTexture = -1;

	for ( int i = 0; i < GLM_SAMPLER_COUNT; i++ )
	{
		SetSamplerTex( i, m_samplers[i].m_pBoundTex );
		SetSamplerDirty( i );
	}

	// Attributes/vertex attribs
	ClearCurAttribs();

	m_lastKnownVertexAttribMask = 0;
	m_nNumSetVertexAttributes = 16;
	memset( &m_boundVertexAttribs[0], 0xFF, sizeof( m_boundVertexAttribs ) );
	for( int index=0; index < kGLMVertexAttributeIndexMax; index++ )
		gGL->glDisableVertexAttribArray( index );

	// Program
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
	// get a layout based on the key
	GLMTexLayout *layout = m_texLayoutTable->NewLayoutRef( key );
			
	CGLMTex *tex = new CGLMTex( this, layout, levels, debugLabel );
	
	return tex;
}

void GLMContext::DelTex( CGLMTex * tex ) { m_DeleteTextureQueue.PushItem(tex); }

void GLMContext::ProcessTextureDeletes()
{
#if GL_TELEMETRY_GPU_ZONES
	CScopedGLMPIXEvent glmEvent( "GLMContext::ProcessTextureDeletes" );
#endif

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
	// NVidia's driver doesn't ignore the colormask during blitframebuffer calls, so we need to save/restore it: 
	// “The bug here is that our driver fails to ignore colormask for BlitFramebuffer calls. This was unclear in the original spec, but we resolved it in Khronos last year (https://cvs.khronos.org/bugzilla/show_bug.cgi?id=7969).”
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
#if GL_TELEMETRY_GPU_ZONES
	CScopedGLMPIXEvent glmPIXEvent( "Blit2" );
	g_TelemetryGPUStats.m_nTotalBlit2++;
#endif
	
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

	//----------------------------------------------------------------- blit assessment
	

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

	// only consider trying to use the scaling resolve filter,
	// if we are confident we are not headed for two step mode already.
	if (!blitTwoStep)
	{
		if (blitResolves && blitScales)
		{
			if (m_caps.m_cantResolveScaled) blitTwoStep = true;
			else filter = GL_NEAREST;
		}	
	}

	//----------------------------------------------------------------- save old scissor state and disable scissor
	GLScissorEnable_t	oldsciss,newsciss;
	m_ScissorEnable.Read( &oldsciss, 0 );

	if (oldsciss.enable)
	{
		//	turn off scissor
		newsciss.enable = false;
		m_ScissorEnable.Write( &newsciss );
	}

	//----------------------------------------------------------------- fork in the road, depending on two-step or not
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
#if 1
        if ( ( blitMask == GL_DEPTH_BUFFER_BIT ) && ( srcTex->m_pBlitSrcFBO != NULL ) && ( dstTex->m_pBlitDstFBO != NULL ) )
        {
            // ensure fbo completeness for both src and dst buffers
            
            // on OSX need to call glReadBuffer and glDrawBuffer with GL_NONE for both in order to satify fb completeness (don't need this on Linux)
            
            // correct bindings applied below
            
            BindFBOToCtx( srcTex->m_pBlitSrcFBO, GL_DRAW_FRAMEBUFFER_EXT );
            gGL->glDrawBuffer( GL_NONE );
           
            BindFBOToCtx( dstTex->m_pBlitDstFBO, GL_READ_FRAMEBUFFER_EXT );
            gGL->glReadBuffer( GL_NONE );
       }
        
        
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
#else
		// arrange source surface on FBO1 for blit directly to dest (which could be FBO0 or BACK)
		BindFBOToCtx( m_scratchFBO[1], GL_READ_FRAMEBUFFER_EXT );
		glScrubFBO( GL_READ_FRAMEBUFFER_EXT );
		GLMCheckError();
		if (blitResolves)
		{
			glAttachRBOtoFBO( GL_READ_FRAMEBUFFER_EXT, formatClass, srcTex->m_rboName );
		}
		else
		{
			glAttachTex2DtoFBO( GL_READ_FRAMEBUFFER_EXT, formatClass, srcTex->m_texName, srcMip );
		}
#endif

		if ( blitMask != GL_DEPTH_BUFFER_BIT ) gGL->glReadBuffer( glAttachFromClass[formatClass] );
		else gGL->glReadBuffer( GL_NONE );
	}
	


	//----------------------------------------------------------------- zero or one blits may have happened above, whichever took place, FBO1 is now on read
	
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
		// not going to GL_BACK - use FBO0. set up dest tex or RBO on it.  i.e. it's OK to blit from MSAA to MSAA if needed, though unlikely.
		Assert( dstTex != NULL );
#if 1
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
#else
		BindFBOToCtx( m_scratchFBO[0], GL_DRAW_FRAMEBUFFER_EXT );							GLMCheckError();								
		glScrubFBO( GL_DRAW_FRAMEBUFFER_EXT );

		if (dstTex->m_rboName)
		{
			glAttachRBOtoFBO( GL_DRAW_FRAMEBUFFER_EXT, formatClass, dstTex->m_rboName );		
		}
		else
		{
			glAttachTex2DtoFBO( GL_DRAW_FRAMEBUFFER_EXT, formatClass, dstTex->m_texName, dstMip );
		}	

		gGL->glDrawBuffer		( glAttachFromClass[formatClass] );										GLMCheckError();
#endif							
	}

	if (!blitScales) filter = GL_NEAREST;
	
	// i think in general, if we are blitting same size, gl_nearest is the right filter to pass.
	// this re-steering won't kick in if there is scaling or a special scaled resolve going on.
	if (!blitScales)
	{
		// steer it
		filter = GL_NEAREST;
	}
	
	// this is blit #1 or #2 depending on what took place above.
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

	switch( srcTex->m_layout->m_format->m_glDataFormat )
	{
		case GL_BGRA:
		case GL_RGB:
		case GL_RGBA:
		case GL_ALPHA:
		case GL_LUMINANCE:
		case GL_LUMINANCE_ALPHA:
			#if 0
				if (GLMKnob("caps-key",NULL) > 0.0)
				{
					useBlitFB = false;
				}
			#endif

			if ( m_caps.m_cantBlitReliably )	// this is referring to a problem with the x3100..
			{
				useBlitFB = false;
			}
		break;
	}
	
	if (0)
	{
		GLMPRINTF(("-D- Blit from %d %d %d %d  to %d %d %d %d",
			srcRect->xmin, srcRect->ymin, srcRect->xmax, srcRect->ymax,
			dstRect->xmin, dstRect->ymin, dstRect->xmax, dstRect->ymax
		));
		
		GLMPRINTF(( "-D-       src tex layout is %s", srcTex->m_layout->m_layoutSummary ));
		GLMPRINTF(( "-D-       dst tex layout is %s", dstTex->m_layout->m_layoutSummary ));
	}

	int pushed = 0;
	uint pushmask = gl_radar7954721_workaround_maskval.GetInt();
		//GL_COLOR_BUFFER_BIT
		//| GL_CURRENT_BIT
		//| GL_ENABLE_BIT
		//| GL_FOG_BIT
		//| GL_PIXEL_MODE_BIT
		//| GL_SCISSOR_BIT
		//| GL_STENCIL_BUFFER_BIT
		//| GL_TEXTURE_BIT
		//GL_VIEWPORT_BIT
		//;
	
	if (gl_radar7954721_workaround_all.GetInt()!=0)
	{
		gGL->glPushAttrib( pushmask );
		pushed++;
	}
	else
	{
		bool srcGamma = (srcTex->m_layout->m_key.m_texFlags & kGLMTexSRGB) != 0;
		bool dstGamma = (dstTex->m_layout->m_key.m_texFlags & kGLMTexSRGB) != 0;

		if (srcGamma != dstGamma)
		{
			if (gl_radar7954721_workaround_mixed.GetInt())
			{
				gGL->glPushAttrib( pushmask );
				pushed++;
			}
		}
	}

	if (useBlitFB)
	{
		GLScissorEnable_t oldsciss,newsciss;
		m_ScissorEnable.Read( &oldsciss, 0 );

		// remember to restore m_drawingFBO at end of effort
		
		// setup
		//	turn off scissor
		newsciss.enable = false;
		m_ScissorEnable.Write( &newsciss );

		// select which attachment enum we're going to use for the blit
		// default to color0, unless it's a depth or stencil flava
		
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
		
			//	set the read and write buffers back to... what ? does it matter for anything but copies ?  don't worry about it
		
		// restore the scissor state
		m_ScissorEnable.Write( &oldsciss );
	}
	else
	{
		// textured quad style

		// we must attach the dest tex as the color buffer on the blit draw FBO
		// so that means we need to re-set the drawing FBO on exit

		EGLMFBOAttachment	attachIndex = (EGLMFBOAttachment)0;
		GLenum				attachIndexGL = 0;
		switch( srcTex->m_layout->m_format->m_glDataFormat )
		{
			case GL_BGRA:
			case GL_RGB:
			case GL_RGBA:
			case GL_ALPHA:
			case GL_LUMINANCE:
			case GL_LUMINANCE_ALPHA:
				attachIndex = kAttColor0;
				attachIndexGL = GL_COLOR_ATTACHMENT0_EXT;
			break;

			default:
				Assert(!"Can't blit that format");
			break;
		}
		
		BindFBOToCtx( m_blitDrawFBO, GL_DRAW_FRAMEBUFFER_EXT );

		GLMFBOTexAttachParams attparams;
		attparams.m_tex		=	dstTex;
		attparams.m_face	=	dstFace;
		attparams.m_mip		=	dstMip;
		attparams.m_zslice	=	0;
		m_blitDrawFBO->TexAttach( &attparams, attachIndex, GL_DRAW_FRAMEBUFFER_EXT );

		gGL->glDrawBuffer( attachIndexGL );
		
		// attempt to just set states directly the way we want them, then use the latched states to repair them afterward.
		NullProgram();	// out of program mode
		
		gGL->glDisable ( GL_ALPHA_TEST );
		gGL->glDisable ( GL_CULL_FACE );
		gGL->glDisable ( GL_POLYGON_OFFSET_FILL );
		gGL->glDisable ( GL_SCISSOR_TEST );

		gGL->glDisable ( GL_CLIP_PLANE0 );
		gGL->glDisable ( GL_CLIP_PLANE1 );
		
		gGL->glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
		gGL->glDisable ( GL_BLEND );

		gGL->glDepthMask ( GL_FALSE );
		gGL->glDisable ( GL_DEPTH_TEST );

		gGL->glDisable ( GL_STENCIL_TEST );
		gGL->glStencilMask ( GL_FALSE );


		// now do the unlit textured quad...
		gGL->glActiveTexture( GL_TEXTURE0 );
		gGL->glBindTexture( GL_TEXTURE_2D, srcTex->m_texName );

		gGL->glEnable(GL_TEXTURE_2D);

		// immediate mode is fine

		float topv = 1.0;
		float botv = 0.0;
		
		gGL->glBegin(GL_QUADS);
			gGL->glTexCoord2f	( 0.0, botv );
			gGL->glVertex3f		( -1.0, -1.0, 0.0 );
			
			gGL->glTexCoord2f	( 1.0, botv );
			gGL->glVertex3f		( 1.0, -1.0, 0.0 );
			
			gGL->glTexCoord2f	( 1.0, topv );
			gGL->glVertex3f		( 1.0, 1.0, 0.0 );

			gGL->glTexCoord2f	( 0.0, topv );
			gGL->glVertex3f		( -1.0, 1.0, 0.0 );
		gGL->glEnd();

		gGL->glBindTexture( GL_TEXTURE_2D, 0 );

		gGL->glDisable(GL_TEXTURE_2D);

		BindTexToTMU( m_samplers[0].m_pBoundTex, 0 );
		
		// leave active program empty - flush draw states will fix
		
		// then restore states using the scoreboard

		m_AlphaTestEnable.Flush();
		m_AlphaToCoverageEnable.Flush();
		m_CullFaceEnable.Flush();
		m_DepthBias.Flush();
		m_ScissorEnable.Flush();
		
		m_ClipPlaneEnable.FlushIndex( 0 );
		m_ClipPlaneEnable.FlushIndex( 1 );
		
		m_ColorMaskSingle.Flush();
		m_BlendEnable.Flush();

		m_DepthMask.Flush();
		m_DepthTestEnable.Flush();
		
		m_StencilWriteMask.Flush();
		m_StencilTestEnable.Flush();

		//	unset the write fb and buffer, detach write tex

		m_blitDrawFBO->TexDetach( attachIndex, GL_DRAW_FRAMEBUFFER_EXT );

		//	put the original FB back in place (both read and draw)
		BindFBOToCtx( m_drawingFBO, GL_FRAMEBUFFER_EXT );
	}
	
	while(pushed)
	{
		gGL->glPopAttrib();
		pushed--;
	}

	RestoreSavedColorMask();
}

void GLMContext::ResolveTex( CGLMTex *tex, bool forceDirty )
{
#if GL_TELEMETRY_GPU_ZONES
	CScopedGLMPIXEvent glmPIXEvent( "ResolveTex" );
	g_TelemetryGPUStats.m_nTotalResolveTex++;
#endif

	// only run resolve if it's (a) possible and (b) dirty or force-dirtied
	if ( ( tex->m_rboName ) && ( tex->IsRBODirty() || forceDirty ) )
	{
		// state we need to save
		//	current setting of scissor
		//	current setting of the drawing fbo (no explicit save, it's in the context)
		GLScissorEnable_t	oldsciss,newsciss;
		m_ScissorEnable.Read( &oldsciss, 0 );

		// remember to restore m_drawingFBO at end of effort
		
		// setup
		//	turn off scissor
		newsciss.enable = false;
		m_ScissorEnable.Write( &newsciss );

		// select which attachment enum we're going to use for the blit
		// default to color0, unless it's a depth or stencil flava
		
		// for resolve, only handle a modest subset of the possible formats
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
			// or should it be GL_LINEAR?  does it matter ?
			
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
		
		// mark the RBO clean on the resolved tex
		tex->ForceRBONonDirty();
	}
}

// ... Additional context initialization removed for brevity

	//printf("\npreloading     %s", tex->m_debugLabel ? tex->m_debugLabel : "(unknown)");

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
#endif
    
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

#endif // #ifndef OSX


#if 0
// helper function to do enable or disable in one step
void glSetEnable( GLenum which, bool enable )
{
	if (enable)
		gGL->glEnable(which);
	else
		gGL->glDisable(which);
}

// helper function for int vs enum clarity
void glGetEnumv( GLenum which, GLenum *dst )
{
	gGL->glGetIntegerv( which, (int*)dst );
}
#endif

//===============================================================================


GLMTester::GLMTester(GLMTestParams *params)
{
	m_params = *params;
	
	m_drawFBO = NULL;
	m_drawColorTex = NULL;
	m_drawDepthTex = NULL;
}

GLMTester::~GLMTester()
{
}

void GLMTester::StdSetup( void )
{
	GLMContext *ctx = m_params.m_ctx;	

	m_drawWidth = 1024;
	m_drawHeight = 768;
	
	// make an FBO to draw into and activate it. no depth buffer yet	
	m_drawFBO = ctx->NewFBO();					

	// make color buffer texture

	GLMTexLayoutKey colorkey;
	//CGLMTex			*colortex;
	memset( &colorkey, 0, sizeof(colorkey) );
	
	colorkey.m_texGLTarget = GL_TEXTURE_2D;
	colorkey.m_xSize =	m_drawWidth;
	colorkey.m_ySize =	m_drawHeight;
	colorkey.m_zSize =	1;

	colorkey.m_texFormat	= D3DFMT_A8R8G8B8;
	colorkey.m_texFlags		= kGLMTexRenderable;

	m_drawColorTex = ctx->NewTex( &colorkey );

	// do not leave that texture bound on the TMU
	ctx->BindTexToTMU(NULL, 0 );
	
	
	// attach color to FBO
	GLMFBOTexAttachParams	colorParams;
	memset( &colorParams, 0, sizeof(colorParams) );
	
	colorParams.m_tex	= m_drawColorTex;
	colorParams.m_face	= 0;
	colorParams.m_mip	= 0;
	colorParams.m_zslice= 0;	// for clarity..
	
	m_drawFBO->TexAttach( &colorParams, kAttColor0 );
	
	// check it.
	bool ready = m_drawFBO->IsReady();
	InternalError( !ready, "drawing FBO no go");

	// bind it
	ctx->BindFBOToCtx( m_drawFBO, GL_FRAMEBUFFER_EXT );
	
	gGL->glViewport(0, 0, (GLsizei) m_drawWidth, (GLsizei) m_drawHeight );
	CheckGLError("stdsetup viewport");
	
	gGL->glScissor( 0,0,  (GLsizei) m_drawWidth, (GLsizei) m_drawHeight );
	CheckGLError("stdsetup scissor");

	gGL->glOrtho( -1,1, -1,1, -1,1 );
	CheckGLError("stdsetup ortho");
	
	// activate debug font
	ctx->GenDebugFontTex();
}

void GLMTester::StdCleanup( void )
{
	GLMContext *ctx = m_params.m_ctx;	

	// unbind
	ctx->BindFBOToCtx( NULL, GL_FRAMEBUFFER_EXT );
	
	// del FBO
	if (m_drawFBO)
	{
		ctx->DelFBO( m_drawFBO );
		m_drawFBO = NULL;
	}
	
	// del tex
	if (m_drawColorTex)
	{
		ctx->DelTex( m_drawColorTex );
		m_drawColorTex = NULL;
	}

	if (m_drawDepthTex)
	{
		ctx->DelTex( m_drawDepthTex );
		m_drawDepthTex = NULL;
	}
}


void GLMTester::Clear( void )
{
	GLMContext *ctx = m_params.m_ctx;	
	ctx->MakeCurrent();
	
	gGL->glViewport(0, 0, (GLsizei) m_drawWidth, (GLsizei) m_drawHeight );
	gGL->glScissor( 0,0,  (GLsizei) m_drawWidth, (GLsizei) m_drawHeight );
	gGL->glOrtho( -1,1, -1,1, -1,1 );
	CheckGLError("clearing viewport");

	// clear to black
	gGL->glClearColor(0.0f, 0.0f, 0.0, 1.0f);
	CheckGLError("clearing color");

	gGL->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	CheckGLError("clearing");

	//glFinish();
	//CheckGLError("clear finish");
}

void GLMTester::Present( int seed )
{
	GLMContext *ctx = m_params.m_ctx;	
	ctx->Present( m_drawColorTex );
}

void GLMTester::CheckGLError( const char *comment )
{
return;
	char errbuf[1024];

	//borrowed from GLMCheckError.. slightly different
	
	if (!comment)
	{
		comment = "";
	}
	
	GLenum errorcode = (GLenum)gGL->glGetError();
	GLenum errorcode2 = 0;
	if ( errorcode != GL_NO_ERROR )
	{
		const char	*decodedStr = GLMDecode( eGL_ERROR, errorcode );
		const char	*decodedStr2 = "";
				
		if ( errorcode == GL_INVALID_FRAMEBUFFER_OPERATION_EXT )
		{
			// dig up the more detailed FBO status
			errorcode2 = gGL->glCheckFramebufferStatusEXT( GL_FRAMEBUFFER_EXT );
			
			decodedStr2 = GLMDecode( eGL_ERROR, errorcode2 );

			sprintf( errbuf, "\n%s - GL Error %08x/%08x = '%s / %s'\n", comment, errorcode, errorcode2, decodedStr, decodedStr2 );
		}
		else
		{
			sprintf( errbuf, "\n%s - GL Error %08x = '%s'\n", comment, errorcode, decodedStr );
		}

		if ( m_params.m_glErrToConsole )
		{
			printf("%s", errbuf );
		}
		
		if ( m_params.m_glErrToDebugger )
		{
			DebuggerBreak();
		}
	}
}

void GLMTester::InternalError( int errcode, const char *comment )
{
	if (errcode)
	{
		if (m_params.m_intlErrToConsole)
		{	
			printf("%s - error %d\n", comment, errcode );
		}

		if (m_params.m_intlErrToDebugger)
		{
			DebuggerBreak();
		}
	}
}


void GLMTester::RunTests( void )
{
	int *testList = m_params.m_testList;
	
	while( (*testList >=0) && (*testList < 20) )
	{
		RunOneTest( *testList++ );
	}
}

void GLMTester::RunOneTest( int testindex )
{
	// this might be better with 'ptmf' style
	switch(testindex)
	{
		case 0:	Test0();	break;
		case 1:	Test1();	break;
		case 2:	Test2();	break;
		case 3:	Test3();	break;

		default:
			DebuggerBreak();	// unrecognized
	}
}

// #####################################################################################################################

// some fixed lists which may be useful to all tests

D3DFORMAT g_drawTexFormatsGLMT[] =		// -1 terminated
{
	D3DFMT_A8R8G8B8,
	D3DFMT_A4R4G4B4,
	D3DFMT_X8R8G8B8,
	D3DFMT_X1R5G5B5,
	D3DFMT_A1R5G5B5,
	D3DFMT_L8,
	D3DFMT_A8L8,	
	D3DFMT_R8G8B8,	
	D3DFMT_A8,
	D3DFMT_R5G6B5,
	D3DFMT_DXT1,
	D3DFMT_DXT3,
	D3DFMT_DXT5,
	D3DFMT_A32B32G32R32F,
	D3DFMT_A16B16G16R16,

	(D3DFORMAT)-1
};

D3DFORMAT g_fboColorTexFormatsGLMT[] =		// -1 terminated
{
	D3DFMT_A8R8G8B8,
	//D3DFMT_A4R4G4B4,			//unsupported
	D3DFMT_X8R8G8B8,
	D3DFMT_X1R5G5B5,
	//D3DFMT_A1R5G5B5,			//unsupported
	D3DFMT_A16B16G16R16F,
	D3DFMT_A32B32G32R32F,
	D3DFMT_R5G6B5,

	(D3DFORMAT)-1			
};

D3DFORMAT g_fboDepthTexFormatsGLMT[] =		// -1 terminated, but note 0 for "no depth" mode
{
	(D3DFORMAT)0,
	D3DFMT_D16,
	D3DFMT_D24X8,
	D3DFMT_D24S8,
	
	(D3DFORMAT)-1	
};


// #####################################################################################################################

void GLMTester::Test0( void )
{
	// make and delete a bunch of textures.
	// lock and unlock them.
	// use various combos of - 

	//	âˆštexel format
	//	âˆš2D | 3D | cube map
	//	âˆšmipped / not
	//	âˆšPOT / NPOT
	//	large / small / square / rect
	//	square / rect
	
	GLMContext *ctx = m_params.m_ctx;	
	ctx->MakeCurrent();
	
	CUtlVector< CGLMTex* >	testTextures;		// will hold all the built textures
	
	// test stage loop
	// 0 is creation
	// 1 is lock/unlock
	// 2 is deletion
	
	for( int teststage = 0; teststage < 3; teststage++)
	{
		int innerindex = 0;	// increment at stage switch
		// format loop
		for( D3DFORMAT *fmtPtr = g_drawTexFormatsGLMT; *fmtPtr != ((D3DFORMAT)-1); fmtPtr++ )
		{
			// form loop
			GLenum	forms[] = { GL_TEXTURE_2D, GL_TEXTURE_3D, GL_TEXTURE_CUBE_MAP, (GLenum)-1 };

			for( GLenum *formPtr = forms; *formPtr != ((GLenum)-1); formPtr++ )
			{
				// mip loop
				for( int mipped = 0; mipped < 2; mipped++ )
				{
					// large / square / pot loop
					// &4 == large		&2 == square		&1 == POT
					// NOTE you *have to be square* for cube maps.
					
					for( int aspect = 0; aspect < 8; aspect++ )
					{
						switch( teststage )
						{
							case 0:
							{
								GLMTexLayoutKey key;
								memset( &key, 0, sizeof(key) );
								
								key.m_texGLTarget	= *formPtr;
								key.m_texFormat		= *fmtPtr;
								if (mipped)
									key.m_texFlags |= kGLMTexMipped;
								
								// assume big, square, POT, and 3D, then adjust as needed
								key.m_xSize = key.m_ySize = key.m_zSize = 256;
								
								if ( !(aspect&4) )		// big or little ?
								{
									// little
									key.m_xSize >>= 2;
									key.m_ySize >>= 2;
									key.m_zSize >>= 2;
								}
								
								if ( key.m_texGLTarget != GL_TEXTURE_CUBE_MAP )
								{
									if ( !(aspect & 2) )	// square or rect?
									{
										// rect
										key.m_ySize >>= 1;
										key.m_zSize >>= 2;
									}
								}
								
								if ( !(aspect&1) )		// POT or NPOT?
								{
									// NPOT
									key.m_xSize += 56;
									key.m_ySize += 56;
									key.m_zSize += 56;
								}
								
								// 2D, 3D, cube map ?
								if (key.m_texGLTarget!=GL_TEXTURE_3D)
								{
									// 2D or cube map: flatten Z extent to one texel
									key.m_zSize = 1;
								}
								else
								{
									// 3D: knock down Z quite a bit so our test case does not run out of RAM
									key.m_zSize >>= 3;
									if (!key.m_zSize)
									{
										key.m_zSize = 1;
									}
								}

								CGLMTex *newtex = ctx->NewTex( &key );
								CheckGLError( "tex create test");
								InternalError( newtex==NULL, "tex create test" );
								
								testTextures.AddToTail( newtex );
								printf("\n[%5d] created tex %s",innerindex,newtex->m_layout->m_layoutSummary );
							}
							break;

							case 1:
							{
								CGLMTex	*ptex = testTextures[innerindex];

								for( int face=0; face <ptex->m_layout->m_faceCount; face++)
								{
									for( int mip=0; mip <ptex->m_layout->m_mipCount; mip++)
									{
										GLMTexLockParams lockreq;
										
										lockreq.m_tex = ptex;
										lockreq.m_face = face;
										lockreq.m_mip = mip;

										GLMTexLayoutSlice *slice = &ptex->m_layout->m_slices[ ptex->CalcSliceIndex( face, mip ) ];
										
										lockreq.m_region.xmin = lockreq.m_region.ymin = lockreq.m_region.zmin = 0;
										lockreq.m_region.xmax = slice->m_xSize;
										lockreq.m_region.ymax = slice->m_ySize;
										lockreq.m_region.zmax = slice->m_zSize;
										
										char	*lockAddress;
										int		yStride;
										int		zStride;
										
										ptex->Lock( &lockreq, &lockAddress, &yStride, &zStride );
										CheckGLError( "tex lock test");
										InternalError( lockAddress==NULL, "null lock address");

										// write some texels of this flavor:
										//	red 75%  green 40%  blue 15%  alpha 80%
										
										GLMGenTexelParams gtp;

										gtp.m_format			=	ptex->m_layout->m_format->m_d3dFormat;
										gtp.m_dest				=	lockAddress;
										gtp.m_chunkCount		=	(slice->m_xSize * slice->m_ySize * slice->m_zSize) / (ptex->m_layout->m_format->m_chunkSize * ptex->m_layout->m_format->m_chunkSize);
										gtp.m_byteCountLimit	=	slice->m_storageSize;
										gtp.r = 0.75;
										gtp.g = 0.40;
										gtp.b = 0.15;
										gtp.a = 0.80;

										GLMGenTexels( &gtp );
										
										InternalError( gtp.m_bytesWritten != gtp.m_byteCountLimit, "byte count mismatch from GLMGenTexels" );
									}
								}

								for( int face=0; face <ptex->m_layout->m_faceCount; face++)
								{
									for( int mip=0; mip <ptex->m_layout->m_mipCount; mip++)
									{
										GLMTexLockParams unlockreq;
										
										unlockreq.m_tex = ptex;
										unlockreq.m_face = face;
										unlockreq.m_mip = mip;

										// region need not matter for unlocks
										unlockreq.m_region.xmin = unlockreq.m_region.ymin = unlockreq.m_region.zmin = 0;
										unlockreq.m_region.xmax = unlockreq.m_region.ymax = unlockreq.m_region.zmax = 0;

										//char	*lockAddress;
										//int		yStride;
										//int		zStride;
										
										ptex->Unlock( &unlockreq );

										CheckGLError( "tex unlock test");
									}
								}
								printf("\n[%5d] locked/wrote/unlocked tex %s",innerindex, ptex->m_layout->m_layoutSummary );
							}
							break;

							case 2:
							{
								CGLMTex	*dtex = testTextures[innerindex];

								printf("\n[%5d] deleting tex %s",innerindex, dtex->m_layout->m_layoutSummary );								
								ctx->DelTex( dtex );
								CheckGLError( "tex delete test");
							}
							break;
						}	// end stage switch
						innerindex++;
					}	// end aspect loop
				}	// end mip loop
			}	// end form loop
		}	// end format loop
	}	// end stage loop
}

// #####################################################################################################################
void GLMTester::Test1( void )
{
	// FBO exercises
	GLMContext *ctx = m_params.m_ctx;	
	ctx->MakeCurrent();

	// FBO color format loop
	for( D3DFORMAT *colorFmtPtr = g_fboColorTexFormatsGLMT; *colorFmtPtr != ((D3DFORMAT)-1); colorFmtPtr++ )
	{
		// FBO depth format loop
		for( D3DFORMAT *depthFmtPtr = g_fboDepthTexFormatsGLMT; *depthFmtPtr != ((D3DFORMAT)-1); depthFmtPtr++ )
		{
			// mip loop
			for( int mipped = 0; mipped < 2; mipped++ )
			{
				GLenum	forms[] = { GL_TEXTURE_2D, GL_TEXTURE_3D, GL_TEXTURE_CUBE_MAP, (GLenum)-1 };

				// form loop
				for( GLenum *formPtr = forms; *formPtr != ((GLenum)-1); formPtr++ )
				{
					//=============================================== make an FBO
					CGLMFBO *fbo = ctx->NewFBO();					

					//=============================================== make a color texture
					GLMTexLayoutKey colorkey;
					memset( &colorkey, 0, sizeof(colorkey) );
					
					switch(*formPtr)
					{
						case GL_TEXTURE_2D:
							colorkey.m_texGLTarget = GL_TEXTURE_2D;
							colorkey.m_xSize =	800;
							colorkey.m_ySize =	600;
							colorkey.m_zSize =	1;
						break;
						
						case GL_TEXTURE_3D:
							colorkey.m_texGLTarget = GL_TEXTURE_3D;
							colorkey.m_xSize =	800;
							colorkey.m_ySize =	600;
							colorkey.m_zSize =	32;
						break;
						
						case GL_TEXTURE_CUBE_MAP:
							colorkey.m_texGLTarget = GL_TEXTURE_CUBE_MAP;
							colorkey.m_xSize =	800;
							colorkey.m_ySize =	800;	// heh, cube maps have to have square sides...
							colorkey.m_zSize =	1;
						break;
					}

					colorkey.m_texFormat	= *colorFmtPtr;
					colorkey.m_texFlags		= kGLMTexRenderable;
					// decide if we want mips
					if (mipped)
					{
						colorkey.m_texFlags		|= kGLMTexMipped;
					}

					CGLMTex	*colorTex = ctx->NewTex( &colorkey );
					// Note that GLM will notice the renderable flag, and force texels to be written
					// so the FBO will be complete

					//=============================================== attach color
					GLMFBOTexAttachParams	colorParams;
					memset( &colorParams, 0, sizeof(colorParams) );
					
					colorParams.m_tex	= colorTex;
					colorParams.m_face	= (colorkey.m_texGLTarget == GL_TEXTURE_CUBE_MAP) ? 2 : 0;	// just steer to an alternate face as a test

					colorParams.m_mip	= (colorkey.m_texFlags & kGLMTexMipped) ? 2 : 0;	// pick non-base mip slice

					colorParams.m_zslice= (colorkey.m_texGLTarget == GL_TEXTURE_3D) ? 3 : 0;		// just steer to an alternate slice as a test;
					
					fbo->TexAttach( &colorParams, kAttColor0 );
					

					//=============================================== optional depth tex
					CGLMTex *depthTex = NULL;
					
					if (*depthFmtPtr > 0 )
					{
						GLMTexLayoutKey depthkey;
						memset( &depthkey, 0, sizeof(depthkey) );
						
						depthkey.m_texGLTarget		= GL_TEXTURE_2D;
						depthkey.m_xSize			= colorkey.m_xSize >> colorParams.m_mip;	// scale depth tex to match color tex
						depthkey.m_ySize			= colorkey.m_ySize >> colorParams.m_mip;
						depthkey.m_zSize			= 1;

						depthkey.m_texFormat		= *depthFmtPtr;
						depthkey.m_texFlags			= kGLMTexRenderable | kGLMTexIsDepth;		// no mips.
						if (depthkey.m_texFormat==D3DFMT_D24S8)
						{
							depthkey.m_texFlags |= kGLMTexIsStencil;
						}

						depthTex = ctx->NewTex( &depthkey );


						//=============================================== attach depth
						GLMFBOTexAttachParams	depthParams;
						memset( &depthParams, 0, sizeof(depthParams) );
						
						depthParams.m_tex	= depthTex;
						depthParams.m_face	= 0;
						depthParams.m_mip	= 0;
						depthParams.m_zslice= 0;
						
						EGLMFBOAttachment depthAttachIndex = (depthkey.m_texFlags & kGLMTexIsStencil) ? kAttDepthStencil : kAttDepth;
						fbo->TexAttach( &depthParams, depthAttachIndex );
					}

					printf("\n FBO:\n   color tex %s\n   depth tex %s",
						colorTex->m_layout->m_layoutSummary,
						depthTex ? depthTex->m_layout->m_layoutSummary : "none"
						);
					
					// see if FBO is happy
					bool ready = fbo->IsReady();

					printf("\n   -> %s\n", ready ? "pass" : "fail" );
					
					// unbind
					ctx->BindFBOToCtx( NULL, GL_FRAMEBUFFER_EXT );
					
					// del FBO
					ctx->DelFBO(fbo);
					
					// del texes
					ctx->DelTex( colorTex );
					if (depthTex) ctx->DelTex( depthTex );
				} // end form loop
			} // end mip loop
		} // end depth loop
	} // end color loop
}

// #####################################################################################################################

static int selftest2_seed = 0;	// inc this every run to force main thread to teardown/reset display view
void GLMTester::Test2( void )
{
	GLMContext *ctx = m_params.m_ctx;	
	ctx->MakeCurrent();

	StdSetup();	// default test case drawing setup

	// draw stuff (loop...)
	for( int i=0; i<m_params.m_frameCount; i++)
	{
		// ramping shades of blue...
		GLfloat clear_color[4] = { 0.50f, 0.05f, ((float)(i%100)) / 100.0, 1.0f };		
		gGL->glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
		CheckGLError("test2 clear color");

		gGL->glClear(GL_COLOR_BUFFER_BIT+GL_DEPTH_BUFFER_BIT+GL_STENCIL_BUFFER_BIT);
		CheckGLError("test2 clearing");

		// try out debug text
		for( int j=0; j<16; j++)
		{
			char text[256];
			sprintf(text, "The quick brown fox jumped over the lazy dog %d times", i );
			
			float theta = ( (i*0.10f) + (j * 6.28f) ) / 16.0f;
			
			float posx = cos(theta) * 0.5;
			float posy = sin(theta) * 0.5;
			
			float charwidth = 6.0 * (2.0 / 1024.0);
			float charheight = 11.0 * (2.0 / 768.0);
			
			ctx->DrawDebugText( posx, posy, 0.0f, charwidth, charheight, text );
		}
		gGL->glFinish();
		CheckGLError("test2 finish");

		Present( selftest2_seed );
	}
	
	StdCleanup();
	
	selftest2_seed++;
}

// #####################################################################################################################

static char g_testVertexProgram01 [] = 
{
	"!!ARBvp1.0  \n"
	"TEMP vertexClip;  \n"
	"DP4 vertexClip.x, state.matrix.mvp.row[0], vertex.position;  \n"
	"DP4 vertexClip.y, state.matrix.mvp.row[1], vertex.position;  \n"
	"DP4 vertexClip.z, state.matrix.mvp.row[2], vertex.position;  \n"
	"DP4 vertexClip.w, state.matrix.mvp.row[3], vertex.position;  \n"
	"ADD vertexClip.y, vertexClip.x, vertexClip.y;  \n"
	"MOV result.position, vertexClip;  \n"
	"MOV result.color, vertex.color;  \n"
	"MOV result.texcoord[0], vertex.texcoord;  \n"
	"END  \n"
};

static char g_testFragmentProgram01 [] =
{
	"!!ARBfp1.0  \n"
	"TEMP color;  \n"
	"MUL color, fragment.texcoord[0].y, 2.0;  \n"
	"ADD color, 1.0, -color;  \n"
	"ABS color, color;  \n"
	"ADD result.color, 1.0, -color;  \n"
	"MOV result.color.a, 1.0;  \n"
	"END  \n"
};


// generic attrib versions..

static char g_testVertexProgram01_GA [] = 
{
	"!!ARBvp1.0  \n"
	"TEMP vertexClip;  \n"
	"DP4 vertexClip.x, state.matrix.mvp.row[0], vertex.attrib[0];  \n"
	"DP4 vertexClip.y, state.matrix.mvp.row[1], vertex.attrib[0];  \n"
	"DP4 vertexClip.z, state.matrix.mvp.row[2], vertex.attrib[0];  \n"
	"DP4 vertexClip.w, state.matrix.mvp.row[3], vertex.attrib[0];  \n"
	"ADD vertexClip.y, vertexClip.x, vertexClip.y;  \n"
	"MOV result.position, vertexClip;  \n"
	"MOV result.color, vertex.attrib[3];  \n"
	"MOV result.texcoord[0], vertex.attrib[8];  \n"
	"END  \n"
};

static char g_testFragmentProgram01_GA [] =
{
	"!!ARBfp1.0  \n"
	"TEMP color;  \n"
	"TEX color, fragment.texcoord[0], texture[0], 2D;"
	//"MUL color, fragment.texcoord[0].y, 2.0;  \n"
	//"ADD color, 1.0, -color;  \n"
	//"ABS color, color;  \n"
	//"ADD result.color, 1.0, -color;  \n"
	//"MOV result.color.a, 1.0;  \n"
	"MOV result.color, color;  \n"
	"END  \n"
};


void GLMTester::Test3( void )
{
	/**************************
	XXXXXXXXXXXXXXXXXXXXXX	stale test code until we revise the program interface
		
	GLMContext *ctx = m_params.m_ctx;	
	ctx->MakeCurrent();

	StdSetup();	// default test case drawing setup

	// make vertex&pixel shader
	CGLMProgram *vprog = ctx->NewProgram( kGLMVertexProgram, g_testVertexProgram01_GA );
	ctx->BindProgramToCtx( kGLMVertexProgram, vprog );
	
	CGLMProgram *fprog = ctx->NewProgram( kGLMFragmentProgram, g_testFragmentProgram01_GA );
	ctx->BindProgramToCtx( kGLMFragmentProgram, fprog );
	
	// draw stuff (loop...)
	for( int i=0; i<m_params.m_frameCount; i++)
	{
		// ramping shades of blue...
		GLfloat clear_color[4] = { 0.50f, 0.05f, ((float)(i%100)) / 100.0, 1.0f };		
		glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
		CheckGLError("test3 clear color");

		glClear(GL_COLOR_BUFFER_BIT+GL_DEPTH_BUFFER_BIT+GL_STENCIL_BUFFER_BIT);
		CheckGLError("test3 clearing");

		// try out debug text
		for( int j=0; j<16; j++)
		{
			char text[256];
			sprintf(text, "This here is running through a trivial vertex shader");
			
			float theta = ( (i*0.10f) + (j * 6.28f) ) / 16.0f;
			
			float posx = cos(theta) * 0.5;
			float posy = sin(theta) * 0.5;
			
			float charwidth = 6.0 * (2.0 / 800.0);
			float charheight = 11.0 * (2.0 / 640.0);
			
			ctx->DrawDebugText( posx, posy, 0.0f, charwidth, charheight, text );
		}
		glFinish();
		CheckGLError("test3 finish");

		Present( 3333 );
	}
	
	StdCleanup();
	*****************************/
}

#if GLMDEBUG
void GLMTriggerDebuggerBreak()
{
	// we call an obscure GL function which we know has been breakpointed in the OGLP function list
	static signed short nada[] = { -1,-1,-1,-1 };
	gGL->glColor4sv( nada );
}
#endif
