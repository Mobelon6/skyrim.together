#pragma once
#include "CampaignIdentity.h"
#include "HomeRegistry.h"
#include "HomePermissions.h"
#include "QuestJournalDisk.h"
#include <array>
#include <bit>
#include <cmath>
#include <locale>
#include <sstream>

// Observed is the downloaded journal prefix, NOT proof of applied game effects.
namespace CampaignCheckpoint
{
inline constexpr std::string_view Kind="save-checkpoint-v1";
inline bool DigestValid(std::string_view s)
{
    if (s.size()!=64) return false;
    for (char c:s) if (!(c>='0'&&c<='9')&&!(c>='a'&&c<='f')) return false;
    return true;
}
struct Record
{
    CampaignIdentity::Stamp Save;
    CampaignJournal::Head Observed;
    std::string Digest;
    TradeProperties::FormKey Cell,WorldSpace;
    uint32_t HomeSlot{};
    std::array<uint32_t,6> Pose{};
    TradeProperties::FormKey Entrance;
    uint64_t Shape{};
    // Optional tail for a secondary original room. Older checkpoint rows stay canonical.
    uint32_t EntrySlot{};
    std::array<uint32_t,6> ArrivalPose{};
    bool Valid() const
    {
        if (!CampaignIdentity::Encode(Save)||!Observed.Tag||Observed.Revision<Save.Applied.Revision||
            Observed.Revision==UINT32_MAX||!DigestValid(Digest)||
            (!Cell.Empty()&&!Cell.Valid())||(!WorldSpace.Empty()&&!WorldSpace.Valid())||
            (Cell.Empty()&&WorldSpace.Empty())||HomeSlot>HomeRegistry::MaxSlot||
            (HomeSlot&&(!Cell.Valid()||!WorldSpace.Empty()))) return false;
        if (Entrance.Empty()?Shape!=0:(!Entrance.Valid()||!HomeSlot||!Shape)) return false;
        if (EntrySlot && (EntrySlot==HomeSlot||EntrySlot>HomeRegistry::MaxSlot||Entrance.Empty())) return false;
        for (auto v:ArrivalPose)
            if (!std::isfinite(std::bit_cast<float>(v))||(!EntrySlot&&v)) return false;
        for (auto v:Pose) if (!std::isfinite(std::bit_cast<float>(v))) return false;
        return true;
    }
    std::string Encode() const
    {
        std::string s=std::to_string(Save.Campaign)+" "+std::to_string(Save.Character)+" "+
            std::to_string(Save.Applied.Revision)+" "+std::to_string(Save.Applied.Tag)+" "+
            std::to_string(Observed.Revision)+" "+std::to_string(Observed.Tag)+" "+Digest+
            " "+Cell.Encode()+" "+WorldSpace.Encode()+" "+std::to_string(HomeSlot);
        for (auto v:Pose) s+=" "+std::to_string(v);
        if (!Entrance.Empty()) s+=" "+Entrance.Encode()+" "+std::to_string(Shape);
        if (EntrySlot)
        {
            s+=" "+std::to_string(EntrySlot);
            for (auto v:ArrivalPose) s+=" "+std::to_string(v);
        }
        return s;
    }
};
inline std::optional<Record> Decode(std::string_view s)
{
    if (s.empty()||s.size()>1024) return {};
    std::istringstream in{std::string(s)};in.imbue(std::locale::classic());
    Record r;std::string cell,world,extra;
    if (!(in>>r.Save.Campaign>>r.Save.Character>>r.Save.Applied.Revision>>r.Save.Applied.Tag
          >>r.Observed.Revision>>r.Observed.Tag>>r.Digest>>cell>>world>>r.HomeSlot)) return {};
    for (auto& v:r.Pose) if (!(in>>v)) return {};
    auto c=TradeProperties::DecodeForm(cell),w=TradeProperties::DecodeForm(world);
    if (!c||!w) return {};
    r.Cell=std::move(*c);r.WorldSpace=std::move(*w);
    if (in>>extra)
    {
        auto entrance=TradeProperties::DecodeForm(extra);
        if (!entrance||!entrance->Valid()||!(in>>r.Shape)) return {};
        r.Entrance=std::move(*entrance);
        if (in>>r.EntrySlot)
        {
            if (!r.EntrySlot) return {};
            for (auto& v:r.ArrivalPose) if (!(in>>v)) return {};
            if (in>>extra) return {};
        }
    }
    if (!r.Valid()||r.Encode()!=s) return {};
    return r;
}
inline std::string Id(uint64_t character,std::string_view digest)
{return "save-"+std::to_string(character)+"-"+std::string(digest);}
// A checkpoint describes a past visit, not a current admission ticket.
// Read the permission at the captured journal prefix, so a lost ACK or
// later revocation cannot make an already confirmed save unrecognizable.
inline bool HadHomeAccess(const CampaignJournal::Journal& journal,const Record& r)
{
    const auto home=HomeRegistry::BySlot(journal,r.HomeSlot);
    if (!home||home->Root.Encode()!=r.Cell.Encode()) return false;
    if (HomePermissions::EffectiveAt(journal,r.HomeSlot,r.Save.Character,r.Observed.Revision)==
        HomePermissions::Right::None) return false;
    if (!r.EntrySlot) return true;
    const auto entry=HomeRegistry::BySlot(journal,r.EntrySlot);
    if (!entry||entry->Owner!=home->Owner||entry->Slot==home->Slot) return false;
    auto prior=r;prior.HomeSlot=entry->Slot;prior.Cell=entry->Root;prior.EntrySlot=0;prior.ArrivalPose={};
    return HadHomeAccess(journal,prior);
}
inline bool Known(const CampaignJournal::Journal& journal,uint64_t campaign,const Record& r)
{
    if (!r.Valid()||!CampaignIdentity::Known(journal,campaign,r.Save)) return false;
    const auto observed=journal.At(r.Observed.Revision);
    if (!observed||*observed!=r.Observed) return false;
    if (r.HomeSlot&&!HadHomeAccess(journal,r)) return false;
    return true;
}
inline std::optional<Record> Find(const CampaignJournal::Journal& journal,uint64_t campaign,
                                  uint64_t character,std::string_view digest)
{
    if (!character||!DigestValid(digest)) return {};
    const auto id=Id(character,digest);
    if (const auto* op=journal.Find(id); op&&op->Kind==Kind)
    {
        auto r=Decode(op->Payload);
        if (r&&r->Save.Character==character&&r->Digest==digest&&Known(journal,campaign,*r)) return r;
    }
    return {};
}
// Call again immediately before requesting the ordinary server visit lease.
// A valid past checkpoint never grants current access by itself.
inline bool CanResume(const CampaignJournal::Journal& journal,uint64_t campaign,const Record& r)
{
    return r.HomeSlot&&Known(journal,campaign,r)&&
        HomePermissions::Effective(journal,r.HomeSlot,r.Save.Character)!=HomePermissions::Right::None&&
        (!r.EntrySlot||HomePermissions::Effective(journal,r.EntrySlot,r.Save.Character)!=HomePermissions::Right::None);
}
inline CampaignJournal::Result Publish(CampaignJournal::Journal& journal,uint64_t campaign,
                                        uint64_t character,const Record& r)
{
    if (!character||r.Save.Character!=character||!Known(journal,campaign,r)) return CampaignJournal::Result::Invalid;
    return journal.Append(journal.Current(),{Id(character,r.Digest),std::string(Kind),r.Encode()});
}
// Retry copy only: the same durable primitive, not a second authority.
inline std::filesystem::path Outbox(const std::filesystem::path& root,uint64_t campaign,std::string_view digest)
{return root/"checkpoint-outbox"/std::to_string(campaign)/std::string(digest);}
inline bool StoreLocal(const std::filesystem::path& root,const Record& r)
{
    if (!r.Valid()) return false;
    try
    {
        auto path=Outbox(root,r.Save.Campaign,r.Digest);
        std::filesystem::create_directories(path/std::to_string(r.Save.Character));
        return QuestJournalDisk::Store(path,r.Save.Character,1,r.Encode());
    }
    catch (...) {return false;}
}
inline std::optional<Record> ReadLocal(const std::filesystem::path& root,uint64_t campaign,
                                      uint64_t character,std::string_view digest)
{
    if (!campaign||!character||!DigestValid(digest)) return {};
    try
    {
        std::string row;
        if (!QuestJournalDisk::Read(Outbox(root,campaign,digest),character,1,row)) return {};
        auto r=Decode(row);
        if (r&&r->Save.Campaign==campaign&&r->Save.Character==character&&r->Digest==digest) return r;
    }
    catch (...) {}
    return {};
}

// After a process restart the loaded ESS may be newer than several unsent
// checkpoints. Recover each immutable retry record belonging to this character.
inline bool RestoreOutbox(const std::filesystem::path& root,const CampaignJournal::Journal& journal,
    uint64_t campaign,uint64_t character,std::map<std::string,Record>& pending)
{
    if (!journal.Ready()||!campaign||!character) return false;
    const auto directory=Outbox(root,campaign,"");
    std::error_code ec;
    if (!std::filesystem::exists(directory,ec)) return !ec;
    std::filesystem::directory_iterator it(directory,ec), end;
    for (; !ec&&it!=end; it.increment(ec))
    {
        const auto digest=it->path().filename().string();
        if (!DigestValid(digest)) continue;
        const auto id=Id(character,digest);
        if (journal.Find(id)||pending.contains(id)) continue;
        if (auto record=ReadLocal(root,campaign,character,digest))
            pending.emplace(id,std::move(*record));
    }
    return !ec;
}
}
