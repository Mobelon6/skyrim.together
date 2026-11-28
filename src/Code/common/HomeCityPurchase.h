#pragma once
#include "CampaignJournal.h"
#include "TradeProperties.h"
#include <array>
#include <sstream>

// Five native HousePurchase QF branches, identified by original CELL.
// These are the Skyrim.esm/USSEP VMAD and INFO bindings, not runtime slots.
// Never forward HousePurchase:10 or infer the house from peer location.
namespace HomeCityPurchase
{
inline constexpr uint32_t Quest=0xA7B33, Gold=15, PlayerFaction=3505, Operation=12;
struct House
{
    uint32_t Cell,Key,Price,Info;
    const char* Fragment;
    const char* Variable;
};
inline constexpr std::array<House,5> Houses{{
    {91560,604936,1012363,0xA7B1E,"Fragment_0","WhiterunHouseVar"},
    {92678,686903,1012364,0xA851F,"Fragment_3","SolitudeHouseVar"},
    {93149,686902,1012365,0xA7B28,"Fragment_5","RiftenHouseVar"},
    {93690,607121,1012366,0xA7B23,"Fragment_6","MarkarthHouseVar"},
    {92024,136825,1012362,0xA7B2A,"Fragment_7","WindhelmHouseVar"}
}};
inline const House* Find(uint32_t cell) noexcept
{for(const auto& h:Houses) if(h.Cell==cell)return &h;return nullptr;}
inline const House* FromInfo(uint32_t info) noexcept
{
    for(const auto& h:Houses) if(h.Info==info)return &h;
    // The verified shared-response INFO has the Solitude location condition.
    return info==0xC6E28?&Houses[1]:nullptr;
}
inline std::string Key(uint32_t cell)
{return TradeProperties::FormKey{"skyrim.esm",cell}.Encode();}
inline const House* FromKey(std::string_view key)
{
    const auto f=TradeProperties::DecodeForm(key);
    return f&&f->Plugin=="skyrim.esm"&&f->Encode()==key?Find(f->Local):nullptr;
}
enum class Phase:uint8_t { Free, Reserved, Purchased };
struct State
{
    uint32_t Revision{},Cost{};
    uint64_t Buyer{};
    Phase Status{Phase::Free};
};
enum class Action:uint32_t { Cancel, Reserve, Confirm };
struct Change
{
    uint32_t Cell{},Expected{},Cost{};
    uint64_t Buyer{};
    Action Op{};
    std::string Encode()const
    {
        return Key(Cell)+" "+std::to_string(Expected)+" "+std::to_string(Cost)+" "+
            std::to_string(Buyer)+" "+std::to_string(static_cast<uint32_t>(Op));
    }
};
inline std::optional<Change> Decode(std::string_view value)
{
    Change c;std::string key,extra;uint32_t action{};
    std::istringstream in{std::string(value)};
    if(!(in>>key>>c.Expected>>c.Cost>>c.Buyer>>action)||in>>extra||
        !c.Buyer||!c.Cost||c.Cost>INT32_MAX||action>2)return {};
    const auto* h=FromKey(key);if(!h)return {};
    c.Cell=h->Cell;c.Op=static_cast<Action>(action);
    return c.Encode()==value?std::optional<Change>{c}:std::nullopt;
}
inline bool Apply(State& s,const Change& c,uint32_t revision)
{
    if(!Find(c.Cell)||!c.Buyer||!c.Cost||c.Cost>INT32_MAX||
        c.Expected!=s.Revision||revision<=s.Revision||s.Status==Phase::Purchased)return false;
    if(c.Op==Action::Reserve)
    {
        if(s.Status!=Phase::Free)return false;
        s={revision,c.Cost,c.Buyer,Phase::Reserved};return true;
    }
    if(s.Status!=Phase::Reserved||c.Buyer!=s.Buyer||c.Cost!=s.Cost)return false;
    if(c.Op==Action::Confirm){s.Revision=revision;s.Status=Phase::Purchased;return true;}
    if(c.Op==Action::Cancel){s={revision,0,0,Phase::Free};return true;}
    return false;
}
inline std::optional<State> Read(const CampaignJournal::Journal& journal,uint32_t cell)
{
    if(!journal.Ready()||!Find(cell))return {};
    State result;uint32_t revision=0;
    for(const auto& op:journal.Operations())
    {
        ++revision;
        if(op.Kind!="home-city-purchase-v1")continue;
        const auto c=Decode(op.Payload);
        if(!c||op.Id!="home-city-"+std::to_string(revision))return {};
        if(c->Cell==cell&&!Apply(result,*c,revision))return {};
    }
    return result;
}
inline bool Publish(CampaignJournal::Journal& journal,const Change& c)
{
    auto s=Read(journal,c.Cell);const auto head=journal.Current();
    if(!s||head.Revision==UINT32_MAX||!Apply(*s,c,head.Revision+1))return false;
    return journal.Append(head,{"home-city-"+std::to_string(head.Revision+1),
        "home-city-purchase-v1",c.Encode()})==CampaignJournal::Result::Committed;
}
}
