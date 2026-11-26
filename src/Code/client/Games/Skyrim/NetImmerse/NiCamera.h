#pragma once

#include <NetImmerse/NiAVObject.h>

struct NiNode;

template <typename T> struct NiRect
{
    T left;
    T right;
    T top;
    T bottom;
};

struct NiCamera : public NiAVObject
{
    virtual ~NiCamera() = default;

    bool WorldPtToScreenPt3(const NiPoint3& in, NiPoint3& out, float zeroTolerance = 1e-5f);

    static bool WorldPtToScreenPt3(float* matrix, const NiRect<float>* port, const NiPoint3* p_in, float* x_out, float* y_out, float* z_out, float zeroTolerance = 1e-5f);

    float worldToCam[4][4]; // NiCamera SE/AE
    uint8_t projectionData[0x24];
    NiRect<float> port;

};

static_assert(offsetof(NiCamera, worldToCam) == 0x110);
static_assert(offsetof(NiCamera, port) == 0x174);
