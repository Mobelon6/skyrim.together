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
    // For ordinary paid decoration, Cell is a stable operation ID; Parent is
    // the original house CELL, Variable is the exact native TIF class.
    uint32_t Parent{},Marker{},OldMarker{};
    bool Hide{},Hearthfire{};
    uint32_t Property()const noexcept{return Parent?Parent:Cell;}
};
inline constexpr std::array<House,5> Houses{{
    {91560,604936,1012363,0xA7B1E,"Fragment_0","WhiterunHouseVar"},
    {92678,686903,1012364,0xA851F,"Fragment_3","SolitudeHouseVar"},
    {93149,686902,1012365,0xA7B28,"Fragment_5","RiftenHouseVar"},
    {93690,607121,1012366,0xA7B23,"Fragment_6","MarkarthHouseVar"},
    {92024,136825,1012362,0xA7B2A,"Fragment_7","WindhelmHouseVar"}
}};
// Exact, non-replaceable decoration effects of the winning Skyrim/USSEP
// INFO VMAD bindings and corresponding PSC. No inferred enable graph.
// Room replacement and adoption helper operations are deliberately separate.
inline constexpr uint32_t DecorId(uint32_t info,bool hearthfire) noexcept
{return 0x40000000u|(hearthfire?0x01000000u:0u)|info;}
inline constexpr std::array<House,26> Furniture{{
    {DecorId(0xC6E12,false),0,1012343,0xC6E12,"Fragment_1","TIF__000C6E12",91560,814652,937699,false,false},
    {DecorId(0xC6E15,false),0,1012340,0xC6E15,"Fragment_1","TIF__000C6E15",91560,814651,0,false,false},
    {DecorId(0xC6E1F,false),0,1012344,0xC6E1F,"Fragment_1","TIF__000C6E1F",91560,937673,937713,false,false},
    {DecorId(0xC6E23,false),0,1012345,0xC6E23,"Fragment_1","TIF__000C6E23",91560,814649,937723,false,false},
    {DecorId(0x3DA7,true),0,1012341,0x3DA7,"Fragment_1","BYOH_TIF__01003DA7",91560,814650,937675,false,true},
    {DecorId(0xC896A,false),0,1012351,0xC896A,"Fragment_1","TIF__000C896A",93149,818969,0,false,false},
    {DecorId(0xC9D9E,false),0,1012349,0xC9D9E,"Fragment_1","TIF__000C9D9E",93149,818971,0,false,false},
    {DecorId(0xCAD07,false),0,1012350,0xCAD07,"Fragment_1","TIF__000CAD07",93149,818974,0,false,false},
    {DecorId(0xCAD05,false),0,1012347,0xCAD05,"Fragment_1","TIF__000CAD05",93149,818973,0,false,false},
    {DecorId(0x3D8B,true),0,1012348,0x3D8B,"Fragment_1","BYOH_TIF__01003D8B",93149,818968,0,false,true},
    {DecorId(0xE24EF,false),0,1012355,0xE24EF,"Fragment_1","TIF__000E24EF",92678,927185,0,false,false},
    {DecorId(0xE24D7,false),0,1012352,0xE24D7,"Fragment_1","TIF__000E24D7",92678,927183,0,false,false},
    {DecorId(0xE24DB,false),0,1012353,0xE24DB,"Fragment_1","TIF__000E24DB",92678,927186,0,false,false},
    {DecorId(0xE24DF,false),0,1012354,0xE24DF,"Fragment_1","TIF__000E24DF",92678,927187,0,false,false},
    {DecorId(0x3DDE,true),0,1012357,0x3DDE,"Fragment_1","BYOH_TIF__01003DDE",92678,927189,0,false,true},
    {DecorId(0xE4935,false),0,1012361,0xE4935,"Fragment_1","TIF__000E4935",93690,929111,0,false,false},
    {DecorId(0xE4939,false),0,1012358,0xE4939,"Fragment_1","TIF__000E4939",93690,929110,0,false,false},
    {DecorId(0xE4933,false),0,997670,0xE4933,"Fragment_1","TIF__000E4933",93690,929107,0,false,false},
    {DecorId(0xE4937,false),0,1012360,0xE4937,"Fragment_1","TIF__000E4937",93690,929106,0,false,false},
    {DecorId(0xE67CA,false),0,1012337,0xE67CA,"Fragment_1","TIF__000E67CA",92024,914575,0,false,false},
    {DecorId(0xE67BE,false),0,1012335,0xE67BE,"Fragment_1","TIF__000E67BE",92024,914576,0,false,false},
    {DecorId(0xE67CC,false),0,1012334,0xE67CC,"Fragment_1","TIF__000E67CC",92024,914577,0,false,false},
    {DecorId(0xE67C6,false),0,1012338,0xE67C6,"Fragment_1","TIF__000E67C6",92024,914574,0,false,false},
    {DecorId(0xE67C2,false),0,1012339,0xE67C2,"Fragment_1","TIF__000E67C2",92024,914573,0,false,false},
    {DecorId(0xF726C,false),0,1012333,0xF726C,"Fragment_1","TIF__000F726C",92024,914578,0,true,false},
    {DecorId(0x3D97,true),0,1012336,0x3D97,"Fragment_1","BYOH_TIF__01003D97",92024,914579,0,false,true}
}};
inline constexpr auto All=[]
{
    std::array<const House*,Houses.size()+Furniture.size()> all{};
    size_t i=0;
    for(const auto& h:Houses)all[i++]=&h;
    for(const auto& h:Furniture)all[i++]=&h;
    return all;
}();
inline const House* Find(uint32_t id) noexcept
{for(const auto* h:All) if(h->Cell==id)return h;return nullptr;}
inline const House* FromInfo(uint32_t info,bool hearthfire=false) noexcept
{
    if(!hearthfire)
    {
        for(const auto& h:Houses) if(h.Info==info)return &h;
        if(info==0xC6E28)return &Houses[1];
    }
    for(const auto& h:Furniture)if(h.Info==info&&h.Hearthfire==hearthfire)return &h;
    return nullptr;
}
inline std::string Key(uint32_t id)
{
    const auto* h=Find(id);
    if(!h)return {};
    return TradeProperties::FormKey{h->Hearthfire?"hearthfires.esm":"skyrim.esm",h->Parent?h->Info:h->Cell}.Encode();
}
inline const House* FromKey(std::string_view key)
{
    const auto f=TradeProperties::DecodeForm(key);
    if(!f||f->Encode()!=key)return nullptr;
    for(const auto* h:All)
        if(f->Plugin==(h->Hearthfire?"hearthfires.esm":"skyrim.esm")&&
            f->Local==(h->Parent?h->Info:h->Cell))return h;
    return nullptr;
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
    const auto* op=Find(c.Cell);
    if(!op||!s||head.Revision==UINT32_MAX||!Apply(*s,c,head.Revision+1))return false;
    if(c.Op==Action::Reserve)
    {
        if(op->Parent)
        {
            auto property=Read(journal,op->Parent);
            if(!property||property->Status!=Phase::Purchased)return false;
        }
        // Serialize paid operations of one physical house; other houses stay independent.
        for(const auto* other:All)if(other->Cell!=op->Cell&&other->Property()==op->Property())
        {
            auto prior=Read(journal,other->Cell);
            if(!prior||prior->Status==Phase::Reserved)return false;
        }
    }
    return journal.Append(head,{"home-city-"+std::to_string(head.Revision+1),
        "home-city-purchase-v1",c.Encode()})==CampaignJournal::Result::Committed;
}
}
