#pragma once
#include <CampaignJournal.h>
#include <TradeProperties.h>
#include <set>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace KeywordQuestReward
{
// Keyword record IDs, never item or quest IDs. Resolve against current
// loaded forms so overrides, DLC and mod items use the same classification.
inline constexpr uint32_t Artifact=0xA8668, VendorArtifact=0x917E8, DisallowEnchanting=0xC27BD;
enum class Policy { Native, Protected, Choice };
inline Policy Classify(bool shared,bool protectedObject,bool artifact,bool vendor,bool noEnchant)
{
    if (!shared) return Policy::Native;
    if (protectedObject) return Policy::Protected;
    return artifact||vendor||noEnchant ? Policy::Choice : Policy::Native;
}
struct Key
{
    std::string Quest,Item;
    uint16_t Stage{};
    std::string Encode() const {return Quest+" "+std::to_string(Stage)+" "+Item;}
};
inline std::optional<Key> ReadKey(std::istream& in)
{
    Key k;std::string stage;
    uint32_t n{};
    if (!(in>>k.Quest>>stage>>k.Item)||!TradeProperties::Number(stage,n)||!n||n>UINT16_MAX) return {};
    auto q=TradeProperties::DecodeForm(k.Quest),i=TradeProperties::DecodeForm(k.Item);
    if (!q||!i||!q->Valid()||!i->Valid()||q->Encode()!=k.Quest||i->Encode()!=k.Item) return {};
    k.Stage=static_cast<uint16_t>(n);
    return k;
}
// The existing journal establishes who initiated this exact common stage.
// Restrict to a first unscoped run; don't conflate different radiant targets.
inline uint64_t StageSource(const std::vector<CampaignJournal::Operation>& ops,const Key& k)
{
    uint64_t source{};
    for (const auto& op:ops)
    {
        if (op.Kind!="quest-progress-v1"&&op.Kind!="quest-completed-v1"&&op.Kind!="quest-binding-v1") continue;
        std::istringstream in(op.Payload);std::string form,extra;
        uint32_t run{},status{},stage{},type{};uint64_t who{};
        if (!(in>>form)) continue;
        if (form==k.Quest+"~") return 0;
        if (form!=k.Quest) continue;
        if (!(in>>run>>status>>stage>>type>>who)||run!=1||!who||in>>extra) return 0;
        if (op.Kind=="quest-progress-v1"&&status==0&&stage==k.Stage)
        {
            if (source&&source!=who) return 0;
            source=who;
        }
    }
    return source;
}
enum class Action { Ready, Choice, Keep, Removed };
inline const char* Kind(Action a)
{
    switch(a)
    {
        case Action::Ready:return "quest-keyword-ready-v1";
        case Action::Choice:return "quest-keyword-choice-v1";
        case Action::Keep:return "quest-keyword-keep-v1";
        case Action::Removed:return "quest-keyword-removed-v1";
    }
    return "";
}
inline std::optional<Action> RecordAction(std::string_view kind)
{
    for (auto a:{Action::Ready,Action::Choice,Action::Keep,Action::Removed})
        if (kind==Kind(a)) return a;
    return {};
}
struct State
{
    uint64_t Source{},Recipient{};
    std::set<uint64_t> Ready;
    uint32_t ChoiceRevision{};
    bool Kept{},Removed{};
    bool BothReady() const {return Source&&Ready.size()==2&&Ready.contains(Source);}
};
inline bool Advance(State& s,Action a,uint64_t who,uint64_t recipient,uint32_t revision)
{
    if (!who||!s.Source) return false;
    switch(a)
    {
        case Action::Ready:
            return !recipient&&!s.Recipient&&s.Ready.size()<2&&s.Ready.insert(who).second;
        case Action::Choice:
            if (who!=s.Source||!s.BothReady()||s.Recipient||!s.Ready.contains(recipient)||!revision) return false;
            s.Recipient=recipient;s.ChoiceRevision=revision;return true;
        case Action::Keep:
            if (recipient||who!=s.Recipient||s.Kept) return false;
            s.Kept=true;return true;
        case Action::Removed:
            if (recipient||!s.Kept||who==s.Recipient||!s.Ready.contains(who)||s.Removed) return false;
            s.Removed=true;return true;
    }
    return false;
}
inline std::optional<State> Read(const std::vector<CampaignJournal::Operation>& ops,const Key& k)
{
    State s;s.Source=StageSource(ops,k);
    if (!s.Source) return {};
    uint32_t revision{};
    for (const auto& op:ops)
    {
        ++revision;
        auto a=RecordAction(op.Kind);if (!a) continue;
        std::istringstream in(op.Payload);auto key=ReadKey(in);
        if (!key) return {};
        if (key->Encode()!=k.Encode()) continue;
        uint64_t who{},recipient{};std::string extra;
        if (!(in>>who)||(*a==Action::Choice&&!(in>>recipient))||in>>extra||
            !Advance(s,*a,who,recipient,revision)) return {};
    }
    return s;
}
struct Command {Action Type;Key Reward;uint32_t Peer{};};
inline std::optional<Command> Decode(std::string_view text)
{
    if (text.size()>600) return {};
    std::istringstream in{std::string(text)};std::string verb,extra;
    if (!(in>>verb)) return {};
    Action a;
    if (verb=="reward-keyword-ready") a=Action::Ready;
    else if (verb=="reward-keyword-choice") a=Action::Choice;
    else if (verb=="reward-keyword-keep") a=Action::Keep;
    else if (verb=="reward-keyword-removed") a=Action::Removed;
    else return {};
    auto key=ReadKey(in);uint32_t peer{};
    if (!key||(a==Action::Choice&&(!(in>>peer)||!peer))||in>>extra) return {};
    return Command{a,std::move(*key),peer};
}
// A prior save's choice is not proof that this newly observed native grant
// should be removed. Never replay a destructive debit from old history.
inline bool LiveChoice(const State& s,uint32_t observedRevision)
{return s.ChoiceRevision>observedRevision;}
inline bool CanRemove(const State& s,uint64_t who,uint32_t observedRevision,bool exactLiveCopy)
{return exactLiveCopy&&LiveChoice(s,observedRevision)&&s.Kept&&!s.Removed&&who!=s.Recipient&&s.Ready.contains(who);}
}
