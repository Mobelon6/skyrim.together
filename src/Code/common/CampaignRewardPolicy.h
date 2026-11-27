#pragma once

// Native-first acceptance baseline. Skyrim owns the individual word/soul
// state; normal STR quest and death replication remain in charge.
// Keep the former supplement implementation dormant for comparison until
// two-client tests demonstrate a concrete missing effect. Existing journal
// rows and ESS fields must remain readable; do not activate old grants.
namespace CampaignRewardPolicy
{
inline constexpr bool SupplementWords = false;
inline constexpr bool SupplementDragonSouls = false;
}
