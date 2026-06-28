//============ Copyright (c) Valve Corporation, All rights reserved. ============
//
// cglmbuffer.cpp
//
//===============================================================================

#include "togl/rendermechanism.h"
#include "tier0/memdbgon.h"

// Force client-side memory buffering for OpenGL ES.
// Mobile GPUs handle dynamic/streaming VBO updates very poorly without it.
bool g_bUsePseudoBufs = true;
bool g_bDisableStaticBuffer = true;

ConVar gl_bufmode( "gl_bufmode", "1" );

char ALIGN16 CGLMBuffer::m_StaticBuffers[ GL_MAX_STATIC_BUFFERS ][ GL_STATIC_BUFFER_SIZE ] ALIGN16_POST;
bool CGLMBuffer::m_bStaticBufferUsed[ GL_MAX_STATIC_BUFFERS ];

extern bool g_bNullD3DDevice;

CGLMBuffer::CGLMBuffer( GLMContext *pCtx, EGLMBufferType type, uint size, uint options )
{
	m_pCtx = pCtx;
	m_type = type;
	m_bDynamic = ( options & GLMBufferOptionDynamic ) != 0;

	switch ( m_type )
	{
		case kGLMVertexBuffer:	m_buffGLTarget = GL_ARRAY_BUFFER; break;
		case kGLMIndexBuffer:	m_buffGLTarget = GL_ELEMENT_ARRAY_BUFFER; break;
		case kGLMUniformBuffer:	m_buffGLTarget = GL_UNIFORM_BUFFER; break;
		case kGLMPixelBuffer:	m_buffGLTarget = GL_PIXEL_UNPACK_BUFFER; break;
		default: Assert(!"Unknown buffer type" ); DXABSTRACT_BREAK_ON_ERROR();
	}

	m_nSize = size;
	m_nActualSize = size;
	m_bMapped = false;
	m_pLastMappedAddress = NULL;
	m_pStaticBuffer = NULL;
	m_bEnableAsyncMap = false;
	m_bEnableExplicitFlush = false;
	m_dirtyMinOffset = m_dirtyMaxOffset = 0;

	m_pCtx->CheckCurrent();
	m_nRevision = rand();
	m_pPseudoBuf = NULL;
	m_pActualPseudoBuf = NULL;

	// Always use pseudo bufs in GLES for dynamic geometry
	m_bPseudo = m_bDynamic;

	if ( m_bPseudo )
	{
		m_nHandle = 0;
		m_nActualSize = size + 15;
		m_pActualPseudoBuf = (char*)malloc( m_nActualSize );
		m_pPseudoBuf = (char*)(((intp)m_pActualPseudoBuf + 15) & ~15);
		m_pCtx->BindBufferToCtx( m_type, NULL );
	}
	else
	{
		gGL->glGenBuffers( 1, &m_nHandle );
		m_pCtx->BindBufferToCtx( m_type, this );

		GLenum hint = GL_STATIC_DRAW;
		switch (m_type)
		{
			case kGLMVertexBuffer:	hint = m_bDynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW; break;
			case kGLMIndexBuffer:	hint = m_bDynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW; break;
			case kGLMUniformBuffer:	hint = GL_DYNAMIC_DRAW; break;
			case kGLMPixelBuffer:	hint = m_bDynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW; break;
		}

		gGL->glBufferData( m_buffGLTarget, m_nSize, (const GLvoid*)NULL, hint );

		SetModes( false, true, true );
		m_pCtx->BindBufferToCtx( m_type, NULL );
	}
}

CGLMBuffer::~CGLMBuffer( )
{
	m_pCtx->CheckCurrent();
	if ( m_bPseudo )
	{
		free( m_pActualPseudoBuf );
		m_pActualPseudoBuf = NULL;
		m_pPseudoBuf = NULL;
	}
	else
	{
		gGL->glDeleteBuffers( 1, &m_nHandle );
	}

	m_pCtx = NULL;
	m_nHandle = 0;
	m_pLastMappedAddress = NULL;
}

void CGLMBuffer::SetModes( bool bAsyncMap, bool bExplicitFlush, bool bForce )
{
	if ( !m_bPseudo )
	{
		if ( bForce || ( m_bEnableAsyncMap != bAsyncMap ) ) m_bEnableAsyncMap = bAsyncMap;
		if ( bForce || ( m_bEnableExplicitFlush != bExplicitFlush ) ) m_bEnableExplicitFlush = bExplicitFlush;
	}
}

void CGLMBuffer::FlushRange( uint offset, uint size )
{
	if ( !m_pStaticBuffer && !m_bPseudo )
	{
		// GLES 3.0 has MapBufferRange
		if ( gGL->glFlushMappedBufferRange )
		{
			gGL->glFlushMappedBufferRange( m_buffGLTarget, (GLintptr)( offset - m_dirtyMinOffset ), (GLsizeiptr)size );
		}
	}
}

void CGLMBuffer::Lock( GLMBuffLockParams *pParams, char **pAddressOut )
{
	char *resultPtr = NULL;
	if ( m_bMapped ) return;

	m_pCtx->CheckCurrent();
	Assert( pParams->m_nSize );
	m_LockParams = *pParams;

	if ( pParams->m_nOffset >= m_nSize || ( pParams->m_nOffset + pParams->m_nSize ) > m_nSize ) return;

	m_pStaticBuffer = NULL;

	if ( m_bPseudo )
	{
		if ( pParams->m_bDiscard ) m_nRevision++;
		resultPtr = m_pPseudoBuf + pParams->m_nOffset;
	}
	else if ( !g_bDisableStaticBuffer && ( pParams->m_bDiscard || pParams->m_bNoOverwrite ) && ( pParams->m_nSize <= GL_STATIC_BUFFER_SIZE ) )
	{
		if ( pParams->m_bDiscard )
		{
			m_pCtx->BindBufferToCtx( m_type, this );
			GLenum hint = gl_bufmode.GetInt() ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW;
			gGL->glBufferData( m_buffGLTarget, m_nSize, (const GLvoid*)NULL, hint );
			m_nRevision++;
		}

		m_dirtyMinOffset = pParams->m_nOffset;
		m_dirtyMaxOffset = pParams->m_nOffset + pParams->m_nSize;

		if ( m_type == kGLMVertexBuffer ) m_pStaticBuffer = m_StaticBuffers[ 0 ];
		else if( m_type == kGLMIndexBuffer ) m_pStaticBuffer = m_StaticBuffers[ 1 ];

		resultPtr = m_pStaticBuffer;
	}
	else
	{
		m_pCtx->BindBufferToCtx( m_type, this );

		if ( pParams->m_bDiscard )
		{
			GLenum hint = gl_bufmode.GetInt() ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW;
			gGL->glBufferData( m_buffGLTarget, m_nSize, (const GLvoid*)NULL, hint );
			m_nRevision++;
		}

		SetModes( pParams->m_bNoOverwrite, m_bEnableExplicitFlush );

		GLbitfield parms = GL_MAP_WRITE_BIT | ( m_bEnableAsyncMap ? GL_MAP_UNSYNCHRONIZED_BIT : 0 ) | ( pParams->m_bDiscard ? GL_MAP_INVALIDATE_BUFFER_BIT : 0 ) | ( m_bEnableExplicitFlush ? GL_MAP_FLUSH_EXPLICIT_BIT : 0 );

		char *mapPtr = (char*)gGL->glMapBufferRange( m_buffGLTarget, pParams->m_nOffset, pParams->m_nSize, parms);

		Assert( mapPtr );
		resultPtr = mapPtr;
		m_dirtyMinOffset = pParams->m_nOffset;
		m_dirtyMaxOffset = pParams->m_nOffset + pParams->m_nSize;
	}

	m_bMapped = true;
	m_pLastMappedAddress = (float*)resultPtr;
	*pAddressOut = resultPtr;
}

void CGLMBuffer::Unlock( int nActualSize, const void *pActualData )
{
	m_pCtx->CheckCurrent();
	if ( !m_bMapped ) return;

	if ( nActualSize < 0 ) nActualSize = m_LockParams.m_nSize;
	if ( nActualSize > (int)m_LockParams.m_nSize ) return;

	if ( m_pStaticBuffer )
	{
		if ( nActualSize )
		{
			m_pCtx->BindBufferToCtx( m_type, this );
			Assert( nActualSize <= (int)( m_dirtyMaxOffset - m_dirtyMinOffset ) );
			gGL->glBufferSubData( m_buffGLTarget, m_dirtyMinOffset, nActualSize, pActualData ? pActualData : m_pStaticBuffer );
		}
		m_pStaticBuffer = NULL;
	}
	else if ( m_bPseudo )
	{
		if ( pActualData )
		{
			memcpy( m_pLastMappedAddress, pActualData, nActualSize );
		}
	}
	else
	{
		if ( pActualData ) memcpy( m_pLastMappedAddress, pActualData, nActualSize );

		m_pCtx->BindBufferToCtx( m_type, this );
		Assert( nActualSize <= (int)( m_dirtyMaxOffset - m_dirtyMinOffset ) );

		if ( m_bEnableExplicitFlush ) FlushRange( m_dirtyMinOffset, nActualSize );

		m_dirtyMinOffset = m_dirtyMaxOffset = 0;
		gGL->glUnmapBuffer( m_buffGLTarget );
	}

	m_bMapped = false;
}

GLuint CGLMBuffer::GetHandle() const
{
	return m_nHandle;
}
