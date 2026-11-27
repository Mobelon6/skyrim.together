#pragma once
#include <CampaignJournal.h>
#include <TradeProperties.h>
#include <chrono>
#include <cmath>
#include <map>
#include <optional>
#include <sstream>
#include <string>

namespace QuestPickupPermit
{
struct Claim
{
    std::string Quest, Reference, Item;
    uint32_t Alias{};
    bool Valid() const
    {
        for (const auto* value:{&Quest,&Reference,&Item})
        {
            const auto key=TradeProperties::DecodeForm(*value);
            if (!key||!key->Valid()||key->Encode()!=*value) return false;
        }
        return true;
    }
    std::string Encode() const {return Quest+" "+std::to_string(Alias)+" "+Reference+" "+Item;}
};
inline std::optional<Claim> Decode(const std::string& value)
{
    if (value.size()>768) return {};
    Claim claim;
    std::string extra;
    std::istringstream in(value);
    if (!(in>>claim.Quest>>claim.Alias>>claim.Reference>>claim.Item)||in>>extra||
        !claim.Valid()||claim.Encode()!=value) return {};
    return claim;
}
struct Receipt
{
    Claim Object;
    int32_t Count{};
    bool Initial{},Shared{};
    bool Valid() const
    {return Object.Valid()&&(Count==1||Count==-1)&&(!Initial||Count==1)&&(!Shared||(Count==-1&&!Initial));}
    std::string Encode() const
    {return (Shared?"turnin ":"")+Object.Encode()+" "+std::to_string(Count)+" "+std::to_string(Initial?1:0);}
};
inline std::optional<Receipt> DecodeReceipt(const std::string& value)
{
    if (value.size()>800) return {};
    Receipt receipt;
    auto& c=receipt.Object;
    unsigned initial{};
    std::string extra;
    receipt.Shared=value.starts_with("turnin ");
    std::istringstream row(receipt.Shared?value.substr(7):value);
    if (!(row>>c.Quest>>c.Alias>>c.Reference>>c.Item>>receipt.Count>>initial)||row>>extra||initial>1) return {};
    receipt.Initial=initial==1;
    if (!receipt.Valid()||receipt.Encode()!=value) return {};
    return receipt;
}
inline std::string Payload(const Claim& c,uint64_t character)
{
    return c.Quest+" 1 "+std::to_string(c.Alias)+" "+c.Reference+" "+c.Item+" 1 "+std::to_string(character);
}
inline bool Acknowledged(const std::vector<CampaignJournal::Operation>& ops,const Receipt& receipt,uint64_t character)
{
    if (!character||!receipt.Valid()) return false;
    const auto own=Payload(receipt.Object,character);
    const auto prefix=own.substr(0,own.rfind(' ')+1);
    bool holder=false,spent=false;
    for (const auto& op:ops)
    {
        if ((op.Kind=="quest-pickup-v1"||op.Kind=="quest-copy-v1")&&op.Payload==own) holder=true;
        if ((op.Kind=="quest-consumed-v1"||op.Kind=="quest-consumed-v2")&&op.Payload.starts_with(prefix)) spent=true;
    }
    return receipt.Shared?spent:holder&&(receipt.Count==1||spent);
}
// Find the authoritative original, independent of the submitting player.
inline std::optional<std::string> AvailableOriginal(const std::vector<CampaignJournal::Operation>& ops,const Claim& claim)
{
    if (!claim.Valid()) return {};
    const auto key=Payload(claim,0);
    const auto prefix=key.substr(0,key.rfind(' ')+1);
    std::optional<std::string> original;
    for (const auto& op:ops)
    {
        if (!op.Payload.starts_with(prefix)) continue;
        if (op.Kind=="quest-consumed-v1"||op.Kind=="quest-consumed-v2") return {};
        if (op.Kind=="quest-pickup-v1")
        {
            if (original&&*original!=op.Payload) return {};
            original=op.Payload;
        }
    }
    return original;
}
// The journal can attest an acquisition, not the current contents of a
// backpack. The client must additionally check the live exact stack.
inline bool HasAvailableHolder(const std::vector<CampaignJournal::Operation>& ops,const Claim& claim,uint64_t character)
{
    if (!character||!AvailableOriginal(ops,claim)) return false;
    const auto own=Payload(claim,character);
    for (const auto& op:ops)
        if ((op.Kind=="quest-pickup-v1"||op.Kind=="quest-copy-v1")&&op.Payload==own) return true;
    return false;
}
inline bool PhysicalHolder(uint64_t character,uint64_t owner,bool recordedCopy,int64_t count)
{
    return character&&owner&&(character==owner||recordedCopy)&&count==1;
}
// Even if the quest has since finished, an old physical reference cannot
// be acquired again by this shared first-run path.
inline bool AlreadyTaken(const std::vector<CampaignJournal::Operation>& ops,const Claim& claim)
{
    for (const auto& op:ops)
        if (op.Kind=="quest-pickup-v1")
        {
            std::string q,ref,item,extra;uint32_t generation{},alias{};int count{};uint64_t owner{};
            std::istringstream in(op.Payload);
            if (!(in>>q>>generation>>alias>>ref>>item>>count>>owner)||in>>extra||
                !generation||count<=0||!owner) return true;
            if (ref==claim.Reference||(q==claim.Quest&&generation==1&&alias==claim.Alias)) return true;
        }
    return false;
}
struct Leases
{
    using Clock=std::chrono::steady_clock;
    struct Lease {std::string ClaimKey;uint64_t Character{};Clock::time_point Until;};
    std::map<std::string,Lease> Active;
    bool Acquire(const Claim& claim,uint64_t character,Clock::time_point now)
    {
        if (!character||!claim.Valid()) return false;
        for (auto it=Active.begin();it!=Active.end();)
            if (it->second.Until<=now) it=Active.erase(it);else ++it;
        const auto key=claim.Encode();
        const auto alias=claim.Quest+" "+std::to_string(claim.Alias)+" ";
        for (const auto& [ref,lease]:Active)
            if (ref==claim.Reference||lease.ClaimKey.starts_with(alias))
                return lease.Character==character&&lease.ClaimKey==key;
        if (Active.size()>=64) return false;
        Active.emplace(claim.Reference,Lease{key,character,now+std::chrono::seconds(30)});
        return true;
    }
};
struct TurnInLeases
{
    using Clock=Leases::Clock;
    struct Lease {std::string Key;uint64_t Character{},Nonce{};uint32_t Peer{};Clock::time_point Until;};
    std::map<std::string,Lease> Active;
    bool Allows(const Claim& claim,uint64_t character,Clock::time_point now) const
    {
        const auto it=Active.find(claim.Reference);
        return it!=Active.end()&&it->second.Key==claim.Encode()&&it->second.Character==character&&it->second.Until>now;
    }
    bool Acquire(const Claim& claim,uint64_t character,uint32_t peer,uint64_t nonce,Clock::time_point now)
    {
        if (!claim.Valid()||!character||!peer||!nonce) return false;
        for (auto it=Active.begin();it!=Active.end();)
            if (it->second.Until<=now) it=Active.erase(it);else ++it;
        const auto key=claim.Encode();
        const auto alias=claim.Quest+" "+std::to_string(claim.Alias)+" ";
        for (const auto& [ref,lease]:Active)
            if (ref==claim.Reference||lease.Key.starts_with(alias))
                return lease.Key==key&&lease.Character==character&&lease.Peer==peer&&lease.Nonce==nonce;
        if (Active.size()>=64) return false;
        Active.emplace(claim.Reference,Lease{key,character,nonce,peer,now+std::chrono::minutes(2)});
        return true;
    }
};
// CTDA comparison opcodes, restricted to a mandatory requirement satisfied
// by exactly one item. Never change global/alias/package operands or OR terms.
// Only literal, non-OR GetItemCount conditions with the six native
// comparison operators. Do not interpret global/alias/package parameters.
inline std::optional<bool> CompareItemCount(uint8_t flags,float value,int count)
{
    if ((flags&0x1f)||!std::isfinite(value)||count<0||count>9999) return {};
    const auto n=static_cast<float>(count);
    switch (flags>>5)
    {
    case 0:return n==value;
    case 1:return n!=value;
    case 2:return n>value;
    case 3:return n>=value;
    case 4:return n<value;
    case 5:return n<=value;
    default:return {};
    }
}
inline std::optional<bool> CompareSingleItem(uint8_t flags,float value,int count)
{
    if (count!=0&&count!=1) return {};
    return CompareItemCount(flags,value,count);
}
// For a direct literal requirement, determine its least positive satisfying
// quantity. This is an eligibility test, NOT proof that the dialogue consumes
// that quantity. Do not treat price/ownership/access conditions as expenses.
inline std::optional<uint32_t> MinimumRequiredCount(uint8_t flags,float value)
{
    if ((flags&0x1f)||!std::isfinite(value)||CompareItemCount(flags,value,0)!=false) return {};
    const auto op=flags>>5;
    if (op!=0&&op!=2&&op!=3) return {};
    if (value<0.f||value>9999.f) return {};
    const auto candidate=op==2?std::floor(value)+1.f:std::ceil(value);
    if (candidate<1.f||candidate>9999.f) return {};
    const auto count=static_cast<int>(candidate);
    if (CompareItemCount(flags,value,count)!=true) return {};
    return static_cast<uint32_t>(count);
}
inline bool PositiveSingleItem(uint8_t flags,float value)
{
    return CompareSingleItem(flags,value,0)==false&&CompareSingleItem(flags,value,1)==true;
}
// A local ordinary item sharing the base form is not the tracked original.
// Only substitute a binary group count if the native inventory is unambiguous.
inline int SharedSingleItemCount(int64_t local,bool consumed,bool holder,bool exact)
{
    if (local<0||local>1) return -1;
    if (!local) return consumed?0:holder?-1:1;
    if (!holder||!exact) return -1;
    return consumed?0:1;
}
}
