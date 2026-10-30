#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
namespace qwui {
constexpr char FlagName[] = "DebugDontRenderUI";
constexpr size_t FlagLength = sizeof(FlagName)-1;
static_assert(FlagLength==17, "Exact flag name");
 
 
inline bool Descriptor(const unsigned char* p, size_t size, uint64_t nameAddress) {
    if(size<24 || p[0]>1) return false;
    for(size_t i=1;i<8;++i) if(p[i]!=0) return false;
    uint64_t name=0,length=0;
    std::memcpy(&name,p+8,8); std::memcpy(&length,p+16,8);
    return name==nameAddress && length==FlagLength;
}
}
