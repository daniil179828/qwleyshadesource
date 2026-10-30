#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>

#define PIPE_NAME    L"\\\\.\\pipe\\qwleyshade"
#define PAYLOAD_SIZE 0x40u

#define VT_PRESENT          8
#define VT_RESIZEBUFFERS    13
#define VT_CREATETEXTURE2D  5

#define DEPTH_BINDFLAG_DS_SRV (D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE)

#pragma pack(push, 1)
struct PipePayload
{
    uint64_t depthHandle;    
    uint64_t colorHandle;    
    uint32_t depthWidth;     
    uint32_t depthHeight;    
    uint32_t colorWidth;     
    uint32_t colorHeight;    
    uint32_t depthFormat;    
    uint32_t colorFormat;    
    uint32_t frameCount;     
    uint32_t flags;          
    uint32_t vpX;            
    uint32_t vpY;            
    uint32_t vpWidth;        
    uint32_t vpHeight;       
};
#pragma pack(pop)
static_assert(sizeof(PipePayload) == PAYLOAD_SIZE, "PipePayload size mismatch");

#define FLAG_COLOR_OK    0x01u
#define FLAG_DEPTH_OK    0x02u
#define FLAG_FPS_OK      0x04u
#define FLAG_VIEWPORT_OK 0x10u
#define FLAG_FULL_FRAME 0x20u
