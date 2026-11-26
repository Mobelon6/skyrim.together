#pragma once
#include <CampaignJournal.h>
#include <TradeProperties.h>
#include <algorithm>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace QuestWordReward
{
struct Sample
{
    uint64_t Character{};
    uint32_t Cell{},Peer{};
    std::set<uint32_t> Known;
};
inline std::vector<uint32_t> NewWords(const Sample& before,const Sample& after)
{
    if (!before.Character||!before.Cell||!before.Peer||
        before.Character!=after.Character||before.Cell!=after.Cell||before.Peer!=after.Peer||
        !std::includes(after.Known.begin(),after.Known.end(),before.Known.begin(),before.Known.end())) return {};
    std::vector<uint32_t> result;
    for (auto id:after.Known)
        if (!before.Known.contains(id))
        {
            if (!id) return {};
            result.push_back(id);
        }
    return result;
}
inline constexpr size_t MaxBatch=16,MaxPayload=1024;
inline std::string EncodeBatch(const std::set<std::string>& words)
{
    std::string result;
    size_t count{};
    for (const auto& word:words)
    {
        const auto key=TradeProperties::DecodeForm(word);
        if (!key||!key->Valid()||key->Encode()!=word) return {};
        if (count==MaxBatch||result.size()+(count?1:0)+word.size()>MaxPayload) break;
        if (count++) result+=' ';
        result+=word;
    }
    return result;
}
inline std::optional<std::set<std::string>> DecodeBatch(std::string_view input)
{
    if (input.empty()||input.size()>MaxPayload) return {};
    std::istringstream in{std::string(input)};
    std::set<std::string> words;
    std::string word;
    while (in>>word)
    {
        const auto key=TradeProperties::DecodeForm(word);
        if (!key||!key->Valid()||key->Encode()!=word||!words.insert(word).second||words.size()>MaxBatch) return {};
    }
    if (EncodeBatch(words)!=input) return {};
    return words;
}
struct Grant
{
    std::string Word;
    uint64_t Source{},Recipient{};
    bool Valid() const
    {
        const auto key=TradeProperties::DecodeForm(Word);
        return key&&key->Valid()&&key->Encode()==Word&&Source&&Recipient&&Source!=Recipient;
    }
    std::string Encode() const {return Word+" "+std::to_string(Source)+" "+std::to_string(Recipient);}
    bool Entitles(uint64_t character) const {return character&&(Source==character||Recipient==character);}
};
inline std::optional<Grant> Decode(const std::string& payload)
{
    Grant grant;
    std::istringstream row(payload);
    std::string extra;
    if (!(row>>grant.Word>>grant.Source>>grant.Recipient)||row>>extra||!grant.Valid()||grant.Encode()!=payload) return {};
    return grant;
}
inline std::optional<std::vector<Grant>> Read(const std::vector<CampaignJournal::Operation>& operations)
{
    std::vector<Grant> grants;
    for (const auto& op:operations)
        if (op.Kind=="quest-word-v1")
        {
            const auto grant=Decode(op.Payload);
            if (!grant) return {};
            for (const auto& old:grants)
                if (old.Word==grant->Word&&old.Entitles(grant->Source)) return {};
            grants.push_back(*grant);
        }
    return grants;
}
inline bool Entitled(const std::vector<Grant>& grants,const std::string& word,uint64_t character)
{
    return std::any_of(grants.begin(),grants.end(),[&](const auto& grant){
        return grant.Word==word&&grant.Entitles(character);
    });
}
}
