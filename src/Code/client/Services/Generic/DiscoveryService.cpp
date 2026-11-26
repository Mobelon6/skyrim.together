#include <TiltedOnlinePCH.h>
#include <Services/OverlayService.h>

#include <Services/DiscoveryService.h>
#include <Services/HomeInstance.h>
#include <Games/TES.h>

#include <Games/References.h>

#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/TESNPC.h>

#include <Events/ActorAddedEvent.h>
#include <Events/ActorRemovedEvent.h>
#include <Events/PreUpdateEvent.h>
#include <Events/GridCellChangeEvent.h>
#include <Events/CellChangeEvent.h>
#include <Events/LocationChangeEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/ConnectionErrorEvent.h>

#include <World.h>
#include <Components.h>
#include <BranchInfo.h>

namespace
{
bool IsActiveFollower(const Actor& acActor, const GameId& acFollowerFaction) noexcept
{
    const Factions factions = acActor.GetFactions();
    const auto matchesFaction = [&acFollowerFaction](const Faction& acFaction) {
        return acFaction.Id == acFollowerFaction;
    };

    const auto extra = std::find_if(factions.ExtraFactions.begin(), factions.ExtraFactions.end(), matchesFaction);
    if (extra != factions.ExtraFactions.end())
        return extra->Rank >= 0;

    const auto base = std::find_if(factions.NpcFactions.begin(), factions.NpcFactions.end(), matchesFaction);
    return base != factions.NpcFactions.end() && base->Rank >= 0;
}

#if (IS_MASTER)
bool IsDefaultModlist(GameList<Mod>& aCurrentModlist) noexcept
{
    static const auto s_defaultModlist = std::to_array<TiltedPhoques::String>(
        {"Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", 
        "Dragonborn.esm", "_ResourcePack.esl", "SkyrimTogether.esp", "SkyrimTogetherQuestPatches.esp"}
    );

    if (aCurrentModlist.Size() != s_defaultModlist.size())
        return false;

    auto expectedMod = s_defaultModlist.begin();
    for (const auto* pCurrentMod : aCurrentModlist)
    {
        if (pCurrentMod->filename != *expectedMod)
            return false;

        ++expectedMod;
    }

    return true;
}
#endif
} // namespace

DiscoveryService::DiscoveryService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
{
    m_preUpdateConnection = m_dispatcher.sink<PreUpdateEvent>().connect<&DiscoveryService::OnUpdate>(this);
    m_connectedConnection = m_dispatcher.sink<ConnectedEvent>().connect<&DiscoveryService::OnConnected>(this);

    EventDispatcherManager::Get()->loadGameEvent.RegisterSink(this);
}

void DiscoveryService::VisitCell(bool aForceTrigger) noexcept
{
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    if (pPlayer->GetWorldSpace())
        VisitExteriorCell(aForceTrigger);
    else if (pPlayer->GetParentCellEx())
        VisitInteriorCell(aForceTrigger);

    // exactly how the game does it too
    if (m_pLocation != pPlayer->locationForm)
    {
        m_dispatcher.trigger(LocationChangeEvent());
        m_pLocation = pPlayer->locationForm;
    }
}

void DiscoveryService::VisitExteriorCell(bool aForceTrigger) noexcept
{
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    const auto pWorldSpace = pPlayer->GetWorldSpace();

    HomeInstance::Prepare(m_world,nullptr);
    m_interiorNetworkCell={};
    m_interiorCellId = 0;

    const TES* pTES = TES::Get();
    const uint32_t worldSpaceId = pWorldSpace->formID;
    const GridCellCoords gameCurrentGrid(pTES->currentGridX, pTES->currentGridY);
    const GridCellCoords gameCenterGrid(pTES->centerGridX, pTES->centerGridY);

    if (m_worldSpaceId != worldSpaceId || aForceTrigger)
    {
        DetectGridCellChange(pWorldSpace, true);
        // If the world space changes, then we want to send out a CellChangeEvent out too.
        aForceTrigger = true;
    }
    else if (gameCenterGrid != m_centerGrid)
    {
        DetectGridCellChange(pWorldSpace, false);
    }

    if (gameCurrentGrid != m_currentGrid || aForceTrigger)
    {
        CellChangeEvent cellChangeEvent{};

        if (!m_world.GetModSystem().GetServerModId(pWorldSpace->formID, cellChangeEvent.WorldSpaceId))
        {
            spdlog::error("Failed to find world space id for form id {:X}", pWorldSpace->formID);
            return;
        }

        TESObjectCELL* pCell = pPlayer->GetParentCellEx();
        if (!pCell)
            pCell = ModManager::Get()->GetCellFromCoordinates(gameCurrentGrid.X, gameCurrentGrid.Y, pWorldSpace, false);

        if (!pCell) return;
        if (!m_world.GetModSystem().GetServerModId(pCell->formID, cellChangeEvent.CellId))
        {
            spdlog::error("Failed to find cell id for form id {:X}", pCell->formID);
            return;
        }

        cellChangeEvent.CurrentCoords = gameCurrentGrid;

        m_dispatcher.trigger(cellChangeEvent);

        m_currentGrid = gameCurrentGrid;
    }
}

void DiscoveryService::VisitInteriorCell(bool aForceTrigger) noexcept
{
    ResetCachedCellData();

    auto* cell=PlayerCharacter::Get()->GetParentCellEx();
    HomeInstance::Prepare(m_world,cell);
    const uint32_t cellId=cell->formID;
    CellChangeEvent cellChangeEvent{};
    if (!m_world.GetModSystem().GetServerModId(cellId,cellChangeEvent.CellId))
    {
        spdlog::error("Failed to find cell id {:X}",cellId);
        return;
    }
    // A new owner scope may be confirmed without any engine CELL change.
    if (m_interiorCellId!=cellId || m_interiorNetworkCell!=cellChangeEvent.CellId || aForceTrigger)
    {
        m_dispatcher.trigger(cellChangeEvent);
        m_interiorCellId=cellId;
        m_interiorNetworkCell=cellChangeEvent.CellId;
    }
}

void DiscoveryService::DetectGridCellChange(TESWorldSpace* aWorldSpace, bool aNewCellGrid) noexcept
{
    GridCellChangeEvent changeEvent{};

    const uint32_t worldSpaceId = aWorldSpace->formID;
    changeEvent.WorldSpaceId = worldSpaceId;

    const TES* pTES = TES::Get();
    const int32_t startGridX = pTES->centerGridX - GridCellCoords::m_gridsToLoad / 2;
    const int32_t startGridY = pTES->centerGridY - GridCellCoords::m_gridsToLoad / 2;

    for (int32_t i = 0; i < GridCellCoords::m_gridsToLoad; ++i)
    {
        for (int32_t j = 0; j < GridCellCoords::m_gridsToLoad; ++j)
        {
            // If it is a new cell grid, don't check for previously loaded cells.
            if (!aNewCellGrid)
            {
                if (GridCellCoords::IsCellInGridCell(m_centerGrid, {startGridX + i, startGridY + j}, false))
                    continue;
            }

            const TESObjectCELL* pCell = ModManager::Get()->GetCellFromCoordinates(startGridX + i, startGridY + j, aWorldSpace, 0);

            if (!pCell)
            {
                spdlog::warn("Cell not found at coordinates ({}, {}) in worldspace {:X}", startGridX + i, startGridY + j, aWorldSpace->formID);
                continue;
            }

            GameId cellId{};
            if (!m_world.GetModSystem().GetServerModId(pCell->formID, cellId))
            {
                spdlog::error("Failed to find cell id for form id {:X}", pCell->formID);
                continue;
            }

            changeEvent.Cells.push_back(cellId);
        }
    }

    TESObjectCELL* pCell = PlayerCharacter::Get()->GetParentCellEx();
    if (!pCell)
        pCell = ModManager::Get()->GetCellFromCoordinates(pTES->currentGridX, pTES->currentGridY, aWorldSpace, false);

    if (!pCell) return;
    if (!m_world.GetModSystem().GetServerModId(pCell->formID, changeEvent.PlayerCell))
    {
        spdlog::error("Failed to find cell id for form id {:X}", pCell->formID);
        return;
    }

    changeEvent.CenterCoords = m_centerGrid = {pTES->centerGridX, pTES->centerGridY};

    m_dispatcher.trigger(changeEvent);

    m_worldSpaceId = worldSpaceId;
}

void DiscoveryService::VisitForms() noexcept
{
    auto previousForms=m_forms;

    const auto visitor = [this,&previousForms](TESObjectREFR* apReference)
    {
        const auto formId = apReference->formID;
        m_followerRemovalDeadlines.erase(formId);

        if (!m_forms.count(formId))
        {
            m_forms.insert(formId);

            m_dispatcher.enqueue(ActorAddedEvent(formId));
        }
        else
            previousForms.erase(formId);
    };

    ProcessLists* const pProcessLists = ProcessLists::Get();
    if (!pProcessLists)
        return;

    for (uint32_t i = 0; i < pProcessLists->highActorHandleArray.length; ++i)
    {
        TESObjectREFR* const pRefr = TESObjectREFR::GetByHandle(pProcessLists->highActorHandleArray[i]);
        if (pRefr)
        {
            if (pRefr->GetNiNode())
            {
                visitor(pRefr);
            }
        }
    }

    // Not in actor holder
    if (auto* player=PlayerCharacter::Get()) visitor(player);

    constexpr auto cFollowerLoadGrace = std::chrono::milliseconds(1500);
    const auto cNow = std::chrono::steady_clock::now();
    const auto locallyOwnedActors = m_world.view<FormIdComponent, LocalComponent>();

    // We dispatch removal events first to prevent needless reallocations
    for (uint32_t formId : previousForms)
    {
        // A conform can remove both the 3D and the high-process handle. Keep the
        // existing discovery entry so rebuilding it does not cancel its assignment.
        // TODO: GetById performance in loop?
        if (auto* pActor = Cast<Actor>(TESForm::GetById(formId)); pActor && !pActor->IsDeleted())
        {
            using ReconciliationStage = ActorExtension::ReconciliationStage;
            const auto cStage = pActor->GetExtension()->Reconciliation;
            const auto* pCell = pActor->GetParentCellEx();
            // Finish the disable/enable pair even if the cell starts unloading.
            // Once enabled, an unloaded cell is a real removal.
            if (cStage == ReconciliationStage::WaitingForDisable || (cStage == ReconciliationStage::WaitingFor3D && pCell && pCell->IsAttached()))
            {
                continue;
            }

            // A follower can drop out of the loaded actor list as vanilla moves it
            // through a door. Keep the assignment while the same reference reloads.
            if (!pActor->GetExtension()->IsRemote() && !pActor->IsDead())
            {
                const auto owner = std::find_if(locallyOwnedActors.begin(), locallyOwnedActors.end(), [&locallyOwnedActors, formId](entt::entity aEntity) {
                    return locallyOwnedActors.get<FormIdComponent>(aEntity).Id == formId;
                });

                if (owner != locallyOwnedActors.end())
                {
                    const auto pending = m_followerRemovalDeadlines.find(formId);
                    if (pending != m_followerRemovalDeadlines.end())
                    {
                        if (cNow < pending->second)
                            continue;
                    }
                    else
                    {
                        // Skyrim.esm: CurrentFollowerFaction
                        GameId followerFaction{};
                        if (m_world.GetModSystem().GetServerModId(0x5C84E, followerFaction) && IsActiveFollower(*pActor, followerFaction))
                        {
                            m_followerRemovalDeadlines.emplace(formId, cNow + cFollowerLoadGrace);
                            spdlog::info("Holding ownership of follower {:X} while its 3D reloads", formId);
                            continue;
                        }
                    }
                }
            }
        }

        m_followerRemovalDeadlines.erase(formId);
        m_dispatcher.trigger(ActorRemovedEvent(formId));
        m_forms.erase(formId);
    }

    // Dispatch all adds
    m_dispatcher.update<ActorAddedEvent>();
}

void DiscoveryService::OnUpdate(const PreUpdateEvent& acUpdateEvent) noexcept
{
    TP_UNUSED(acUpdateEvent);
    const bool loaded=m_loadEventPending.exchange(false,std::memory_order_acq_rel);
#if (IS_MASTER)
    if (loaded && !IsDefaultModlist(ModManager::Get()->mods))
    {
        ConnectionErrorEvent errorEvent{};
        errorEvent.ErrorDetail = "{\"error\": \"non_default_install\"}";
        m_world.GetRunner().Trigger(errorEvent);
    }
#endif
    VisitCell(loaded);
    VisitForms();
}

void DiscoveryService::OnConnected(const ConnectedEvent& acEvent) noexcept
{
    m_followerRemovalDeadlines.clear();

    // uGridsToLoad should always be 5, as this is what the server enforces
    auto* pSetting = INISettingCollection::Get()->GetSetting("uGridsToLoad:General");
    if (pSetting && pSetting->data != 5)
    {
        ConnectionErrorEvent errorEvent{};
        errorEvent.ErrorDetail = "{\"error\": \"bad_uGridsToLoad\"}";

        m_world.GetRunner().Trigger(errorEvent);
    }

    VisitCell(true);
}

BSTEventResult DiscoveryService::OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*)
{
    // The engine may send this while loading is still in progress. It is a
    // discovery hint, NOT a safe time to delete old actors or mutate the world.
    m_loadEventPending.store(true,std::memory_order_release);
    return BSTEventResult::kOk;
}

void DiscoveryService::ResetWorldTracking() noexcept
{
    m_forms.clear();
    m_followerRemovalDeadlines.clear();
    ResetCachedCellData();
    m_interiorCellId=0;
    m_pLocation=nullptr;
    m_loadEventPending.store(true,std::memory_order_release);
}

void DiscoveryService::ResetCachedCellData() noexcept
{
    m_worldSpaceId = 0;
    m_centerGrid.Reset();
    m_currentGrid.Reset();
}
