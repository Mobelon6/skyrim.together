#pragma once
#include "CampaignJournal.h"
#include "TradeProperties.h"

namespace CampaignFollower
{
using Ownership=std::map<std::string,uint64_t>;

inline bool Apply(Ownership& owners,const std::string& npc,uint64_t character,bool hire)
{
    if (!character) return false;
    const auto it=owners.find(npc);
    if (!hire)
    {
        if (it==owners.end()||it->second!=character) return false;
        it->second=0;
        return true;
    }
    if (it!=owners.end() && it->second) return it->second==character;
    for (const auto& [key,holder]:owners)
        if (holder==character) return false;
    owners.insert_or_assign(npc,character);
    return true;
}

inline std::optional<Ownership> Read(const CampaignJournal::Journal& journal)
{
    if (!journal.Ready()) return {};
    Ownership result;
    for (const auto& op:journal.Operations())
    {
        if (op.Kind!="follower-hired-v1"&&op.Kind!="follower-dismissed-v1") continue;
        const auto space=op.Payload.find(' ');
        if (space==std::string::npos) return {};
        const auto npc=op.Payload.substr(space+1);
        const auto key=TradeProperties::DecodeForm(npc);
        uint64_t character{};
        const auto begin=op.Payload.data();
        const auto parsed=std::from_chars(begin,begin+space,character);
        if (parsed.ec!=std::errc{}||parsed.ptr!=begin+space||!key||!key->Valid()||
            !Apply(result,npc,character,op.Kind=="follower-hired-v1")) return {};
    }
    return result;
}

inline bool Write(CampaignJournal::Journal& journal,Ownership& owners,
    const TradeProperties::FormKey& npc,uint64_t character,bool hire,bool reserved=false)
{
    if (!npc.Valid()||!character||!journal.Ready()) return false;
    const auto key=npc.Encode();
    const auto prior=owners.find(key);
    if (hire && prior!=owners.end() && !prior->second && !reserved) return false;
    auto next=owners;
    if (!Apply(next,key,character,hire)) return false;
    if (next==owners) return true;
    const auto head=journal.Current();
    if (journal.Append(head,{"follower-"+std::to_string(head.Revision+1),
        hire?"follower-hired-v1":"follower-dismissed-v1",std::to_string(character)+" "+key})
        !=CampaignJournal::Result::Committed) return false;
    owners.swap(next);
    return true;
}
}
