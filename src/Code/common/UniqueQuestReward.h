#pragma once
#include <CampaignJournal.h>
#include <TradeProperties.h>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace UniqueQuestReward
{
struct Entry
{
    uint32_t Key,Quest,Item;
    uint16_t Stage;
    uint32_t Receipt,Group;
    const char* QuestPlugin;
    const char* ItemPlugin;
    const char* Script;
    const char* Fragment;
    const char* ReceiptName;
};
#define STM_UNIQUE_REWARD(...) {__VA_ARGS__},
inline constexpr Entry Catalog[]={
#include "UniqueQuestRewardCatalog.inc"
};
#undef STM_UNIQUE_REWARD
inline constexpr size_t CatalogSize=sizeof(Catalog)/sizeof(Catalog[0]);
inline constexpr bool ValidCatalog()
{
    for (size_t i=0;i<CatalogSize;++i)
    {
        const auto& a=Catalog[i];
        if (!a.Key||!a.Quest||!a.Item||!a.Stage||a.Receipt!=0x833+i) return false;
        for (size_t j=0;j<i;++j)
            if (a.Key==Catalog[j].Key) return false;
    }
    return true;
}
static_assert(ValidCatalog());
inline bool SameName(std::string_view a,std::string_view b)
{
    const auto fold=[](char c){return c>='A'&&c<='Z'?static_cast<char>(c-'A'+'a'):c;};
    if (a.size()!=b.size()) return false;
    for (size_t i=0;i<a.size();++i) if (fold(a[i])!=fold(b[i])) return false;
    return true;
}
inline bool SameSource(const Entry& a,const Entry& b)
{
    return a.Quest==b.Quest&&a.Stage==b.Stage&&SameName(a.QuestPlugin,b.QuestPlugin)&&
        SameName(a.Script,b.Script)&&SameName(a.Fragment,b.Fragment);
}
inline bool SameGroup(const Entry& a,const Entry& b)
{
    return a.Key==b.Key||(a.Group&&a.Group==b.Group&&a.Quest==b.Quest&&SameName(a.QuestPlugin,b.QuestPlugin));
}
inline const Entry* FindSource(uint32_t quest,std::string_view script,std::string_view fragment)
{
    for (const auto& entry:Catalog)
        if (entry.Quest==quest&&SameName(entry.QuestPlugin,"skyrim.esm")&&SameName(entry.Script,script)&&SameName(entry.Fragment,fragment)) return &entry;
    return nullptr;
}
inline const Entry* Find(uint32_t key)
{
    for (const auto& entry:Catalog) if (entry.Key==key) return &entry;
    return nullptr;
}
inline uint64_t StageSource(const std::vector<CampaignJournal::Operation>& ops,const Entry& entry)
{
    const auto key=TradeProperties::FormKey{entry.QuestPlugin,entry.Quest}.Encode();
    for (const auto& op:ops)
    {
        if (op.Kind!="quest-progress-v1") continue;
        std::istringstream row(op.Payload);
        std::string form;uint32_t run{},status{},stage{},type{};uint64_t source{};
        if (row>>form>>run>>status>>stage>>type>>source)
            if (form==key&&run==1&&(status==0||status==1)&&stage==entry.Stage&&source) return source;
    }
    return 0;
}
inline bool KnownQuest(const std::vector<CampaignJournal::Operation>& ops,const Entry& entry)
{
    const auto key=TradeProperties::FormKey{entry.QuestPlugin,entry.Quest}.Encode();
    bool known=false;
    for (const auto& op:ops)
    {
        if (op.Kind!="quest-progress-v1") continue;
        std::istringstream row(op.Payload);
        std::string form;uint32_t run{};
        if (row>>form>>run&&form==key)
        {
            if (run!=1) return false; // no suppression in a restarted quest
            known=true;
        }
    }
    return known;
}
struct State {uint32_t Item{};uint64_t Source{},Recipient{};bool Delivered{},Legacy{};};
inline std::optional<State> Read(const std::vector<CampaignJournal::Operation>& ops,const Entry& target)
{
    State state;
    for (const auto& op:ops)
    {
        const bool choice=op.Kind=="quest-unique-choice-v1";
        const bool delivered=op.Kind=="quest-unique-delivered-v1";
        const bool legacy=op.Kind=="quest-unique-offer-v1";
        if (!choice&&!delivered&&!legacy&&op.Kind!="quest-unique-offer-v2") continue;
        std::istringstream row(op.Payload);
        std::string extra;uint32_t item{};uint64_t source{},recipient{};
        if (!(row>>item>>source)||!source||((choice||delivered)&&(!(row>>recipient)||!recipient))||row>>extra) return {};
        const auto* entry=Find(item);
        if (!entry) return {};
        if (!SameGroup(*entry,target)) continue;
        if (delivered)
        {
            if (state.Item!=item||state.Source!=source||state.Recipient!=recipient||state.Delivered) return {};
            state.Delivered=true;
        }
        else if (!choice)
        {
            if (state.Item||StageSource(ops,*entry)!=source) return {};
            state={item,source,0,false,legacy};
        }
        else
        {
            if (state.Item!=item||state.Source!=source||state.Recipient) return {};
            state.Recipient=recipient;
        }
    }
    return state;
}
struct Command {uint32_t Item{},Peer{};bool Choice{},Delivered{};};
inline std::optional<Command> Decode(std::string_view value)
{
    const bool choice=value.starts_with("reward-choice ");
    const bool delivered=value.starts_with("reward-delivered ");
    if (!choice&&!delivered&&!value.starts_with("reward-offer ")) return {};
    std::istringstream row{std::string(value.substr(choice?14:delivered?17:13))};
    Command command;command.Choice=choice;command.Delivered=delivered;std::string extra;
    if (!(row>>command.Item)||!Find(command.Item)||(choice&&(!(row>>command.Peer)||!command.Peer))||row>>extra) return {};
    return command;
}
// Values 1 and 2 are the existing ESS receipts. 3 records a native-first
// fallback: never take away that item or issue it again because it was spent.
inline bool Pending(float receipt){return receipt==1.f;}
inline bool DeliveredLocally(float receipt){return receipt==2.f||receipt==3.f;}
inline bool CanSuppress(float receipt){return receipt==1.f||receipt==2.f;}
}
