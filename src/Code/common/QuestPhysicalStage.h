#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <optional>

namespace QuestPhysicalStage
{
// Observe calls from a real quest fragment; no form/quest catalogue.
// Player movement takes precedence over geometry in the same stage so a
// mandatory shared travel scene is still executed natively by both.
enum class Effect : uint8_t { None, Geometry, PlayerTravel };
struct Observation
{
    uint32_t Remaining{};
    bool Geometry{},Travel{};
    void Seen(Effect effect)
    {
        Geometry|=effect==Effect::Geometry;
        Travel|=effect==Effect::PlayerTravel;
    }
    void Done(){if(Remaining) --Remaining;}
    bool Individual() const {return Geometry&&!Travel;}
};
// Stable generation/stage and authenticated character. Separate marker rows
// leave old quest-progress rows and the existing reward/readers unchanged.
using Members=std::set<uint64_t>;
using Stages=std::map<uint16_t,Members>;
using Records=std::map<std::pair<std::string,uint32_t>,Stages>;
template<class Operations>
std::optional<Records> Read(const Operations& ops)
{
    Records records;
    for(const auto& op:ops)
    {
        if(op.Kind!="quest-physical-stage-v1") continue;
        std::istringstream row(op.Payload);
        std::string key,extra;uint32_t run{},stage{};uint64_t who{};
        if(!(row>>key>>run>>stage>>who)||key.empty()||key.size()>256||
            !run||stage>UINT16_MAX||!who||row>>extra) return {};
        records[{key,run}][static_cast<uint16_t>(stage)].insert(who);
    }
    return records;
}
inline bool Any(const Records& r,const std::string& key,uint32_t run)
{return r.contains({key,run});}
inline bool Has(const Records& r,const std::string& key,uint32_t run,uint16_t stage)
{
    const auto it=r.find({key,run});
    return it!=r.end()&&it->second.contains(stage);
}
inline bool Reached(const Records& r,const std::string& key,uint32_t run,uint16_t stage,uint64_t who)
{
    const auto it=r.find({key,run});
    if(it==r.end()) return false;
    const auto s=it->second.find(stage);
    return s!=it->second.end()&&s->second.contains(who);
}
inline std::string Payload(const std::string& key,uint32_t run,uint16_t stage,uint64_t who)
{return key+" "+std::to_string(run)+" "+std::to_string(stage)+" "+std::to_string(who);}
inline bool Blocks(const Records& r,const std::string& key,uint32_t run,uint16_t stage,
    bool reached,bool pending)
{return Has(r,key,run,stage)&&(!reached||pending);}
}
