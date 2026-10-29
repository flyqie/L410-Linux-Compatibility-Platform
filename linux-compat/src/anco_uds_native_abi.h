// Current original 6.0 native UdsIpc ABI, verified against retained/live ELF.
// Not the OHOS IPC types, Android std::__1 types or a copied vendor vtable.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace UdsIpc {
class Parcel;
class MessageParcel;
class MessageOption;
class IRemoteInvoker;
class IRemoteBroker;
template<class T> struct sptr { T *pointer; };
class RefBase {
public:
    RefBase();
    virtual ~RefBase();
    virtual void RefPtrCallback();
    virtual void OnLastWeakRef(const void*) {}
    virtual void OnLastStrongRef(const void*) {}
    virtual void OnFirstStrongRef(const void*) {}
    virtual bool OnAttemptPromoted(const void*);
    void IncStrongRef(const void*);
    void DecStrongRef(const void*);
private:
    void *refs_;
};
// The original UdsIpc Parcelable is nearly empty. It has neither OHOS's
// asRemote/behavior members nor the rest of the OHOS remote-object interface.
class Parcelable : public virtual RefBase {
public:
    virtual ~Parcelable() = default;
    virtual bool Marshalling(Parcel&) const = 0;
};
class IRemoteObject : public virtual Parcelable,public virtual RefBase {
public:
    class DeathRecipient;
    virtual ~IRemoteObject() = default;
    bool Marshalling(Parcel&) const override;
    virtual bool AddDeathRecipient(const sptr<DeathRecipient>&) = 0;
    virtual bool RemoveDeathRecipient(const sptr<DeathRecipient>&) = 0;
    // Original primary slot 11: IPCObjectProxy overrides SendObituary(),
    // and IpcSkeleton::SendObituary calls this slot (+0x28). The stub's
    // original implementation is RET. RefBase::OnLastWeakRef stays inherited.
    virtual void SendObituary() {}
    void OnFirstStrongRef(const void*) override;
    void OnLastStrongRef(const void*) override;
    virtual int SendRequest(uint32_t,MessageParcel&,MessageParcel&,MessageOption&) = 0;
    virtual bool IsProxyObject() const;
    virtual sptr<IRemoteBroker> AsInterface();
    int SetInvoker(std::shared_ptr<IRemoteInvoker>);
protected:
    explicit IRemoteObject(std::u16string);
private:
    // Exact nonvirtual region [8,d8) owned solely by the original C2/D2:
    // cookie/invoker state [8,30), original shared_mutex [30,c0), descriptor
    // [c0,d8). No local construction/destruction or access to those fields.
    alignas(8) unsigned char original_members_[0xd0];
};
class IPCObjectStub : public IRemoteObject {
public:
    explicit IPCObjectStub(std::u16string);
    ~IPCObjectStub() override;
    bool AddDeathRecipient(const sptr<DeathRecipient>&) override;
    bool RemoveDeathRecipient(const sptr<DeathRecipient>&) override;
    int SendRequest(uint32_t,MessageParcel&,MessageParcel&,MessageOption&) override;
    bool IsProxyObject() const override {return false;}
    virtual int OnRemoteRequest(uint32_t,MessageParcel&,MessageParcel&,MessageOption&);
};
class Parcel {
public:
    int ReadInt32();
    bool ReadBool(bool&);
    bool WriteInt32(int);
    bool ReadInt64(long&);
    bool ReadInt32(int&);
    bool ReadUint32(unsigned int&);
    bool ReadUint64(unsigned long&);
    bool ReadFloat(float&);
    bool ReadFloatVector(std::vector<float>*);
    bool ReadInt32Vector(std::vector<int>*);
    bool ReadString(std::string&);
    bool ReadUint16(unsigned short&);
    std::size_t GetReadableBytes() const;
};
class MessageParcel {
public:
    MessageParcel();
    ~MessageParcel();
    bool WriteInterfaceToken(std::u16string);
    std::u16string ReadInterfaceToken();
    bool WriteRemoteObject(const sptr<IRemoteObject>&);
private:
    alignas(8) unsigned char original_members_[72];
};
class MessageOption {
public:
    explicit MessageOption(int);
private:
    int original_flags_;
};
class IUdsBus {
public:
    virtual ~IUdsBus() = default;
    virtual bool Open() = 0;
    virtual int Recv(void*,std::size_t,int) = 0;
    virtual int Send(const void*,int) = 0;
    virtual void Close() = 0;
    virtual int GetFd() = 0;
};
class UdsClientInvoker {
public:
    explicit UdsClientInvoker(std::shared_ptr<IUdsBus>);
    ~UdsClientInvoker();
    bool Open();
    void Close();
    unsigned long GetHandle();
    int SendRequest(unsigned long,unsigned int,MessageParcel&,MessageParcel&,MessageOption&);
private:
    alignas(8) unsigned char original_members_[456];
};
static_assert(sizeof(RefBase)==16);
static_assert(sizeof(IPCObjectStub)==232);
static_assert(sizeof(MessageParcel)==72);
static_assert(sizeof(MessageOption)==4);
static_assert(sizeof(UdsClientInvoker)==456);
}
