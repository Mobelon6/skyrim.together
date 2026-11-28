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
// A baseline is immutable. Use the existing journal; no save-local ids,
// no replacement by a later entrant's older snapshot.
inline std::optional<HomeSnapshot::Ref> Read(const CampaignJournal::Journal& j,const Address& a,size_t before=SIZE_MAX)
{
    if(!j.Ready())return {};
    std::optional<HomeSnapshot::Ref> result;
    size_t pos=0;
    for(const auto& op:j.Operations())
    {
        if(++pos>=before)break;
        if(op.Kind!="home-shared-stock-v1")continue;
        const auto sep=op.Payload.find('\n');
        if(sep==std::string::npos||op.Payload.substr(0,sep)!=a.Encode())continue;
        if(result||op.Id!="home-shared-stock-"+std::to_string(pos))return {};
        result=Validate(a,std::string_view(op.Payload).substr(sep+1));
        if(!result)return {};
    }
    return result;
}
inline bool Seed(CampaignJournal::Journal& j,const Address& a,std::string_view bytes)
{
    if(!Decode(a.Encode())||!HomeCityPurchase::NativeContainerRoute(j,a.Cell))return false;
    if(Read(j,a))return true;
    const auto account=a.Encode();
    // Do not repair a corrupt/duplicate baseline by publishing another.
    for(const auto& op:j.Operations())
        if(op.Kind=="home-shared-stock-v1"&&op.Payload.starts_with(account+"\n"))return false;
    if(!Validate(a,bytes))return false;
    const auto payload=account+"\n"+std::string(bytes);
    const auto head=j.Current();
    if(payload.size()>1800||head.Revision==UINT32_MAX)return false;
    return j.Append(head,{"home-shared-stock-"+std::to_string(head.Revision+1),
        "home-shared-stock-v1",payload})==CampaignJournal::Result::Committed;
}
}
