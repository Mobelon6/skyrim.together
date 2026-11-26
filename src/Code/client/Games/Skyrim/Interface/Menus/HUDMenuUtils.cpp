
#include <Games/Skyrim/Interface/Menus/HUDMenuUtils.h>
#include <Games/Skyrim/NetImmerse/NiCamera.h>
#include <Camera/PlayerCamera.h>

namespace HUDMenuUtils
{
static float (*CameraWorldToCam)[4][4] = nullptr;
static const NiRect<float>* CameraPort = nullptr;

bool WorldPtToScreenPt3(const NiPoint3& aWorldPt, NiPoint3& aScreenPt)
{
    return NiCamera::WorldPtToScreenPt3(reinterpret_cast<float*>(CameraWorldToCam), CameraPort, &aWorldPt, &aScreenPt.x, &aScreenPt.y, &aScreenPt.z, 1e-5f);
}
bool SceneDepth(const NiPoint3& p,float& depth)
{
    if (!CameraWorldToCam) return false;
    const auto& m=*CameraWorldToCam;
    const auto component=[&](int i){return m[i][0]*p.x+m[i][1]*p.y+m[i][2]*p.z+m[i][3];};
    const float w=component(3);
    if (!(w>1e-5f)) return false;
    depth=component(2)/w;
    return std::isfinite(depth) && depth>=0.f && depth<=1.f;
}
bool ProjectForOverlay(const NiPoint3& point, NiPoint3& uv)
{
    // The engine publishes the projection used for the HUD separately from the player camera.
    return CameraWorldToCam && CameraPort && WorldPtToScreenPt3(point, uv);
}
} // namespace HUDMenuUtils

static TiltedPhoques::Initializer s_Init(
    []()
    {
        POINTER_SKYRIMSE(float[4][4], s_matrix, 406126);
        POINTER_SKYRIMSE(NiRect<float>, s_port, 406160);

        HUDMenuUtils::CameraWorldToCam = s_matrix.Get();
        HUDMenuUtils::CameraPort = s_port.Get();
    });
