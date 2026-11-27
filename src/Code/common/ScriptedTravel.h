#pragma once
#include <CampaignJournal.h>
#include <TradeProperties.h>
#include <bit>
#include <cmath>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace ScriptedTravel
{
// One observed one-shot native sleep-triggered transfer, not a generic RPC.
// The server stores only stable forms and the original two event arguments.
struct Move
{
    std::string Quest,Target,Script;
    uint16_t Stage{};
    uint32_t StartBits{},EndBits{};
    bool Valid() const
    {
        const auto q=TradeProperties::DecodeForm(Quest),t=TradeProperties::DecodeForm(Target);
        if (!q||!t||!q->Valid()||!t->Valid()||q->Encode()!=Quest||t->Encode()!=Target||
            !Stage||Script.empty()||Script.size()>128) return false;
        for (char c:Script)
            if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_')) return false;
        const auto start=std::bit_cast<float>(StartBits),end=std::bit_cast<float>(EndBits);
        return std::isfinite(start)&&std::isfinite(end)&&start>=0&&end>start&&end-start<=24.5f;
    }
    std::string Key() const {return Quest+" "+std::to_string(Stage);}
    std::string Encode() const
    {return Key()+" "+Target+" "+Script+" "+std::to_string(StartBits)+" "+std::to_string(EndBits);}
};
inline std::optional<Move> ReadMove(std::istream& in)
{
    Move m;uint32_t stage{};
    if (!(in>>m.Quest>>stage>>m.Target>>m.Script>>m.StartBits>>m.EndBits)||
        !stage||stage>UINT16_MAX) return {};
    m.Stage=static_cast<uint16_t>(stage);
    return m.Valid()?std::optional<Move>{m}:std::nullopt;
}
inline std::optional<Move> Decode(std::string_view text)
{
    if (text.size()>720) return {};
    std::istringstream in{std::string(text)};
    auto m=ReadMove(in);std::string extra;
    if (!m||in>>extra||m->Encode()!=text) return {};
    return m;
}
// Only the current, already shared, unscoped first-run stage is eligible.
// Later stage/stop or a repeatable target-specific run invalidates replay.
inline bool CurrentStage(const std::vector<CampaignJournal::Operation>& ops,const Move& m)
{
    bool known=false;uint32_t last{};
    for (const auto& op:ops)
    {
        if (op.Kind!="quest-progress-v1"&&op.Kind!="quest-completed-v1"&&op.Kind!="quest-binding-v1") continue;
        std::istringstream row(op.Payload);
        std::string quest,extra;uint32_t run{},status{},stage{},type{};uint64_t who{};
        if (!(row>>quest)) continue;
        if (quest==m.Quest+"~") return false;
        if (quest!=m.Quest) continue;
        if (!(row>>run>>status>>stage>>type>>who)||!who||run!=1||status>2||
            stage>UINT16_MAX||row>>extra||op.Kind!="quest-progress-v1"||status==2) return false;
        if (status==0||status==1) {known=true;last=stage;}
    }
    return known&&last==m.Stage;
}
struct Record
{
    Move Event;
    uint64_t Source{},Recipient{};
    uint32_t Revision{};
    bool Arrived{};
};
inline std::optional<std::vector<Record>> Read(const std::vector<CampaignJournal::Operation>& ops)
{
    std::vector<Record> result;uint32_t revision{};
    for (const auto& op:ops)
    {
        ++revision;
        if (op.Kind!="quest-sleep-move-v1"&&op.Kind!="quest-sleep-arrived-v1") continue;
        std::istringstream in(op.Payload);auto move=ReadMove(in);
        uint64_t source{},recipient{};std::string extra;
        if (!move||!(in>>source>>recipient)||!source||!recipient||source==recipient||in>>extra) return {};
        auto it=std::find_if(result.begin(),result.end(),[&](const Record& r){return r.Event.Key()==move->Key();});
        if (op.Kind=="quest-sleep-move-v1")
        {
            if (it!=result.end()) return {};
            result.push_back({*move,source,recipient,revision,false});
        }
        else
        {
            if (it==result.end()||it->Arrived||it->Source!=source||it->Recipient!=recipient||
                it->Event.Encode()!=move->Encode()) return {};
            it->Arrived=true;
        }
    }
    return result;
}
inline const Record* Find(const std::vector<Record>& records,const Move& m)
{
    for (const auto& r:records) if (r.Event.Key()==m.Key()) return &r;
    return nullptr;
}
inline std::string Payload(const Move& m,uint64_t source,uint64_t recipient)
{return m.Encode()+" "+std::to_string(source)+" "+std::to_string(recipient);}
inline bool MayPublish(bool leader,uint64_t who,uint64_t other,const Record* existing,bool current)
{return leader&&who&&other&&who!=other&&!existing&&current;}
inline bool MayAck(const Record* record,uint64_t who,const Move& m)
{return record&&!record->Arrived&&record->Recipient==who&&record->Event.Encode()==m.Encode();}
inline bool MayDispatch(const Record& r,uint64_t who,bool current,bool nativeSeen,bool attempted,bool atTarget)
{return r.Recipient==who&&!r.Arrived&&current&&!nativeSeen&&!attempted&&!atTarget;}
}
