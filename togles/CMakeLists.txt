cmake_minimum_required(VERSION 3.16)
project(libtogles_client)

set(SRCDIR "${CMAKE_SOURCE_DIR}")
set(CMAKE_MODULE_PATH ${SRCDIR}/cmake)


set(OUTBINNAME "libtogl_client") 
set(OUTBINDIR ${SRCDIR}/../game/bin)


set(TOGLES_SRCDIR "${SRCDIR}/togles/linuxwin")
set(TOGLES_INCDIR "${SRCDIR}/public/togles")

include(${CMAKE_MODULE_PATH}/detect_platform.cmake)
include(${CMAKE_MODULE_PATH}/source_dll_base.cmake)

include_directories("../")
include_directories("../public")
include_directories("../public/togles")
#include_directories("../public/togl")

if(OSXALL)
    add_definitions(-DTOGL_DLL_EXPORT)
else()
    add_definitions(-DTOGL_DLL_EXPORT -DPROTECTED_THINGS_ENABLE -Dstrncpy=use_Q_strncpy_instead -D_snprintf=use_Q_snprintf_instead)
endif()


add_definitions(-DTOGLES -DUSE_OPENGLES -DDX_TO_GL_ABSTRACTION)

if(POSIX)
    target_compile_options(${OUTBINNAME} PRIVATE -fpic -fno-semantic-interposition -Wno-narrowing)
endif()

if( GL ) 
    target_sources(${OUTBINNAME} PRIVATE "${TOGLES_SRCDIR}/dx9asmtogl2.cpp")
    target_sources(${OUTBINNAME} PRIVATE "${TOGLES_SRCDIR}/dxabstract.cpp")
    target_sources(${OUTBINNAME} PRIVATE "${TOGLES_SRCDIR}/glentrypoints.cpp")
    target_sources(${OUTBINNAME} PRIVATE "${TOGLES_SRCDIR}/glmgr.cpp")
    target_sources(${OUTBINNAME} PRIVATE "${TOGLES_SRCDIR}/glmgrbasics.cpp")
    target_sources(${OUTBINNAME} PRIVATE "${TOGLES_SRCDIR}/cglmtex.cpp")
    target_sources(${OUTBINNAME} PRIVATE "${TOGLES_SRCDIR}/cglmfbo.cpp")
    target_sources(${OUTBINNAME} PRIVATE "${TOGLES_SRCDIR}/cglmprogram.cpp")
    target_sources(${OUTBINNAME} PRIVATE "${TOGLES_SRCDIR}/cglmbuffer.cpp")
    target_sources(${OUTBINNAME} PRIVATE "${TOGLES_SRCDIR}/cglmquery.cpp")

    target_sources(${OUTBINNAME} PRIVATE "${TOGLES_SRCDIR}/asanstubs.cpp")
    target_sources(${OUTBINNAME} PRIVATE "${TOGLES_SRCDIR}/decompress.c")

    if( OSXALL )
        target_sources(${OUTBINNAME} PRIVATE "${TOGLES_SRCDIR}/glmgrcocoa.mm")
        target_sources(${OUTBINNAME} PRIVATE "${TOGLES_SRCDIR}/intelglmallocworkaround.cpp")
        target_sources(${OUTBINNAME} PRIVATE "${TOGLES_SRCDIR}/mach_override.c")
    endif()
endif()

target_link_libraries(${OUTBINNAME} tier2_client vstdlib_client mathlib_client )
