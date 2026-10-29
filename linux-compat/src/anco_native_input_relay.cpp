#include "anco_native_input_session.hpp"
#include "anco_native_input_windows.hpp"
#include "anco_native_input_relay.h"
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
namespace {
volatile std::sig_atomic_t stopping=0;
void stop(int){stopping=1;}
class Relay final : public AncoNativeInput::Consumer {
    int fd_;
    std::mutex order_;
    AncoNativeInput::HostWindows windows_;
    bool knuckle_=false;
    AncoNativeInputRelay SnapshotLocked(unsigned code,unsigned bytes,int status=0){
        const auto state=windows_.Read();
        return AncoNativeInputRelay{code,bytes,status,
            state.focus,state.container,state.key_excluded,
            static_cast<uint32_t>(state.records.size()),knuckle_};
    }
    int SendPacketLocked(const void* packet,unsigned bytes){
        ssize_t rc;
        do{rc=send(fd_,packet,bytes,MSG_NOSIGNAL|MSG_DONTWAIT);}while(rc<0&&errno==EINTR);
        if(rc!=static_cast<ssize_t>(bytes)){
            std::printf("NATIVE_INPUT_RELAY_FAILED errno=%d bytes=%zd\n",errno,rc);
            return 203;
        }
        return 0; // Actual atomic enqueue, not proof of client/UI consumption.
    }
    int SendLocked(unsigned code,int status=0){
        const auto packet=SnapshotLocked(code,sizeof(AncoNativeInputRelay),status);
        const int result=SendPacketLocked(&packet,sizeof(packet));
        if(result)return result;
        std::printf("NATIVE_INPUT_RELAY_QUEUED code=%u focus=%d container=%u excluded=%u windows=%u\n",
            code,packet.host_focus,packet.container,packet.key_excluded,packet.retained_windows);
        return 0; // Actual local state applied and atomic datagram enqueued.
    }
public:
    explicit Relay(int fd):fd_(fd){}
    int SessionResult(int result){std::lock_guard<std::mutex> guard(order_);return SendLocked(ANCO_INPUT_RELAY_SESSION,result);}
    int Windows(const AncoOriginalInput::WindowsFields& fields) override {
        std::lock_guard<std::mutex> guard(order_);
        windows_.Apply(fields);
        const auto actual=windows_.Read();
        for(const auto& window:actual.records){
            std::printf("NATIVE_INPUT_HOST_WINDOW id=%d display=%d flags=%u z=%.9g matrix_count=%zu hot_count=%zu excluded_count=%zu\n",
                window.window_id,window.display_id,window.flags,window.z_order,
                window.transform.size(),window.hot_areas.size(),window.pointer_areas.size());
            for(size_t i=0;i<window.transform.size();++i)
                std::printf("NATIVE_INPUT_HOST_MATRIX id=%d index=%zu value=%.9g\n",
                    window.window_id,i,window.transform[i]);
            for(const auto& area:window.hot_areas)
                std::printf("NATIVE_INPUT_HOST_HOT id=%d rect=%d,%d,%d,%d\n",window.window_id,
                    area.values[0],area.values[1],area.values[2],area.values[3]);
            for(const auto& area:window.pointer_areas)
                std::printf("NATIVE_INPUT_HOST_EXCLUDED id=%d rect=%d,%d,%d,%d\n",window.window_id,
                    area.values[0],area.values[1],area.values[2],area.values[3]);
        }
        std::vector<unsigned char> packet(sizeof(AncoNativeInputWindows));
        const auto append=[&packet](const void* values,size_t bytes){
            const auto* first=static_cast<const unsigned char*>(values);
            if(bytes)packet.insert(packet.end(),first,first+bytes);
        };
        for(const auto& window:actual.records){
            if(window.transform.size()>UINT32_MAX || window.hot_areas.size()>UINT32_MAX ||
               window.pointer_areas.size()>UINT32_MAX)return 203;
            const AncoNativeHostWindowFields row{window.display_id,window.window_id,
                window.action,window.flags,window.z_order,
                static_cast<uint32_t>(window.transform.size()),
                static_cast<uint32_t>(window.hot_areas.size()),
                static_cast<uint32_t>(window.pointer_areas.size())};
            append(&row,sizeof(row));
            append(window.transform.data(),window.transform.size()*sizeof(float));
            for(const auto& area:window.hot_areas)append(area.values,sizeof(area.values));
            for(const auto& area:window.pointer_areas)append(area.values,sizeof(area.values));
        }
        if(packet.size()>UINT32_MAX || actual.records.size()>UINT32_MAX)return 203;
        const AncoNativeInputWindows header{SnapshotLocked(2,static_cast<unsigned>(packet.size())),
            static_cast<uint32_t>(actual.records.size()),0};
        std::memcpy(packet.data(),&header,sizeof(header));
        const int result=SendPacketLocked(packet.data(),static_cast<unsigned>(packet.size()));
        std::printf("NATIVE_INPUT_WINDOWS_SNAPSHOT records=%u bytes=%u result=%d\n",
            header.record_count,header.state.bytes,result);
        return result;
    }
    int ExcludedKeys(const std::vector<int>& ids) override {
        std::lock_guard<std::mutex> guard(order_);
        windows_.SetExcludedKeys(ids);return SendLocked(5);
    }
    int Knuckle(bool value) override {
        std::lock_guard<std::mutex> guard(order_);knuckle_=value;return SendLocked(3);
    }
    int Key(const AncoOriginalInput::KeyFields& fields) override {
        std::lock_guard<std::mutex> guard(order_);
        if(fields.field_1c!=0&&fields.field_1c!=1)return 203;
        const AncoNativeInputKey packet{
            SnapshotLocked(1,sizeof(AncoNativeInputKey)),
            fields.field_00,fields.field_08,fields.field_10,fields.field_14,
            fields.field_18,fields.field_1c,fields.field_20,0};
        const int result=SendPacketLocked(&packet,sizeof(packet));
        std::printf("NATIVE_INPUT_KEY_ENQUEUE event=%d device=%d android=%d action=%d result=%d\n",
            packet.event_id,packet.device_id,packet.android_key,packet.android_action,result);
        return result;
    }
    int Pointer(const AncoOriginalInput::PointerFields& fields) override {
        std::lock_guard<std::mutex> guard(order_);
        if(fields.item_count>32)return 203;
        // SyncPointerEvent is already the output of the original host
        // MouseTargetIsInAnco/SetAncoDeal decision.  Forward that callback
        // unchanged; a second Linux-side host-window hit test compares a
        // different display-id domain and can only discard valid AnCo input.
        AncoNativeInputPointer packet{};
        packet.state=SnapshotLocked(0,sizeof(packet));
        packet.field_00=fields.field_00;packet.field_08=fields.field_08;
        for(unsigned i=0;i<7;++i)packet.fields_10_to_28[i]=fields.fields_10_to_28[i];
        packet.field_2c=fields.field_2c;packet.field_30=fields.field_30;packet.field_34=fields.field_34;
        for(unsigned i=0;i<5;++i)packet.fields_38_to_48[i]=fields.fields_38_to_48[i];
        packet.item_count=static_cast<uint32_t>(fields.item_count);
        for(size_t i=0;i<fields.item_count;++i){
            const auto& item=fields.items[i];
            packet.items[i]={item.field_00,item.field_08,item.field_0c,item.field_10,
                item.field_14,item.field_18,item.field_1c};
        }
        const int result=SendPacketLocked(&packet,sizeof(packet));
        std::printf("NATIVE_INPUT_POINTER_ENQUEUE event=%d raw_action=%d source=%d action=%d items=%u result=%d client_consumption_proven=0\n",
            packet.fields_10_to_28[1],packet.fields_10_to_28[2],packet.fields_38_to_48[1],
            packet.fields_38_to_48[2],packet.item_count,result);
        return result; // Actual local packet enqueue only, not Wayland receipt.
    }
    // OneHand/Devices/TripleFinger retain actual unavailable203.
    // Decoding alone must not acknowledge unfinished event/policy consumers.
};
}
int main(int argc,char** argv){
    static_assert(sizeof(AncoNativeInputRelay)==32);
    static_assert(sizeof(AncoNativeInputKey)==72);
    static_assert(sizeof(AncoNativeInputPointerItem)==32);
    static_assert(sizeof(AncoNativeInputPointer)==1136);
    static_assert(sizeof(AncoNativeInputWindows)==40);
    static_assert(sizeof(AncoNativeHostWindowFields)==32);
    static_assert(sizeof(AncoNativeHostRect)==16);
    std::setbuf(stdout,nullptr);
    if(argc!=2)return 64;
    char* end=nullptr;errno=0;long number=std::strtol(argv[1],&end,10);
    if(errno||!argv[1][0]||*end||number<3||number>2147483647)return 64;
    int fd=static_cast<int>(number),type=0;socklen_t size=sizeof(type);
    if(getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size)||type!=SOCK_SEQPACKET)return 65;
    std::signal(SIGTERM,stop);std::signal(SIGINT,stop);
    auto relay=std::make_shared<Relay>(fd);
    AncoNativeInput::Session session(relay);
    const int opened=session.Open();
    std::printf("NATIVE_INPUT_RELAY_SESSION result=%d\n",opened);
    const int queued=relay->SessionResult(opened);
    bool transport_failed=false;
    if(!opened&&!queued){
        while(!stopping){
            const int bus=session.ConnectionFd();
            if(bus<0){transport_failed=true;break;}
            pollfd peers[2]={{fd,POLLIN|POLLHUP|POLLERR,0},{bus,0,0}};
            const int rc=poll(peers,2,-1);
            if(rc<0&&errno==EINTR)continue;
            if(rc<0||(peers[0].revents&(POLLHUP|POLLERR|POLLNVAL)))break;
            if(peers[0].revents&POLLIN)break; // No inbound messages in this state-only boundary.
            if(peers[1].revents&(POLLHUP|POLLERR|POLLNVAL)){transport_failed=true;break;}
        }
    }
    if(transport_failed){
        puts("NATIVE_INPUT_RELAY_TRANSPORT_CLOSED");
        relay->SessionResult(203); // Actual original transport loss, not live.
    }
    session.Close();close(fd);
    puts("NATIVE_INPUT_RELAY_CLOSED original_callbacks_joined=1");
    return opened||queued?66:transport_failed?67:0;
}
