#include <Components.h>
#include <Messages/RequestActorValueChanges.h>
#include <Messages/RequestActorMaxValueChanges.h>
#include <Messages/RequestHealthChangeBroadcast.h>
#include <Messages/RequestDeathStateChange.h>
#include <Messages/RequestNpcLifeCheck.h>
#include <Messages/NotifyNpcLifeCheck.h>
#include <Game/Player.h>
#include <Services/ActorValueService.h>
#include <cmath>
#include <World.h>
#include <GameServer.h>
#include <Messages/NotifyActorValueChanges.h>
#include <Messages/NotifyActorMaxValueChanges.h>
#include <Messages/NotifyHealthChangeBroadcast.h>
#include <Messages/NotifyDeathStateChange.h>
void RecordCampaignDragonLife(World&,Player&,entt::entity,bool) noexcept;


ActorValueService::ActorValueService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_updateHealthConnection = aDispatcher.sink<PacketEvent<RequestActorValueChanges>>().connect<&ActorValueService::OnActorValueChanges>(this);
    m_updateMaxValueConnection = aDispatcher.sink<PacketEvent<RequestActorMaxValueChanges>>().connect<&ActorValueService::OnActorMaxValueChanges>(this);
    m_updateDeltaHealthConnection = aDispatcher.sink<PacketEvent<RequestHealthChangeBroadcast>>().connect<&ActorValueService::OnHealthChangeBroadcast>(this);
    m_lifeCheck = aDispatcher.sink<PacketEvent<RequestNpcLifeCheck>>().connect<&ActorValueService::OnLifeCheck>(this);
    m_deathStateConnection = aDispatcher.sink<PacketEvent<RequestDeathStateChange>>().connect<&ActorValueService::OnDeathStateChange>(this);
}

void ActorValueService::OnActorValueChanges(const PacketEvent<RequestActorValueChanges>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    auto actorValuesView = m_world.view<ActorValuesComponent, OwnerComponent>();

    auto it = actorValuesView.find(static_cast<entt::entity>(message.Id));

    if (it == actorValuesView.end() || !actorValuesView.get<OwnerComponent>(*it).IsCurrentOwner(acMessage.pPlayer, message.OwnershipEpoch))
        return;

    auto& actorValuesComponent = actorValuesView.get<ActorValuesComponent>(*it);
    for (auto& [id, value] : message.Values)
    {
        actorValuesComponent.CurrentActorValues.ActorValuesList[id] = value;
    }

    NotifyActorValueChanges notify;
    notify.OwnershipEpoch = message.OwnershipEpoch;
    notify.Id = acMessage.Packet.Id;
    notify.Values = acMessage.Packet.Values;

    const entt::entity cEntity = static_cast<entt::entity>(message.Id);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void ActorValueService::OnActorMaxValueChanges(const PacketEvent<RequestActorMaxValueChanges>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    auto actorValuesView = m_world.view<ActorValuesComponent, OwnerComponent>();

    auto it = actorValuesView.find(static_cast<entt::entity>(message.Id));

    if (it == actorValuesView.end() || !actorValuesView.get<OwnerComponent>(*it).IsCurrentOwner(acMessage.pPlayer, message.OwnershipEpoch))
        return;

    auto& actorValuesComponent = actorValuesView.get<ActorValuesComponent>(*it);
    for (auto& [id, value] : message.Values)
    {
        actorValuesComponent.CurrentActorValues.ActorMaxValuesList[id] = value;
    }

    NotifyActorMaxValueChanges notify;
    notify.OwnershipEpoch = message.OwnershipEpoch;
    notify.Id = message.Id;
    notify.Values = message.Values;

    const entt::entity cEntity = static_cast<entt::entity>(message.Id);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void ActorValueService::OnHealthChangeBroadcast(const PacketEvent<RequestHealthChangeBroadcast>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    auto actorValuesView = m_world.view<ActorValuesComponent, OwnerComponent>();
    const auto it = actorValuesView.find(static_cast<entt::entity>(message.Id));
    if (it == actorValuesView.end() || message.OwnershipEpoch == 0 || !std::isfinite(message.DeltaHealth))
        return;
    const auto& owner = actorValuesView.get<OwnerComponent>(*it);
    if (!owner.GetOwner() || owner.OwnershipEpoch != message.OwnershipEpoch)
        return;

    auto& values = actorValuesView.get<ActorValuesComponent>(*it);
    float& health = values.CurrentActorValues.ActorValuesList[24];
    if (message.KeepAlive && health <= 0.f)
        return;
    const float next = health + message.DeltaHealth;
    health = message.KeepAlive ? std::max(std::min(1.f, health), next) : next;
    NotifyHealthChangeBroadcast notify;
    notify.Id = message.Id;
    notify.OwnershipEpoch = message.OwnershipEpoch;
    notify.DeltaHealth = message.DeltaHealth;
    notify.KeepAlive = message.KeepAlive;

    const entt::entity cEntity = static_cast<entt::entity>(message.Id);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void ActorValueService::OnDeathStateChange(const PacketEvent<RequestDeathStateChange>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    auto characterView = m_world.view<CharacterComponent, OwnerComponent>();

    const auto it = characterView.find(static_cast<entt::entity>(message.Id));

    if (it == characterView.end() || !characterView.get<OwnerComponent>(*it).IsCurrentOwner(acMessage.pPlayer, message.OwnershipEpoch))
        return;

    auto& characterComponent = characterView.get<CharacterComponent>(*it);
    const bool lifeChanged=characterComponent.IsDead()!=message.IsDead;
    characterComponent.SetDead(message.IsDead);
    if (lifeChanged&&characterComponent.IsDragon())
        RecordCampaignDragonLife(m_world,*acMessage.pPlayer,*it,message.IsDead);
    spdlog::debug("Updating death state {:x}:{}", message.Id, message.IsDead);

    NotifyDeathStateChange notify;
    notify.OwnershipEpoch = message.OwnershipEpoch;
    notify.Id = message.Id;
    notify.IsDead = message.IsDead;

    const entt::entity cEntity = static_cast<entt::entity>(message.Id);
    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.pPlayer))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void ActorValueService::OnLifeCheck(const PacketEvent<RequestNpcLifeCheck>& event) const noexcept
{
    const auto& packet = event.Packet;
    const auto entity = static_cast<entt::entity>(packet.Id);
    if (!m_world.valid(entity))
        return;
    const auto* owner = m_world.try_get<OwnerComponent>(entity);
    const auto* cell = m_world.try_get<CellIdComponent>(entity);
    if (!owner || !cell || !packet.OwnershipEpoch || owner->OwnershipEpoch != packet.OwnershipEpoch ||
        !owner->GetOwner() || owner->GetOwner() == event.pPlayer || !cell->IsInRange(event.pPlayer->GetCellComponent(), false))
        return;
    NotifyNpcLifeCheck query;
    query.Id = packet.Id;
    query.OwnershipEpoch = packet.OwnershipEpoch;
    owner->GetOwner()->Send(query);
}
