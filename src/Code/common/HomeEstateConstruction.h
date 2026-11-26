#pragma once
#include "HomeEstateCatalog.h"
#include "HomeSnapshot.h"

namespace HomeEstateConstruction
{
// Ordered original references on both ends, no permission to mutate.
inline uint64_t Binding(const HomeEstateCatalog::Estate& estate)
{
    if (estate.Construction.empty()) return 0;
    std::string keys;
    for (const auto& [ref,base]:estate.Construction) keys+=ref+" "+base+"\n";
    return HomeSnapshot::Digest(keys);
}
struct Record
{
    uint64_t Owner{},BindingId{};
    uint32_t Revision{},SaveRevision{};
    std::string Plot,Bits;
    std::string Encode() const
    {
        return std::to_string(Owner)+" "+Plot+" "+std::to_string(Revision)+" "+
            std::to_string(SaveRevision)+" "+std::to_string(BindingId)+" "+Bits;
    }
};
inline bool ValidBits(std::string_view bits)
{return !bits.empty()&&bits.size()<=1024&&bits.find_first_not_of("01")==std::string_view::npos;}
inline std::optional<Record> Decode(std::string_view payload)
{
    Record r;std::istringstream in{std::string(payload)};std::string extra;
    if (!(in>>r.Owner>>r.Plot>>r.Revision>>r.SaveRevision>>r.BindingId>>r.Bits)||
        in>>extra||!r.Owner||!HomeEstateCatalog::Key(r.Plot)||!r.Revision||!r.BindingId||
        !ValidBits(r.Bits)||r.Encode()!=payload) return {};
    return r;
}
inline std::optional<Record> Read(const CampaignJournal::Journal& journal,uint64_t owner,const std::string& plot)
{
    if (!journal.Ready()||!owner||!HomeEstateCatalog::Key(plot)) return {};
    Record latest;latest.Owner=owner;latest.Plot=plot;
    size_t position=0;
    for (const auto& op:journal.Operations())
    {
        ++position;
        if (op.Kind!="home-estate-construction-v1") continue;
        const auto r=Decode(op.Payload);
        if (!r||op.Id!="home-estate-build-"+std::to_string(position)||r->SaveRevision>=position) return {};
        if (r->Owner!=owner||r->Plot!=plot) continue;
        if (latest.Revision==UINT32_MAX||r->Revision!=latest.Revision+1||
            r->SaveRevision<latest.SaveRevision||
            (latest.Revision&&(r->BindingId!=latest.BindingId||r->Bits==latest.Bits||
                r->Bits.size()!=latest.Bits.size()))) return {};
        latest=*r;
    }
    return latest;
}
inline bool Publish(CampaignJournal::Journal& journal,const HomeEstateCatalog::Estate& estate,
    uint64_t owner,uint32_t expected,uint32_t saveRevision,uint64_t binding,std::string_view bits)
{
    if (!owner||!binding||binding!=Binding(estate)||!ValidBits(bits)||bits.size()!=estate.Construction.size()||
        saveRevision>journal.Current().Revision) return false;
    const auto old=Read(journal,owner,estate.Quest);
    if (!old||old->Revision!=expected||saveRevision<old->SaveRevision||
        (old->Revision&&old->BindingId!=binding)) return false;
    if (old->Revision&&old->Bits==bits) return true;
    if (expected==UINT32_MAX||journal.Current().Revision==UINT32_MAX) return false;
    const Record r{owner,binding,expected+1,saveRevision,estate.Quest,std::string(bits)};
    return journal.Append(journal.Current(),{"home-estate-build-"+std::to_string(journal.Current().Revision+1),
        "home-estate-construction-v1",r.Encode()})==CampaignJournal::Result::Committed;
}
// Reuse the native visit shape/restore path: a construction revision is a
// structural change for every room of this owner's estate, not for other owners.
inline std::optional<std::string> RoomShape(const CampaignJournal::Journal& journal,
    const HomeEstateCatalog::Catalog& catalog,uint32_t slot,const std::string& root)
{
    const auto home=HomeRegistry::BySlot(journal,slot);
    if (!home||home->Root.Encode()!=root) return {};
    const auto* estate=catalog.ForRoom(root);
    if (!estate) return std::string{};
    const auto record=Read(journal,home->Owner,estate->Quest);
    if (!record) return {};
    if (!record->Revision) return std::string{};
    if (record->BindingId!=Binding(*estate)||record->Bits.size()!=estate->Construction.size()) return {};
    return "\nestate-construction "+record->Plot+" "+std::to_string(record->Owner)+" "+
        std::to_string(record->Revision)+" "+std::to_string(record->BindingId);
}

// One observation per plot in this campaign; author is audit metadata only.
// The bitmap is evidence, not a native building receipt or ownership grant.
inline std::optional<Record> ReadShared(const CampaignJournal::Journal& journal,const std::string& plot)
{
    if (!journal.Ready()||!HomeEstateCatalog::Key(plot)) return {};
    Record latest;latest.Plot=plot;
    size_t position=0;
    for (const auto& op:journal.Operations())
    {
        ++position;
        if (op.Kind!="home-shared-construction-observed-v1") continue;
        const auto r=Decode(op.Payload);
        if (!r||op.Id!="home-shared-observed-"+std::to_string(position)||r->SaveRevision>=position) return {};
        if (r->Plot!=plot) continue;
        if (latest.Revision==UINT32_MAX||r->Revision!=latest.Revision+1||
            (latest.Revision&&(r->BindingId!=latest.BindingId||r->Bits.size()!=latest.Bits.size()||
                r->Bits==latest.Bits))) return {};
        latest=*r;
    }
    return latest;
}
enum class SharedObservation { Accepted, Unchanged, Conflict, Invalid };
inline uint64_t PriorDigest(const Record& r)
{return r.Revision?HomeSnapshot::Digest(r.Bits):0;}
inline SharedObservation ObserveShared(CampaignJournal::Journal& journal,const HomeEstateCatalog::Estate& estate,
    uint64_t author,uint32_t expected,uint32_t saveRevision,uint64_t binding,
    uint64_t previous,std::string_view bits)
{
    if (!author||!binding||binding!=Binding(estate)||!ValidBits(bits)||bits.size()!=estate.Construction.size()||
        saveRevision>journal.Current().Revision) return SharedObservation::Invalid;
    const auto old=ReadShared(journal,estate.Quest);
    if (!old||(old->Revision&&old->BindingId!=binding)) return SharedObservation::Invalid;
    if (old->Revision!=expected||PriorDigest(*old)!=previous) return SharedObservation::Conflict;
    if (old->Revision&&old->Bits==bits) return SharedObservation::Unchanged;
    if (expected==UINT32_MAX||journal.Current().Revision==UINT32_MAX) return SharedObservation::Invalid;
    const Record r{author,binding,expected+1,saveRevision,estate.Quest,std::string(bits)};
    return journal.Append(journal.Current(),{"home-shared-observed-"+std::to_string(journal.Current().Revision+1),
        "home-shared-construction-observed-v1",r.Encode()})==CampaignJournal::Result::Committed?
            SharedObservation::Accepted:SharedObservation::Invalid;
}
inline bool SharedMode() noexcept
{
    const auto* flag=std::getenv("STM_SHARED_PROPERTY");
    return flag&&std::string_view(flag)=="1";
}
inline std::optional<std::pair<uint64_t,std::string_view>> Proposal(std::string_view payload)
{
    const auto split=payload.find(' ');
    if (split==std::string_view::npos||!split) return {};
    uint64_t previous{};
    const auto parsed=std::from_chars(payload.data(),payload.data()+split,previous);
    const auto bits=payload.substr(split+1);
    if (parsed.ec!=std::errc{}||parsed.ptr!=payload.data()+split||
        std::to_string(previous)!=payload.substr(0,split)||!ValidBits(bits)) return {};
    return std::pair{previous,bits};
}
}
