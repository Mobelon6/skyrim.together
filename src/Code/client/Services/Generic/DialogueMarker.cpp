#include <TiltedOnlinePCH.h>
#include <Services/OverlayService.h>
#include <Services/DialogueMarkerLayout.h>
#include <Interface/Menus/HUDMenuUtils.h>
#include <imgui.h>
#include <Services/MarkerDepth.h>
#include <Interface/UI.h>

void OverlayService::DrawDialogueMarker() noexcept
{
    if (ObservingDialogue()) return;
    DialogueMarker frame;
    {
        std::lock_guard guard(m_markerMutex);
        if (!m_marker.Active)
            return;
        frame = m_marker;
    }
    const auto* ui = UI::Get();
    if (!ui || ui->BlocksPartyHud())
        return;
    const auto screen = ImGui::GetIO().DisplaySize;
    if (screen.x < 4.f || screen.y < 4.f)
        return;
    using namespace DialogueMarkerLayout;
    const auto project = [screen, &frame](glm::vec3 position, bool mirrorBehind = false) -> Projection {
        NiPoint3 uv{};
        bool front = HUDMenuUtils::ProjectForOverlay(position, uv) && uv.z >= 0.f;
        if (!front)
        {
            if (!mirrorBehind)
                return {};
            if (HUDMenuUtils::ProjectForOverlay(frame.Eye + (frame.Eye - position), uv))
            {
                uv.x = 1.f - uv.x;
                uv.y = 1.f - uv.y;
            }
            else
                return {{screen.x * .5f, screen.y}, false};
        }
        if (!std::isfinite(uv.x) || !std::isfinite(uv.y))
            return {{screen.x * .5f, screen.y}, false};
        return {{uv.x * screen.x, (1.f - uv.y) * screen.y}, front};
    };
    const float scale = std::clamp(screen.y / 720.f, .75f, 3.f);
    const Projection anchor = project(frame.Anchor, true);
    const Projection npc = frame.HasNpc ? project(frame.Npc) : Projection{};
    const Projection speaker = frame.HasSpeaker ? project(frame.Speaker) : Projection{};
    const Layout layout = Place(screen.x, screen.y, scale, anchor, npc, speaker);
    const ImVec2 center{layout.center.x, layout.center.y};
    const auto at = [center, scale](float x, float y) { return ImVec2(center.x + x * scale, center.y + y * scale); };
    auto* draw = ImGui::GetBackgroundDrawList();
    constexpr ImU32 ink = IM_COL32(19, 20, 23, 238), light = IM_COL32(249, 248, 239, 255);
    const auto line = [&](Point tip) {
        const ImVec2 end{tip.x, tip.y};
        draw->AddLine(center, end, ink, 3.1f * scale);
        draw->AddLine(center, end, light, 1.15f * scale);
    };
    if (layout.npcLine || layout.speakerLine)
    {
        // Use the same camera matrix as the pixel anchors. Put the leaders behind both actors.
        bool drawn = false;
        float a{}, b{}, farther{};
        const auto point = layout.npcLine ? frame.Npc : frame.Speaker;
        if (HUDMenuUtils::SceneDepth(point, a) &&
            HUDMenuUtils::SceneDepth(point + (point - frame.Eye), farther) && std::abs(farther-a)>1e-8f)
        {
            const bool reversed = farther < a;
            float depth = a;
            if (layout.speakerLine && HUDMenuUtils::SceneDepth(frame.Speaker, b))
                depth = reversed ? std::min(depth,b) : std::max(depth,b);
            if (MarkerDepth::Begin(draw,screen.x,screen.y,depth,reversed))
            {
                if (layout.npcLine) line(npc.p);
                if (layout.speakerLine) line(speaker.p);
                MarkerDepth::End(draw);
                drawn = true;
            }
        }
        // Never lose the leaders solely because this renderer has no sampleable depth.
        if (!drawn)
        {
            if (layout.npcLine) line(npc.p);
            if (layout.speakerLine) line(speaker.p);
        }
    }
    // A rounded speech balloon with a cut-out mouth and three dots.
    draw->AddTriangleFilled(at(-10, 10), at(-10, 22), at(4, 11), ink);
    draw->AddRectFilled(at(-22,-16), at(22,15), ink, 11.f * scale);
    draw->AddRectFilled(at(-19.5f,-13.5f), at(19.5f,12.5f), light, 9.f * scale);
    draw->AddTriangleFilled(at(-8, 10), at(-8, 17.5f), at(2, 10), light);
    for (int i = -1; i <= 1; ++i)
        draw->AddCircleFilled(at(8.f * i, -.5f), 2.4f * scale, ink);
    if (layout.edge)
    {
        const auto arrow = [&](float forward, float side) {
            return at(layout.direction.x * forward - layout.direction.y * side,
                      layout.direction.y * forward + layout.direction.x * side);
        };
        draw->AddTriangleFilled(arrow(38,0), arrow(23,-9), arrow(23,9), ink);
        draw->AddTriangleFilled(arrow(35,0), arrow(25,-6), arrow(25,6), light);
    }
}
