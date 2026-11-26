#pragma once

namespace HUDMenuUtils
{
bool SceneDepth(const NiPoint3& point,float& depth);
bool ProjectForOverlay(const NiPoint3& point, NiPoint3& uv);
bool WorldPtToScreenPt3(const NiPoint3& aWorldPt, NiPoint3& aScreenPt);
}
