#pragma once
#include <charconv>
#include <cstdint>
#include <istream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace QuestRunMember
{
// Keep the full instance history, not just the latest binding. Loading an
// earlier ESS must not let a previously consumed instance open another run.
using Members = std::map<uint64_t,std::map<uint32_t,uint32_t>>; // character -> instance -> generation
// Form keys contain hex digits and a dot, never '~'. The exact canonical
// target tuple distinguishes runs without hash collisions.
inline std::string RecordKey(const std::string& form,std::string_view targets)
{
    return targets.empty()?form:form+"~";
}
inline std::string ScopeKey(const std::string& form,std::string_view targets)
{
    return RecordKey(form,targets)+std::string(targets);
}
inline std::string BaseKey(std::string_view scope)
{
    return std::string(scope.substr(0,scope.find('~')));
}
inline bool ReadTail(std::istream& in,std::string& targets,std::optional<uint32_t>& instance)
{
    targets.clear();instance.reset();
    std::string token,extra;
    if (!(in>>token)) return true;
    if (!token.starts_with('@'))
    {
        targets=std::move(token);
        if (!(in>>token)) return true;
    }
    if (!token.starts_with('@')||token.size()<2||in>>extra) return false;
    uint32_t id{};
    const auto result=std::from_chars(token.data()+1,token.data()+token.size(),id);
    if (result.ec!=std::errc{}||result.ptr!=token.data()+token.size()||
        token!="@"+std::to_string(id)) return false;
    instance=id;
    return true;
}
inline std::optional<uint32_t> Instance(const Members& members,uint64_t character,uint32_t generation)
{
    const auto it=members.find(character);
    if (it!=members.end())
        for (const auto& [instance,run]:it->second)
            if (run==generation) return instance;
    return {};
}
inline bool Bound(const Members& members,uint64_t character,uint32_t generation,uint32_t instance)
{
    return Instance(members,character,generation)==instance;
}
inline bool Remember(Members& members,uint64_t character,uint32_t generation,uint32_t instance)
{
    if (!character||!generation) return false;
    if (const auto old=Instance(members,character,generation)) return *old==instance;
    auto& history=members[character];
    if (history.contains(instance)) return false;
    for (const auto& [id,run]:history) if (run>generation) return false;
    history.emplace(instance,generation);
    return true;
}
inline bool CanAttach(const Members& members,uint64_t character,uint32_t generation,uint32_t expected,
    uint32_t instance,bool started,bool stopped)
{
    if (!character||!generation||generation!=expected||!started||stopped) return false;
    const auto current=Instance(members,character,generation);
    if (current) return *current==instance;
    const auto it=members.find(character);
    if (it==members.end()) return true;
    if (it->second.contains(instance)) return false;
    for (const auto& [id,run]:it->second) if (run>=generation) return false;
    return true;
}
inline bool CanStartMissing(const Members& members,uint64_t character,uint32_t generation,uint32_t stoppedInstance)
{
    if (!character||!generation||Instance(members,character,generation)) return false;
    const auto it=members.find(character);
    if (it==members.end()) return true;
    const auto prior=it->second.find(stoppedInstance);
    return prior!=it->second.end()&&prior->second<generation;
}
inline bool Admit(const Members& members,uint64_t character,uint32_t generation,uint32_t instance,
    bool stopped,bool start,bool stage,bool legacyDifferentTargets,bool scoped=false)
{
    if (!character||!generation) return false;
    const auto it=members.find(character);
    const auto current=Instance(members,character,generation);
    const bool used=it!=members.end()&&it->second.contains(instance);
    if (stopped)
        return start&&!used&&(scoped||current.has_value()||(members.empty()&&legacyDifferentTargets));
    if (current) return *current==instance;
    if (used) return false;
    return start||(generation==1&&stage);
}
}
