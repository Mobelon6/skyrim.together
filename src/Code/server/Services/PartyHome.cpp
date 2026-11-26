#include <GameServer.h>
#include <World.h>
#include <Game/Player.h>
#include <Services/OverlayService.h>
#include <Services/CampaignAccess.h>
#include <HomeRegistry.h>
#include <HomeSnapshot.h>
#include <HomePermissions.h>
#include <HomeLinkedAccess.h>
#include <HomeEstateCatalog.h>
#include <HomeEstateConstruction.h>
#include <HomeLocks.h>
#include <HomeResidents.h>
#include <HomeObjects.h>
#include <HomeSeatLeases.h>
#include <Services/HomeVisit.h>
#include <chrono>
#include <map>
#include <sstream>
#include <iomanip>
#include <InventoryEvidence.h>
#include <Messages/RequestPartyHome.h>
#include <Messages/NotifyPartyHome.h>

namespace
{
using Clock=std::chrono::steady_clock;
struct PendingSnapshot {uint64_t Campaign{},Owner{};uint32_t Connection{};Clock::time_point Until;HomeSnapshot::Upload Data;};
std::map<uint32_t,PendingSnapshot> pendingSnapshots;
struct Lease {uint64_t Campaign{},Character{},Connection{};uint32_t Slot{};Clock::time_point Until;};
std::map<uint32_t,Lease> visits;
}
bool HomeVisit::CanEnter(Player& player,uint32_t mod,uint32_t base)
{
    if (HomeEstateConstruction::SharedMode()) return false;
    auto& link=CampaignAccess::Authority();
    const auto character=CampaignAccess::Character(player);
    if (link.State()!=CampaignReplication::Link::Mode::Ready || !HomeRegistry::Scoped(mod) || !character) return false;
    if (HomeRegistry::OwnsCell(link.Journal,character,mod,base)) return true;
    const auto slot=mod&HomeRegistry::MaxSlot;
    const auto home=HomeRegistry::BySlot(link.Journal,slot);
    const auto found=visits.find(player.GetId());
    return home && home->Root.Local==base && found!=visits.end() &&
        found->second.Campaign==link.Campaign && found->second.Character==character &&
        found->second.Connection==uint64_t(player.GetConnectionId()) &&
        found->second.Slot==slot && Clock::now()<found->second.Until &&
        HomePermissions::Effective(link.Journal,slot,character)!=HomePermissions::Right::None;
}
HomeScope::EstateContext HomeVisit::EstateFor(Player& player,uint32_t mod,uint32_t base) noexcept
{
    try
    {
        if (!CanEnter(player,mod,base)) return {};
        auto& link=CampaignAccess::Authority();
        const auto& catalog=HomeEstateCatalog::Host();
        const auto room=HomeRegistry::BySlot(link.Journal,mod&HomeRegistry::MaxSlot);
        if (!catalog||!room) return {};
        return HomeEstateCatalog::Identity(*catalog,link.Journal,link.Campaign,*room).value_or(HomeScope::EstateContext{});
    }
    catch (...) {return {};}
}
void OverlayService::OnHome(const PacketEvent<RequestPartyHome>& packet) noexcept
{
    try
    {
        auto* owner=packet.pPlayer;
        if (!owner) return;
        const auto& e=packet.Packet.Event;
        auto& link=CampaignAccess::Authority();
        if (link.State()!=CampaignReplication::Link::Mode::Disabled)
        {
            // Only the authenticated character may register its observed home.
            // This is not a guest grant or a write to an existing house snapshot.
            if (link.State()!=CampaignReplication::Link::Mode::Ready || e.Owner ||
                !e.Campaign || !e.Character || e.Root.size()>220 ||
                e.Profile.size()>128 || e.Credential.size()!=64 ||
                e.Visible.size()>HomeSnapshot::PartBytes) return;
            const CampaignIdentity::Stamp stamp{e.Campaign,e.Character,{e.SaveRevision,e.SaveTag}};
            if (!CampaignAccess::Admit(m_world,*owner,stamp,{e.Profile.data(),e.Profile.size()},
                {e.Credential.data(),e.Credential.size()})) return;
            auto root=TradeProperties::DecodeForm({e.Root.data(),e.Root.size()});
            if (!root || !root->Valid()) return;
            const bool shared=HomeEstateConstruction::SharedMode();
            if (shared&&e.AccessOp!=10) return; // no private registration/visit in transition campaign
            if (e.AccessOp==9||e.AccessOp==10)
            {
                if (shared!=(e.AccessOp==10)||e.House||e.AccessGuest||e.AccessRight||
                    e.SnapshotPart||e.SnapshotParts||!e.SnapshotDigest) return;
                const std::string_view raw(e.Visible.data(),e.Visible.size());
                const auto proposal=shared?HomeEstateConstruction::Proposal(raw):std::nullopt;
                if (shared?!proposal:!HomeEstateConstruction::ValidBits(raw)) return;
                const auto& catalog=HomeEstateCatalog::Host();
                if (!catalog||!owner->GetCharacter()) return;
                const auto found=catalog->Estates.find(root->Encode());
                if (found==catalog->Estates.end()) return;
                const auto& estate=found->second;
                const auto world=TradeProperties::DecodeForm(estate.World);
                const auto& location=owner->GetCellComponent();
                if (!world||!location.WorldSpaceId||location.WorldSpaceId.BaseId!=world->Local||
                    location.HasExteriorEstate()) return;
                const auto& mods=m_world.ctx().at<ModsComponent>();
                bool sameWorld=false;
                for (const auto* list:{&mods.GetStandardMods(),&mods.GetLiteMods()})
                    for (const auto& [name,entry]:*list)
                        if (entry.id==location.WorldSpaceId.ModId&&
                            InventoryEvidence::CanonicalPlugin({name.data(),name.size()})==world->Plugin) sameWorld=true;
                const auto* movement=m_world.try_get<MovementComponent>(*owner->GetCharacter());
                if (!sameWorld||!movement||!movement->LastMovement.Seen||
                    movement->LastMovement.Location!=owner->LocationEpoch()||
                    !estate.ContainsCandidate(estate.World,movement->Position.x,movement->Position.y)) return;
                const auto character=CampaignAccess::Character(*owner);
                if (shared)
                {
                    HomeEstateConstruction::ObserveShared(link.Journal,estate,character,e.AccessRevision,
                        e.SaveRevision,e.SnapshotDigest,proposal->first,proposal->second);
                    return;
                }
                HomeEstateConstruction::Publish(link.Journal,estate,character,e.AccessRevision,
                    e.SaveRevision,e.SnapshotDigest,{e.Visible.data(),e.Visible.size()});
                return;
            }
            if (e.AccessOp==8)
            {
                if (!e.House||!e.AccessGuest||e.AccessRight>2||e.AccessRevision||
                    e.SnapshotDigest||e.SnapshotPart||e.SnapshotParts||e.Visible.empty()||e.Visible.size()>220) return;
                const auto key=TradeProperties::DecodeForm({e.Visible.data(),e.Visible.size()});
                if (!key||!key->Valid()||key->Encode()!=std::string(e.Visible.data(),e.Visible.size())) return;
                const auto character=CampaignAccess::Character(*owner);
                const auto home=HomeRegistry::BySlot(link.Journal,e.House);
                if (!home||home->Root.Encode()!=root->Encode()) return;
                static HomeSeatLeases::Table seats;
                const auto now=Clock::now();
                seats.Prune(now,[&](const HomeSeatLeases::Claim& c)
                {
                    auto* p=m_world.GetPlayerManager().GetById(c.Player);
                    if (!p||c.Campaign!=link.Campaign||uint64_t(p->GetConnectionId())!=c.Connection||
                        CampaignAccess::Character(*p)!=c.Character) return false;
                    const auto& cell=p->GetCellComponent().Cell;
                    return cell.ModId==(HomeRegistry::ScopePrefix|c.Slot)&&cell.BaseId==c.Cell&&
                        HomeVisit::CanEnter(*p,cell.ModId,cell.BaseId);
                });
                HomeSeatLeases::Claim claim{link.Campaign,character,uint64_t(owner->GetConnectionId()),e.AccessGuest,
                    owner->GetId(),e.House,home->Root.Local,key->Encode()};
                const auto& cell=owner->GetCellComponent().Cell;
                bool eligible=e.AccessRight==0;
                if (e.AccessRight&&cell.ModId==(HomeRegistry::ScopePrefix|e.House)&&
                    cell.BaseId==claim.Cell&&HomeVisit::CanEnter(*owner,cell.ModId,cell.BaseId)&&owner->GetCharacter())
                {
                    if (e.AccessRight==2) eligible=true;
                    else
                    {
                        const auto objects=HomeObjects::Read(link.Journal,e.House);
                        const auto* movement=m_world.try_get<MovementComponent>(*owner->GetCharacter());
                        if (objects&&movement)
                        {
                            const auto it=objects->find(claim.Ref);
                            if (it!=objects->end()&&it->second.Type==40&&!it->second.Value.Disabled&&!it->second.Inventory)
                            {
                                const auto& pose=it->second.Value.Transform;
                                const double x=double(movement->Position.x)-std::bit_cast<float>(pose[0]);
                                const double y=double(movement->Position.y)-std::bit_cast<float>(pose[1]);
                                const double z=double(movement->Position.z)-std::bit_cast<float>(pose[2]);
                                eligible=std::isfinite(x+y+z)&&x*x+y*y+z*z<=350.0*350.0;
                            }
                        }
                    }
                }
                const bool granted=eligible&&seats.Change(e.AccessRight,claim,now);
                NotifyPartyHome result;auto& r=result.Event;
                r.Campaign=link.Campaign;r.Character=character;r.Owner=owner->GetId();r.House=e.House;
                r.Root=root->Encode().c_str();r.Visible=claim.Ref.c_str();r.AccessOp=8;
                r.AccessGuest=e.AccessGuest;r.AccessRevision=e.AccessRight;r.AccessRight=granted?1u:0u;
                owner->Send(result);return;
            }
            if (e.AccessOp==7)
            {
                if (!e.House || e.AccessGuest || (e.AccessRight!=1 && e.AccessRight!=3) ||
                    e.SnapshotDigest || e.SnapshotPart || e.SnapshotParts || e.Visible.empty()) return;
                const auto character=CampaignAccess::Character(*owner);
                const auto home=HomeRegistry::BySlot(link.Journal,e.House);
                const auto& cell=owner->GetCellComponent().Cell;
                if (!home || home->Owner==character || home->Root.Encode()!=root->Encode() ||
                    cell.ModId!=(HomeRegistry::ScopePrefix|e.House) ||
                    !HomeVisit::CanEnter(*owner,cell.ModId,cell.BaseId) || !owner->GetCharacter()) return;
                const auto* movement=m_world.try_get<MovementComponent>(*owner->GetCharacter());
                if (!movement) return;
                HomeObjects::SetGuestDoor(link.Journal,character,e.House,
                    std::string(e.Visible.data(),e.Visible.size()),e.AccessRevision,e.AccessRight,
                    {movement->Position.x,movement->Position.y,movement->Position.z});
                return;
            }
            if (e.AccessOp==6)
            {
                if (!e.House || e.AccessGuest || e.AccessRight || e.SnapshotDigest ||
                    e.SnapshotPart || e.SnapshotParts || e.Visible.empty()) return;
                const auto character=CampaignAccess::Character(*owner);
                const auto home=HomeRegistry::BySlot(link.Journal,e.House);
                const auto& cell=owner->GetCellComponent().Cell;
                if (!home || !home->Same(character,*root) || cell.ModId!=(HomeRegistry::ScopePrefix|e.House) ||
                    !HomeRegistry::OwnsCell(link.Journal,character,cell.ModId,cell.BaseId)) return;
                const auto change=HomeObjects::Decode({e.Visible.data(),e.Visible.size()});
                if (change && change->Slot==e.House && change->SaveRevision==e.SaveRevision)
                    HomeObjects::Set(link.Journal,character,*change,e.AccessRevision);
                return;
            }
            if (e.AccessOp==5)
            {
                if (!e.House || e.AccessGuest || e.AccessRight || e.SnapshotDigest ||
                    e.SnapshotPart || e.SnapshotParts || e.Visible.empty()) return;
                const auto character=CampaignAccess::Character(*owner);
                const auto home=HomeRegistry::BySlot(link.Journal,e.House);
                const auto& cell=owner->GetCellComponent().Cell;
                if (!home || !home->Same(character,*root) || cell.ModId!=(HomeRegistry::ScopePrefix|e.House) ||
                    !HomeRegistry::OwnsCell(link.Journal,character,cell.ModId,cell.BaseId)) return;
                auto resident=HomeResidents::Decode({e.Visible.data(),e.Visible.size()});
                if (resident) HomeResidents::Set(link.Journal,character,e.House,e.AccessRevision,e.SaveRevision,*resident);
                return;
            }
            if (e.AccessOp==4)
            {
                if (!e.House || e.AccessRight<1 || e.AccessRight>2 || e.AccessGuest>255 ||
                    e.SnapshotDigest || e.SnapshotPart || e.SnapshotParts || e.Visible.empty()) return;
                const auto character=CampaignAccess::Character(*owner);
                const auto home=HomeRegistry::BySlot(link.Journal,e.House);
                const auto& cell=owner->GetCellComponent().Cell;
                if (!home || !home->Same(character,*root) ||
                    cell.ModId!=(HomeRegistry::ScopePrefix|e.House) ||
                    !HomeRegistry::OwnsCell(link.Journal,character,cell.ModId,cell.BaseId)) return;
                HomeLocks::Set(link.Journal,character,e.House,{e.Visible.data(),e.Visible.size()},
                    e.AccessRevision,e.AccessRight,static_cast<uint32_t>(e.AccessGuest));
                return;
            }
            if (e.AccessOp==3)
            {
                if (!e.House || !e.AccessGuest || e.AccessGuest>UINT32_MAX || e.AccessRevision || e.AccessRight ||
                    e.SnapshotDigest || e.SnapshotPart || e.SnapshotParts || !e.Visible.empty()) return;
                const auto character=CampaignAccess::Character(*owner);
                const auto home=HomeRegistry::BySlot(link.Journal,e.House);
                auto right=HomePermissions::Right::None;
                if (home && home->Owner!=character && home->Root.Encode()==root->Encode() &&
                    HomeSnapshot::Read(link.Journal,e.House).State==HomeSnapshot::Status::Ready)
                    right=HomePermissions::Effective(link.Journal,e.House,character);
                const auto now=Clock::now();
                std::erase_if(visits,[&](const auto& row){return row.second.Until<=now;});
                if (right==HomePermissions::Right::None) visits.erase(owner->GetId());
                else visits[owner->GetId()]={link.Campaign,character,uint64_t(owner->GetConnectionId()),e.House,now+std::chrono::seconds(30)};
                NotifyPartyHome result;auto& r=result.Event;
                r.Campaign=link.Campaign;r.Character=character;r.Owner=owner->GetId();r.House=e.House;
                r.Root=root->Encode().c_str();r.AccessOp=3;r.AccessGuest=e.AccessGuest;r.AccessRight=static_cast<uint32_t>(right);
                owner->Send(result);return;
            }
            if (e.AccessOp)
            {
                if ((e.AccessOp!=1 && e.AccessOp!=2) || !e.House || e.AccessRight>2 || !e.AccessGuest ||
                    e.SnapshotDigest || e.SnapshotPart || e.SnapshotParts || !e.Visible.empty()) return;
                const auto character=CampaignAccess::Character(*owner);
                const auto home=HomeRegistry::BySlot(link.Journal,e.House);
                if (!home || !home->Same(character,*root)) return;
                const auto prior=HomePermissions::Read(link.Journal);
                const auto roomSlots=HomeLinkedAccess::RegisteredRooms(link.Journal,*home);
                if (!prior||roomSlots.empty()) return;
                const auto& estateCatalog=HomeEstateCatalog::Host();
                const auto* estate=estateCatalog?estateCatalog->ForRoom(home->Root.Encode()):nullptr;
                // Resolve stable character IDs on the authority. Include
                // existing grants even when the guest is not connected.
                std::map<uint64_t,std::string> candidates;
                for (const auto& [key,right]:prior->Guests)
                    if (std::binary_search(roomSlots.begin(),roomSlots.end(),key.first))
                        candidates[key.second]="персонаж "+std::to_string(key.second);
                if (auto* party=m_world.GetPartyService().GetPlayerParty(owner))
                    for (auto* member:party->Members)
                    {
                        const auto id=CampaignAccess::Character(*member);
                        if (!id || id==character) continue;
                        std::string name(member->GetUsername().begin(),member->GetUsername().end());
                        for (char& c:name) if (static_cast<unsigned char>(c)<32 || c==127) c=' ';
                        if (name.empty() || name.size()>128) name="персонаж "+std::to_string(id);
                        candidates[id]=std::move(name);
                    }
                if (e.AccessOp==2)
                {
                    if (e.AccessRevision || e.AccessRight || e.AccessGuest>UINT32_MAX) return;
                    std::ostringstream rows;
                    for (const auto& [id,name]:candidates)
                    {
                        const auto right=HomePermissions::Effective(link.Journal,e.House,id);
                        const auto estateRight=estate?HomePermissions::EstateRight(*prior,character,estate->Quest,id):right;
                        const bool mixed=(estate&&estateRight!=right)||std::any_of(roomSlots.begin(),roomSlots.end(),[&](uint32_t slot)
                            {return HomePermissions::Effective(link.Journal,slot,id)!=right;});
                        rows<<id<<' '<<(mixed?3u:static_cast<uint32_t>(right))
                            <<' '<<std::quoted(name+(mixed?" (частичные права)":estate?" (всё поместье, включая будущие помещения)":roomSlots.size()>1?" (все связанные помещения)":""))<<'\n';
                    }
                    const auto list=rows.str();
                    if (list.size()>HomeSnapshot::PartBytes) return;
                    NotifyPartyHome response;auto& r=response.Event;
                    r.Campaign=link.Campaign;r.Character=character;r.Owner=owner->GetId();
                    r.House=e.House;r.Root=home->Root.Encode().c_str();r.AccessOp=2;
                    r.AccessGuest=e.AccessGuest;r.AccessRevision=prior->Revision;r.Visible=list.c_str();
                    owner->Send(response);return;
                }
                if (!candidates.count(e.AccessGuest)) return;
                const auto right=static_cast<HomePermissions::Right>(e.AccessRight);
                const bool applied=estate?
                    HomePermissions::SetEstate(link.Journal,character,estate->Quest,estate->Rooms,e.AccessGuest,right,e.AccessRevision):
                    roomSlots.size()==1?
                    HomePermissions::Set(link.Journal,character,e.House,e.AccessGuest,right,e.AccessRevision):
                    HomePermissions::SetGroup(link.Journal,character,roomSlots,e.AccessGuest,right,e.AccessRevision);
                if (!applied) return;
                const auto rules=HomePermissions::Read(link.Journal);
                if (!rules) return;
                NotifyPartyHome result;
                result.Event.Campaign=link.Campaign;result.Event.Character=character;
                result.Event.Owner=owner->GetId();result.Event.House=e.House;
                result.Event.Root=home->Root.Encode().c_str();
                result.Event.AccessOp=1;result.Event.AccessGuest=e.AccessGuest;
                result.Event.AccessRight=e.AccessRight;result.Event.AccessRevision=rules->Revision;
                owner->Send(result);
                return;
            }
            if (e.AccessRevision || e.AccessRight || e.AccessGuest) return;
            if (e.SnapshotParts)
            {
                const auto home=HomeRegistry::BySlot(link.Journal,e.House);
                const auto character=CampaignAccess::Character(*owner);
                if (!home || !home->Same(character,*root) ||
                    !HomeRegistry::OwnsCell(link.Journal,character,owner->GetCellComponent().Cell.ModId,
                    owner->GetCellComponent().Cell.BaseId) ||
                    (owner->GetCellComponent().Cell.ModId&HomeRegistry::MaxSlot)!=e.House) return;
                if (HomeSnapshot::Read(link.Journal,e.House).State!=HomeSnapshot::Status::Missing) return;
                const auto now=Clock::now();
                std::erase_if(pendingSnapshots,[&](const auto& row){return row.second.Until<now;});
                if (!pendingSnapshots.count(e.House) && (e.SnapshotPart || pendingSnapshots.size()>=16)) return;
                auto& p=pendingSnapshots[e.House];
                if (!e.SnapshotPart) {p={};p.Campaign=e.Campaign;p.Owner=character;p.Connection=owner->GetId();}
                if (p.Campaign!=e.Campaign || p.Owner!=character || p.Connection!=owner->GetId()) return;
                p.Until=now+std::chrono::seconds(30);
                if (!p.Data.Add(e.SnapshotDigest,e.SnapshotParts,e.SnapshotPart,{e.Visible.data(),e.Visible.size()}))
                {pendingSnapshots.erase(e.House);return;}
                if (p.Data.Next==p.Data.Parts)
                {
                    if (p.Data.Complete() && HomeSnapshot::Publish(link.Journal,e.House,p.Data.Bytes))
                        spdlog::info("Initial static home baseline published for slot {}",e.House);
                    pendingSnapshots.erase(e.House);
                }
                return;
            }
            if (e.House || e.SnapshotDigest || e.SnapshotPart || !e.Visible.empty()) return;
            const auto& mods=m_world.ctx().at<ModsComponent>();
            bool known=false;
            for (const auto* list:{&mods.GetStandardMods(),&mods.GetLiteMods()})
                for (const auto& [name,entry]:*list)
                {
                    (void)entry;
                    if (InventoryEvidence::CanonicalPlugin({name.data(),name.size()})==root->Plugin &&
                        (list!=&mods.GetLiteMods() || root->Local<=0xFFFu)) known=true;
                }
            if (!known) return;
            // Existing registration is immutable, including across stale saves
            // and reconnects. The normal campaign replica delivers this record.
            auto home=HomeRegistry::Register(link.Journal,CampaignAccess::Character(*owner),*root);
            if (!home) return;
            NotifyPartyHome accepted;
            accepted.Event.Campaign=link.Campaign;
            accepted.Event.Character=home->Owner;
            accepted.Event.Owner=owner->GetId();
            accepted.Event.House=home->Slot;
            accepted.Event.Root=home->Root.Encode().c_str();
            owner->Send(accepted); // Never echo the credential.
            return;
        }
        if (e.Campaign || e.Character || e.SaveRevision || e.SaveTag ||
            !e.Profile.empty() || !e.Credential.empty() || !e.Root.empty() ||
            e.SnapshotDigest || e.SnapshotPart || e.SnapshotParts ||
            e.AccessOp || e.AccessRevision || e.AccessRight || e.AccessGuest) return;
        auto* party=m_world.GetPartyService().GetPlayerParty(owner);
        if (!party || !e.House || e.Visible.size()>16384 ||
            std::any_of(e.Visible.begin(),e.Visible.end(),[](char c){return c!='1' && c!='0' && c!='?';})) return;
        NotifyPartyHome msg;
        msg.Event=e; msg.Event.Owner=owner->GetId();
        for (auto* other:party->Members) if (other!=owner) other->Send(msg);
    }
    catch (const std::exception& e) {spdlog::error("Home registration: {}",e.what());}
}
