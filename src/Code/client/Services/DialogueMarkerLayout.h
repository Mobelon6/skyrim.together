#pragma once

#include <algorithm>
#include <cmath>

// Inputs are screen pixels in the game's current viewport. No per-frame smoothing or history.
namespace DialogueMarkerLayout
{
struct Point { float x{}, y{}; };
struct Projection { Point p{}; bool front{}; };
struct Layout { Point center{}, direction{}; bool edge{}, npcLine{}, speakerLine{}; };

inline Layout Place(float width, float height, float scale, Projection anchor, Projection npc, Projection speaker)
{
    const auto visible = [=](Projection p) {
        return p.front && p.p.x >= 0.f && p.p.y >= 0.f && p.p.x <= width && p.p.y <= height;
    };
    Layout out{};
    out.npcLine = visible(npc);
    out.speakerLine = visible(speaker);
    Point mid{width * .5f, height * .5f};
    const float margin = std::min(40.f * scale, std::min(width, height) * .24f);
    if (out.npcLine || out.speakerLine || visible(anchor))
    {
        Point at = out.npcLine ? npc.p : out.speakerLine ? speaker.p : anchor.p;
        if (out.npcLine && out.speakerLine)
        {
            at.x = (npc.p.x + speaker.p.x) * .5f;
            at.y = std::min(npc.p.y, speaker.p.y);
        }
        out.center = {std::clamp(at.x, margin, width - margin),
                      std::clamp(at.y - 46.f * scale, margin, height - margin)};
        return out;
    }
    out.edge = true;
    float dx = anchor.p.x - mid.x, dy = anchor.p.y - mid.y;
    float distance = std::hypot(dx, dy);
    if (!std::isfinite(distance) || distance < .01f)
    {
        dx = 0.f;
        dy = distance = 1.f;
    }
    out.direction = {dx / distance, dy / distance};
    const float factor = std::max(std::abs(dx) / (mid.x - margin), std::abs(dy) / (mid.y - margin));
    out.center = {mid.x + dx / factor, mid.y + dy / factor};
    return out;
}
}
