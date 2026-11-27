#include <TiltedOnlinePCH.h>
#include <World.h>
#include <OverlayApp.hpp>
#include <Services/OverlayClient.h>
#include <Services/OverlayService.h>
#include <PlayerCharacter.h>
#include <Interface/UI.h>

void OverlayService::SetPeerRange(uint32_t peer, bool withinRange) noexcept
{
    if (!m_pOverlay) return;
    auto args = CefListValue::Create();
    args->SetDouble(0, peer);
    args->SetBool(1, withinRange);
    m_pOverlay->ExecuteAsync("peerRange", args);
}

void OverlayService::QueueProgress(bool quest, uint32_t form, uint32_t speaker, uint32_t session, const char* label, std::function<void()> apply,
                                   std::function<bool()> needed, uint32_t subject, uint32_t encounter) noexcept
{
    if (!m_world.GetPartyService().IsInParty()) return;
    ProgressKey key = subject && encounter ? ProgressKey{false, subject, speaker, encounter} :
                                            ProgressKey{quest, form, quest ? 0u : speaker, session};
    auto it = std::find_if(m_progress.begin(), m_progress.end(), [&](const auto& item){ return item.Key == key; });
    const bool different = !needed || needed();
    // A later stop may become effective only after an earlier, still-pending start.
    if (!different && it == m_progress.end()) return;
    if (quest) m_currentQuest.insert_or_assign(form, key);
    if (auto answer = m_progressAnswers.find(key); answer != m_progressAnswers.end())
    {
        if (answer->second && different) apply();
        return;
    }
    if (it == m_progress.end())
    {
        if (++m_progressToken == 0) ++m_progressToken;
        m_progress.push_back({key, m_progressToken, label ? label : "", {}});
        it = std::prev(m_progress.end());
    }
    it->Changes.push_back({std::move(apply), std::move(needed)});
    it->Ready = std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
}

void OverlayService::QueueRewardChoice(uint32_t form,const char* label,std::string self,std::string peer,
                                      std::function<void(bool)> choice,std::function<bool()> needed) noexcept
{
    if (!m_world.GetPartyService().IsInParty()||!choice||self.empty()||peer.empty()||(needed&&!needed())) return;
    for (const auto& item:m_progress)
        if (item.RewardChoice&&std::get<1>(item.Key)==form) return;
    if (++m_progressToken==0) ++m_progressToken;
    PendingProgress item{};
    item.Key={true,form,0,0};item.Token=m_progressToken;item.Label=label?label:"";
    item.SelfName=std::move(self);item.PeerName=std::move(peer);
    item.RewardChoice=std::move(choice);item.Changes.push_back({{},std::move(needed)});
    m_progress.push_back(std::move(item));
}

bool OverlayService::IsPersonalQuest(uint32_t form) const noexcept
{
    const auto current = m_currentQuest.find(form);
    if (current == m_currentQuest.end()) return false;
    const auto answer = m_progressAnswers.find(current->second);
    return answer == m_progressAnswers.end() || !answer->second;
}

void OverlayService::AnswerProgress(uint32_t token, bool accept) noexcept
{
    if (m_progress.empty() || m_progress.front().Token != token || !m_showingProgress) return;
    auto item = std::move(m_progress.front());
    m_progress.pop_front();
    m_progressAnswers[item.Key] = accept;
    m_showingProgress = false;
    m_pOverlay->ExecuteAsync("hideProgress");
    m_gameOverlayClient->SetUIVisible(false);
    if (item.RewardChoice)
    {
        m_progressAnswers.erase(item.Key);
        if (m_world.GetPartyService().IsInParty()&&(!item.Changes.front().Needed||item.Changes.front().Needed()))
            item.RewardChoice(accept);
        return;
    }
    if (accept && m_world.GetPartyService().IsInParty())
        for (auto& change : item.Changes)
            if (!change.Needed || change.Needed()) change.Apply();
}

void OverlayService::ClearProgress() noexcept
{
    m_progress.clear();
    m_progressAnswers.clear();
    m_currentQuest.clear();
    m_peaceSession.clear();
    m_favorSettlements.clear();
    if (m_showingProgress && m_pOverlay && m_gameOverlayClient)
    {
        m_pOverlay->ExecuteAsync("hideProgress");
        m_gameOverlayClient->SetUIVisible(false);
    }
    m_showingProgress = false;
}

void OverlayService::UpdateProgress() noexcept
{
    // A result stays queued across the reward dialogue, loading screens and a carriage invitation.
    if (m_progress.empty() || m_showingProgress || m_offer || m_active || m_observerCamera || m_requestedTalk ||
        !m_gameOverlayClient || !m_pOverlay || !m_inGame || !UI::Get() || UI::Get()->BlocksPartyHud()) return;
    auto* player = PlayerCharacter::Get();
    if (!player || player->IsDead()) return;
    if (player->IsInCombat())
    {
        auto* threat = player->GetCombatTarget();
        if (threat && (!m_peaceSession.count(threat->formID) || threat->IsInCombat())) return;
    }
    auto& item = m_progress.front();
    if (std::chrono::steady_clock::now() < item.Ready) return;
    if (std::none_of(item.Changes.begin(), item.Changes.end(), [](auto& change) { return !change.Needed || change.Needed(); }))
    {
        // No answer was given. A later real change in this encounter can still be offered.
        std::erase_if(m_currentQuest, [&](const auto& entry) { return entry.second == item.Key; });
        m_progress.pop_front();
        return;
    }
    auto args = CefListValue::Create();
    args->SetInt(0, item.Token);
    args->SetString(1, item.Label);
    if (item.RewardChoice)
    {
        args->SetString(2,item.SelfName);
        args->SetString(3,item.PeerName);
    }
    m_showingProgress = true;
    m_pOverlay->ExecuteAsync("offerProgress", args);
    m_gameOverlayClient->SetUIVisible(true);
}

void OverlayService::ResetSessionProgress() noexcept
{
    ClearInteraction();
    ClearCarriage();
    m_conversations.clear();
    ClearDialogueHud();
}

void OverlayService::ShowRescueReturn(bool visible) noexcept
{
    if (!m_pOverlay || !m_gameOverlayClient) return;
    auto args = CefListValue::Create();
    args->SetBool(0,visible);
    m_pOverlay->ExecuteAsync("rescueReturn",args);
    if (visible || GetActive()) m_gameOverlayClient->SetUIVisible(visible);
}
