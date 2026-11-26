#pragma once

#include "TESForm.h"
#include <Components/TESCondition.h>

struct TESTopicInfo : TESForm
{
    TESForm* parentTopic; // TESTopic, CommonLibSSE-NG offset 0x20
    uint8_t padding[8];
    TESCondition conditions;
};
static_assert(offsetof(TESTopicInfo, conditions) == 0x30);
