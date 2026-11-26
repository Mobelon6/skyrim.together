#pragma once

struct TESObjectREFR;
struct TESForm;
struct TESTopicInfo;

struct MenuTopicManager
{
    static MenuTopicManager* Get() noexcept;
    static bool IsPlayerDialogueSpeaker(const TESObjectREFR* apSpeaker) noexcept;

    bool IsDialogueOpen() const noexcept;
    bool IsCurrentSpeaker(const BSPointerHandle<TESObjectREFR>& acHandle) const noexcept
    {
        return IsDialogueOpen() && acHandle && speaker.handle.iBits == acHandle.handle.iBits;
    }

    struct DialogueOption
    {
        const char* text;
        uint8_t padding[0x28 - sizeof(text)];
        TESForm* quest; // MenuTopicManager::Dialogue::parentQuest
        TESTopicInfo* info; // CommonLibSSE MenuTopicManager::Dialogue::parentTopicInfo

    };

    static_assert(offsetof(DialogueOption, info) == 0x30);
    bool OffersHire() const noexcept;
    bool ResumeHire(int32_t index, uint32_t target, uint32_t info) noexcept;
    bool ResumeQuestTurnIn(uint32_t target,uint32_t info,uint32_t quest,uint32_t base) noexcept;

    uint8_t pad0[0x20];
    GameList<DialogueOption>* pOptions;

    uint8_t pad28[0x68 - 0x28];
    BSPointerHandle<TESObjectREFR> speaker;
    uint8_t pad6C[0xB1 - 0x6C];
    bool menuOpen;
};

static_assert(offsetof(MenuTopicManager, pOptions) == 0x20);
static_assert(offsetof(MenuTopicManager, speaker) == 0x68);
static_assert(offsetof(MenuTopicManager, menuOpen) == 0xB1);
