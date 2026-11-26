#pragma once
#include <HomeProjectionPlan.h>
struct TESObjectCELL;
namespace HomeProjection
{
// Materialize into separate, script-free projection references only.
// The caller must keep the shell inaccessible until this returns true.
// No original home, player inventory, or quest alias is written.
bool Apply(TESObjectCELL&,const HomeSnapshot::State&,const std::vector<Binding>&);
}
