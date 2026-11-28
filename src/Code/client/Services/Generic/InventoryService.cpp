#include <Services/InventoryService.h>

#include <Messages/RequestObjectInventoryChanges.h>
#include <Messages/NotifyObjectInventoryChanges.h>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/RequestEquipmentChanges.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/NotifyInventorySnapshot.h>
#include <Messages/DrawWeaponRequest.h>
#include <Messages/NotifyDrawWeapon.h>

#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Services/CampaignAccess.h>
#include <Services/QuestService.h>
#include <Services/OverlayService.h>
#include <Forms/TESQuest.h>
#include <Messages/RequestQuestUpdate.h>
#include <Services/QuestPhysicalStageClient.h>
#include <Components/TESContainer.h>
#include <ExtraData/ExtraContainerChanges.h>
#include <cstdlib>
#include <algorithm>
#include <set>
#include <Events/InventoryChangeEvent.h>
#include <Events/EquipmentChangeEvent.h>

#include <World.h>
#include <Services/HomeActorProtection.h>
#include <Services/HomePlacementClient.h>
#include <Systems/OwnershipSystem.h>
#include <Games/Skyrim/Interface/UI.h>
#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Actor.h>
#include <Structs/ObjectData.h>
#include <Forms/TESWorldSpace.h>
#include <Games/TES.h>
#include <Games/Overrides.h>
#include <EquipManager.h>
#include <Games/ActorExtension.h>
#include <Forms/TESNPC.h>
#include <DefaultObjectManager.h>

InventoryService::InventoryService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_updateConnection = m_dispatcher.sink<UpdateEvent>().connect<&InventoryService::OnUpdate>(this);
    m_inventoryConnection = m_dispatcher.sink<InventoryChangeEvent>().connect<&InventoryService::OnInventoryChangeEvent>(this);
    m_equipmentConnection = m_dispatcher.sink<EquipmentChangeEvent>().connect<&InventoryService::OnEquipmentChangeEvent>(this);
    m_inventoryChangeConnection = m_dispatcher.sink<NotifyInventoryChanges>().connect<&InventoryService::OnNotifyInventoryChanges>(this);
    m_equipmentChangeConnection = m_dispatcher.sink<NotifyEquipmentChanges>().connect<&InventoryService::OnNotifyEquipmentChanges>(this);
    m_inventorySnapshotConnection = m_dispatcher.sink<NotifyInventorySnapshot>().connect<&InventoryService::OnNotifyInventorySnapshot>(this);
    const auto* evidence=std::getenv("STM_INVENTORY_EVIDENCE");
    if (evidence && std::string_view(evidence)=="1")
    {
        if (auto* events=EventDispatcherManager::Get())
        {
            m_evidenceEnabled=true;
            m_evidenceDisconnect=m_dispatcher.sink<DisconnectedEvent>().connect<&InventoryService::OnEvidenceDisconnected>(this);
            events->containerChangedEvent.RegisterSink(this);
            spdlog::info("STM inventory evidence enabled: diagnostic only, not campaign history");
        }
    }
}

void InventoryService::OnUpdate(const UpdateEvent& acUpdateEvent) noexcept
{
    RunWeaponStateUpdates();
    if (m_evidenceEnabled) ObserveInventory();
}

namespace
{
std::optional<InventoryEvidence::Counts> ReadCounts(TESObjectREFR& object)
{
    // Same base + runtime count representation used by STR inventory sync.
    // Do not dereference the extra-data lists or try to identify unique items.
    InventoryEvidence::Counts result;
    const auto* base=object.GetContainer();
    const auto* changes=object.GetContainerChanges();
    if (!base || (base->count && !base->entries) || (changes && !changes->entries)) return {};
    for (uint32_t i=0;i<base->count;++i)
    {
        const auto* entry=base->entries[i];
        if (!entry || !entry->form) continue;
        if (!InventoryEvidence::Accumulate(result,entry->form->formID,static_cast<int32_t>(entry->count))) return {};
    }
    if (changes)
        for (const auto* entry:*changes->entries)
        {
            if (!entry || !entry->form) continue;
            if (!InventoryEvidence::Accumulate(result,entry->form->formID,entry->count)) return {};
        }
    for (const auto& [id,count]:result)
        if (count<0 || count>INT32_MAX) return {};
    return result;
}

// Resolve names on the game thread, without relying on the transient server mod IDs.
std::vector<InventoryEvidence::PluginSlot> ReadPluginSlots()
{
    std::vector<InventoryEvidence::PluginSlot> result;
    if (auto* manager=ModManager::Get())
        for (const auto* mod:manager->mods)
        {
            if (!mod || !mod->IsLoaded()) continue;
            const auto* end=std::find(mod->filename,mod->filename+sizeof(mod->filename),'\0');
            if (end==mod->filename+sizeof(mod->filename)) continue;
            result.push_back({mod->GetId(),mod->IsLite(),std::string(mod->filename,end)});
        }
    return result;
}

std::string KeyLabel(uint32_t form,const std::vector<InventoryEvidence::PluginSlot>& slots)
{
    if (form==InventoryEvidence::Player) return "local-player";
    if (!form) return "no-endpoint";
    auto key=InventoryEvidence::Resolve(form,slots);
    return key ? fmt::format("{}:{:X}",key->Plugin,key->Local) : "unresolved";
}

InventoryEvidence::Endpoints ReadEndpoints(World& world,PlayerCharacter& player,
    const std::vector<InventoryEvidence::Transfer>& events,const InventoryEvidence::Endpoints& previous)
{
    // Only inspect loaded, registered static containers in the player's current cell.
    // Do not resolve arbitrary event handles or read remote actor inventories.
    std::set<uint32_t> preferred;
    for (const auto& e:events)
    {
        if (e.Source!=InventoryEvidence::Player && e.Source) preferred.insert(e.Source);
        if (e.Destination!=InventoryEvidence::Player && e.Destination) preferred.insert(e.Destination);
    }
    struct Candidate { uint32_t Form,Server; entt::entity Entity; int Priority; };
    std::vector<Candidate> candidates;
    auto view=world.view<FormIdComponent,ObjectComponent>();
    for (auto entity:view)
    {
        const auto id=view.get<FormIdComponent>(entity).Id;
        if (!id || (id>>24)==0xFF) continue;
        const auto server=view.get<ObjectComponent>(entity).Id;
        const int priority=preferred.count(id)?0:previous.count(id)?1:2;
        candidates.push_back({id,server,entity,priority});
    }
    std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b)
    {
        if (a.Priority!=b.Priority) return a.Priority<b.Priority;
        return a.Form<b.Form;
    });
    InventoryEvidence::Endpoints result;
    for (const auto& candidate:candidates)
    {
        if (result.size()>=InventoryEvidence::MaxEndpoints) break;
        auto* object=Cast<TESObjectREFR>(TESForm::GetById(candidate.Form));
        if (!object || !object->baseForm || object->baseForm->formType!=FormType::Container ||
            object->IsDeleted() || object->IsDisabled() || object->IsStillLoading() ||
            object->parentCell!=player.parentCell || !object->GetNiNode()) continue;
        auto counts=ReadCounts(*object);
        if (!counts) continue;
        result.emplace(candidate.Form,InventoryEvidence::Endpoint{
            candidate.Server,static_cast<uint32_t>(candidate.Entity),std::move(*counts)});
    }
    return result;
}
}

BSTEventResult InventoryService::OnEvent(const TESContainerChangedEvent* event,const EventDispatcher<TESContainerChangedEvent>*)
{
    // The engine may dispatch on job threads. Copy scalar evidence only;
    // Runner drops callbacks belonging to the previous world/session.
    if (!event || (event->oldContainerID!=InventoryEvidence::Player && event->newContainerID!=InventoryEvidence::Player))
        return BSTEventResult::kOk;
    const InventoryEvidence::Transfer copy{event->oldContainerID,event->newContainerID,event->baseObjectID,
        event->itemCount,event->referenceHandle,event->uniqueID,ScopedInventoryOverride::IsOverriden()};
    m_world.GetRunner().Queue([this,copy]()
    {
        if (!m_transport.IsOnline()) return;
        if (m_evidenceEvents.size()>=InventoryEvidence::MaxEvents)
            m_evidenceOverflow=true;
        else m_evidenceEvents.push_back(copy);
    });
    return BSTEventResult::kOk;
}

void InventoryService::ResetEvidence() noexcept
{
    if (m_evidenceBaseline || !m_evidenceEvents.empty() || m_evidenceOverflow)
        spdlog::info("STM inventory evidence gap/reset: discarded events={}",m_evidenceEvents.size());
    m_evidenceBaseline.reset();
    m_evidenceEndpoints.clear();
    m_evidenceEvents.clear();
    m_evidenceOverflow=false;
    m_evidenceNext={};
}

void InventoryService::OnEvidenceDisconnected(const DisconnectedEvent&) noexcept
{
    ResetEvidence();
}

void InventoryService::ObserveInventory() noexcept
{
    if (!m_transport.IsOnline() || !CampaignAccess::GameReady())
    {
        ResetEvidence();
        return;
    }
    const auto now=std::chrono::steady_clock::now();
    if (now<m_evidenceNext) return;
    m_evidenceNext=now+std::chrono::seconds(1);
    auto* player=PlayerCharacter::Get();
    auto current=player ? ReadCounts(*player) : std::nullopt;
    if (!current)
    {
        ResetEvidence();
        return;
    }
    auto endpoints=ReadEndpoints(m_world,*player,m_evidenceEvents,m_evidenceEndpoints);
    if (m_evidenceBaseline && !m_evidenceOverflow)
    {
        const auto result=InventoryEvidence::Compare(*m_evidenceBaseline,*current,m_evidenceEvents);
        const auto slots=ReadPluginSlots();
        for (const auto& e:m_evidenceEvents)
            spdlog::info("STM inventory event src={:X} dst={:X} base={:X} count={} handle={:X} unique={} override={} srcKey={} dstKey={} itemKey={}",
                e.Source,e.Destination,e.Item,e.Count,e.Reference,e.Unique,e.Override,
                KeyLabel(e.Source,slots),KeyLabel(e.Destination,slots),KeyLabel(e.Item,slots));
        for (const auto& row:result.Rows)
            spdlog::info("STM inventory net base={:X} before={} after={} eventNet={} status={}",
                row.Item,row.Before,row.After,row.Expected,InventoryEvidence::Label(row.State));
        for (const auto& row:InventoryEvidence::CompareEndpoints(m_evidenceEndpoints,endpoints,m_evidenceEvents,result))
            spdlog::info("STM inventory counterpart ref={} item={} playerNet={} counterpartNet={} status={}",
                KeyLabel(row.Container,slots),KeyLabel(row.Item,slots),row.PlayerNet,row.OtherNet,
                InventoryEvidence::PairLabel(row.State));
        if (!result.Valid) spdlog::warn("STM inventory evidence batch invalid; no transaction inferred");
    }
    else if (!m_evidenceEvents.empty() || m_evidenceOverflow)
        spdlog::warn("STM inventory evidence gap: no baseline or overflow, events={}",m_evidenceEvents.size());
    m_evidenceBaseline=std::move(current);
    m_evidenceEndpoints=std::move(endpoints);
    m_evidenceEvents.clear();
    m_evidenceOverflow=false;
}

void InventoryService::OnInventoryChangeEvent(const InventoryChangeEvent& acEvent) noexcept
{
    RequestInventoryChanges request;
    request.Item = acEvent.Item;
    request.Drop = acEvent.Drop;
    request.UpdateClients = acEvent.UpdateClients;
    request.QuestId = acEvent.QuestId;
    request.QuestRef = acEvent.QuestRef;
    request.QuestAlias = acEvent.QuestAlias;
    RequestQuestUpdate observedStage{};
    bool reportStage=false;
    const auto* localPlayer=PlayerCharacter::Get();
    const bool own=localPlayer&&acEvent.FormId==localPlayer->formID;
    if (request.QuestId&&own)
    {
        const auto form=m_world.GetModSystem().GetGameId(request.QuestId);
        auto* quest=Cast<TESQuest>(TESForm::GetById(form));
        if (!quest||QuestService::IsNonSyncableQuest(quest)||
            !(quest->flags&TESQuest::RunOnce)||(quest->flags&TESQuest::AllowRepeatStages)||
            m_world.GetOverlayService().IsPersonalQuest(form))
        {
            request.QuestId={};request.QuestRef={};request.QuestAlias=0;
        }
        else if (request.Item.Count==1&&!request.Drop&&!quest->IsStopped()&&
            (quest->flags&TESQuest::RunOnce)&&!(quest->flags&(TESQuest::Failed|TESQuest::Completed|TESQuest::AllowRepeatStages)))
        {
            // This check runs on the main thread. An alias alone does not
            // establish that a background or personal quest is shared.
            for (const auto& objective:localPlayer->objectives)
                if (objective.instance&&objective.instance->quest==quest)
                {
                    request.QuestInitialEvidence=true;
                    break;
                }
            if (request.QuestInitialEvidence&&quest->IsStageReached(quest->currentStage))
            {
                observedStage.Id=request.QuestId;
                observedStage.Stage=quest->currentStage;
                observedStage.Status=RequestQuestUpdate::StageUpdate;
                observedStage.ClientQuestType=static_cast<uint8_t>(quest->type);
                observedStage.JournalObjective=true;
                reportStage=true;
            }
        }
    }

    if (request.QuestId&&request.QuestRef&&own)
    {
        extern void QueueQuestItemReceipt(World&,const RequestInventoryChanges&) noexcept;
        QueueQuestItemReceipt(m_world,request);
    }
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>();

    const auto iter = std::find_if(std::begin(view), std::end(view), [view, formId = acEvent.FormId](auto entity) { return view.get<FormIdComponent>(entity).Id == formId; });

    if (iter == std::end(view))
        return;

    uint32_t serverId = 0;
    uint32_t ownershipEpoch = 0;
    if (Cast<Actor>(TESForm::GetById(acEvent.FormId)))
    {
        // The player's own changes to someone else's actor (looting, pickpocketing) are relayed to its owner
        const auto ownership = OwnershipSystem::GetOutgoingOwnership(m_world, *iter, acEvent.IsPlayerTransfer);
        if (!ownership)
        {
            spdlog::debug("Discarded an inventory change for actor {:X} that isn't owned locally (item {:X}, count {})", acEvent.FormId, acEvent.Item.BaseId.BaseId, acEvent.Item.Count);
            return;
        }

        serverId = ownership->ServerId;
        ownershipEpoch = ownership->OwnershipEpoch;
    }
    else
    {
        const std::optional<uint32_t> serverIdRes = Utils::GetServerId(*iter);
        if (!serverIdRes)
        {
            spdlog::warn(
                "Discarded inventory change for form {:X} because it has no server entity (item {:X}, count {})", acEvent.FormId, acEvent.Item.BaseId.BaseId, acEvent.Item.Count);
            return;
        }
        serverId = *serverIdRes;
    }

    request.ServerId = serverId;
    request.OwnershipEpoch = ownershipEpoch;
    m_transport.Send(request);
    // Inventory evidence is sent first. If the native stage event already
    // arrived, the existing server deduplicator ignores this observation.
    if (reportStage)
        SubmitObservedQuestStage(m_world,Cast<TESQuest>(TESForm::GetById(m_world.GetModSystem().GetGameId(observedStage.Id))),observedStage);

    spdlog::info("Sending item request, item: {:X}, count: {}, target object: {:X}", acEvent.Item.BaseId.BaseId, acEvent.Item.Count, acEvent.FormId);
}

void InventoryService::OnEquipmentChangeEvent(const EquipmentChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>();

    const auto iter = std::find_if(std::begin(view), std::end(view), [view, formId = acEvent.ActorId](auto entity) { return view.get<FormIdComponent>(entity).Id == formId; });

    if (iter == std::end(view))
        return;

    const auto ownership = OwnershipSystem::GetOutgoingOwnership(m_world, *iter, false);
    if (!ownership)
    {
        spdlog::debug("Discarded an equipment change for actor {:X} that isn't owned locally (item {:X})", acEvent.ActorId, acEvent.ItemId);
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(acEvent.ActorId));
    if (!pActor)
        return;

    auto& modSystem = World::Get().GetModSystem();

    RequestEquipmentChanges request;
    request.ServerId = ownership->ServerId;
    request.OwnershipEpoch = ownership->OwnershipEpoch;

    if (!modSystem.GetServerModId(acEvent.EquipSlotId, request.EquipSlotId))
        return;
    if (!modSystem.GetServerModId(acEvent.ItemId, request.ItemId))
        return;

    request.Count = acEvent.Count;
    request.Unequip = acEvent.Unequip;
    request.IsSpell = acEvent.IsSpell;
    request.IsShout = acEvent.IsShout;
    request.IsAmmo = acEvent.IsAmmo;
    request.CurrentInventory = pActor->GetEquipment();

    m_transport.Send(request);

    spdlog::info("Sending equipment request, item: {:X}, count: {}, target object: {:X}", acEvent.ItemId, acEvent.Count, acEvent.ActorId);
}

void InventoryService::OnNotifyInventoryChanges(const NotifyInventoryChanges& acMessage) noexcept
{
    if (acMessage.OwnershipEpoch != 0)
    {
        Actor* pActor = nullptr;

        const auto remoteEntity = OwnershipSystem::AcceptRemoteUpdate(m_world, acMessage.ServerId, acMessage.OwnershipEpoch);
        if (remoteEntity)
        {
            // The spawn request's inventory replaces the actor's once its 3D loads, so the change has to go there too
            if (auto* pWaitingFor3D = m_world.try_get<WaitingFor3D>(*remoteEntity))
            {
                pWaitingFor3D->SpawnRequest.InventoryContent.AddOrRemoveEntry(acMessage.Item);
                if (!acMessage.Drop)
                    return;
            }

            if (const auto* pFormIdComponent = m_world.try_get<FormIdComponent>(*remoteEntity))
                pActor = Cast<Actor>(TESForm::GetById(pFormIdComponent->Id));
        }
        else if (const auto localEntity = OwnershipSystem::AcceptOwnerUpdate(m_world, acMessage.ServerId, acMessage.OwnershipEpoch))
        {
            // Remote interactions with our own actor, e.g. a player looting it, are relayed back to the owner.
            if (const auto* pFormIdComponent = m_world.try_get<FormIdComponent>(*localEntity))
                pActor = Cast<Actor>(TESForm::GetById(pFormIdComponent->Id));
        }

        if (!pActor)
        {
            spdlog::debug("Discarded an inventory update for actor {:X} because epoch {} is no longer current", acMessage.ServerId, acMessage.OwnershipEpoch);
            return;
        }

        ScopedInventoryOverride _;

        // Journal projections own the world stack. Preserve the inventory
        // delta even when native drop replication is suppressed.
        if (acMessage.Drop && !HomeActorProtection::Resident(pActor) && !HomePlacementClient::Scoped(pActor))
            pActor->DropOrPickUpObject(acMessage.Item, nullptr, nullptr);
        else
            pActor->AddOrRemoveItem(acMessage.Item);

        return;
    }

    TESObjectREFR* pObject = Utils::GetByServerId<TESObjectREFR>(acMessage.ServerId);
    if (!pObject)
        return;

    ScopedInventoryOverride _;
    pObject->AddOrRemoveItem(acMessage.Item);
}

void InventoryService::OnNotifyEquipmentChanges(const NotifyEquipmentChanges& acMessage) noexcept
{
    const auto entity = OwnershipSystem::AcceptRemoteUpdate(m_world, acMessage.ServerId, acMessage.OwnershipEpoch);
    if (!entity)
    {
        spdlog::debug("Discarded an equipment update for actor {:X} because epoch {} is no longer current", acMessage.ServerId, acMessage.OwnershipEpoch);
        return;
    }

    // Equipping before the 3D loads would be undone by the spawn request's inventory
    if (auto* pWaitingFor3D = m_world.try_get<WaitingFor3D>(*entity))
    {
        pWaitingFor3D->PendingEquipmentChanges.push_back(acMessage);
        return;
    }

    const auto* pFormIdComponent = m_world.try_get<FormIdComponent>(*entity);
    if (!pFormIdComponent)
        return;

    ApplyEquipmentChange(Cast<Actor>(TESForm::GetById(pFormIdComponent->Id)), acMessage);
}

void InventoryService::ApplyEquipmentChange(Actor* apActor, const NotifyEquipmentChanges& acMessage) noexcept
{
    if (!apActor)
        return;

    auto& modSystem = World::Get().GetModSystem();

    uint32_t itemId = modSystem.GetGameId(acMessage.ItemId);
    TESForm* pItem = TESForm::GetById(itemId);

    if (!pItem)
    {
        spdlog::error("Could not find inventory item {:X}:{:X}", acMessage.ItemId.ModId, acMessage.ItemId.BaseId);
        return;
    }

    uint32_t equipSlotId = modSystem.GetGameId(acMessage.EquipSlotId);
    TESForm* pEquipSlot = TESForm::GetById(equipSlotId);

    uint32_t slotId = 0;
    if (pEquipSlot == DefaultObjectManager::Get().rightEquipSlot)
        slotId = 1;

    auto* pEquipManager = EquipManager::Get();

    if (acMessage.IsSpell)
    {
        if (acMessage.Unequip)
            pEquipManager->UnEquipSpell(apActor, pItem, slotId);
        else
            pEquipManager->EquipSpell(apActor, pItem, slotId);

        return;
    }
    else if (acMessage.IsShout)
    {
        if (acMessage.Unequip)
            pEquipManager->UnEquipShout(apActor, pItem);
        else
            pEquipManager->EquipShout(apActor, pItem);

        return;
    }

    // TODO: ExtraData necessary? probably
    if (acMessage.Unequip)
    {
        pEquipManager->UnEquip(apActor, pItem, nullptr, acMessage.Count, pEquipSlot, false, true, false, false, nullptr);
    }
    else
    {
        // Unequip all armor first, since the game won't auto unequip armor
        Inventory wornArmor{};
        if (pItem->formType == FormType::Armor)
        {
            wornArmor = apActor->GetWornArmor();
            for (const auto& armor : wornArmor.Entries)
            {
                uint32_t armorId = modSystem.GetGameId(armor.BaseId);
                TESForm* pArmor = TESForm::GetById(armorId);
                if (pArmor)
                    pEquipManager->UnEquip(apActor, pArmor, nullptr, 1, pEquipSlot, false, true, false, false, nullptr);
            }
        }

        pEquipManager->Equip(apActor, pItem, nullptr, acMessage.Count, pEquipSlot, false, true, false, false);

        for (const auto& armor : wornArmor.Entries)
        {
            uint32_t armorId = modSystem.GetGameId(armor.BaseId);
            TESForm* pArmor = TESForm::GetById(armorId);
            if (pArmor)
                pEquipManager->Equip(apActor, pArmor, nullptr, 1, pEquipSlot, false, true, false, false);
        }
    }
}

void InventoryService::OnNotifyInventorySnapshot(const NotifyInventorySnapshot& acMessage) noexcept
{
    const auto entity = OwnershipSystem::AcceptRemoteUpdate(m_world, acMessage.ServerId, acMessage.OwnershipEpoch);
    if (!entity)
    {
        spdlog::debug("Discarded an inventory snapshot for actor {:X} because epoch {} is no longer current", acMessage.ServerId, acMessage.OwnershipEpoch);
        return;
    }

    ApplyRemoteInventory(m_world, *entity, acMessage.CurrentInventory);
}

void InventoryService::ApplyRemoteInventory(World& aWorld, const entt::entity aEntity, const Inventory& acInventory) noexcept
{
    // The spawn request's inventory is applied once the 3D loads, anything applied before that would be overwritten
    if (auto* pWaitingFor3D = aWorld.try_get<WaitingFor3D>(aEntity))
    {
        pWaitingFor3D->SpawnRequest.InventoryContent = acInventory;
        pWaitingFor3D->PendingEquipmentChanges.clear();
        return;
    }

    const auto* pFormIdComponent = aWorld.try_get<FormIdComponent>(aEntity);
    Actor* pActor = pFormIdComponent ? Cast<Actor>(TESForm::GetById(pFormIdComponent->Id)) : nullptr;
    if (!pActor || !pActor->GetNiNode())
        return;

    const Inventory currentInventory = pActor->GetActorInventory();
    if (currentInventory.Entries != acInventory.Entries || currentInventory.CurrentMagicEquipment != acInventory.CurrentMagicEquipment)
        pActor->SetActorInventory(acInventory);
}

void InventoryService::RunWeaponStateUpdates() noexcept
{
    if (!m_transport.IsConnected())
        return;

    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 500ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    auto view = m_world.view<FormIdComponent, LocalComponent>();

    for (auto entity : view)
    {
        const auto& formIdComponent = view.get<FormIdComponent>(entity);
        Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        auto& localComponent = view.get<LocalComponent>(entity);

        bool isWeaponDrawn = pActor->actorState.IsWeaponDrawn();
        if (isWeaponDrawn != localComponent.IsWeaponDrawn)
        {
            localComponent.IsWeaponDrawn = isWeaponDrawn;

            DrawWeaponRequest request;
            request.Id = localComponent.Id;
            request.IsWeaponDrawn = isWeaponDrawn;

            m_transport.Send(request);
        }
    }
}
