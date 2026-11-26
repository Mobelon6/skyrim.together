#pragma once

struct PlayerDialogueEvent
{
    PlayerDialogueEvent(String aText, uint32_t aSpeakerHandle, uint32_t aInfo, uint32_t aQuest)
        : Text(std::move(aText)), SpeakerHandle(aSpeakerHandle), Info(aInfo), Quest(aQuest)
    {
    }

    String Text;
    uint32_t SpeakerHandle;
    uint32_t Info;
    uint32_t Quest;
};
