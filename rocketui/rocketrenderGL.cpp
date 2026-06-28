#include "rocketrender.h"

#if defined( TOGLES )
#include <GLES3/gl3.h>
#elif defined RMLUI_PLATFORM_WIN32
#include <win32/IncludeWindows.h>
#include <gl/Gl.h>
#include <gl/Glu.h>
#elif defined RMLUI_PLATFORM_MACOSX
#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glu.h>
#include <OpenGL/glext.h>
#elif defined RMLUI_PLATFORM_UNIX
#include <GL/glx.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glu.h>
// The None define from X.h conflicts with RmlUi code base,
// use the constant 0L instead where necessary
#ifdef None
#undef None
#endif
#endif

#include <RmlUi/Core.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

RocketRender RocketRender::m_Instance;

RocketRender::RocketRender()
    : m_glContext( nullptr ),
      m_width( 0 ),
      m_height( 0 ),
      m_transformEnabled( false )
#if defined( TOGLES )
      , m_program( 0 ),
      m_uniformViewport( -1 ),
      m_uniformTranslation( -1 ),
      m_uniformTexture( -1 ),
      m_uniformUseTexture( -1 ),
      m_programReady( false )
#endif
{
}

#if defined( TOGLES )

static bool CompileRocketShader( GLuint shader, const char *source )
{
    glShaderSource( shader, 1, &source, NULL );
    glCompileShader( shader );

    GLint status = 0;
    glGetShaderiv( shader, GL_COMPILE_STATUS, &status );
    if ( status )
        return true;

    GLchar log[1024];
    GLsizei length = 0;
    glGetShaderInfoLog( shader, sizeof( log ), &length, log );
    fprintf( stderr, "RocketUI GLES shader compile failed: %s\n", log );
    return false;
}

bool RocketRender::InitGLESProgram()
{
    if ( m_programReady )
        return true;

    static const char *vertexShader =
        "#version 300 es\n"
        "precision highp float;\n"
        "in vec2 inPosition;\n"
        "in vec4 inColor;\n"
        "in vec2 inTexCoord;\n"
        "uniform vec4 uViewport;\n"
        "uniform vec4 uTranslation;\n"
        "out vec4 vColor;\n"
        "out vec2 vTexCoord;\n"
        "void main()\n"
        "{\n"
        "    vec2 p = inPosition + uTranslation.xy;\n"
        "    vec2 ndc = vec2((p.x / uViewport.x) * 2.0 - 1.0, (p.y / uViewport.y) * 2.0 - 1.0);\n"
        "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
        "    vColor = inColor;\n"
        "    vTexCoord = inTexCoord;\n"
        "}\n";

    static const char *fragmentShader =
        "#version 300 es\n"
        "precision highp float;\n"
        "in vec4 vColor;\n"
        "in vec2 vTexCoord;\n"
        "uniform sampler2D uTexture;\n"
        "uniform bool uUseTexture;\n"
        "out vec4 fragColor;\n"
        "void main()\n"
        "{\n"
        "    vec4 texColor = uUseTexture ? texture(uTexture, vTexCoord) : vec4(1.0);\n"
        "    fragColor = texColor * vColor;\n"
        "}\n";

    GLuint vs = glCreateShader( GL_VERTEX_SHADER );
    GLuint fs = glCreateShader( GL_FRAGMENT_SHADER );
    if ( !vs || !fs || !CompileRocketShader( vs, vertexShader ) || !CompileRocketShader( fs, fragmentShader ) )
    {
        if ( vs ) glDeleteShader( vs );
        if ( fs ) glDeleteShader( fs );
        return false;
    }

    m_program = glCreateProgram();
    glAttachShader( m_program, vs );
    glAttachShader( m_program, fs );
    glBindAttribLocation( m_program, 0, "inPosition" );
    glBindAttribLocation( m_program, 1, "inColor" );
    glBindAttribLocation( m_program, 2, "inTexCoord" );
    glLinkProgram( m_program );

    glDeleteShader( vs );
    glDeleteShader( fs );

    GLint linked = 0;
    glGetProgramiv( m_program, GL_LINK_STATUS, &linked );
    if ( !linked )
    {
        GLchar log[1024];
        GLsizei length = 0;
        glGetProgramInfoLog( m_program, sizeof( log ), &length, log );
        fprintf( stderr, "RocketUI GLES program link failed: %s\n", log );
        glDeleteProgram( m_program );
        m_program = 0;
        return false;
    }

    m_uniformViewport = glGetUniformLocation( m_program, "uViewport" );
    m_uniformTranslation = glGetUniformLocation( m_program, "uTranslation" );
    m_uniformTexture = glGetUniformLocation( m_program, "uTexture" );
    m_uniformUseTexture = glGetUniformLocation( m_program, "uUseTexture" );

    glUseProgram( m_program );
    glUniform1i( m_uniformTexture, 0 );
    glUseProgram( 0 );

    m_programReady = true;
    return true;
}

void RocketRender::PrepareGLState()
{
    if ( !InitGLESProgram() )
        return;

    glActiveTexture( GL_TEXTURE0 );
    glBindBuffer( GL_ARRAY_BUFFER, 0 );
    glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
    glDisable( GL_CULL_FACE );
    glDisable( GL_DEPTH_TEST );
    glDisable( GL_STENCIL_TEST );
    glEnable( GL_BLEND );
    glBlendColor( 1, 1, 1, 1 );
    glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
    glBlendEquation( GL_FUNC_ADD );
    glDepthMask( GL_FALSE );
    glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
    glViewport( 0, 0, m_width, m_height );
    glUseProgram( m_program );

    const GLfloat viewport[4] = { (GLfloat)m_width, (GLfloat)m_height, 0.0f, 0.0f };
    glUniform4fv( m_uniformViewport, 1, viewport );
}

void RocketRender::RenderGeometry( Rml::Vertex *vertices, int num_vertices, int *indices, int num_indices,
                                   Rml::TextureHandle texture, const Rml::Vector2f &translation )
{
    if ( !vertices || !indices || num_vertices <= 0 || num_indices <= 0 || !InitGLESProgram() )
        return;

    glUseProgram( m_program );
    const GLfloat translationUniform[4] = { translation.x, translation.y, 0.0f, 0.0f };
    glUniform4fv( m_uniformTranslation, 1, translationUniform );
    glUniform1i( m_uniformUseTexture, texture ? 1 : 0 );

    glActiveTexture( GL_TEXTURE0 );
    glBindTexture( GL_TEXTURE_2D, (GLuint)texture );

    glEnableVertexAttribArray( 0 );
    glEnableVertexAttribArray( 1 );
    glEnableVertexAttribArray( 2 );
    glVertexAttribPointer( 0, 2, GL_FLOAT, GL_FALSE, sizeof( Rml::Vertex ), &vertices[0].position );
    glVertexAttribPointer( 1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof( Rml::Vertex ), &vertices[0].colour );
    glVertexAttribPointer( 2, 2, GL_FLOAT, GL_FALSE, sizeof( Rml::Vertex ), &vertices[0].tex_coord );

    glDrawRangeElements( GL_TRIANGLES, 0, (GLuint)( num_vertices - 1 ), num_indices, GL_UNSIGNED_INT, indices );

    glDisableVertexAttribArray( 0 );
    glDisableVertexAttribArray( 1 );
    glDisableVertexAttribArray( 2 );
}

Rml::CompiledGeometryHandle RocketRender::CompileGeometry(Rml::Vertex *vertices, int num_vertices, int *indices, int num_indices, Rml::TextureHandle texture)
{
    return 0;
}

void RocketRender::EnableScissorRegion(bool enable)
{
    if ( enable )
        glEnable( GL_SCISSOR_TEST );
    else
        glDisable( GL_SCISSOR_TEST );
}

void RocketRender::SetScissorRegion(int x, int y, int width, int height)
{
    glScissor( x, y, width, height );
}

#pragma pack(1)
struct TGAHeader
{
    char  idLength;
    char  colourMapType;
    char  dataType;
    short int colourMapOrigin;
    short int colourMapLength;
    char  colourMapDepth;
    short int xOrigin;
    short int yOrigin;
    short int width;
    short int height;
    char  bitsPerPixel;
    char  imageDescriptor;
};
#pragma pack()

bool RocketRender::LoadTexture(Rml::TextureHandle &texture_handle, Rml::Vector2i &texture_dimensions, const Rml::String &source)
{
    Rml::FileInterface* file_interface = Rml::GetFileInterface();
    Rml::FileHandle file_handle = file_interface->Open(source);
    if (!file_handle)
        return false;

    file_interface->Seek(file_handle, 0, SEEK_END);
    size_t buffer_size = file_interface->Tell(file_handle);
    file_interface->Seek(file_handle, 0, SEEK_SET);

    if(buffer_size <= sizeof(TGAHeader))
    {
        file_interface->Close(file_handle);
        return false;
    }

    char* buffer = new char[buffer_size];
    file_interface->Read(buffer, buffer_size, file_handle);
    file_interface->Close(file_handle);

    TGAHeader header;
    memcpy(&header, buffer, sizeof(TGAHeader));

    int color_mode = header.bitsPerPixel / 8;
    int image_size = header.width * header.height * 4;

    if (header.dataType != 2 || color_mode < 3)
    {
        delete [] buffer;
        return false;
    }

    const char* image_src = buffer + sizeof(TGAHeader);
    unsigned char* image_dest = new unsigned char[image_size];

    for (long y = 0; y < header.height; y++)
    {
        long read_index = y * header.width * color_mode;
        long write_index = ((header.imageDescriptor & 32) != 0) ? read_index : (header.height - y - 1) * header.width * 4;
        for (long x = 0; x < header.width; x++)
        {
            image_dest[write_index] = image_src[read_index+2];
            image_dest[write_index+1] = image_src[read_index+1];
            image_dest[write_index+2] = image_src[read_index];
            image_dest[write_index+3] = (color_mode == 4) ? image_src[read_index+3] : 255;

            write_index += 4;
            read_index += color_mode;
        }
    }

    texture_dimensions.x = header.width;
    texture_dimensions.y = header.height;

    bool success = GenerateTexture(texture_handle, image_dest, texture_dimensions);

    delete [] image_dest;
    delete [] buffer;

    return success;
}

bool RocketRender::GenerateTexture(Rml::TextureHandle &texture_handle, const Rml::byte *source, const Rml::Vector2i &source_dimensions)
{
    GLuint texture_id = 0;
    glGenTextures(1, &texture_id);
    if (texture_id == 0)
        return false;

    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, source_dimensions.x, source_dimensions.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, source);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    texture_handle = (Rml::TextureHandle) texture_id;
    return true;
}

void RocketRender::ReleaseTexture(Rml::TextureHandle texture)
{
    GLuint texture_id = (GLuint) texture;
    glDeleteTextures(1, &texture_id);
}

void RocketRender::SetTransform(const Rml::Matrix4f *transform)
{
    // The GLES path currently renders untransformed geometry; normal HUD/menu documents do not rely on this.
    m_transformEnabled = (bool)transform;
}

#else

void RocketRender::PrepareGLState()
{
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_CULL_FACE);

    //make sure to set both of these to zero otherwise mesa will segfault even though it only mentions GL_ARRAY_BUFFER in the docs
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);



    //int backup0, backup1, backup2, backup3, backup4, backup5, backup6, backup7, backup8, backup9, backup10,
    //        backup11, backup12, backup13, backup14, backup15;
    //glGetVertexAttribIiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &backup0 );
    //glGetVertexAttribIiv(1, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &backup1 );
    //glGetVertexAttribIiv(2, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &backup2 );
    //glGetVertexAttribIiv(3, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &backup3 );
    //glGetVertexAttribIiv(4, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &backup4 );
    //glGetVertexAttribIiv(5, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &backup5 );
    //glGetVertexAttribIiv(6, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &backup6 );
    //glGetVertexAttribIiv(7, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &backup7 );
    //glGetVertexAttribIiv(8, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &backup8 );
    //glGetVertexAttribIiv(9, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &backup9 );
    //glGetVertexAttribIiv(10, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &backup10 );
    //glGetVertexAttribIiv(11, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &backup11 );
    //glGetVertexAttribIiv(12, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &backup12 );
    //glGetVertexAttribIiv(13, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &backup13 );
    //glGetVertexAttribIiv(14, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &backup14 );
    //glGetVertexAttribIiv(15, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &backup15 );
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(3);
    glDisableVertexAttribArray(4);
    glDisableVertexAttribArray(5);
    glDisableVertexAttribArray(6);
    glDisableVertexAttribArray(7);
    glDisableVertexAttribArray(8);
    glDisableVertexAttribArray(9);
    glDisableVertexAttribArray(10);
    glDisableVertexAttribArray(11);
    glDisableVertexAttribArray(12);
    glDisableVertexAttribArray(13);
    glDisableVertexAttribArray(14);
    glDisableVertexAttribArray(15);

    glDisable(GL_ALPHA_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);

    glEnable(GL_BLEND);
    glBlendColor(1, 1, 1, 1);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, m_width, 0, m_height, -10000, 10000);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);

    //glStencilFunc( GL_GEQUAL, 253, -1 );
    //glAlphaFunc(GL_GEQUAL, 0);

}

void RocketRender::RenderGeometry( Rml::Vertex *vertices, int num_vertices, int *indices, int num_indices,
                                   Rml::TextureHandle texture, const Rml::Vector2f &translation )
{
    RMLUI_UNUSED(num_vertices);
    glPushMatrix();

    glTranslatef(translation.x, translation.y, 0);

    glVertexPointer(2, GL_FLOAT, sizeof(Rml::Vertex), &vertices[0].position);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Rml::Vertex), &vertices[0].colour);

    if (!texture)
    {
        glDisable(GL_TEXTURE_2D);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    else
    {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, (GLuint) texture);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, sizeof(Rml::Vertex), &vertices[0].tex_coord);
    }

    glDrawElements(GL_TRIANGLES, num_indices, GL_UNSIGNED_INT, indices);

    glPopMatrix();
}


Rml::CompiledGeometryHandle RocketRender::CompileGeometry(Rml::Vertex *vertices, int num_vertices, int *indices, int num_indices, Rml::TextureHandle texture)
{
    return 0;
}

// Called by RmlUi when it wants to enable or disable scissoring to clip content.
void RocketRender::EnableScissorRegion(bool enable)
{
    if (enable) {
        if (!m_transformEnabled) {
            glEnable(GL_SCISSOR_TEST);
            glDisable(GL_STENCIL_TEST);
        } else {
            glDisable(GL_SCISSOR_TEST);
            glEnable(GL_STENCIL_TEST);
        }
    } else {
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
    }
}

// Called by RmlUi when it wants to change the scissor region.
void RocketRender::SetScissorRegion(int x, int y, int width, int height)
{
    if (!m_transformEnabled) {
        glScissor(x, y, width, height);
    } else {
        // clear the stencil buffer
        glStencilMask(GLuint(-1));
        glClear(GL_STENCIL_BUFFER_BIT);

        // fill the stencil buffer
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDepthMask(GL_FALSE);
        glStencilFunc(GL_NEVER, 1, GLuint(-1));
        glStencilOp(GL_REPLACE, GL_KEEP, GL_KEEP);

        float fx = (float)x;
        float fy = (float)y;
        float fwidth = (float)width;
        float fheight = (float)height;

        // draw transformed quad
        GLfloat vertices[] = {
                fx, fy, 0,
                fx, fy + fheight, 0,
                fx + fwidth, fy + fheight, 0,
                fx + fwidth, fy, 0
        };
        glDisableClientState(GL_COLOR_ARRAY);
        glVertexPointer(3, GL_FLOAT, 0, vertices);
        GLushort indices[] = { 1, 2, 0, 3 };
        glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_SHORT, indices);
        glEnableClientState(GL_COLOR_ARRAY);

        // prepare for drawing the real thing
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glStencilMask(0);
        glStencilFunc(GL_EQUAL, 1, GLuint(-1));
    }
}

// Set to byte packing, or the compiler will expand our struct, which means it won't read correctly from file
#pragma pack(1)
struct TGAHeader
{
    char  idLength;
    char  colourMapType;
    char  dataType;
    short int colourMapOrigin;
    short int colourMapLength;
    char  colourMapDepth;
    short int xOrigin;
    short int yOrigin;
    short int width;
    short int height;
    char  bitsPerPixel;
    char  imageDescriptor;
};
// Restore packing
#pragma pack()

bool RocketRender::LoadTexture(Rml::TextureHandle &texture_handle, Rml::Vector2i &texture_dimensions, const Rml::String &source)
{
    Rml::FileInterface* file_interface = Rml::GetFileInterface();
    Rml::FileHandle file_handle = file_interface->Open(source);
    if (!file_handle)
    {
        return false;
    }

    file_interface->Seek(file_handle, 0, SEEK_END);
    size_t buffer_size = file_interface->Tell(file_handle);
    file_interface->Seek(file_handle, 0, SEEK_SET);

    RMLUI_ASSERTMSG(buffer_size > sizeof(TGAHeader), "Texture file size is smaller than TGAHeader, file must be corrupt or otherwise invalid");
    if(buffer_size <= sizeof(TGAHeader))
    {
        file_interface->Close(file_handle);
        return false;
    }

    char* buffer = new char[buffer_size];
    file_interface->Read(buffer, buffer_size, file_handle);
    file_interface->Close(file_handle);

    TGAHeader header;
    memcpy(&header, buffer, sizeof(TGAHeader));

    int color_mode = header.bitsPerPixel / 8;
    int image_size = header.width * header.height * 4; // We always make 32bit textures

    if (header.dataType != 2)
    {
        Rml::Log::Message(Rml::Log::LT_ERROR, "Only 24/32bit uncompressed TGAs are supported.");
        return false;
    }

    // Ensure we have at least 3 colors
    if (color_mode < 3)
    {
        Rml::Log::Message(Rml::Log::LT_ERROR, "Only 24 and 32bit textures are supported");
        return false;
    }

    const char* image_src = buffer + sizeof(TGAHeader);
    unsigned char* image_dest = new unsigned char[image_size];

    // Targa is BGR, swap to RGB and flip Y axis
    for (long y = 0; y < header.height; y++)
    {
        long read_index = y * header.width * color_mode;
        long write_index = ((header.imageDescriptor & 32) != 0) ? read_index : (header.height - y - 1) * header.width * color_mode;
        for (long x = 0; x < header.width; x++)
        {
            image_dest[write_index] = image_src[read_index+2];
            image_dest[write_index+1] = image_src[read_index+1];
            image_dest[write_index+2] = image_src[read_index];
            if (color_mode == 4)
                image_dest[write_index+3] = image_src[read_index+3];
            else
                image_dest[write_index+3] = 255;

            write_index += 4;
            read_index += color_mode;
        }
    }

    texture_dimensions.x = header.width;
    texture_dimensions.y = header.height;

    bool success = GenerateTexture(texture_handle, image_dest, texture_dimensions);

    delete [] image_dest;
    delete [] buffer;

    return success;
}

bool RocketRender::GenerateTexture(Rml::TextureHandle &texture_handle, const Rml::byte *source, const Rml::Vector2i &source_dimensions)
{
    GLuint texture_id = 0;
    glGenTextures(1, &texture_id);
    if (texture_id == 0)
    {
        fprintf(stdout,"Failed to generate textures\n");
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, texture_id);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, source_dimensions.x, source_dimensions.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, source);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    texture_handle = (Rml::TextureHandle) texture_id;

    return true;
}

void RocketRender::ReleaseTexture(Rml::TextureHandle texture)
{
    glDeleteTextures(1, (GLuint*) &texture);
}

void RocketRender::SetTransform(const Rml::Matrix4f *transform)
{
    m_transformEnabled = (bool)transform;

    if (transform)
    {
        if (std::is_same<Rml::Matrix4f, Rml::ColumnMajorMatrix4f>::value)
            glLoadMatrixf(transform->data());
        else if (std::is_same<Rml::Matrix4f, Rml::RowMajorMatrix4f>::value)
            glLoadMatrixf(transform->Transpose().data());
    }
    else
        glLoadIdentity();
}

#endif
