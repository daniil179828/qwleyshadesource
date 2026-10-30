#pragma once
#include "UiControlProtocol.h"
#include "UiFlagLayout.h"
#include <vector>
#include <utility>
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <intrin.h>
#if !defined(_WIN64)
#error Build this DLL for Windows x64
#endif
namespace qwui {
class Server {
    HANDLE mapping_=nullptr, mutex_=nullptr;
    Shared* shared_=nullptr;
    uintptr_t image_=0, flag_=0, name_=0;
    uint32_t status_=Searching, error_=0;
    bool owned_=false;
    unsigned char original_=0, lastWritten_=0;
    uint64_t nextTick_=0;
    bool Read(uintptr_t address, void* dst, size_t n) const {
        SIZE_T got=0;
        return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(address),dst,n,&got) && got==n;
    }
    bool WritableFlag() const {
        MEMORY_BASIC_INFORMATION m{};
        if(!flag_ || !VirtualQuery(reinterpret_cast<void*>(flag_),&m,sizeof m)) return false;
        const DWORD p=m.Protect&0xff;
        if(m.State!=MEM_COMMIT || m.Type!=MEM_IMAGE || reinterpret_cast<uintptr_t>(m.AllocationBase)!=image_ ||
            (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)) || p!=PAGE_READWRITE) return false;
        unsigned char descriptor[24]{};
        return Read(flag_,descriptor,sizeof descriptor) && Descriptor(descriptor,sizeof descriptor,name_);
    }
     
    static bool ExchangeByte(uintptr_t address, unsigned char expected, unsigned char desired) {
        __try {
            return static_cast<unsigned char>(_InterlockedCompareExchange8(
                reinterpret_cast<volatile char*>(address),static_cast<char>(desired),static_cast<char>(expected)))==expected;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool Observe(unsigned char& value) const { return WritableFlag() && Read(flag_,&value,1) && value<=1; }
    void Restore() {
        if(!owned_) return;
        unsigned char value=0;
        if(!Observe(value)) {status_=AccessFailed;error_=ERROR_NOACCESS;return;}
        if(value!=lastWritten_) {owned_=false;status_=ExternalChange;return;}
        if(!ExchangeByte(flag_,lastWritten_,original_)) {status_=ExternalChange;return;}
        owned_=false;
    }
    void Hide() {
        unsigned char value=0;
        if(status_!=Ready || !Observe(value)) {if(status_==Ready)status_=InvalidLayout;return;}
        if(owned_) {
            if(value!=lastWritten_) {owned_=false;status_=ExternalChange;}
            return;
        }
        if(!ExchangeByte(flag_,value,1)) {status_=ExternalChange;return;}
        original_=value;lastWritten_=1;owned_=true;
    }
    template<class Callback> void ScanSection(uintptr_t start, size_t size, Callback callback) {
        constexpr size_t Chunk=1024*1024, Overlap=32;
        if(!size || start+size<start) return;
        const uintptr_t end=start+size;
        for(uintptr_t cursor=start;cursor<end;) {
            MEMORY_BASIC_INFORMATION m{};
            if(!VirtualQuery(reinterpret_cast<void*>(cursor),&m,sizeof m)) break;
            const uintptr_t regionEnd=std::min(end,reinterpret_cast<uintptr_t>(m.BaseAddress)+m.RegionSize);
            if(regionEnd<=cursor) break;
            const DWORD p=m.Protect&0xff;
            const bool readable=m.State==MEM_COMMIT && !(m.Protect&(PAGE_GUARD|PAGE_NOACCESS)) &&
                (p==PAGE_READONLY || p==PAGE_READWRITE || p==PAGE_WRITECOPY);
            if(readable) {
                for(uintptr_t at=cursor;at<regionEnd;) {
                    const size_t n=static_cast<size_t>(std::min<uintptr_t>(Chunk,regionEnd-at));
                    std::vector<unsigned char> bytes(n);
                    if(Read(at,bytes.data(),n)) callback(at,bytes);
                    if(n<=Overlap) break;
                    at+=n-Overlap;
                }
            }
            cursor=regionEnd;
        }
    }
    void Resolve() {
        if(owned_) {status_=ExternalChange;return;}
        flag_=name_=0;status_=Searching;error_=0;
        wchar_t path[MAX_PATH]{};
        if(!GetModuleFileNameW(nullptr,path,MAX_PATH)) {status_=AccessFailed;return;}
        const wchar_t* file=wcsrchr(path,L'\\');file=file?file+1:path;
        if(_wcsicmp(file,L"RobloxPlayerBeta.exe")!=0) {status_=UnsupportedProcess;return;}
        image_=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
        if(!Read(image_,&dos,sizeof dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<=0 || dos.e_lfanew>0x100000 ||
            !Read(image_+dos.e_lfanew,&nt,sizeof nt) || nt.Signature!=IMAGE_NT_SIGNATURE ||
            nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.FileHeader.NumberOfSections==0 || nt.FileHeader.NumberOfSections>96 || nt.OptionalHeader.SizeOfImage>1024u*1024u*1024u) {
            status_=InvalidLayout;return;
        }
        const uintptr_t sectionBase=image_+dos.e_lfanew+offsetof(IMAGE_NT_HEADERS64,OptionalHeader)+nt.FileHeader.SizeOfOptionalHeader;
        std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
        if(!Read(sectionBase,sections.data(),sections.size()*sizeof sections[0])) {status_=AccessFailed;return;}
        auto bounds=[&](const IMAGE_SECTION_HEADER& sec) {
            return sec.VirtualAddress<nt.OptionalHeader.SizeOfImage &&
                sec.Misc.VirtualSize<=nt.OptionalHeader.SizeOfImage-sec.VirtualAddress;
        };
        std::vector<uintptr_t> names;
        for(const auto& sec:sections) {
            if(!bounds(sec) || (sec.Characteristics&IMAGE_SCN_MEM_EXECUTE) ||
                !(sec.Characteristics&IMAGE_SCN_MEM_READ) || (sec.Characteristics&IMAGE_SCN_MEM_WRITE)) continue;
            ScanSection(image_+sec.VirtualAddress,sec.Misc.VirtualSize,[&](uintptr_t base,const std::vector<unsigned char>& data) {
                for(size_t i=1;i+sizeof(FlagName)<=data.size();++i) {
                    if(data[i-1]==0 && std::memcmp(data.data()+i,FlagName,sizeof FlagName)==0) {
                        const uintptr_t address=base+i;
                        if(std::find(names.begin(),names.end(),address)==names.end() && names.size()<65) names.push_back(address);
                    }
                }
            });
        }
        if(names.empty()) {status_=NotFound;return;}
        if(names.size()>64) {status_=Ambiguous;return;}
        std::vector<std::pair<uintptr_t,uintptr_t>> candidates;
        for(const auto& sec:sections) {
            if(!bounds(sec) || !(sec.Characteristics&IMAGE_SCN_MEM_WRITE) || (sec.Characteristics&IMAGE_SCN_MEM_EXECUTE))continue;
            ScanSection(image_+sec.VirtualAddress,sec.Misc.VirtualSize,[&](uintptr_t base,const std::vector<unsigned char>& data) {
                const size_t align=static_cast<size_t>((8-(base&7))&7);
                for(size_t i=align;i+24<=data.size();i+=8) {
                    for(uintptr_t name:names) if(Descriptor(data.data()+i,data.size()-i,name)) {
                        const auto value=std::make_pair(base+i,name);
                        if(std::find(candidates.begin(),candidates.end(),value)==candidates.end() && candidates.size()<3)
                            candidates.push_back(value);
                    }
                }
            });
        }
        if(candidates.empty()) {status_=NotFound;return;}
        if(candidates.size()!=1) {status_=Ambiguous;return;}
        flag_=candidates[0].first;name_=candidates[0].second;
        status_=WritableFlag()?Ready:InvalidLayout;
    }
    bool Lock() const {
        if(!mutex_)return false;
        const DWORD r=WaitForSingleObject(mutex_,0);
        return r==WAIT_OBJECT_0 || r==WAIT_ABANDONED;
    }
    void Publish() {
        unsigned char observed=0;
        const bool readable=Observe(observed);
        shared_->status=status_;shared_->lastError=error_;
        shared_->observedHidden=readable?observed:0;shared_->owned=owned_?1u:0u;
        shared_->serverHeartbeat=GetTickCount64();
    }
public:
    void Initialize() {
        wchar_t map[96],mutex[96];Names(GetCurrentProcessId(),map,mutex);
        mutex_=CreateMutexW(nullptr,FALSE,mutex);if(!mutex_)return;
        mapping_=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Shared),map);
        const DWORD mappingError=GetLastError();
        if(!mapping_ || mappingError==ERROR_ALREADY_EXISTS) {Close();return;}
        shared_=static_cast<Shared*>(MapViewOfFile(mapping_,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));
        if(!shared_ || !Lock()) {Close();return;}
        std::memset(shared_,0,sizeof *shared_);
        shared_->version=Version;shared_->bytes=sizeof(Shared);shared_->producerPid=GetCurrentProcessId();
        LARGE_INTEGER counter{};QueryPerformanceCounter(&counter);
        shared_->session=static_cast<uint64_t>(counter.QuadPart)^GetTickCount64();
        shared_->status=Searching;shared_->serverHeartbeat=GetTickCount64();shared_->magic=Magic;
        ReleaseMutex(mutex_);
        try {Resolve();} catch(...) {status_=AccessFailed;error_=ERROR_NOT_ENOUGH_MEMORY;}
        Tick(true);
    }
    void Tick(bool force=false) {
        if(!shared_)return;
        const uint64_t now=GetTickCount64();
        if(!force && now<nextTick_)return;
        nextTick_=now+20;
        if(!Lock())return;
         
        const bool valid=Valid(*shared_,GetCurrentProcessId());
        const bool lease=valid && shared_->clientPid && Fresh(now,shared_->clientHeartbeat);
        const uint32_t owner=shared_->clientPid;
        const uint32_t request=shared_->requestId,opcode=shared_->opcode;
        const bool hide=shared_->requestedHide==1;
        const bool pending=lease && request!=shared_->ackId;
        ReleaseMutex(mutex_);
        if(!lease) Restore();
        else if(pending) {
            if(opcode==Rescan) {
                Restore();  
                if(!owned_) {
                    try {Resolve();} catch(...) {status_=AccessFailed;error_=ERROR_NOT_ENOUGH_MEMORY;}
                }
            } else if(opcode==SetHidden) {
                if(hide) Hide();else Restore();
            }
        }
        if(owned_) {
            unsigned char current=0;
            if(!Observe(current)) {status_=AccessFailed;error_=ERROR_NOACCESS;}
            else if(current!=lastWritten_) {owned_=false;status_=ExternalChange;}
        }
        if(Lock()) {
            if(pending && shared_->requestId==request && shared_->clientPid==owner)shared_->ackId=request;
            Publish();ReleaseMutex(mutex_);
        }
    }
    void Close() {
        Restore();
        if(shared_ && Lock()) {
            status_=Stopped;Publish();shared_->serverHeartbeat=0;ReleaseMutex(mutex_);
        }
        if(shared_) UnmapViewOfFile(shared_);
        shared_=nullptr;
        if(mapping_) CloseHandle(mapping_);
        mapping_=nullptr;
        if(mutex_) CloseHandle(mutex_);
        mutex_=nullptr;
    }
};
}
