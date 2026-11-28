#pragma once
#include "HomeCityPurchase.h"
#include "HomeSnapshot.h"

// Reuse the campaign trade receipts; a shared account is keyed by the
// original property and original reference, never by a character/instance.
namespace HomeSharedStorage
{
struct Address
{
    uint32_t Cell{};
    TradeProperties::FormKey Ref;
    std::string Encode()const{return "home/shared/"+std::to_string(Cell)+"/"+Ref.Encode();}
};
inline std::optional<Address> Decode(std::string_view value)
{
    constexpr std::string_view prefix="home/shared/";
    if(!value.starts_with(prefix))return {};
    const auto rest=value.substr(prefix.size());const auto sep=rest.find('/');
    Address a;
    if(sep==std::string_view::npos||!TradeProperties::Number(rest.substr(0,sep),a.Cell))return {};
    const auto* house=HomeCityPurchase::Find(a.Cell);
    auto key=TradeProperties::DecodeForm(rest.substr(sep+1));
    if(!house||house->Parent||!key||!key->Valid())return {};
    a.Ref=*key;
    return a.Encode()==value?std::optional<Address>{a}:std::nullopt;
}
inline std::optional<HomeSnapshot::Ref> Validate(const Address& a,std::string_view bytes)
{
    auto s=HomeSnapshot::Decode(bytes);
    if(!s||s->Omitted||!s->Legacy.empty()||!s->ResidentKeys.empty()||s->Refs.size()!=1)return {};
    const auto& r=s->Refs.front();
    if(r.Key.Encode()!=a.Ref.Encode()||r.Type!=28||r.Inventory!=1||r.Flags||r.LockState||r.LockLevel)return {};
    return r;
}
// An immutable baseline is visible only after its complete marker.
// Keep reading old v1 single-record baselines byte-for-byte.
inline bool MarkerKind(std::string_view kind)
{return kind=="home-shared-stock-v1"||kind=="home-shared-stock-v2";}
inline std::string PartId(uint32_t first,uint32_t index)
{return "home-stock-part-"+std::to_string(first)+"-"+std::to_string(index);}
inline std::string Manifest(uint32_t first,uint64_t digest,uint32_t count,size_t size)
{return std::to_string(first)+" "+std::to_string(digest)+" "+std::to_string(count)+" "+std::to_string(size);}
inline std::optional<HomeSnapshot::Ref> Read(const CampaignJournal::Journal& j,const Address& a,size_t before=SIZE_MAX)
{
    if(!j.Ready())return {};
    std::optional<HomeSnapshot::Ref> result;
    size_t pos=0;
    const auto account=a.Encode();
    const auto& ops=j.Operations();
    for(const auto& op:ops)
    {
        if(++pos>=before)break;
        if(!MarkerKind(op.Kind))continue;
        const auto sep=op.Payload.find('\n');
        if(sep==std::string::npos||op.Payload.substr(0,sep)!=account)continue;
        if(result||op.Id!="home-shared-stock-"+std::to_string(pos))return {};
        const auto payload=std::string_view(op.Payload).substr(sep+1);
        if(op.Kind=="home-shared-stock-v1")result=Validate(a,payload);
        else
        {
            uint32_t first{},count{};uint64_t digest{};size_t size{};
            std::istringstream in{std::string(payload)};std::string extra;
            if(!(in>>first>>digest>>count>>size)||in>>extra||!first||!count||
                count>HomeSnapshot::MaxParts||!size||size>HomeSnapshot::MaxBytes||
                uint64_t(first)+count!=pos||count!=(size+HomeSnapshot::PartBytes-1)/HomeSnapshot::PartBytes||
                Manifest(first,digest,count,size)!=payload)return {};
            HomeSnapshot::Upload upload;
            for(uint32_t n=0;n<count;++n)
            {
                const auto& part=ops[first-1+n];
                if(part.Kind!="home-shared-stock-part-v2"||part.Id!=PartId(first,n)||
                    !upload.Add(digest,count,n,part.Payload))return {};
            }
            if(!upload.Complete()||upload.Bytes.size()!=size)return {};
            result=Validate(a,upload.Bytes);
        }
        if(!result)return {};
    }
    return result;
}
inline bool Seed(CampaignJournal::Journal& j,const Address& a,std::string_view bytes)
{
    if(!Decode(a.Encode())||!HomeCityPurchase::NativeContainerRoute(j,a.Cell))return false;
    if(Read(j,a))return true;
    const auto account=a.Encode();
    // Never repair a bad committed marker by replacing its balance.
    for(const auto& op:j.Operations())
        if(MarkerKind(op.Kind)&&op.Payload.starts_with(account+"\n"))return false;
    if(!Validate(a,bytes))return false;
    const auto head=j.Current();
    if(head.Revision==UINT32_MAX)return false;
    const auto payload=account+"\n"+std::string(bytes);
    if(payload.size()<=1800)
        return j.Append(head,{"home-shared-stock-"+std::to_string(head.Revision+1),
            "home-shared-stock-v1",payload})==CampaignJournal::Result::Committed;
    const auto count=static_cast<uint32_t>((bytes.size()+HomeSnapshot::PartBytes-1)/HomeSnapshot::PartBytes);
    if(!count||count>HomeSnapshot::MaxParts||uint64_t(head.Revision)+count+1>=UINT32_MAX)return false;
    const auto first=head.Revision+1;
    const auto digest=HomeSnapshot::Digest(bytes);
    for(uint32_t n=0;n<count;++n)
        if(j.Append(j.Current(),{PartId(first,n),"home-shared-stock-part-v2",
            std::string(bytes.substr(size_t(n)*HomeSnapshot::PartBytes,HomeSnapshot::PartBytes))})!=
            CampaignJournal::Result::Committed)return false;
    // Incomplete records remain invisible. A retry uses a new first revision.
    const auto end=j.Current();
    return j.Append(end,{"home-shared-stock-"+std::to_string(end.Revision+1),
        "home-shared-stock-v2",account+"\n"+Manifest(first,digest,count,bytes.size())})==
        CampaignJournal::Result::Committed;
}
}
