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
struct Entry { uint32_t Quest, Item; uint16_t Stage; uint32_t Receipt; const char* Script; const char* Fragment; };
// Historical DA01 bindings: retained solely to interpret existing receipts and
// journal choices. Do not extend or use these entries to identify new awards.
inline constexpr Entry Catalog[]={
    {0x28ad6,0x63b27,100,0x833,"qf_da01_00028ad6","fragment_15"},
    {0x28ad6,0x63b29,110,0x834,"qf_da01_00028ad6","fragment_17"}};
inline bool SameName(std::string_view a,std::string_view b)
{
    const auto fold=[](char c){return c>='A'&&c<='Z'?static_cast<char>(c-'A'+'a'):c;};
    if (a.size()!=b.size()) return false;
    for (size_t i=0;i<a.size();++i) if (fold(a[i])!=fold(b[i])) return false;
    return true;
}
inline const Entry* FindSource(uint32_t quest,std::string_view script,std::string_view fragment)
{
    for (const auto& entry:Catalog)
        if (entry.Quest==quest&&SameName(entry.Script,script)&&SameName(entry.Fragment,fragment)) return &entry;
    return nullptr;
}
inline const Entry* Find(uint32_t item)
{
    for (const auto& entry:Catalog) if (entry.Item==item) return &entry;
    return nullptr;
}
inline uint64_t StageSource(const std::vector<CampaignJournal::Operation>& ops,const Entry& entry)
{
    const auto key=TradeProperties::FormKey{"skyrim.esm",entry.Quest}.Encode();
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
inline bool KnownQuest(const std::vector<CampaignJournal::Operation>& ops,uint32_t quest)
{
    const auto prefix=TradeProperties::FormKey{"skyrim.esm",quest}.Encode()+" 1 ";
    for (const auto& op:ops)
        if (op.Kind=="quest-progress-v1"&&op.Payload.starts_with(prefix)) return true;
    return false;
}
struct State {uint32_t Item{};uint64_t Source{},Recipient{};};
inline std::optional<State> Read(const std::vector<CampaignJournal::Operation>& ops,uint32_t quest)
{
    State state;
    for (const auto& op:ops)
    {
        const bool choice=op.Kind=="quest-unique-choice-v1";
        if (!choice&&op.Kind!="quest-unique-offer-v1") continue;
        std::istringstream row(op.Payload);
        std::string extra;uint32_t item{};uint64_t source{},recipient{};
        if (!(row>>item>>source)||!source||(choice&&(!(row>>recipient)||!recipient))||row>>extra) return {};
        const auto* entry=Find(item);
        if (!entry) return {};
        if (entry->Quest!=quest) continue;
        if (!choice)
        {
            if (state.Item||StageSource(ops,*entry)!=source) return {};
            state={item,source,0};
        }
        else
        {
            if (state.Item!=item||state.Source!=source||state.Recipient) return {};
            state.Recipient=recipient;
        }
    }
    return state;
}
struct Command {uint32_t Item{},Peer{};bool Choice{};};
inline std::optional<Command> Decode(std::string_view value)
{
    const bool choice=value.starts_with("reward-choice ");
    if (!choice&&!value.starts_with("reward-offer ")) return {};
    std::istringstream row{std::string(value.substr(choice?14:13))};
    Command command;command.Choice=choice;std::string extra;
    if (!(row>>command.Item)||!Find(command.Item)||(choice&&(!(row>>command.Peer)||!command.Peer))||row>>extra) return {};
    return command;
}
}
