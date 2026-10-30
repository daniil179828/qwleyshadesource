#pragma once
#include "UiControlProtocol.h"
namespace qwui {
class Client {
    HANDLE mapping_=nullptr, mutex_=nullptr;
    Shared* shared_=nullptr;
    DWORD pid_=0;
    uint64_t retry_=0, session_=0;
    bool desired_=false, dirty_=false;
    uint32_t opcode_=SetHidden;
    Shared status_{};
    bool live_=false, busy_=false;
    bool Lock() const {
        if(!mutex_)return false;
        const DWORD r=WaitForSingleObject(mutex_,0);
        return r==WAIT_OBJECT_0 || r==WAIT_ABANDONED;
    }
public:
    bool Desired() const{return desired_;}
    bool Live() const{return live_;}
    bool ReadyForRequest() const{return live_ && !busy_ && status_.status==Ready;}
    bool Pending() const{return live_ && status_.requestId!=status_.ackId;}
    bool ObservedHidden() const{return live_ && status_.observedHidden==1;}
    const char* Text() const {
        if(busy_)return "Another overlay owns the UI control lease";
        if(!live_)return "UI control unavailable: matching DLL not connected or heartbeat expired";
        return StatusText(status_.status);
    }
    void Set(bool hidden) {
        if(hidden && !ReadyForRequest())return;
        desired_=hidden;dirty_=true;opcode_=SetHidden;
    }
    void RetrySearch(){if(live_&&!busy_&&!status_.owned){dirty_=true;opcode_=Rescan;desired_=false;}}
    void Close() {
        if(shared_ && Lock()) {
            if(Valid(*shared_,pid_) && shared_->clientPid==GetCurrentProcessId()) {
                shared_->requestedHide=0;shared_->opcode=SetHidden;
                ++shared_->requestId;shared_->clientHeartbeat=0;  
            }
            ReleaseMutex(mutex_);
        }
        if(shared_) UnmapViewOfFile(shared_);
        shared_=nullptr;
        if(mapping_) CloseHandle(mapping_);
        mapping_=nullptr;
        if(mutex_) CloseHandle(mutex_);
        mutex_=nullptr;
        live_=false;busy_=false;desired_=false;dirty_=false;session_=0;status_={};
    }
    void Tick(DWORD pid) {
        const uint64_t now=GetTickCount64();
        if(pid!=pid_){Close();pid_=pid;retry_=0;}
        if(!pid_)return;
        if(!shared_) {
            if(now<retry_)return;
            retry_=now+500;
            wchar_t map[96],mutex[96];Names(pid_,map,mutex);
            mutex_=OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,mutex);
            if(!mutex_)return;
            mapping_=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,map);
            if(mapping_)shared_=static_cast<Shared*>(MapViewOfFile(mapping_,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));
            if(!shared_){Close();return;}
        }
        if(!Lock())return;
        if(!Valid(*shared_,pid_)) {ReleaseMutex(mutex_);Close();return;}
        status_=*shared_;live_=Fresh(now,status_.serverHeartbeat);
        busy_=status_.clientPid && status_.clientPid!=GetCurrentProcessId() && Fresh(now,status_.clientHeartbeat);
        if(session_!=status_.session){session_=status_.session;desired_=false;dirty_=true;opcode_=SetHidden;}
         
         
        if(live_ && !busy_ && desired_ && !dirty_ && status_.requestId==status_.ackId &&
            (!status_.owned || status_.status!=Ready)) desired_=false;
        if(live_ && !busy_) {
            shared_->clientPid=GetCurrentProcessId();shared_->clientHeartbeat=now;
            if(dirty_) {
                shared_->requestedHide=desired_?1u:0u;shared_->opcode=opcode_;++shared_->requestId;dirty_=false;
            }
            status_=*shared_;
        }
        ReleaseMutex(mutex_);
        if(!live_) {Close();retry_=now+500;}
    }
};
}
