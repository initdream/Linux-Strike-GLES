#ifndef CS_WORKSHOP_MANAGER_H
#define CS_WORKSHOP_MANAGER_H

#include "platform.h"

class CCSWorkshopManager
{
public:
    CCSWorkshopManager() {}
    
    bool IsDownloadingMap( uint64 id ) { return false; }
    float GetMapDownloadingProgress( uint64 id ) { return 0.0f; }
    void ViewCommunityMapInWorkshop( uint64 id ) {}

    // Stub for fetching UGC file paths
    bool GetUGCFullPath( uint64 hContent, char* pOutPath, int nMaxLen )
    {
        if ( pOutPath && nMaxLen > 0 )
        {
            pOutPath[0] = '\0';
        }
        return false;
    }
};

// Declare the global instance
extern CCSWorkshopManager g_CSGOWorkshopMaps;

// Helper function used to access the manager in scaleform code
inline CCSWorkshopManager& WorkshopManager()
{
    return g_CSGOWorkshopMaps;
}

#endif // CS_WORKSHOP_MANAGER_H