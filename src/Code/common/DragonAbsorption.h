#pragma once
#include <CampaignJournal.h>
#include <TradeProperties.h>
#include <optional>
#include <cmath>
#include <map>
#include <vector>
#include <set>
#include <sstream>
#include <string>

namespace DragonAbsorption
{
// This is a receipt for a witnessed native award, NOT permission to award
// another soul. Death, a change of balance or a dragon skeleton alone cannot
// create this receipt. Static reference identity is deliberately conservative
// until repeated/dynamic dragon lives can be distinguished durably.
struct Receipt
{
    std::string Dragon;
    uint64_t Character{};
    bool Valid() const
    {
        const auto key=TradeProperties::DecodeForm(Dragon);
        return key&&key->Valid()&&key->Encode()==Dragon&&Character;
    }
    std::string Encode() const {return Dragon+" "+std::to_string(Character);}
};
inline std::optional<Receipt> Decode(const std::string& payload)
{
    Receipt receipt;
    std::istringstream row(payload);
    std::string extra;
    if (!(row>>receipt.Dragon>>receipt.Character)||row>>extra||
        !receipt.Valid()||receipt.Encode()!=payload) return {};
    return receipt;
}
inline std::optional<std::set<std::string>> Read(const std::vector<CampaignJournal::Operation>& operations)
{
    std::set<std::string> receipts;
    for (const auto& op:operations)
        if (op.Kind=="dragon-absorbed-v1")
        {
            if (!Decode(op.Payload)||!receipts.insert(op.Payload).second) return {};
        }
    return receipts;
}

inline constexpr uint32_t ReceiptBase=0x813, ReceiptWords=32, BitsPerWord=24;
inline constexpr uint32_t MaxGrants=ReceiptWords*BitsPerWord;
inline std::optional<uint32_t> Mask(float value)
{
    if (!std::isfinite(value)||value<0.f||value>16777215.f) return {};
    const auto mask=static_cast<uint32_t>(value);
    return static_cast<float>(mask)==value?std::optional<uint32_t>(mask):std::nullopt;
}
struct Kill
{
    std::string Dragon;
    uint64_t First{},Second{};
    uint32_t Revision{};
    bool Valid() const
    {
        const auto key=TradeProperties::DecodeForm(Dragon);
        return key&&key->Valid()&&key->Encode()==Dragon&&First&&First<Second;
    }
    std::string Encode() const {return Dragon+" "+std::to_string(First)+" "+std::to_string(Second);}
    uint64_t Other(uint64_t c) const {return c==First?Second:c==Second?First:0;}
};
inline std::optional<Kill> DecodeKill(const std::string& payload)
{
    Kill kill;
    std::istringstream row(payload);std::string extra;
    if (!(row>>kill.Dragon>>kill.First>>kill.Second)||row>>extra||!kill.Valid()||kill.Encode()!=payload) return {};
    return kill;
}
struct Grant
{
    std::string Dragon;
    uint64_t Source{},Recipient{};
    std::string Encode() const {return Dragon+" "+std::to_string(Source)+" "+std::to_string(Recipient);}
};
struct SharedHistory
{
    std::map<std::string,Kill> Kills;
    std::set<std::string> Retired;
    std::vector<Grant> Grants;
    bool Eligible(const Receipt& receipt) const
    {
        if (!receipt.Valid()||Retired.contains(receipt.Dragon)) return false;
        const auto kill=Kills.find(receipt.Dragon);
        return kill!=Kills.end()&&kill->second.Other(receipt.Character)!=0;
    }
    std::optional<Grant> Plan(const Receipt& receipt) const
    {
        if (!Eligible(receipt)||Grants.size()>=MaxGrants) return {};
        const auto kill=Kills.find(receipt.Dragon);
        if (kill==Kills.end()) return {};
        const auto other=kill->second.Other(receipt.Character);
        if (!other) return {};
        for (const auto& grant:Grants) if (grant.Dragon==receipt.Dragon) return {};
        return Grant{receipt.Dragon,receipt.Character,other};
    }
};
inline std::optional<SharedHistory> ReadShared(const std::vector<CampaignJournal::Operation>& operations)
{
    SharedHistory history;
    std::set<std::string> native;
    uint32_t revision{};
    for (const auto& op:operations)
    {
        ++revision;
        if (op.Kind=="dragon-death-v1")
        {
            auto kill=DecodeKill(op.Payload);
            if (!kill||history.Retired.contains(kill->Dragon)||history.Kills.contains(kill->Dragon)) return {};
            kill->Revision=revision;history.Kills.emplace(kill->Dragon,*kill);
        }
        else if (op.Kind=="dragon-retired-v1")
        {
            const auto key=TradeProperties::DecodeForm(op.Payload);
            if (!key||!key->Valid()||key->Encode()!=op.Payload||!history.Retired.insert(op.Payload).second) return {};
        }
        else if (op.Kind=="dragon-absorbed-v1")
        {
            if (!Decode(op.Payload)||!native.insert(op.Payload).second) return {};
        }
        else if (op.Kind=="dragon-shared-v1")
        {
            Grant grant;std::string extra;std::istringstream row(op.Payload);
            if (!(row>>grant.Dragon>>grant.Source>>grant.Recipient)||row>>extra||grant.Encode()!=op.Payload) return {};
            const Receipt receipt{grant.Dragon,grant.Source};
            const auto plan=history.Plan(receipt);
            if (!plan||plan->Encode()!=op.Payload||!native.contains(receipt.Encode())) return {};
            history.Grants.push_back(std::move(grant));
        }
    }
    return history;
}
inline std::vector<Grant> MissingGrants(SharedHistory history,const std::vector<CampaignJournal::Operation>& operations)
{
    std::vector<Grant> result;
    // Original native receipt order decides the one supplemental entitlement.
    for (const auto& op:operations)
        if (op.Kind=="dragon-absorbed-v1")
            if (const auto receipt=Decode(op.Payload))
                if (const auto grant=history.Plan(*receipt))
                {
                    result.push_back(*grant);
                    history.Grants.push_back(*grant);
                }
    return result;
}
}
