#pragma once
struct ImDrawList;
namespace MarkerDepth
{
bool Begin(ImDrawList*, float width, float height, float depth, bool reversed);
void End(ImDrawList*);
}
