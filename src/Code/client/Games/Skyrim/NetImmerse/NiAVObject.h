#pragma once

#include <NetImmerse/NiObjectNET.h>
#include <Games/Primitives.h>

struct BSFixedString;

struct NiAVObject : NiObjectNET
{
    virtual ~NiAVObject();

    virtual void sub_26();
    virtual void sub_27();
    virtual void sub_28();
    virtual void sub_29();
    virtual NiAVObject* GetByName(BSFixedString& aName);

    // Skyrim SE/AE NiAVObject::world (0x7C), NiTransform::translate (+0x24).
    uint8_t pad30[0xA0 - 0x30];
    NiPoint3 worldPosition;
    uint8_t padAC[0x110 - 0xAC];
};

static_assert(offsetof(NiAVObject, worldPosition) == 0xA0);
static_assert(sizeof(NiAVObject) == 0x110);
