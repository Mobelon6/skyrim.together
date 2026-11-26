#pragma once
#include "HomeCatalog.h"
#include "HomeResidents.h"

namespace HomeResidentPlan
{
// Static coverage remains mandatory. Additional occupants require a stable
// profile binding and a committed observation; unknown ACHR remain unsupported.
inline std::optional<HomeResidents::Residents> Build(const HomeCatalog::Cell& cell,
    const HomeSnapshot::State& baseline,const HomeResidents::Residents& records)
{
    if (!HomeCatalog::Bind(cell,baseline) || records.size()<cell.ResidentBases.size() ||
        records.size()>HomeResidents::MaxResidents) return {};
    for (const auto& [key,_]:cell.ResidentBases) if (!records.count(key)) return {};
    for (const auto& [key,record]:records)
    {
        const auto* base=HomeCatalog::ResidentBase(cell,key);
        if (!base || !record.Revision || !record.Value.Valid() ||
            record.Value.Reference.Encode()!=key || record.Value.Base.Encode()!=base->Encode()) return {};
    }
    return records;
}
}
