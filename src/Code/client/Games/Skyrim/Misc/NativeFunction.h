#pragma once

#include <Misc/BSFixedString.h>

struct NativeFunction
{
    virtual ~NativeFunction() = 0;

    void* pad8;
    BSFixedString functionName;
    BSFixedString typeName;
    uint8_t pad20[0x50 - 0x20];
    void* functionAddress;
    uint8_t usesLongSignature; // Bethesda native callback ABI
};

static_assert(offsetof(NativeFunction, functionName) == 0x10);
static_assert(offsetof(NativeFunction, typeName) == 0x18);
static_assert(offsetof(NativeFunction, functionAddress) == 0x50);
static_assert(offsetof(NativeFunction, usesLongSignature) == 0x58);
