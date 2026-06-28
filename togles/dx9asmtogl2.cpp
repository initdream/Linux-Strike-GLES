//------------------------------------------------------------------------------
// DX9AsmToGL2.cpp
//------------------------------------------------------------------------------
#include <GL/gl.h>
#include <GL/glext.h>

#include "togl/rendermechanism.h"
#include "tier0/dbg.h"
#include "tier1/strtools.h"
#include "tier1/utlbuffer.h"
#include "dx9asmtogl2.h"

#include "materialsystem/IShader.h"
#include "tier0/memdbgon.h"

#ifdef POSIX
#define strcat_s( a, b, c) V_strcat( a, c, b )
#endif

#define DST_REGISTER		0
#define SRC_REGISTER		1
#define SEMANTIC_OUTPUT		0x01
#define SEMANTIC_INPUT		0x02
#define UNDECLARED_OUTPUT	0xFFFFFFFF
#define UNDECLARED_INPUT	0xFFFFFFFF

#ifndef POSIX
#define Debugger() Assert(0)
#endif

static const char *g_szVecZeros[] = { NULL, "0.0", "vec2( 0.0, 0.0 )", "vec3( 0.0, 0.0, 0.0 )", "vec4( 0.0, 0.0, 0.0, 0.0 )" };
static const char *g_szVecOnes[] = { NULL, "1.0", "vec2( 1.0, 1.0 )", "vec3( 1.0, 1.0, 1.0 )", "vec4( 1.0, 1.0, 1.0, 1.0 )" };
static const char *g_szDefaultSwizzle = "xyzw";
static const char *g_szDefaultSwizzleStrings[] = { "x", "y", "z", "w" };
static const char *g_szSamplerStrings[] = { "2D", "CUBE", "3D" };

static const char *g_pAtomicTempVarName = "atomic_temp_var";
static const char *g_pTangentAttributeName = "g_tangent";

int __cdecl SortInts( const int *a, const int *b )
{
	if ( *a < *b ) return -1;
	else if ( *a > *b ) return 1;
	else return 0;
}

void StripExtraTrailingZeros( char *pStr )
{
	int len = (int)V_strlen( pStr );
	while ( len >= 2 && pStr[len-1] == '0' && pStr[len-2] != '.' )
	{
		pStr[len-1] = 0;
		--len;
	}
}

void D3DToGL::PrintToBufWithIndents( CUtlBuffer &buf, const char *pFormat, ... )
{
	va_list marker;
	va_start( marker, pFormat );
	char szTemp[1024];
	V_vsnprintf( szTemp, sizeof( szTemp ), pFormat, marker );
	va_end( marker );

	PrintIndentation( (char*)buf.Base(), buf.Size() );
	strcat_s( (char*)buf.Base(), buf.Size(), szTemp );
}

void PrintToBuf( CUtlBuffer &buf, const char *pFormat, ... )
{
	va_list marker;
	va_start( marker, pFormat );
	char szTemp[1024];
	V_vsnprintf( szTemp, sizeof( szTemp ), pFormat, marker );
	va_end( marker );

	strcat_s( (char*)buf.Base(), buf.Size(), szTemp );
}

void PrintToBuf( char *pOut, int nOutSize, const char *pFormat, ... )
{
	int nStrlen = V_strlen( pOut );
	pOut += nStrlen;
	nOutSize -= nStrlen;

	va_list marker;
	va_start( marker, pFormat );
	V_vsnprintf( pOut, nOutSize, pFormat, marker );
	va_end( marker );
}

int GetNumWriteMaskEntries( const char *pParam )
{
	const char *pDot = strchr( pParam, '.' );
	if ( pDot ) return V_strlen( pDot + 1 );
	else return 4;
}

const char* GetSwizzleDot( const char *pParam )
{
	const char *pDot = strrchr( pParam, '.' );
	const char *pSquareClose = strrchr( pParam, ']' );

	if ( pSquareClose )
	{
		if ( pDot && ( pSquareClose < pDot  ) ) return pDot;
		else return NULL;
	}

	if ( pDot && ( ( *(pDot+1) == 'x' ) || ( *(pDot+1) == 'y' ) || ( *(pDot+1) == 'z' ) || ( *(pDot+1) == 'w' ) ||
		( *(pDot+1) == 'r' ) || ( *(pDot+1) == 'g' ) || ( *(pDot+1) == 'b' ) || ( *(pDot+1) == 'z' ) ) )
	{
		return pDot;
	}

	return NULL;
}

int GetNumSwizzleComponents( const char *pParam )
{
	if ( !V_stricmp( pParam, "gl_FogFragCoord" ) ) return 1;
	if ( !V_stricmp( pParam, "gl_FragDepth" ) ) return 1;
	if ( !V_stricmp( pParam, "a0" ) ) return 1;

	const char *pDot = GetSwizzleDot( pParam );
	if ( pDot )
	{
		pDot++;
		int nNumSwizzleComponents = 0;
		while ( ( *pDot == 'x' ) || ( *pDot == 'y' ) || ( *pDot == 'z' ) || ( *pDot == 'w' ) ||
			( *pDot == 'r' ) || ( *pDot == 'g' ) || ( *pDot == 'b' ) || ( *pDot == 'z' ) )
		{
			nNumSwizzleComponents++;
			pDot++;
		}
		return nNumSwizzleComponents;
	}

	return 0;
}

char GetSwizzleComponent( const char *pParam, int n )
{
	Assert( n < 4 );
	const char *pDot = GetSwizzleDot( pParam );
	if ( pDot )
	{
		++pDot;
		int nComponents = (int)V_strlen( pDot );
		Assert( nComponents > 0 );

		if ( n < nComponents ) return pDot[n];
		else return pDot[nComponents-1];
	}

	return g_szDefaultSwizzle[n];
}

void ReplaceParamName( const char *pSrc, const char *pNewParamName, char *pOut, int nOutLen )
{
	V_strncpy( pOut, pNewParamName, nOutLen );
	const char *pDot = GetSwizzleDot( pSrc );
	if ( pDot )
	{
		V_strncat( pOut, pDot, nOutLen );
	}
}

void GetParamNameWithoutSwizzle( const char *pParam, char *pOut, int nOutLen )
{
	char *pParamStart = (char *) pParam;
	const char *pParamEnd = GetSwizzleDot( pParam );
	bool bAbsWrapper = false;

	if ( !V_strncmp( pParam, "abs(", 4 ) || !V_strncmp( pParam, "-abs(", 5 ) )
	{
		const char *pOpenParen = strchr( pParam, '(' );
		const char *pClosingParen = strrchr( pParam, ')' );
		Assert ( pOpenParen && pClosingParen );

		pParamStart = (char *) pOpenParen;
		pParamStart++;
		bAbsWrapper = true;

		if ( !pParamEnd )
		{
			pParamEnd = pClosingParen;
		}
	}

	if ( pParamEnd )
	{
		int nToCopy = MIN( nOutLen-1, pParamEnd - pParamStart );
		memcpy( pOut, pParamStart, nToCopy );
		pOut[nToCopy] = 0;
	}
	else
	{
		V_strncpy( pOut, pParamStart, nOutLen );
	}
}

bool DoParamNamesMatch( const char *pParam1, const char *pParam2 )
{
	char szTemp[2][256];
	GetParamNameWithoutSwizzle( pParam1, szTemp[0], sizeof( szTemp[0] ) );
	GetParamNameWithoutSwizzle( pParam2, szTemp[1], sizeof( szTemp[1] ) );
	return ( V_stricmp( szTemp[0], szTemp[1] ) == 0 );
}

void WriteParamWithSingleMaskEntry( const char *pParam, int n, char *pOut, int nOutLen )
{
	bool bCloseParen = false;
	if ( !V_strncmp( pParam, "-abs(", 5 ) )
	{
		V_strcpy( pOut, "-abs(" );
		bCloseParen = true;
		pOut += 5; nOutLen -= 5;
	}
	else if ( !V_strncmp( pParam, "abs(", 4 ) )
	{
		V_strcpy( pOut, "abs(" );
		bCloseParen = true;
		pOut += 4; nOutLen -= 4;
	}

	GetParamNameWithoutSwizzle( pParam, pOut, nOutLen );
	PrintToBuf( pOut, nOutLen, "." );
	PrintToBuf( pOut, nOutLen, "%c", GetSwizzleComponent( pParam, n ) );

	if ( bCloseParen )
	{
		PrintToBuf( pOut, nOutLen, ")" );
	}
}

float uint32ToFloat( uint32 dw )
{
	return *((float*)&dw);
}

CUtlString EnsureNumSwizzleComponents( const char *pSrcRegisterName, int nComponents )
{
	int nExisting = GetNumSwizzleComponents( pSrcRegisterName );
	if ( nExisting == nComponents )
		return pSrcRegisterName;

	bool bAbsWrapper = false;
	bool bAbsNegative = false;
	char szSrcRegister[128];
	V_strncpy( szSrcRegister, pSrcRegisterName, sizeof(szSrcRegister) );

	if ( !V_strncmp( pSrcRegisterName, "abs(", 4 ) || !V_strncmp( pSrcRegisterName, "-abs(", 5 ) )
	{
		bAbsWrapper = true;
		bAbsNegative = pSrcRegisterName[0] == '-';

		const char *pOpenParen = strchr( pSrcRegisterName, '(' );
		const char *pClosingParen = strrchr( pSrcRegisterName, ')' );
		Assert ( pOpenParen && pClosingParen );

		int nRegNameLength = pClosingParen - pOpenParen - 1;
		V_strncpy( szSrcRegister, pOpenParen+1, nRegNameLength + 1 );
	}

	char szReg[256];
	GetParamNameWithoutSwizzle( szSrcRegister, szReg, sizeof( szReg ) );
	if ( nComponents == 0 )
		return szReg;

	PrintToBuf( szReg, sizeof( szReg ), "." );
	if ( nExisting > nComponents )
	{
		for ( int i=0; i < nComponents; i++ )
		{
			PrintToBuf( szReg, sizeof( szReg ), "%c", GetSwizzleComponent( szSrcRegister, i ) );
		}
	}
	else
	{
		if ( nExisting == 0 )
		{
			for ( int i=0; i < nComponents; i++ )
				PrintToBuf( szReg, sizeof( szReg ), "%c", g_szDefaultSwizzle[i] );
		}
		else
		{
			V_strncpy( szReg, szSrcRegister, sizeof( szReg ) );
			char cLast = szSrcRegister[ V_strlen( szSrcRegister ) - 1 ];
			for ( int i=nExisting; i < nComponents; i++ )
			{
				PrintToBuf( szReg, sizeof( szReg ), "%c", cLast );
			}
		}
	}

	if ( bAbsWrapper )
	{
		char szTemp[128];
		V_strncpy( szTemp, szReg, sizeof(szTemp) );
		V_snprintf( szReg, sizeof( szReg ), "%sabs(%s)", bAbsNegative ? "-" : "", szTemp ) ;
	}

	return szReg;
}

static void TranslationError()
{
	Plat_DebugString( "D3DToGL: GLSL translation error!\n" );
	DebuggerBreakIfDebugging();
	Error( "D3DToGL: GLSL translation error!\n" );
}

D3DToGL::D3DToGL()
{
}

uint32 D3DToGL::GetNextToken( void )
{
	uint32 dwToken = *m_pdwNextToken;
	m_pdwNextToken++;
	return dwToken;
}

void D3DToGL::SkipTokens( uint32 numToSkip )
{
	m_pdwNextToken += numToSkip;
}

uint32 D3DToGL::Opcode( uint32 dwToken )
{
	return ( dwToken & D3DSI_OPCODE_MASK );
}

uint32 D3DToGL::OpcodeSpecificData (uint32 dwToken)
{
	return ( ( dwToken & D3DSP_OPCODESPECIFICCONTROL_MASK ) >> D3DSP_OPCODESPECIFICCONTROL_SHIFT );
}

uint32 D3DToGL::TextureType ( uint32 dwToken )
{
	return ( dwToken & D3DSP_TEXTURETYPE_MASK );
}

bool D3DToGL::OpenIntrinsic( uint32 inst, char* buff, int nBufLen, uint32 destDimension, uint32 nArgumentDimension )
{
	bool bDoubleClose = false;

	if ( nArgumentDimension == 0 )
	{
		nArgumentDimension = 4;
	}

	switch ( inst )
	{
		case D3DSIO_RSQ: V_snprintf( buff, nBufLen, "inversesqrt( " ); break;
		case D3DSIO_DP3:
		case D3DSIO_DP4:
			if ( destDimension == 1 )
			{
				V_snprintf( buff, nBufLen, "dot( " );
			}
			else
			{
				if ( !destDimension ) destDimension = 4;
				V_snprintf( buff, nBufLen, "vec%d( dot( ", destDimension );
				bDoubleClose = true;
			}
			break;
		case D3DSIO_MIN: V_snprintf( buff, nBufLen, "min( " ); break;
		case D3DSIO_MAX: V_snprintf( buff, nBufLen, "max( " ); break;
		case D3DSIO_SLT:
			if ( nArgumentDimension == 1 ) V_snprintf( buff, nBufLen, "float( " );
			else
			{
				Assert( nArgumentDimension > 1 );
				V_snprintf( buff, nBufLen, "vec%d( lessThan( ", nArgumentDimension );
				bDoubleClose = true;
			}
			break;
		case D3DSIO_SGE:
			if ( nArgumentDimension == 1 ) V_snprintf( buff, nBufLen, "float( " );
			else
			{
				Assert( nArgumentDimension > 1 );
				V_snprintf( buff, nBufLen, "vec%d( greaterThanEqual( ", nArgumentDimension );
				bDoubleClose = true;
			}
			break;
		case D3DSIO_EXP: V_snprintf( buff, nBufLen, "exp( " ); break;
		case D3DSIO_LOG: V_snprintf( buff, nBufLen, "log( " ); break;
		case D3DSIO_LIT: TranslationError(); V_snprintf( buff, nBufLen, "lit( " ); break;
		case D3DSIO_DST: V_snprintf( buff, nBufLen, "dst( " ); break;
		case D3DSIO_LRP: Assert( !m_bVertexShader ); V_snprintf( buff, nBufLen, "mix( " ); break;
		case D3DSIO_FRC: V_snprintf( buff, nBufLen, "fract( " ); break;
		case D3DSIO_POW: V_snprintf( buff, nBufLen, "pow( " ); break;
		case D3DSIO_CRS: V_snprintf( buff, nBufLen, "cross( " ); break;
		case D3DSIO_ABS: V_snprintf( buff, nBufLen, "abs( " ); break;
		case D3DSIO_TEXDEPTH: V_snprintf( buff, nBufLen, "texdepth" ); break;
		case D3DSIO_TEXKILL: V_snprintf( buff, nBufLen, "kill( " ); break;
		case D3DSIO_TEXCOORD: V_snprintf( buff, nBufLen, "texcoord" ); break;
		case D3DSIO_TEXLDD: V_snprintf( buff, nBufLen, "texldd" ); break;
		case D3DSIO_TEXLDL: V_snprintf( buff, nBufLen, "texldl" ); break;
		case D3DSIO_EXPP: V_snprintf( buff, nBufLen, "exp( " ); break;
		case D3DSIO_LOGP: V_snprintf( buff, nBufLen, "log( " ); break;
		case D3DSIO_DSX: V_snprintf( buff, nBufLen, "dFdx" ); break;
		case D3DSIO_DSY: V_snprintf( buff, nBufLen, "dFdy" ); break;
		default: TranslationError(); break;
	}
	return bDoubleClose;
}

const char* D3DToGL::GetGLSLOperatorString( uint32 inst )
{
	if ( inst == D3DSIO_ADD ) return "+";
	else if ( inst == D3DSIO_SUB ) return "-";
	else if ( inst == D3DSIO_MUL ) return "*";

	Error( "GetGLSLOperatorString: unknown operator" );
	return "zzzz";
}

void D3DToGL::PrintOpcode( uint32 inst, char* buff, int nBufLen )
{
	switch ( inst )
	{
		case D3DSIO_MOV: V_snprintf( buff, nBufLen, "MOV" ); break;
		case D3DSIO_ADD: V_snprintf( buff, nBufLen, "ADD" ); break;
		case D3DSIO_SUB: V_snprintf( buff, nBufLen, "SUB" ); break;
		case D3DSIO_MAD: V_snprintf( buff, nBufLen, "MAD" ); break;
		case D3DSIO_MUL: V_snprintf( buff, nBufLen, "MUL" ); break;
		case D3DSIO_RCP: V_snprintf( buff, nBufLen, "RCP" ); break;
		case D3DSIO_RSQ: V_snprintf( buff, nBufLen, "RSQ" ); break;
		case D3DSIO_DP3: V_snprintf( buff, nBufLen, "DP3" ); break;
		case D3DSIO_DP4: V_snprintf( buff, nBufLen, "DP4" ); break;
		case D3DSIO_MIN: V_snprintf( buff, nBufLen, "MIN" ); break;
		case D3DSIO_MAX: V_snprintf( buff, nBufLen, "MAX" ); break;
		case D3DSIO_SLT: V_snprintf( buff, nBufLen, "SLT" ); break;
		case D3DSIO_SGE: V_snprintf( buff, nBufLen, "SGE" ); break;
		case D3DSIO_EXP: V_snprintf( buff, nBufLen, "EX2" ); break;
		case D3DSIO_LOG: V_snprintf( buff, nBufLen, "LG2" ); break;
		case D3DSIO_LIT: V_snprintf( buff, nBufLen, "LIT" ); break;
		case D3DSIO_DST: V_snprintf( buff, nBufLen, "DST" ); break;
		case D3DSIO_LRP: Assert( !m_bVertexShader ); V_snprintf( buff, nBufLen, "LRP" ); break;
		case D3DSIO_FRC: V_snprintf( buff, nBufLen, "FRC" ); break;
		case D3DSIO_DCL: V_snprintf( buff, nBufLen, "DCL" ); break;
		case D3DSIO_POW: V_snprintf( buff, nBufLen, "POW" ); break;
		case D3DSIO_CRS: V_snprintf( buff, nBufLen, "XPD" ); break;
		case D3DSIO_ABS: V_snprintf( buff, nBufLen, "ABS" ); break;
		case D3DSIO_SINCOS: Assert( !m_bVertexShader ); V_snprintf( buff, nBufLen, "SCS" ); break;
		case D3DSIO_MOVA: Assert( m_bVertexShader ); V_snprintf( buff, nBufLen, "MOV" ); break;
		case D3DSIO_TEXCOORD: V_snprintf( buff, nBufLen, "texcoord" ); break;
		case D3DSIO_TEXKILL: V_snprintf( buff, nBufLen, "KIL" ); break;
		case D3DSIO_TEX: V_snprintf( buff, nBufLen, "TEX" ); break;
		case D3DSIO_EXPP: V_snprintf( buff, nBufLen, "EXP" ); break;
		case D3DSIO_LOGP: V_snprintf( buff, nBufLen, "LOG" ); break;
		case D3DSIO_DEF: V_snprintf( buff, nBufLen, "DEF" ); break;
		case D3DSIO_TEXDEPTH: V_snprintf( buff, nBufLen, "texdepth" ); break;
		case D3DSIO_CMP: Assert( !m_bVertexShader ); V_snprintf( buff, nBufLen, "CMP" ); break;
		case D3DSIO_TEXLDD: V_snprintf( buff, nBufLen, "texldd" ); break;
		case D3DSIO_TEXLDL: V_snprintf( buff, nBufLen, "texldl" ); break;
		default: TranslationError(); break;
	}
}

CUtlString D3DToGL::GetUsageAndIndexString( uint32 dwToken, int fSemanticFlags )
{
	char szTemp[1024];
	PrintUsageAndIndexToString( dwToken, szTemp, sizeof( szTemp ), fSemanticFlags );
	return szTemp;
}

void D3DToGL::PrintUsageAndIndexToString( uint32 dwToken, char* strUsageUsageIndexName, int nBufLen, int fSemanticFlags )
{
	uint32 dwUsage = ( dwToken & D3DSP_DCL_USAGE_MASK );
	uint32 dwUsageIndex = ( dwToken & D3DSP_DCL_USAGEINDEX_MASK ) >> D3DSP_DCL_USAGEINDEX_SHIFT;

	switch ( dwUsage )
	{
		case D3DDECLUSAGE_POSITION:
			if ( m_bVertexShader )
			{
				if ( fSemanticFlags & SEMANTIC_OUTPUT ) V_snprintf( strUsageUsageIndexName, nBufLen, "vTempPos" );
				else V_snprintf( strUsageUsageIndexName, nBufLen, "gl_Vertex" );
			}
			else
			{
				V_snprintf( strUsageUsageIndexName, nBufLen, "gl_FragCoord" );
			}
			break;
		case D3DDECLUSAGE_BLENDWEIGHT: V_snprintf( strUsageUsageIndexName, nBufLen, "vertex.attrib[1]" ); break;
		case D3DDECLUSAGE_BLENDINDICES: V_snprintf( strUsageUsageIndexName, nBufLen, "vertex.attrib[13]" ); break;
		case D3DDECLUSAGE_NORMAL: V_snprintf( strUsageUsageIndexName, nBufLen, "vec4( gl_Normal, 0.0 )" ); break;
		case D3DDECLUSAGE_TEXCOORD: V_snprintf( strUsageUsageIndexName, nBufLen, "oT%d", dwUsageIndex ); break;
		case D3DDECLUSAGE_TANGENT: NoteTangentInputUsed(); V_strncpy( strUsageUsageIndexName, g_pTangentAttributeName, nBufLen ); break;
		case D3DDECLUSAGE_BINORMAL: V_snprintf( strUsageUsageIndexName, nBufLen, "vertex.attrib[14]" ); break;
		case D3DDECLUSAGE_COLOR:
			Assert( dwUsageIndex <= 1 );
			V_snprintf( strUsageUsageIndexName, nBufLen, dwUsageIndex != 0 ? "_gl_FrontSecondaryColor" : "_gl_FrontColor" );
			break;
		default: TranslationError(); break;
	}
}

uint32 D3DToGL::GetRegType( uint32 dwRegToken )
{
	return ( ( dwRegToken & D3DSP_REGTYPE_MASK2 ) >> D3DSP_REGTYPE_SHIFT2 ) | ( ( dwRegToken & D3DSP_REGTYPE_MASK ) >> D3DSP_REGTYPE_SHIFT );
}

void D3DToGL::PrintIndentation( char *pBuf, int nBufLen )
{
	for( int i=0; i<m_NumIndentTabs; i++ ) strcat_s( pBuf, nBufLen, "\t" );
}

CUtlString D3DToGL::GetParameterString( uint32 dwToken, uint32 dwSourceOrDest, bool bForceScalarSource, int *pARLDestReg )
{
	char szTemp[1024];
	PrintParameterToString( dwToken, dwSourceOrDest, szTemp, sizeof( szTemp ), bForceScalarSource, pARLDestReg );
	return szTemp;
}

void SimplifyFourParamRegister( char *pRegister )
{
	int nLen = V_strlen( pRegister );
	if ( nLen > 5 && V_strcmp( &pRegister[nLen-5], ".xyzw" ) == 0 )
		pRegister[nLen-5] = 0;
}

int GetSwizzleComponentVectorIndex( char chMask )
{
	if ( chMask == 'x' ) return 0;
	if ( chMask == 'y' ) return 1;
	if ( chMask == 'z' ) return 2;
	if ( chMask == 'w' ) return 3;
	Error( "GetSwizzleComponentVectorIndex( '%c' ) - invalid parameter.\n", chMask );
	return 0;
}

CUtlString D3DToGL::FixGLSLSwizzle( const char *pDestRegisterName, const char *pSrcRegisterName )
{
	bool bAbsWrapper = false;
	bool bAbsNegative = false;
	char szSrcRegister[128];
	V_strncpy( szSrcRegister, pSrcRegisterName, sizeof(szSrcRegister) );

	if ( !V_strncmp( pSrcRegisterName, "abs(", 4 ) || !V_strncmp( pSrcRegisterName, "-abs(", 5 ) )
	{
		bAbsWrapper = true;
		bAbsNegative = pSrcRegisterName[0] == '-';

		const char *pOpenParen = strchr( pSrcRegisterName, '(' );
		const char *pClosingParen = strrchr( pSrcRegisterName, ')' );
		Assert ( pOpenParen && pClosingParen );

		int nRegNameLength = pClosingParen - pOpenParen - 1;
		V_strncpy( szSrcRegister, pOpenParen+1, nRegNameLength + 1 );
	}

	int nSwizzlesInDest = GetNumSwizzleComponents( pDestRegisterName );
	if ( nSwizzlesInDest == 0 ) nSwizzlesInDest = 4;

	char szFixedSrcRegister[128];
	GetParamNameWithoutSwizzle( szSrcRegister, szFixedSrcRegister, sizeof( szFixedSrcRegister ) );
	V_strncat( szFixedSrcRegister, ".", sizeof( szFixedSrcRegister ) );
	for ( int i=0; i < nSwizzlesInDest; i++ )
	{
		char chDestWriteMask = GetSwizzleComponent( pDestRegisterName, i );
		int nVectorIndex = GetSwizzleComponentVectorIndex( chDestWriteMask );

		char ch[2];
		ch[0] = GetSwizzleComponent( szSrcRegister, nVectorIndex );
		ch[1] = 0;
		V_strncat( szFixedSrcRegister, ch, sizeof( szFixedSrcRegister ) );
	}

	SimplifyFourParamRegister( szFixedSrcRegister );

	if ( bAbsWrapper )
	{
		char szTempSrcRegister[128];
		V_strncpy( szTempSrcRegister, szFixedSrcRegister, sizeof(szTempSrcRegister) );
		V_snprintf( szFixedSrcRegister, sizeof( szFixedSrcRegister ), "%sabs(%s)", bAbsNegative ? "-" : "", szTempSrcRegister ) ;
	}

	return szFixedSrcRegister;
}

inline uint32 GetRegTypeFromToken( uint32 dwToken )
{
	return ( ( dwToken & D3DSP_REGTYPE_MASK2 ) >> D3DSP_REGTYPE_SHIFT2 ) | ( ( dwToken & D3DSP_REGTYPE_MASK ) >> D3DSP_REGTYPE_SHIFT );
}

void D3DToGL::FlagIndirectRegister( uint32 dwToken, int *pARLDestReg )
{
	if ( !pARLDestReg ) return;

	switch ( dwToken & D3DVS_SWIZZLE_MASK & D3DVS_X_W )
	{
		case D3DVS_X_X: *pARLDestReg = ARL_DEST_X; break;
		case D3DVS_X_Y: *pARLDestReg = ARL_DEST_Y; break;
		case D3DVS_X_Z: *pARLDestReg = ARL_DEST_Z; break;
		case D3DVS_X_W: *pARLDestReg = ARL_DEST_W; break;
	}
}

void D3DToGL::PrintParameterToString ( uint32 dwToken, uint32 dwSourceOrDest, char *pRegisterName, int nBufLen, bool bForceScalarSource, int *pARLDestReg )
{
	char buff[32];
	bool bAllowWriteMask = true;
	bool bAllowSwizzle = true;

	uint32 dwRegNum = dwToken & D3DSP_REGNUM_MASK;
	uint32 dwRegType, dwSwizzle;
	uint32 dwSrcModifier = D3DSPSM_NONE;

	pRegisterName[ 0 ] = 0;
	dwRegType = GetRegTypeFromToken( dwToken );

	if ( dwSourceOrDest == SRC_REGISTER )
	{
		dwSrcModifier = dwToken & D3DSP_SRCMOD_MASK;
		if ( dwSrcModifier != D3DSPSM_NONE )
		{
			switch ( dwSrcModifier )
			{
				case D3DSPSM_NEG: strcat_s( pRegisterName, nBufLen, "-" ); break;
				case D3DSPSM_ABS: strcat_s( pRegisterName, nBufLen, "abs(" ); break;
				case D3DSPSM_ABSNEG: strcat_s( pRegisterName, nBufLen, "-abs(" ); break;
				default: TranslationError(); strcat_s( pRegisterName, nBufLen, "-" ); break;
			}
		}
	}

	switch ( dwRegType )
	{
		case D3DSPR_TEMP:
			V_snprintf( buff, sizeof( buff ), "r%d", dwRegNum );
			strcat_s( pRegisterName, nBufLen, buff );
			m_dwTempUsageMask |= 0x00000001 << dwRegNum;
			break;
		case D3DSPR_INPUT:
			if ( !m_bVertexShader && ( dwSourceOrDest == SRC_REGISTER ) )
			{
				if ( m_dwMajorVersion == 3 ) V_snprintf( buff, sizeof( buff ), "oTempT%d", dwRegNum );
				else V_snprintf( buff, sizeof( buff ), dwRegNum == 0 ? "_gl_FrontColor" : "_gl_FrontSecondaryColor" );
				strcat_s( pRegisterName, nBufLen, buff );
			}
			else
			{
				V_snprintf( buff, sizeof( buff ), "v%d", dwRegNum );
				strcat_s( pRegisterName, nBufLen, buff );
			}
			break;
		case D3DSPR_CONST:
			if ( m_bConstantRegisterDefined[dwRegNum] )
			{
				char szConstantRegName[3];
				V_snprintf( szConstantRegName, 3, m_bVertexShader ? "vd" : "pd" );
				V_snprintf( buff, sizeof( buff ), "%s%d", szConstantRegName, dwRegNum );
				strcat_s( pRegisterName, nBufLen, buff );
			}
			else if ( dwToken & D3DSHADER_ADDRESSMODE_MASK )
			{
				char szConstantRegName[16];
				if ( m_bVertexShader ) V_snprintf( szConstantRegName, 3, "vc" );
				else TranslationError();

				if ( ( m_bGenerateBoneUniformBuffer ) && ( dwRegNum >= DXABSTRACT_VS_FIRST_BONE_SLOT ) )
				{
					if( dwRegNum < DXABSTRACT_VS_LAST_BONE_SLOT )
					{
						dwRegNum -= DXABSTRACT_VS_FIRST_BONE_SLOT;
						V_strcpy( szConstantRegName, "vcbones" );
						m_nHighestBoneRegister = ( DXABSTRACT_VS_PARAM_SLOTS - 1 ) - DXABSTRACT_VS_FIRST_BONE_SLOT;
					}
					else
					{
						dwRegNum -= ( DXABSTRACT_VS_LAST_BONE_SLOT + 1 ) - DXABSTRACT_VS_FIRST_BONE_SLOT;
						m_nHighestRegister = m_bGenerateBoneUniformBuffer ? ( ( DXABSTRACT_VS_PARAM_SLOTS - 1 ) - ( ( DXABSTRACT_VS_LAST_BONE_SLOT + 1 ) -  DXABSTRACT_VS_FIRST_BONE_SLOT )  ): ( DXABSTRACT_VS_PARAM_SLOTS - 1 );
					}
				}
				else
				{
					m_nHighestRegister = m_bGenerateBoneUniformBuffer ? ( ( DXABSTRACT_VS_PARAM_SLOTS - 1 ) - ( ( DXABSTRACT_VS_LAST_BONE_SLOT + 1 ) -  DXABSTRACT_VS_FIRST_BONE_SLOT )  ): ( DXABSTRACT_VS_PARAM_SLOTS - 1 );
				}

				int nDstReg = -1;
				FlagIndirectRegister( GetNextToken(), &nDstReg );
				if ( pARLDestReg ) *pARLDestReg = nDstReg;

				Assert( nDstReg != ARL_DEST_NONE );
				int nSrcSwizzle = 'x';
				if ( nDstReg == ARL_DEST_Y ) nSrcSwizzle = 'y';
				else if ( nDstReg == ARL_DEST_Z ) nSrcSwizzle = 'z';
				else if ( nDstReg == ARL_DEST_W ) nSrcSwizzle = 'w';
				V_snprintf( buff, sizeof( buff ), "%s[int(va_r.%c) + %d]", szConstantRegName, nSrcSwizzle, dwRegNum );

				strcat_s( pRegisterName, nBufLen, buff );
			}
			else
			{
				char szConstantRegName[16];
				V_snprintf( szConstantRegName, 3, m_bVertexShader ? "vc" : "pc" );

				if ( ( m_bGenerateBoneUniformBuffer ) && ( dwRegNum >= DXABSTRACT_VS_FIRST_BONE_SLOT ) )
				{
					if( dwRegNum < DXABSTRACT_VS_LAST_BONE_SLOT )
					{
						dwRegNum -= DXABSTRACT_VS_FIRST_BONE_SLOT;
						V_strcpy( szConstantRegName, "vcbones" );
						m_nHighestBoneRegister = MAX( m_nHighestBoneRegister, (int)dwRegNum );
					}
					else
					{
						dwRegNum -= ( DXABSTRACT_VS_LAST_BONE_SLOT + 1 ) - DXABSTRACT_VS_FIRST_BONE_SLOT;
						m_nHighestRegister = MAX( m_nHighestRegister, dwRegNum );
					}
				}
				else
				{
					m_nHighestRegister = MAX( m_nHighestRegister, dwRegNum );
					Assert( m_nHighestRegister < DXABSTRACT_VS_PARAM_SLOTS );
				}

				V_snprintf( buff, sizeof( buff ), "%s[%d]", szConstantRegName, dwRegNum );
				strcat_s( pRegisterName, nBufLen, buff );
			}
			break;
			case D3DSPR_ADDR:
				if ( m_bVertexShader )
				{
					Assert( dwRegNum == 0 );
					V_snprintf( buff, sizeof( buff ), "va_r" );
				}
				else
				{
					if ( dwSourceOrDest == DST_REGISTER )
					{
						if ( m_nCentroidMask & ( 0x00000001 << dwRegNum ) ) V_snprintf( buff, sizeof( buff ), "centroid in vec4 oT%d", dwRegNum );
						else V_snprintf( buff, sizeof( buff ), "in vec4 oT%d", dwRegNum );
						bAllowWriteMask = false;
					}
					else
					{
						V_snprintf( buff, sizeof( buff ), "oT%d", dwRegNum );
					}
				}
				strcat_s( pRegisterName, nBufLen, buff );
				break;
			case D3DSPR_RASTOUT:
				Assert( m_bVertexShader );
				Assert( m_dwMajorVersion == 2 );
				switch( dwRegNum )
				{
					case D3DSRO_POSITION:
						strcat_s( pRegisterName, nBufLen, "vTempPos" );
						m_bDeclareVSOPos = true;
						break;

					case D3DSRO_FOG:
						if( !m_bFogFragCoord )
						{
							StrcatToHeaderCode("varying highp vec4 _gl_FogFragCoord;\n");
							m_bFogFragCoord = true;
						}
						strcat_s( pRegisterName, nBufLen, "_gl_FogFragCoord" );
						m_bDeclareVSOFog = true;
						break;

					default:
						TranslationError();
						break;
				}
				break;
					case D3DSPR_ATTROUT:
						Assert( m_bVertexShader );
						Assert( m_dwMajorVersion == 2 );

						if ( dwRegNum == 0 )
						{
							if( !m_bFrontColor )
							{
								StrcatToHeaderCode("varying highp vec4 _gl_FrontColor;\n");
								m_bFrontColor = true;
							}
							V_snprintf( buff, sizeof( buff ), "_gl_FrontColor" );
						}
						else if ( dwRegNum == 1 )
						{
							if( !m_bFrontSecondaryColor )
							{
								StrcatToHeaderCode("varying highp vec4 _gl_FrontSecondaryColor;\n");
								m_bFrontSecondaryColor = true;
							}
							V_snprintf( buff, sizeof( buff ), "_gl_FrontSecondaryColor" );
						}
						else Error( "Invalid D3DSPR_ATTROUT index" );

						strcat_s( pRegisterName, nBufLen, buff );
		break;
					case D3DSPR_TEXCRDOUT:
						if ( m_bVertexShader )
						{
							if ( m_nVSPositionOutput == (int32) dwRegNum ) V_snprintf( buff, sizeof( buff ), "vTempPos" );
							else if ( m_dwMajorVersion == 3 ) V_snprintf( buff, sizeof( buff ), "oTempT%d", dwRegNum );
							else V_snprintf( buff, sizeof( buff ), "oT%d", dwRegNum );

							m_dwTexCoordOutMask |= ( 0x00000001 << dwRegNum );
						}
						else
						{
							V_snprintf( buff, sizeof( buff ), "oC%d", dwRegNum );
						}
						strcat_s( pRegisterName, nBufLen, buff );
						break;
					case D3DSPR_CONSTINT:
						V_snprintf( buff, sizeof( buff ), "i%d", dwRegNum );
						strcat_s( pRegisterName, nBufLen, buff );
						m_dwConstIntUsageMask |= 0x00000001 << dwRegNum;
						break;
					case D3DSPR_COLOROUT:
						if( dwRegNum+1 > m_iFragDataCount )
							m_iFragDataCount = dwRegNum+1;

		V_snprintf( buff, sizeof( buff ), "gl_FragData[%d]", dwRegNum );
		strcat_s( pRegisterName, nBufLen, buff );
		m_bOutputColorRegister[dwRegNum] = true;
		break;
					case D3DSPR_DEPTHOUT:
						V_snprintf( buff, sizeof( buff ), "gl_FragDepth" );
						strcat_s( pRegisterName, nBufLen, buff );
						m_bOutputDepthRegister = true;
						break;
					case D3DSPR_SAMPLER:
						V_snprintf( buff, sizeof( buff ), "sampler%d", dwRegNum );
						strcat_s( pRegisterName, nBufLen, buff );
						break;
					case D3DSPR_CONSTBOOL:
						V_snprintf( buff, sizeof( buff ), m_bVertexShader ? "b%d" : "fb%d", dwRegNum );
						strcat_s( pRegisterName, nBufLen, buff );
						m_dwConstBoolUsageMask |= 0x00000001 << dwRegNum;
						break;
					case D3DSPR_MISCTYPE:
						Assert( dwRegNum == 0 );
						V_snprintf( buff, sizeof( buff ), "gl_FragCoord" );
						strcat_s( pRegisterName, nBufLen, buff );
						break;
					default:
						TranslationError();
						break;
	}

	if ( dwSourceOrDest == DST_REGISTER )
	{
		if ( bAllowWriteMask && ( !((dwToken & D3DSP_WRITEMASK_ALL) == D3DSP_WRITEMASK_ALL) || ((dwToken & D3DSP_WRITEMASK_ALL) == 0x00000000) ) )
		{
			strcat_s( pRegisterName, nBufLen, "." );
			if ( dwToken & D3DSP_WRITEMASK_0 ) strcat_s( pRegisterName, nBufLen, "x" );
			if ( dwToken & D3DSP_WRITEMASK_1 ) strcat_s( pRegisterName, nBufLen, "y" );
			if ( dwToken & D3DSP_WRITEMASK_2 ) strcat_s( pRegisterName, nBufLen, "z" );
			if ( dwToken & D3DSP_WRITEMASK_3 ) strcat_s( pRegisterName, nBufLen, "w" );
		}
	}
	else
	{
		if ( bAllowSwizzle )
		{
			uint32 dwXSwizzle, dwYSwizzle, dwZSwizzle, dwWSwizzle;
			dwSwizzle = dwToken & D3DVS_SWIZZLE_MASK;

			if ( dwSwizzle != D3DVS_NOSWIZZLE )
			{
				dwXSwizzle = dwSwizzle & D3DVS_X_W;
				dwYSwizzle = dwSwizzle & D3DVS_Y_W;
				dwZSwizzle = dwSwizzle & D3DVS_Z_W;
				dwWSwizzle = dwSwizzle & D3DVS_W_W;

				strcat_s( pRegisterName, nBufLen, "." );

				switch ( dwXSwizzle )
				{
					case D3DVS_X_X: strcat_s( pRegisterName, nBufLen, "x" ); break;
					case D3DVS_X_Y: strcat_s( pRegisterName, nBufLen, "y" ); break;
					case D3DVS_X_Z: strcat_s( pRegisterName, nBufLen, "z" ); break;
					case D3DVS_X_W: strcat_s( pRegisterName, nBufLen, "w" ); break;
				}

				if ( !bForceScalarSource )
				{
					if ( ((dwXSwizzle >> D3DVS_SWIZZLE_SHIFT) != (dwYSwizzle >> (D3DVS_SWIZZLE_SHIFT + 2))) ||
						((dwXSwizzle >> D3DVS_SWIZZLE_SHIFT) != (dwZSwizzle >> (D3DVS_SWIZZLE_SHIFT + 4))) ||
						((dwXSwizzle >> D3DVS_SWIZZLE_SHIFT) != (dwWSwizzle >> (D3DVS_SWIZZLE_SHIFT + 6))))
					{
						switch ( dwYSwizzle )
						{
							case D3DVS_Y_X: strcat_s( pRegisterName, nBufLen, "x" ); break;
							case D3DVS_Y_Y: strcat_s( pRegisterName, nBufLen, "y" ); break;
							case D3DVS_Y_Z: strcat_s( pRegisterName, nBufLen, "z" ); break;
							case D3DVS_Y_W: strcat_s( pRegisterName, nBufLen, "w" ); break;
						}

						switch ( dwZSwizzle )
						{
							case D3DVS_Z_X: strcat_s( pRegisterName, nBufLen, "x" ); break;
							case D3DVS_Z_Y: strcat_s( pRegisterName, nBufLen, "y" ); break;
							case D3DVS_Z_Z: strcat_s( pRegisterName, nBufLen, "z" ); break;
							case D3DVS_Z_W: strcat_s( pRegisterName, nBufLen, "w" ); break;
						}

						switch ( dwWSwizzle )
						{
							case D3DVS_W_X: strcat_s( pRegisterName, nBufLen, "x" ); break;
							case D3DVS_W_Y: strcat_s( pRegisterName, nBufLen, "y" ); break;
							case D3DVS_W_Z: strcat_s( pRegisterName, nBufLen, "z" ); break;
							case D3DVS_W_W: strcat_s( pRegisterName, nBufLen, "w" ); break;
						}
					}
				}
			}
			else
			{
				if ( bForceScalarSource ) strcat_s( pRegisterName, nBufLen, ".x" );
			}
		}

		if ( dwSrcModifier != D3DSPSM_NONE )
		{
			switch ( dwSrcModifier )
			{
				case D3DSPSM_ABS:
				case D3DSPSM_ABSNEG:
					strcat_s( pRegisterName, nBufLen, ")" );
					break;
				default:
					TranslationError();
					break;
			}
		}
	}
}

void D3DToGL::RecordInputAndOutputPositions()
{
	m_pRecordedInputTokenStart = m_pdwNextToken;
	m_nRecordedParamCodeStrlen = V_strlen( (char*)m_pBufParamCode->Base() );
	m_nRecordedALUCodeStrlen = V_strlen( (char*)m_pBufALUCode->Base() );
	m_nRecordedAttribCodeStrlen = V_strlen( (char*)m_pBufAttribCode->Base() );
}

void D3DToGL::AddTokenHexCodeToBuffer( char *pBuffer, int nSize, int nLastStrlen )
{
	int nCurStrlen = V_strlen( pBuffer );
	if ( nCurStrlen == nLastStrlen ) return;

	char szHex[512];
	szHex[0] = '\n';
	V_snprintf( &szHex[1], sizeof( szHex )-1, HEXCODE_HEADER );
	int nTokens = MIN( 10, m_pdwNextToken - m_pRecordedInputTokenStart );
	for ( int i=0; i < nTokens; i++ )
	{
		char szTemp[32];
		V_snprintf( szTemp, sizeof( szTemp ), "0x%x ", m_pRecordedInputTokenStart[i] );
		V_strncat( szHex, szTemp, sizeof( szHex ) );
	}
	V_strncat( szHex, "\n", sizeof( szHex ) );

	int nBytesToInsert = V_strlen( szHex );
	if ( nCurStrlen + nBytesToInsert + 1 >= nSize ) Error( "Buffer overflow writing token hex codes" );

	if ( m_bPutHexCodesAfterLines )
	{
		if ( pBuffer[nCurStrlen-1] == '\n' ) pBuffer[nCurStrlen-1] = 0;
		V_strncat( pBuffer, &szHex[1], nSize );
	}
	else
	{
		memmove( pBuffer + nLastStrlen + nBytesToInsert, pBuffer + nLastStrlen, nCurStrlen - nLastStrlen + 1 );
		memcpy( pBuffer + nLastStrlen, szHex, nBytesToInsert );
	}
}

void D3DToGL::AddTokenHexCode()
{
	if ( m_pdwNextToken > m_pRecordedInputTokenStart )
	{
		AddTokenHexCodeToBuffer( (char*)m_pBufParamCode->Base(), m_pBufParamCode->Size(), m_nRecordedParamCodeStrlen );
		AddTokenHexCodeToBuffer( (char*)m_pBufALUCode->Base(), m_pBufALUCode->Size(), m_nRecordedALUCodeStrlen );
		AddTokenHexCodeToBuffer( (char*)m_pBufAttribCode->Base(), m_pBufAttribCode->Size(), m_nRecordedAttribCodeStrlen );
	}
}

uint32 D3DToGL::MaintainAttributeMap( uint32 dwToken, uint32 dwRegToken )
{
	uint dwRegIndex = dwRegToken & D3DSP_REGNUM_MASK;
	if ( m_dwAttribMap[ dwRegIndex ] == 0xFFFFFFFF )
	{
		uint usage		= dwToken & D3DSP_DCL_USAGE_MASK;
		uint usageindex	= ( dwToken & D3DSP_DCL_USAGEINDEX_MASK ) >> D3DSP_DCL_USAGEINDEX_SHIFT;

		m_dwAttribMap[ dwRegIndex ] = ( usage << 4 ) | usageindex;

		if ( m_dwAttribMap[ dwRegIndex ] == 0xBB ) Debugger();
	}
	else
	{
		Debugger();
	}
	return dwRegIndex;
}

void D3DToGL::Handle_DCL()
{
	uint32 dwToken = GetNextToken();
	uint32 dwRegToken = GetNextToken();

	uint32 dwUsage = ( dwToken & D3DSP_DCL_USAGE_MASK );
	uint32 dwUsageIndex = ( dwToken & D3DSP_DCL_USAGEINDEX_MASK ) >> D3DSP_DCL_USAGEINDEX_SHIFT;

	uint32 dwRegNum = dwRegToken & D3DSP_REGNUM_MASK;
	uint32 nRegType = GetRegTypeFromToken( dwRegToken );

	if ( m_bVertexShader )
	{
		if ( ( m_dwMajorVersion >= 3 ) && ( nRegType == D3DSPR_OUTPUT ) )
		{
			if ( dwRegNum >= MAX_DECLARED_OUTPUTS ) Error( "Output register number (%d) too high (only %d supported).", dwRegNum, MAX_DECLARED_OUTPUTS );
			if ( m_DeclaredOutputs[dwRegNum] != UNDECLARED_OUTPUT ) Error( "Output dcl_ hit for register #%d more than once!", dwRegNum );

			Assert( dwToken != UNDECLARED_OUTPUT );
			m_DeclaredOutputs[dwRegNum] = dwToken;

			if ( dwUsage == D3DDECLUSAGE_POSITION )
			{
				m_nVSPositionOutput = dwUsageIndex;
				m_bDeclareVSOPos = true;
			}

			if ( m_bAddHexCodeComments )
			{
				CUtlString sParam2 = GetUsageAndIndexString( dwToken, SEMANTIC_OUTPUT );
				PrintToBuf( *m_pBufHeaderCode, "// [GL remembering that oT%d maps to %s]\n", dwRegNum, sParam2.String() );
			}

		}
		else if ( GetRegType( dwRegToken ) == D3DSPR_SAMPLER )
		{
			TranslationError();

			int nRegNum = dwRegToken & D3DSP_REGNUM_MASK;
			switch ( TextureType( dwToken ) )
			{
				default:
				case D3DSTT_UNKNOWN:
				case D3DSTT_2D:
					m_dwSamplerTypes[nRegNum] = SAMPLER_TYPE_2D;
					break;
				case D3DSTT_CUBE:
					m_dwSamplerTypes[nRegNum] = SAMPLER_TYPE_CUBE;
					break;
				case D3DSTT_VOLUME:
					m_dwSamplerTypes[nRegNum] = SAMPLER_TYPE_3D;
					break;
			}

			m_dwSamplerUsageMask |= 1 << nRegNum;
		}
		else
		{
			Assert( GetRegType( dwRegToken ) == D3DSPR_INPUT);

			CUtlString sParam1 = GetParameterString( dwRegToken, DST_REGISTER, false, NULL );
			CUtlString sParam2 = GetUsageAndIndexString( dwToken, SEMANTIC_INPUT );

			sParam2 = FixGLSLSwizzle( sParam1, sParam2 );
			PrintToBuf( *m_pBufHeaderCode, "in vec4 %s; // ", sParam1.String() );

			MaintainAttributeMap( dwToken, dwRegToken );

			char temp[128];
			sprintf( temp, "%08x %08x\n", dwToken, dwRegToken );
			StrcatToHeaderCode( temp );
		}
	}
	else
	{
		uint32 nRegType = GetRegType( dwRegToken );
		if ( nRegType == D3DSPR_SAMPLER )
		{
			int nRegNum = dwRegToken & D3DSP_REGNUM_MASK;
			switch ( TextureType( dwToken ) )
			{
				default:
				case D3DSTT_UNKNOWN:
				case D3DSTT_2D:
					m_dwSamplerTypes[nRegNum] = SAMPLER_TYPE_2D;
					break;
				case D3DSTT_CUBE:
					m_dwSamplerTypes[nRegNum] = SAMPLER_TYPE_CUBE;
					break;
				case D3DSTT_VOLUME:
					m_dwSamplerTypes[nRegNum] = SAMPLER_TYPE_3D;
					break;
			}
			m_dwSamplerUsageMask |= 1 << nRegNum;
		}
		else
		{
			if ( ( m_dwMajorVersion == 3 ) && ( nRegType == D3DSPR_INPUT ) )
			{
				Assert( m_DeclaredInputs[dwRegNum] == UNDECLARED_INPUT );
				m_DeclaredInputs[dwRegNum] = dwToken;

				if ( ( dwUsage != D3DDECLUSAGE_COLOR ) && ( dwUsage != D3DDECLUSAGE_TEXCOORD ) ) TranslationError();

				if ( dwUsage == D3DDECLUSAGE_TEXCOORD )
				{
					char buf[256];
					if ( m_nCentroidMask & ( 0x00000001 << dwUsageIndex ) ) V_snprintf( buf, sizeof( buf ), "centroid in vec4 oT%d;\n", dwUsageIndex );
					else V_snprintf( buf, sizeof( buf ), "in vec4 oT%d;\n", dwUsageIndex );
					StrcatToHeaderCode( buf );
				}
			}
			else if ( nRegType == D3DSPR_TEXTURE )
			{
				char buff[256];
				PrintParameterToString( dwRegToken, DST_REGISTER, buff, sizeof( buff ), false, NULL );
				PrintToBuf( *m_pBufHeaderCode, "%s;\n",buff );
			}
		}
	}
}

// Float conversion utilities omitted for brevity as they are unchanged from the prompt.
// (Includes HighPrec, IsFloatNaN, PrintDoubleInt, FloatToString)
