#include <TiltedOnlinePCH.h>
#include <World.h>
#include <Services/QuestService.h>
#include <Services/PartyService.h>
#include <Services/TransportService.h>
#include <Messages/RequestQuestJournal.h>
#include <Messages/NotifyQuestJournal.h>
#include <Events/UpdateEvent.h>
#include <QuestJournalDisk.h>
#include <CampaignReplication.h>
#include <CampaignIdentity.h>
#include <Services/CampaignAccess.h>
#include <Services/CampaignCheckpointClient.h>
#include <QuestWordReward.h>
#include <CampaignRewardPolicy.h>
#include <UniqueQuestReward.h>
#include <KeywordQuestReward.h>
#include <ScriptedTravel.h>
#include <QuestPhysicalStage.h>
#include <Services/QuestPhysicalStageClient.h>
#include <Services/ScriptedTravelClient.h>
#include <AI/Movement/PlayerControls.h>
#include <Services/QuestRewardClient.h>
#include <Components/BGSKeywordForm.h>
#include <Forms/BGSKeyword.h>
#include <array>
#include <Components.h>
#include <QuestPickupPermit.h>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/RequestInventorySnapshot.h>
#include <Messages/RequestQuestUpdate.h>
#include <Games/Misc/MenuTopicManager.h>
#include <QuestRunMember.h>
#include <tuple>
#include <Events/ActivateEvent.h>
#include <DragonAbsorption.h>
#include <Utils.h>
#include <atomic>
#include <Services/HomeInstance.h>
#include <InventoryEvidence.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <algorithm>
#include <mutex>
#include <map>
#include <Forms/TESQuest.h>
#include <Services/OverlayService.h>
#include <TradeProperties.h>
#include <set>
#include <sstream>
#include <Events/InventoryChangeEvent.h>
#include <Games/Overrides.h>
#include <ExtraData/ExtraCount.h>
#include <ExtraData/ExtraContainerChanges.h>
#include <Forms/TESBoundObject.h>
#include <Misc/BSScript.h>
#include <Misc/GameVM.h>
#include <HomeRegistry.h>

bool TeachCampaignWord(TESForm*) noexcept;
bool DragonAwardHookReady() noexcept;
void ResetCampaignDragonAwards() noexcept;
void RefreshCampaignDragonAwards() noexcept;
void RememberCampaignDragonAward(uint32_t) noexcept;
int ApplyCampaignDragonAward(uint32_t) noexcept;
uint32_t NativeUniqueRewardQuest() noexcept;
uint16_t NativeUniqueRewardStage() noexcept;
uint32_t NativeUniqueRewardInstance() noexcept;
#include <HomeSnapshot.h>
#include <HomePermissions.h>
#include <TradeLedger.h>
#include <Services/PapyrusService.h>
#include <PlayerCharacter.h>
#include <Forms/TESGlobal.h>
#include <Games/TES.h>
#include <Interface/UI.h>
#include <cstdlib>
#include <Messages/RequestCampaignPull.h>
#include <Messages/NotifyCampaignRecord.h>

namespace
{
CampaignReplication::Link campaignLink;
CampaignReplication::Pull pending;
CampaignIdentity::Stamp pendingSave;
uint64_t pendingLoadEpoch{};
std::chrono::steady_clock::time_point pendingDeadline{};
std::mutex checkpointLoadMutex;
std::string loadedDigest;
bool confirmedLoad{};
std::atomic<uint64_t> dragonLoadEpoch{1};
std::map<std::string,CampaignCheckpoint::Record> checkpointOutbox;
std::string LoadedDigest()
{
    std::lock_guard lock(checkpointLoadMutex);
    return confirmedLoad?loadedDigest:std::string{};
}
bool enrollmentUsed{};
bool saveWarning{};
uint64_t nonce{};
std::chrono::steady_clock::time_point nextPull{};
bool SaveWorldReady()
{
    auto* p=PlayerCharacter::Get();
    auto* ui=UI::Get();
    static const BSFixedString main("Main Menu"), loading("Loading Menu");
    return p && p->parentCell && p->GetNiNode() && !p->IsDead() && ui &&
        !ui->GetMenuOpen(main) && !ui->GetMenuOpen(loading);
}
std::optional<CampaignIdentity::Stamp> ReadSave(World& world, const CampaignIdentity::Credential& credential, bool enroll)
{
    using namespace CampaignIdentity;
    if (!SaveWorldReady()) return {};
    auto* plugin=ModManager::Get()->GetByName("SkyrimCoopTrade.esp");
    if (!plugin || !plugin->IsLoaded()) return {};
    TESGlobal* fields[FieldCount]{};
    Fields values{};
    for (unsigned i=0;i<FieldCount;++i)
    {
        auto* f=TESForm::GetById(plugin->GetFormId(0x804+i));
        if (!f || static_cast<unsigned>(f->formType)!=9) return {};
        fields[i]=static_cast<TESGlobal*>(f);values[i]=fields[i]->f;
    }
    Stamp stamp;
    auto state=Decode(values,stamp);
    if (state==SaveState::Empty && enroll && !enrollmentUsed &&
        campaignLink.Journal.Current().Revision==0)
    {
        const auto* flag=std::getenv("STM_CAMPAIGN_ENROLL");
        if (!flag || std::string_view(flag)!="1") return {};
        enrollmentUsed=true; // at most one explicit enrollment attempt per process
        stamp={campaignLink.Campaign,credential.Character,campaignLink.Journal.Current()};
        auto encoded=Encode(stamp);
        const auto* method=world.ctx().at<PapyrusService>().Get("GlobalVariable","SetValue");
        if (!encoded || !method) return {};
        PapyrusFunction<void,TESGlobal,float> set(method);
        // Publish schema marker last. A partial initialization is invalid, not a new character.
        for (unsigned i=0;i<FieldCount;++i) set(fields[i],(*encoded)[i]);
        for (unsigned i=0;i<FieldCount;++i) values[i]=fields[i]->f;
        state=Decode(values,stamp);
        if (state==SaveState::Valid) spdlog::info("STM campaign save enrolled; save the game to persist its identity");
    }
    if (state!=SaveState::Valid || stamp.Campaign!=campaignLink.Campaign || stamp.Character!=credential.Character)
        return {};
    return stamp;
}
std::filesystem::path Mirror()
{
    auto* local = _wgetenv(L"LOCALAPPDATA");
    if (!local || !*local)
        throw std::runtime_error("LOCALAPPDATA is unavailable");
    return std::filesystem::path(local) / "SkyrimTogetherReborn" / "CoopJournal";
}
}

CampaignReplication::Link::Mode CampaignAccess::Mode() { return campaignLink.Start(false); }
bool CampaignAccess::GameReady() { return SaveWorldReady(); }
uint32_t CampaignAccess::HomeScope(uint64_t campaign,uint64_t character,std::string_view root)
{
    if (Mode()!=CampaignReplication::Link::Mode::Ready || campaign!=campaignLink.Campaign) return 0;
    auto key=TradeProperties::DecodeForm(root);
    if (!key) return 0;
    auto home=HomeRegistry::Find(campaignLink.Journal,character,*key);
    return home?home->Slot:0;
}

uint32_t CampaignAccess::HomeBaselineState(uint64_t campaign,uint32_t slot)
{
    if (Mode()!=CampaignReplication::Link::Mode::Ready || campaign!=campaignLink.Campaign) return 2;
    const auto state=HomeSnapshot::Read(campaignLink.Journal,slot).State;
    return state==HomeSnapshot::Status::Missing?0:state==HomeSnapshot::Status::Ready?1:2;
}

std::vector<std::pair<uint32_t,uint64_t>> CampaignAccess::HomeOffers(uint64_t campaign,uint64_t character,std::string_view root)
{
    std::vector<std::pair<uint32_t,uint64_t>> out;
    if (Mode()!=CampaignReplication::Link::Mode::Ready || campaign!=campaignLink.Campaign) return out;
    auto key=TradeProperties::DecodeForm(root);
    auto homes=HomeRegistry::Read(campaignLink.Journal);
    if (!key || !homes) return out;
    for (const auto& h:*homes)
        if (h.Owner!=character && h.Root.Encode()==key->Encode() &&
            HomePermissions::Effective(campaignLink.Journal,h.Slot,character)!=HomePermissions::Right::None &&
            HomeSnapshot::Read(campaignLink.Journal,h.Slot).State==HomeSnapshot::Status::Ready)
            out.emplace_back(h.Slot,h.Owner);
    return out;
}
std::optional<std::string> CampaignAccess::HomeBaseline(uint64_t campaign,uint32_t slot)
{
    if (Mode()!=CampaignReplication::Link::Mode::Ready || campaign!=campaignLink.Campaign) return {};
    try
    {
        TradeLedger view(campaignLink.Journal,campaign);
        auto state=view.HomeState(slot);
        if (state) return state->Encode();
    }
    catch (...) {}
    return {};
}

std::optional<std::vector<HomePlacement::Stack>> CampaignAccess::HomePlacements(uint64_t campaign,uint32_t slot)
{
    if (Mode()!=CampaignReplication::Link::Mode::Ready || campaign!=campaignLink.Campaign ||
        !slot || slot>HomeRegistry::MaxSlot) return {};
    try
    {
        TradeLedger view(campaignLink.Journal,campaign);
        if (!view.HomeState(slot)) return {};
        return view.Placed(slot);
    }
    catch (...) {return {};}
}
bool CampaignAccess::HomeCanModify(uint64_t campaign,uint32_t slot,uint64_t character)
{
    return Mode()==CampaignReplication::Link::Mode::Ready && campaign==campaignLink.Campaign &&
        HomePermissions::Effective(campaignLink.Journal,slot,character)==HomePermissions::Right::Modify;
}

std::optional<HomeObjects::Records> CampaignAccess::HomeObjectView(uint64_t campaign,uint32_t slot)
{
    if (Mode()!=CampaignReplication::Link::Mode::Ready || campaign!=campaignLink.Campaign) return {};
    return HomeObjects::Read(campaignLink.Journal,slot);
}
std::optional<HomeEstateConstruction::Record> CampaignAccess::EstateConstruction(uint64_t campaign,uint64_t owner,const std::string& plot)
{
    if (Mode()!=CampaignReplication::Link::Mode::Ready||campaign!=campaignLink.Campaign) return {};
    return HomeEstateConstruction::Read(campaignLink.Journal,owner,plot);
}
std::optional<HomeEstateConstruction::Record> CampaignAccess::SharedEstateConstruction(uint64_t campaign,const std::string& plot)
{
    if (Mode()!=CampaignReplication::Link::Mode::Ready||campaign!=campaignLink.Campaign) return {};
    return HomeEstateConstruction::ReadShared(campaignLink.Journal,plot);
}
std::optional<std::string> CampaignAccess::EstateRoomShape(uint64_t campaign,uint32_t slot,const std::string& root)
{
    const auto& catalog=HomeEstateCatalog::Host();
    if (!catalog||Mode()!=CampaignReplication::Link::Mode::Ready||campaign!=campaignLink.Campaign) return {};
    return HomeEstateConstruction::RoomShape(campaignLink.Journal,*catalog,slot,root);
}
std::optional<HomeResidents::Residents> CampaignAccess::HomeResidentView(uint64_t campaign,uint32_t slot)
{
    if (Mode()!=CampaignReplication::Link::Mode::Ready || campaign!=campaignLink.Campaign) return {};
    return HomeResidents::Read(campaignLink.Journal,slot);
}

std::optional<std::map<std::string,std::array<uint32_t,3>>> CampaignAccess::HomeLockView(uint64_t campaign,uint32_t slot)
{
    if (Mode()!=CampaignReplication::Link::Mode::Ready || campaign!=campaignLink.Campaign) return {};
    auto locks=HomeLocks::Read(campaignLink.Journal,slot);
    if (!locks) return {};
    std::map<std::string,std::array<uint32_t,3>> result;
    for (const auto& [key,value]:*locks) result[key]={value.State,value.Level,value.Revision};
    return result;
}

std::optional<HomeCityPurchase::State> CampaignAccess::CityHome(uint64_t campaign,uint32_t cell)
{
    if (Mode()!=CampaignReplication::Link::Mode::Ready||campaign!=campaignLink.Campaign) return {};
    return HomeCityPurchase::Read(campaignLink.Journal,cell);
}

bool CampaignAccess::NativeSharedCityHome(uint32_t cell)
{
    if(Mode()!=CampaignReplication::Link::Mode::Ready||!HomeEstateConstruction::SharedMode())return false;
    return HomeCityPurchase::NativeContainerRoute(campaignLink.Journal,cell);
}

bool CampaignAccess::TradeEnabled()
{
    using Mode=CampaignReplication::Link::Mode;
    const auto mode=CampaignAccess::Mode();
    if (mode==Mode::Disabled) return true;
    const auto* flag=std::getenv("STM_CAMPAIGN_TRADE");
    return mode==Mode::Ready && flag && std::string_view(flag)=="1";
}
bool CampaignAccess::ConfirmReceipt(World& world,uint64_t stream,uint64_t savedStream,uint32_t cursor,
                                     uint32_t receipt,std::string_view plugin,uint32_t base,int count,int64_t before,std::string_view variant)
{
    try
    {
        const auto identity=Current(world);
        if (!identity || stream!=identity->Save.Campaign) return false;
        TradeLedger view(campaignLink.Journal,stream);
        return view.MatchesReceipt(std::to_string(identity->Save.Character),savedStream,cursor,receipt,
                                   std::string(plugin),base,count,before,std::string(variant));
    }
    catch (...) { return false; }
}
std::optional<CampaignAccess::Identity> CampaignAccess::LocalIdentity(World& world)
{
    if (Mode()!=CampaignReplication::Link::Mode::Ready) return {};
    const auto credential=CampaignIdentity::ReadCredential(campaignLink.Root);
    const auto stamp=credential ? ReadSave(world,*credential,false) : std::nullopt;
    if (!stamp || !CampaignIdentity::Known(campaignLink.Journal,campaignLink.Campaign,*stamp)) return {};
    return Identity{*stamp,*credential,campaignLink.Profile};
}
std::optional<CampaignAccess::Identity> CampaignAccess::Current(World& world)
{
    if (!world.GetTransport().IsOnline()) return {};
    return LocalIdentity(world);
}
uint32_t CampaignAccess::HomeContainerState(uint64_t campaign,uint32_t slot,std::string_view reference)
{
    if (Mode()!=CampaignReplication::Link::Mode::Ready || campaign!=campaignLink.Campaign) return 2;
    auto b=HomeSnapshot::Read(campaignLink.Journal,slot);
    if (b.State==HomeSnapshot::Status::Missing) return 0;
    if (b.State!=HomeSnapshot::Status::Ready) return 2;
    auto state=HomeSnapshot::Decode(b.Bytes);
    if (!state) return 2;
    for (const auto& r:state->Refs)
    {
        if (r.Key.Encode()!=reference) continue;
        if (r.Inventory==1 || r.Inventory==3) return 1;
        if (HomeSnapshot::LooseType(r.Type) && r.Inventory==0) return 3;
    }
    return 2;
}

namespace
{
std::optional<QuestWordReward::Sample> wordBaseline;
uint64_t wordAttemptCharacter{};
std::map<uint32_t,std::chrono::steady_clock::time_point> attemptedWords;
struct PendingWord
{
    std::set<std::string> Words;
    uint64_t Character{};
    uint32_t Peer{},Cell{},CellMod{},CellBase{};
};
std::optional<PendingWord> pendingWord;

bool IsLearnedWord(const TESForm* form)
{
    // WOOP, TESForm::GetKnown reads flag 6.
    return form&&static_cast<uint8_t>(form->formType)==0x76&&(form->flags&0x40u);
}

std::optional<QuestWordReward::Sample> CaptureWordSample(World& world,uint64_t character)
{
    if (!character||world.IsLoadingSave()||!SaveWorldReady()||!world.GetPartyService().IsInParty()) return {};
    const auto own=world.GetTransport().GetLocalPlayerId();
    uint32_t peer{};
    for (auto id:world.GetPartyService().GetPartyMembers())
        if (id&&id!=own)
        {
            if (peer&&peer!=id) return {};
            peer=id;
        }
    auto* player=PlayerCharacter::Get();
    auto* cell=player?player->GetParentCellEx():nullptr;
    auto* mods=ModManager::Get();
    if (!own||!peer||!cell||!cell->IsAttached()||!mods) return {};
    const auto& words=mods->words;
    if (!words.data||!words.length||words.length>words.capacity||words.length>4096) return {};
    QuestWordReward::Sample sample{character,cell->formID,peer,{}};
    for (uint32_t i=0;i<words.length;++i)
    {
        const auto* word=words[i];
        if (IsLearnedWord(word)&&word->formID&&!word->IsTemporary()) sample.Known.insert(word->formID);
    }
    return sample;
}

void ObserveLearnedWord(World& world,uint64_t character,RequestCampaignPull& request)
{
    if (!CampaignRewardPolicy::SupplementWords) return;
    auto sample=CaptureWordSample(world,character);
    if (!sample) {wordBaseline.reset();pendingWord.reset();return;}
    const auto grants=QuestWordReward::Read(campaignLink.Journal.Operations());
    if (!grants) return;
    if (pendingWord&&(pendingWord->Character!=character||pendingWord->Peer!=sample->Peer||
        pendingWord->Cell!=sample->Cell)) pendingWord.reset();
    if (pendingWord)
    {
        std::erase_if(pendingWord->Words,[&](const auto& word){
            return QuestWordReward::Entitled(*grants,word,character);
        });
        if (pendingWord->Words.empty()) pendingWord.reset();
    }
    if (!wordBaseline) return; // seed only after the server confirms a full prefix
    const auto words=QuestWordReward::NewWords(*wordBaseline,*sample);
    wordBaseline=sample;
    if (!words.empty())
    {
        std::vector<InventoryEvidence::PluginSlot> slots;
        auto* manager=ModManager::Get();
        if (!manager) return;
        for (const auto* mod:manager->mods)
        {
            if (!mod||!mod->IsLoaded()) continue;
            const auto* end=std::find(mod->filename,mod->filename+sizeof(mod->filename),'\0');
            if (end!=mod->filename+sizeof(mod->filename))
                slots.push_back({mod->GetId(),mod->IsLite(),std::string(mod->filename,end)});
        }
        GameId cell{};
        if (!world.GetModSystem().GetServerModId(sample->Cell,cell)) return;
        if (!pendingWord) pendingWord=PendingWord{{},character,sample->Peer,sample->Cell,cell.ModId,cell.BaseId};
        for (const auto word:words)
        {
            const auto stable=InventoryEvidence::Resolve(word,slots);
            if (!stable) continue;
            const TradeProperties::FormKey key{stable->Plugin,stable->Local};
            if (!key.Valid()||QuestWordReward::Entitled(*grants,key.Encode(),character)) continue;
            pendingWord->Words.insert(key.Encode());
        }
    }
    if (!pendingWord||pendingWord->Words.empty()) return;
    // Send all observed words that fit the existing authenticated pull.
    // Until their individual rows are confirmed, retain them in this exact
    // context even if another word becomes known in the meantime.
    request.LearnedWord=QuestWordReward::EncodeBatch(pendingWord->Words).c_str();
    request.WordPeer=pendingWord->Peer;request.WordCellMod=pendingWord->CellMod;
    request.WordCellBase=pendingWord->CellBase;
}

void RestoreLearnedWords(World& world,const CampaignJournal::Journal& journal,uint64_t character)
{
    if (!CampaignRewardPolicy::SupplementWords) return;
    if (!character||world.IsLoadingSave()||!SaveWorldReady()) return;
    if (wordAttemptCharacter!=character) {attemptedWords.clear();wordAttemptCharacter=character;}
    const auto grants=QuestWordReward::Read(journal.Operations());
    if (!grants) return;
    for (const auto& grant:*grants)
    {
        if (!grant.Entitles(character)) continue;
        const auto key=TradeProperties::DecodeForm(grant.Word);
        auto* mods=ModManager::Get();
        auto* mod=key&&mods?mods->GetByName(key->Plugin.c_str()):nullptr;
        if (!mod||!mod->IsLoaded()||(mod->IsLite()&&key->Local>0xFFF)) continue;
        auto* word=TESForm::GetById(mod->GetFormId(key->Local));
        if (!word||static_cast<uint8_t>(word->formType)!=0x76) continue;
        if (!IsLearnedWord(word))
        {
            const auto now=std::chrono::steady_clock::now();
            auto& retry=attemptedWords[word->formID];
            if (now>=retry)
            {
                retry=now+std::chrono::seconds(5);
                if (TeachCampaignWord(word)&&IsLearnedWord(word))
                    spdlog::info("STM restored learned word {:X}",word->formID);
                else spdlog::warn("STM native TeachWord failed for {:X}; retry after cooldown, no unlock or soul change attempted",word->formID);
            }
        }
        if (IsLearnedWord(word)) attemptedWords.erase(word->formID);
        // Never propagate a word applied from the campaign as a new acquisition.
        if (IsLearnedWord(word)&&wordBaseline&&wordBaseline->Character==character)
            wordBaseline->Known.insert(word->formID);
    }
    auto current=CaptureWordSample(world,character);
    if (!current) wordBaseline.reset();
    else if (!wordBaseline||wordBaseline->Character!=character||
             wordBaseline->Cell!=current->Cell||wordBaseline->Peer!=current->Peer)
        wordBaseline=std::move(current);
}

struct PendingDragonAbsorption
{
    DragonAbsorption::Receipt Receipt;
    uint64_t Campaign{};
    uint32_t Entity{};
    std::chrono::steady_clock::time_point Deadline{};
};
std::vector<PendingDragonAbsorption> pendingDragonAbsorptions;

void SendDragonAbsorption(const CampaignIdentity::Stamp& stamp,RequestCampaignPull& request)
{
    if (!CampaignRewardPolicy::SupplementDragonSouls) return;
    const auto receipts=DragonAbsorption::Read(campaignLink.Journal.Operations());
    if (!receipts) {pendingDragonAbsorptions.clear();return;}
    const auto now=std::chrono::steady_clock::now();
    std::erase_if(pendingDragonAbsorptions,[&](const auto& entry){
        return entry.Campaign!=stamp.Campaign||entry.Receipt.Character!=stamp.Character||entry.Deadline<=now||
            receipts->contains(entry.Receipt.Encode());
    });
    if (pendingDragonAbsorptions.empty()) return;
    const auto& next=pendingDragonAbsorptions.front();
    request.AbsorbedDragon=next.Receipt.Dragon.c_str();request.DragonEntity=next.Entity;
    std::rotate(pendingDragonAbsorptions.begin(),pendingDragonAbsorptions.begin()+1,pendingDragonAbsorptions.end());
}

uint64_t dragonObservedEpoch{},dragonObservedCharacter{};
std::optional<uint32_t> dragonSessionStart;
void ApplySharedDragonGrants(World& world,const CampaignJournal::Journal& journal,uint64_t character)
{
    if (!CampaignRewardPolicy::SupplementDragonSouls) return;
    if (!character||!DragonAwardHookReady()||!SaveWorldReady()||world.IsLoadingSave()||
        !world.GetPartyService().IsInParty()) return;
    const auto epoch=dragonLoadEpoch.load(std::memory_order_acquire);
    if (dragonObservedEpoch!=epoch||dragonObservedCharacter!=character)
    {
        dragonSessionStart.reset();dragonObservedEpoch=epoch;dragonObservedCharacter=character;
    }
    if (!dragonSessionStart) dragonSessionStart=journal.Current().Revision;
    const auto history=DragonAbsorption::ReadShared(journal.Operations());
    if (!history) return;
    auto* mods=ModManager::Get();
    auto* plugin=mods?mods->GetByName("SkyrimCoopTrade.esp"):nullptr;
    const auto* method=world.ctx().at<PapyrusService>().Get("GlobalVariable","SetValue");
    if (!plugin||!plugin->IsLoaded()||!method) return;
    PapyrusFunction<void,TESGlobal,float> set(method);
    for (uint32_t i=0;i<history->Grants.size();++i)
    {
        const auto& grant=history->Grants[i];
        if (grant.Recipient!=character||history->Retired.contains(grant.Dragon)) continue;
        const auto key=TradeProperties::DecodeForm(grant.Dragon);
        auto* mod=key?mods->GetByName(key->Plugin.c_str()):nullptr;
        if (!mod||!mod->IsLoaded()||(mod->IsLite()&&key->Local>0xFFF)) continue;
        const auto form=mod->GetFormId(key->Local);
        auto* dragon=Cast<Actor>(TESForm::GetById(form));
        if (!dragon||!dragon->IsDragon()||!dragon->IsDead()||!dragon->GetNiNode()) continue;
        auto* field=TESForm::GetById(plugin->GetFormId(DragonAbsorption::ReceiptBase+i/DragonAbsorption::BitsPerWord));
        if (!field||static_cast<unsigned>(field->formType)!=9) continue;
        auto* global=static_cast<TESGlobal*>(field);
        const auto mask=DragonAbsorption::Mask(global->f);
        if (!mask) continue;
        const uint32_t bit=1u<<(i%DragonAbsorption::BitsPerWord);
        if (*mask&bit)
        {
            RememberCampaignDragonAward(form);continue;
        }
        const auto kill=history->Kills.find(grant.Dragon);
        if (kill==history->Kills.end()||kill->second.Revision<=*dragonSessionStart) continue;
        const auto result=ApplyCampaignDragonAward(form);
        if (!result) continue;
        const auto next=*mask|bit;
        set(global,static_cast<float>(next));
        if (DragonAbsorption::Mask(global->f)!=next)
            spdlog::warn("STM shared dragon receipt could not be persisted for {:X}",form);
        else if (result==2)
            spdlog::info("STM shared dragon soul granted for {:X}",form);
    }
}

// Reuse the campaign journal, pull and existing progress window. A native
// quest grant is deferred only for an explicitly catalogued reward.
std::atomic<uint64_t> uniqueRewardEpoch{};
std::map<uint32_t,uint32_t> uniqueRewardChoices;
TESGlobal* UniqueRewardField(const UniqueQuestReward::Entry& entry)
{
    auto* mods=ModManager::Get();
    auto* plugin=mods?mods->GetByName("SkyrimCoopTrade.esp"):nullptr;
    auto* form=plugin&&plugin->IsLoaded()?TESForm::GetById(plugin->GetFormId(entry.Receipt)):nullptr;
    return form&&static_cast<unsigned>(form->formType)==9?static_cast<TESGlobal*>(form):nullptr;
}
bool SetUniqueRewardField(World& world,const UniqueQuestReward::Entry& entry,float value)
{
    auto* field=UniqueRewardField(entry);
    const auto* setter=world.ctx().at<PapyrusService>().Get("GlobalVariable","SetValue");
    if (!field||!setter) return false;
    PapyrusFunction<void,TESGlobal,float> set(setter);set(field,value);
    return field->f==value;
}
void SendUniqueReward(World& world,const CampaignIdentity::Stamp& stamp,RequestCampaignPull& request)
{
    if (!request.QuestReceipt.empty()||!world.GetPartyService().IsInParty()) return;
    for (const auto& entry:UniqueQuestReward::Catalog)
    {
        const auto state=UniqueQuestReward::Read(campaignLink.Journal.Operations(),entry.Quest);
        if (!state||state->Recipient) continue;
        if (state->Item==entry.Item&&state->Source==stamp.Character)
        {
            const auto selection=uniqueRewardChoices.find(entry.Item);
            if (selection!=uniqueRewardChoices.end())
            {
                const auto& members=world.GetPartyService().GetPartyMembers();
                if (std::find(members.begin(),members.end(),selection->second)!=members.end()||
                    selection->second==world.GetTransport().GetLocalPlayerId())
                    request.QuestReceipt=("reward-choice "+std::to_string(entry.Item)+" "+std::to_string(selection->second)).c_str();
                else uniqueRewardChoices.erase(selection);
                if (!request.QuestReceipt.empty()) return;
            }
        }
        else if (!state->Item&&UniqueQuestReward::StageSource(campaignLink.Journal.Operations(),entry)==stamp.Character)
        {
            const auto* field=UniqueRewardField(entry);
            if (field&&field->f==1.f)
            {
                request.QuestReceipt=("reward-offer "+std::to_string(entry.Item)).c_str();
                return;
            }
        }
    }
}
std::string RewardPeerName(World& world,uint32_t peer)
{
    auto view=world.view<PlayerComponent,FormIdComponent>();
    for (auto entity:view)
        if (view.get<PlayerComponent>(entity).Id==peer)
            if (auto* actor=Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));actor&&actor->baseForm)
                if (const auto* name=actor->baseForm->GetName();name&&*name) return name;
    // The existing player list carries account names, not guaranteed actor names.
    const auto& players=world.GetPartyService().GetPlayers();
    const auto it=players.find(peer);
    return it!=players.end()?std::string(it.value().data(),it.value().size())+" (сетевое имя)":"Участник группы";
}
void UpdateUniqueRewards(World& world,const CampaignIdentity::Stamp& stamp)
{
    uniqueRewardEpoch.store(0,std::memory_order_release);
    if (!world.GetTransport().IsOnline()||!world.GetPartyService().IsInParty()||world.IsLoadingSave()||!SaveWorldReady()) return;
    auto* player=PlayerCharacter::Get();
    if (!player) return;
    const auto epoch=dragonLoadEpoch.load(std::memory_order_acquire);
    const auto own=world.GetTransport().GetLocalPlayerId();
    uint32_t peer{};
    for (auto id:world.GetPartyService().GetPartyMembers())
        if (id&&id!=own) {if (peer&&peer!=id) return;peer=id;}
    // Legacy receipt recovery below is independent of new keyword observations.
    // Readiness is the confirmed full journal prefix for this loaded campaign.
    uniqueRewardEpoch.store(own&&peer?epoch:0,std::memory_order_release);
    for (const auto& entry:UniqueQuestReward::Catalog)
    {
        const auto state=UniqueQuestReward::Read(campaignLink.Journal.Operations(),entry.Quest);
        if (!state||state->Item!=entry.Item) continue;
        auto* field=UniqueRewardField(entry);
        if (!field) continue;
        if (state->Recipient)
        {
            uniqueRewardChoices.erase(entry.Item);
            if (field->f==2.f) continue;
            if (state->Recipient!=stamp.Character)
            {SetUniqueRewardField(world,entry,2.f);continue;}
            auto* item=Cast<TESBoundObject>(TESForm::GetById(entry.Item));
            if (!item) continue;
            // A previously obtained native item is not granted a second time.
            if (player->GetItemCountInInventory(item)==0)
                player->AddObjectToContainer(item,nullptr,1,nullptr);
            if (player->GetItemCountInInventory(item)>0)
                SetUniqueRewardField(world,entry,2.f);
            continue;
        }
        if (!own||!peer||state->Source!=stamp.Character||uniqueRewardChoices.contains(entry.Item)) continue;
        auto* item=TESForm::GetById(entry.Item);
        if (!item) continue;
        const auto* name=item->GetName();
        const auto* self=player->baseForm?player->baseForm->GetName():nullptr;
        const auto other=RewardPeerName(world,peer);
        const auto form=entry.Item,quest=entry.Quest;
        const auto character=stamp.Character,campaign=stamp.Campaign;
        auto needed=[epoch,form,quest,character,campaign,peer]{
            if (epoch!=dragonLoadEpoch.load(std::memory_order_acquire)||campaign!=campaignLink.Campaign||
                uniqueRewardChoices.contains(form)||!World::Exists()) return false;
            auto& w=World::Get();
            if (!w.GetTransport().IsOnline()||!w.GetPartyService().IsInParty()||w.IsLoadingSave()) return false;
            const auto& members=w.GetPartyService().GetPartyMembers();
            if (std::find(members.begin(),members.end(),peer)==members.end()) return false;
            const auto state=UniqueQuestReward::Read(campaignLink.Journal.Operations(),quest);
            return state&&state->Item==form&&state->Source==character&&!state->Recipient;
        };
        world.GetOverlayService().QueueRewardChoice(form,name?name:"Уникальная награда",
            self&&*self?self:"Ваш персонаж",other,
            [form,own,peer](bool chooseSelf){uniqueRewardChoices[form]=chooseSelf?own:peer;nextPull={};},needed);
    }
}

struct DeferredQuestPickup
{
    QuestPickupPermit::Claim Claim;
    uint32_t Form{},QuestForm{},Instance{},Cell{};
    uint64_t Character{},Nonce{};
    uint8_t Unk{};
    char DefaultProcessing{};
    std::chrono::steady_clock::time_point Deadline{};
};
std::optional<DeferredQuestPickup> deferredQuestPickup;

std::string StablePickupKey(uint32_t id)
{
    std::vector<InventoryEvidence::PluginSlot> slots;
    auto* mods=ModManager::Get();
    if (!mods) return {};
    for (const auto* mod:mods->mods)
    {
        if (!mod||!mod->IsLoaded()) continue;
        const auto* end=std::find(mod->filename,mod->filename+sizeof(mod->filename),'\0');
        if (end!=mod->filename+sizeof(mod->filename))
            slots.push_back({mod->GetId(),mod->IsLite(),std::string(mod->filename,end)});
    }
    const auto stable=InventoryEvidence::Resolve(id,slots);
    if (!stable) return {};
    const TradeProperties::FormKey key{stable->Plugin,stable->Local};
    return key.Valid()?key.Encode():std::string{};
}
// Q-13 native-first observation. No grants are suppressed, no new
// per-item ESS fields. Only a verified native duplicate may be debited.
namespace KR=KeywordQuestReward;
struct KeywordObservation
{
    KR::Key Key;
    uint32_t Quest{},Item{},Instance{},ObservedRevision{},Selection{};
    uint64_t Epoch{},Version{},Character{},Campaign{};
    bool FragmentDone{},Debited{};
};
std::map<std::string,KeywordObservation> keywordObservations;
std::mutex rewardVersionMutex;
std::map<uint32_t,uint64_t> rewardVersions;
uint64_t rewardVersionCounter{};
thread_local unsigned rewardAddDepth{};
uint64_t TrackRewardVersion(uint32_t id)
{
    std::lock_guard lock(rewardVersionMutex);
    auto version=++rewardVersionCounter;
    rewardVersions[id]=version;
    return version;
}
bool CurrentRewardVersion(uint32_t id,uint64_t version)
{
    std::lock_guard lock(rewardVersionMutex);
    const auto it=rewardVersions.find(id);
    return it!=rewardVersions.end()&&it->second==version;
}
bool HasRewardKeyword(TESForm* item)
{
    if (!item||item->IsTemporary()) return false;
    const auto* form=Cast<BGSKeywordForm>(item);
    if (!form) return false;
    const auto has=[form](uint32_t id){
        auto* keyword=Cast<BGSKeyword>(TESForm::GetById(id));
        return keyword&&form->Contains(keyword);
    };
    return KR::Classify(true,false,has(KR::Artifact),has(KR::VendorArtifact),has(KR::DisallowEnchanting))==KR::Policy::Choice;
}
bool CleanReward(PlayerCharacter& player,TESBoundObject* item)
{
    if (!item||player.GetItemCountInInventory(item)!=1) return false;
    // Same conservative clean-stack rule as native trade; protected,
    // equipped, modified, owned or otherwise ambiguous copies stay native.
    if (const auto* base=player.GetContainer())
    {
        if (base->count&&!base->entries) return false;
        for (uint32_t i=0;i<base->count;++i)
        {
            const auto* e=base->entries[i];
            if (e&&e->form==item&&e->data) return false;
        }
    }
    const auto* changes=player.GetContainerChanges();
    if (!changes||!changes->entries) return false;
    for (auto* e:*changes->entries)
    {
        if (!e||!e->form||e->form!=item) continue;
        if (e->IsQuestObject()) return false;
        if (!e->dataList) continue;
        for (auto* list:*e->dataList)
        {
            if (!list) continue;
            list->lock.LockForRead();
            bool special=false;
            for (auto* x=list->data;x;x=x->next)
                if (x->GetType()!=ExtraDataType::Count) {special=true;break;}
            list->lock.UnlockRead();
            if (special) return false;
        }
    }
    return true;
}
bool LiveReward(World& world,const KeywordObservation& r,const CampaignIdentity::Stamp& stamp)
{
    if (!r.FragmentDone||r.Debited||r.Epoch!=dragonLoadEpoch.load(std::memory_order_acquire)||
        r.Character!=stamp.Character||r.Campaign!=stamp.Campaign||
        !CurrentRewardVersion(r.Item,r.Version)||!world.GetPartyService().IsInParty()||
        !world.GetTransport().IsOnline()||world.IsLoadingSave()||!SaveWorldReady()) return false;
    auto* player=PlayerCharacter::Get();
    auto* q=Cast<TESQuest>(TESForm::GetById(r.Quest));
    auto* item=Cast<TESBoundObject>(TESForm::GetById(r.Item));
    // Complete the quest natively before touching a reward. Intermediate
    // quest items and more complex script paths are not redistributed.
    return player&&q&&q->currentInstanceID==r.Instance&&q->IsStageReached(r.Key.Stage)&&
        (q->flags&TESQuest::Completed)&&!(q->flags&TESQuest::Failed)&&
        !world.GetOverlayService().IsPersonalQuest(r.Quest)&&HasRewardKeyword(item)&&CleanReward(*player,item);
}
void RememberKeywordReward(uint32_t questId,uint16_t stage,uint32_t instance,uint32_t itemId,
                           uint64_t epoch,uint64_t version)
{
    auto& world=World::Get();
    if (epoch!=dragonLoadEpoch.load(std::memory_order_acquire)||!CurrentRewardVersion(itemId,version)||
        world.IsLoadingSave()||!world.GetPartyService().IsInParty()||!SaveWorldReady()) return;
    auto identity=CampaignAccess::Current(world);
    auto* q=Cast<TESQuest>(TESForm::GetById(questId));
    auto* item=Cast<TESBoundObject>(TESForm::GetById(itemId));
    auto* player=PlayerCharacter::Get();
    if (!identity||!q||!item||!player||q->currentInstanceID!=instance||
        !(q->flags&TESQuest::RunOnce)||(q->flags&TESQuest::AllowRepeatStages)||
        q->GetStageItemCount(stage)!=1||QuestService::IsNonSyncableQuest(q)||
        world.GetOverlayService().IsPersonalQuest(questId)||!HasRewardKeyword(item)||!CleanReward(*player,item)) return;
    KR::Key key{StablePickupKey(questId),StablePickupKey(itemId),stage};
    if (key.Quest.empty()||key.Item.empty()) return;
    const auto encoded=key.Encode();
    // Two additions of the same item from one stage are ambiguous.
    if (keywordObservations.contains(encoded)) {keywordObservations.erase(encoded);return;}
    keywordObservations.emplace(encoded,KeywordObservation{key,questId,itemId,instance,
        campaignLink.Journal.Current().Revision,0,epoch,version,identity->Save.Character,identity->Save.Campaign,false,false});
    nextPull={};
}
void ResetKeywordRewards()
{
    keywordObservations.clear();
    std::lock_guard lock(rewardVersionMutex);
    rewardVersions.clear();
}
void SendKeywordRewards(World& world,const CampaignIdentity::Stamp& stamp,RequestCampaignPull& request)
{
    if (!request.QuestReceipt.empty()||!world.GetPartyService().IsInParty()) return;
    for (auto& [encoded,r]:keywordObservations)
    {
        auto state=KR::Read(campaignLink.Journal.Operations(),r.Key);
        if (!state||r.Character!=stamp.Character||r.Campaign!=stamp.Campaign) continue;
        std::string verb,tail;
        if (r.Debited&&state->Kept&&!state->Removed&&state->Recipient!=stamp.Character)
            verb="reward-keyword-removed";
        else if (!LiveReward(world,r,stamp)) continue;
        else if (!state->Recipient&&!state->Ready.contains(stamp.Character))
            verb="reward-keyword-ready";
        else if (!state->Recipient&&state->BothReady()&&state->Source==stamp.Character&&r.Selection)
        {
            const auto own=world.GetTransport().GetLocalPlayerId();
            const auto& members=world.GetPartyService().GetPartyMembers();
            if (r.Selection!=own&&std::find(members.begin(),members.end(),r.Selection)==members.end())
            {r.Selection=0;continue;}
            verb="reward-keyword-choice";tail=" "+std::to_string(r.Selection);
        }
        else if (state->Recipient==stamp.Character&&!state->Kept&&KR::LiveChoice(*state,r.ObservedRevision))
            verb="reward-keyword-keep";
        if (!verb.empty()) {request.QuestReceipt=(verb+" "+encoded+tail).c_str();return;}
    }
}
void UpdateKeywordRewards(World& world,const CampaignIdentity::Stamp& stamp)
{
    if (!world.GetTransport().IsOnline()||!world.GetPartyService().IsInParty()||world.IsLoadingSave()||!SaveWorldReady()) return;
    const auto own=world.GetTransport().GetLocalPlayerId();
    uint32_t peer{};
    for (auto id:world.GetPartyService().GetPartyMembers())
        if (id&&id!=own) {if (peer&&peer!=id) return;peer=id;}
    if (!own||!peer) return;
    for (auto& [encoded,r]:keywordObservations)
    {
        auto state=KR::Read(campaignLink.Journal.Operations(),r.Key);
        if (!state||!LiveReward(world,r,stamp)) continue;
        if (KR::CanRemove(*state,stamp.Character,r.ObservedRevision,true))
        {
            auto* p=PlayerCharacter::Get();
            auto* item=Cast<TESBoundObject>(TESForm::GetById(r.Item));
            // No compensating AddItem is issued. The selected character
            // confirmed its own native copy; only our exact clean unit goes.
            if (p&&item&&LiveReward(world,r,stamp))
            {
                p->RemoveItem(item,1,ITEM_REMOVE_REASON::kRemove,nullptr,nullptr);
                if (p->GetItemCountInInventory(item)==0)
                {r.Debited=true;nextPull={};}
            }
            continue;
        }
        if (state->Recipient||!state->BothReady()||state->Source!=stamp.Character||r.Selection) continue;
        auto* player=PlayerCharacter::Get();
        auto* item=TESForm::GetById(r.Item);
        if (!player||!item) continue;
        const auto key=encoded;
        const auto saved=stamp;
        const auto needed=[key,saved]{
            if (!World::Exists()) return false;
            auto& w=World::Get();
            auto it=keywordObservations.find(key);
            if (it==keywordObservations.end()||it->second.Selection||!LiveReward(w,it->second,saved)) return false;
            auto s=KR::Read(campaignLink.Journal.Operations(),it->second.Key);
            return s&&s->BothReady()&&!s->Recipient&&s->Source==saved.Character;
        };
        const auto* self=player->baseForm?player->baseForm->GetName():nullptr;
        world.GetOverlayService().QueueRewardChoice(r.Item,item->GetName(),self&&*self?self:"Ваш персонаж",
            RewardPeerName(world,peer),[key,own,peer,needed](bool chooseSelf){
                if (!needed()) return;
                keywordObservations.at(key).Selection=chooseSelf?own:peer;nextPull={};
            },needed);
    }
}

struct PendingQuestReceipt
{
    QuestPickupPermit::Receipt Receipt;
    uint64_t Campaign{},Character{};
};
std::vector<PendingQuestReceipt> pendingQuestReceipts;
void SendQuestReceipt(const CampaignIdentity::Stamp& stamp,RequestCampaignPull& request)
{
    std::erase_if(pendingQuestReceipts,[&](const auto& entry){
        if (entry.Campaign!=stamp.Campaign||entry.Character!=stamp.Character) return true;
        if (QuestPickupPermit::Acknowledged(campaignLink.Journal.Operations(),entry.Receipt,stamp.Character)) return true;
        // A confirmed native mutation is not invalidated by elapsed wall
        // time or a short network outage. Retry in this loaded ESS until ACK;
        // load/identity change clears the observation, not the server history.
        return false;
    });
    if (pendingQuestReceipts.empty()) return;
    request.QuestReceipt=pendingQuestReceipts.front().Receipt.Encode().c_str();
    // Do not let one unresolved receipt prevent retry of other actual events.
    std::rotate(pendingQuestReceipts.begin(),pendingQuestReceipts.begin()+1,pendingQuestReceipts.end());
}
std::optional<DeferredQuestPickup> DescribeQuestPickup(World& world,TESObjectREFR* ref)
{
    if (!ref||!ref->baseForm||ref->IsTemporary()||ref->IsDisabled()||ref->IsDeleted()||!ref->GetNiNode()) return {};
    const auto type=ref->baseForm->formType;
    if (type==FormType::Door||type==FormType::Container||type==FormType::Book||type==FormType::Npc) return {};
    auto* player=PlayerCharacter::Get();
    auto* cell=player?player->GetParentCellEx():nullptr;
    if (!cell||ref->GetParentCellEx()!=cell||!cell->IsAttached()) return {};
    InventoryChangeEvent evidence(player->formID,Inventory::Entry{});
    CaptureQuestItem(ref->GetExtraDataList(),ref,ref->baseForm,evidence);
    if (!evidence.QuestId||!evidence.QuestRef) return {};
    const auto questId=world.GetModSystem().GetGameId(evidence.QuestId);
    auto* quest=Cast<TESQuest>(TESForm::GetById(questId));
    if (!quest||quest->IsStopped()||!(quest->flags&TESQuest::RunOnce)||
        (quest->flags&(TESQuest::Failed|TESQuest::Completed|TESQuest::AllowRepeatStages))||
        QuestService::IsNonSyncableQuest(quest)||world.GetOverlayService().IsPersonalQuest(questId)) return {};
    bool objective=false;
    for (const auto& entry:player->objectives)
        if (entry.instance&&entry.instance->quest==quest) {objective=true;break;}
    if (!objective) return {};
    const auto identity=CampaignAccess::Current(world);
    if (!identity) return {};
    DeferredQuestPickup result;
    result.Claim={StablePickupKey(questId),StablePickupKey(ref->formID),StablePickupKey(ref->baseForm->formID),evidence.QuestAlias};
    if (!result.Claim.Valid()) return {};
    result.Form=ref->formID;result.QuestForm=questId;result.Instance=quest->currentInstanceID;
    result.Cell=cell->formID;result.Character=identity->Save.Character;
    return result;
}
void ResolveQuestPickup(World& world,uint64_t nonce,bool permitted,uint64_t character)
{
    if (!deferredQuestPickup||deferredQuestPickup->Nonce!=nonce) return;
    auto pickup=std::move(*deferredQuestPickup);deferredQuestPickup.reset();
    if (!permitted)
    {
        spdlog::info("STM quest pickup refused: already acquired or reserved");
        return;
    }
    if (pickup.Character!=character||pickup.Deadline<=std::chrono::steady_clock::now()||
        !world.GetPartyService().IsInParty()||world.IsLoadingSave()||!SaveWorldReady()) return;
    auto* ref=Cast<TESObjectREFR>(TESForm::GetById(pickup.Form));
    auto current=DescribeQuestPickup(world,ref);
    if (!current||current->Claim.Encode()!=pickup.Claim.Encode()||current->Character!=character||
        current->Cell!=pickup.Cell||current->Instance!=pickup.Instance) return;
    // Existing activation event already supports delayed native activation.
    // Only one local attempt is executed for this reply, never an item grant.
    world.GetRunner().Trigger(ActivateEvent(ref,PlayerCharacter::Get(),nullptr,1,
        pickup.DefaultProcessing,pickup.Unk,TESObjectREFR::kNone,true));
}

std::string NextCheckpoint(const CampaignIdentity::Stamp& stamp)
{
    static std::pair<uint64_t,uint64_t> restored{};
    const auto owner=std::pair{stamp.Campaign,stamp.Character};
    if (restored!=owner && CampaignCheckpoint::RestoreOutbox(campaignLink.Root,campaignLink.Journal,
        stamp.Campaign,stamp.Character,checkpointOutbox))
        restored=owner;
    for (auto it=checkpointOutbox.begin();it!=checkpointOutbox.end();)
    {
        auto& r=it->second;
        if (r.Save.Campaign!=stamp.Campaign||r.Save.Character!=stamp.Character) {++it;continue;}
        if (auto known=CampaignCheckpoint::Find(campaignLink.Journal,stamp.Campaign,stamp.Character,r.Digest))
        {
            if (known->Encode()!=r.Encode()) spdlog::warn("STM checkpoint digest already has different metadata");
            it=checkpointOutbox.erase(it);continue;
        }
        if (CampaignCheckpoint::Known(campaignLink.Journal,stamp.Campaign,r)) return r.Encode();
        ++it;
    }
    return {};
}
}
std::optional<CampaignCheckpoint::Record> CampaignCheckpointClient::Capture(World& world) noexcept
{
    try
    {
        const auto identity=CampaignAccess::Current(world);
        auto* player=PlayerCharacter::Get();
        auto* cell=player?player->GetParentCellEx():nullptr;
        if (!identity||!cell||!cell->IsAttached()||HomeInstance::ProjectionForm(cell->formID)) return {};
        std::vector<InventoryEvidence::PluginSlot> slots;
        if (auto* mods=ModManager::Get())
            for (const auto* mod:mods->mods)
            {
                if (!mod||!mod->IsLoaded()) continue;
                const auto* end=std::find(mod->filename,mod->filename+sizeof(mod->filename),'\0');
                if (end!=mod->filename+sizeof(mod->filename))
                    slots.push_back({mod->GetId(),mod->IsLite(),std::string(mod->filename,end)});
            }
        CampaignCheckpoint::Record r;
        r.Save=identity->Save;r.Observed=campaignLink.Journal.Current();
        if (auto key=InventoryEvidence::Resolve(cell->formID,slots)) r.Cell={key->Plugin,key->Local};
        if (auto* space=player->GetWorldSpace())
            if (auto key=InventoryEvidence::Resolve(space->formID,slots)) r.WorldSpace={key->Plugin,key->Local};
        if (r.Cell.Empty()&&r.WorldSpace.Empty()) return {};
        uint32_t scope{},base{};
        if (HomeInstance::Encode(cell->formID,scope,base))
        {
            if (!HomeRegistry::Scoped(scope)) return {};
            r.HomeSlot=scope&HomeRegistry::MaxSlot;
        }
        if (HomeInstance::PrivateCell(cell->formID)&&!r.HomeSlot) return {};
        r.Pose={std::bit_cast<uint32_t>(player->position.x),std::bit_cast<uint32_t>(player->position.y),
                std::bit_cast<uint32_t>(player->position.z),std::bit_cast<uint32_t>(player->rotation.x),
                std::bit_cast<uint32_t>(player->rotation.y),std::bit_cast<uint32_t>(player->rotation.z)};
        return r;
    }
    catch (...) {return {};}
}
bool CampaignCheckpointClient::Queue(CampaignCheckpoint::Record r) noexcept
{
    try
    {
        if (!CampaignCheckpoint::Known(campaignLink.Journal,campaignLink.Campaign,r)) return false;
        if (!CampaignCheckpoint::StoreLocal(campaignLink.Root,r))
        {spdlog::warn("STM checkpoint local outbox write failed");return false;}
        const auto id=CampaignCheckpoint::Id(r.Save.Character,r.Digest);
        checkpointOutbox.insert_or_assign(id,std::move(r));nextPull={};
        spdlog::info("STM exact ESS checkpoint queued");return true;
    }
    catch (...) {spdlog::warn("STM checkpoint queue failed");return false;}
}
void CampaignCheckpointClient::ForgetLoaded() noexcept
{
    ResetCampaignDragonAwards();dragonLoadEpoch.fetch_add(1,std::memory_order_acq_rel);
    std::lock_guard lock(checkpointLoadMutex);loadedDigest.clear();confirmedLoad=false;
}
void CampaignCheckpointClient::NoteLoadedFile(std::string digest) noexcept
{
    std::lock_guard lock(checkpointLoadMutex);loadedDigest=std::move(digest);confirmedLoad=false;
}
void CampaignCheckpointClient::CompleteLoad() noexcept
{
    std::lock_guard lock(checkpointLoadMutex);confirmedLoad=CampaignCheckpoint::DigestValid(loadedDigest);
}
std::optional<CampaignCheckpoint::Record> CampaignCheckpointClient::Loaded(World& world) noexcept
{
    try
    {
        const auto identity=CampaignAccess::Current(world);
        if (!identity) return {};
        auto r=CampaignCheckpoint::Find(campaignLink.Journal,identity->Save.Campaign,
            identity->Save.Character,LoadedDigest());
        if (r&&r->Save==identity->Save) return r;
    }
    catch (...) {}
    return {};
}

namespace
{
struct QuestReplayState
{
    uint32_t Generation{1};
    bool Stopped{};
    bool CompletedRunOnce{};
    std::string Targets;
    bool Seen{};
    std::vector<std::pair<uint16_t,uint8_t>> Stages;
    uint16_t StopStage{};
    uint8_t StopType{};
    uint64_t StartedBy{};
    uint16_t StartStage{};
    uint8_t StartType{};
    bool CanRestoreStart(uint64_t character) const noexcept
    {
        if (!character||!StartedBy||Generation!=1||!Targets.empty()) return false;
        if (!Stopped) return true;
        // A late participant may replay a witnessed completed one-shot run,
        // but only with its recorded start and final native stage. Never
        // invent completion from a stop row or reopen a failed/abandoned run.
        return CompletedRunOnce&&std::any_of(Stages.begin(),Stages.end(),
            [&](const auto& entry){return entry.first==StopStage;});
    }
};
struct QuestCatchup : QuestReplayState
{
    QuestRunMember::Members Members;
    std::vector<QuestReplayState> Prior;
};

// Read the existing authoritative history, not the numeric maximum stage.
// Do not assume that a local radiant quest belongs to a later shared run.
std::optional<std::map<std::string,QuestCatchup>> ActiveQuestHistory(
    const std::vector<CampaignJournal::Operation>& ops)
{
    std::map<std::string,QuestCatchup> quests;
    for (const auto& op:ops)
    {
        const bool completed=op.Kind=="quest-completed-v1";
        const bool binding=op.Kind=="quest-binding-v1";
        if (!completed && !binding && op.Kind!="quest-progress-v1") continue;
        std::istringstream in(op.Payload);
        std::string key,extra;
        uint32_t run{},status{},stage{},type{};
        uint64_t character{};
        if (!(in>>key>>run>>status>>stage>>type>>character)||
            !run||!character||status>NotifyQuestUpdate::Stopped||stage>UINT16_MAX||type>UINT8_MAX)
            return {};
        std::string targets;
        std::optional<uint32_t> instance;
        if (!QuestRunMember::ReadTail(in,targets,instance)||targets.size()>2048) return {};
        // Scoped rows use the existing canonical target tail for identity.
        // Keep legacy variable history isolated as well.
        if (key.ends_with('~'))
        {
            if (targets.empty()) return {};
            key=QuestRunMember::ScopeKey(key.substr(0,key.size()-1),targets);
        }
        else if (!targets.empty())
            key+="~legacy"; // old unscoped rows retained, never replayed
        auto& quest=quests[key];
        if (binding)
        {
            if (!instance||!quest.Seen||quest.Stopped||quest.Generation!=run||quest.Targets!=targets||
                !QuestRunMember::Remember(quest.Members,character,run,*instance)) return {};
            continue;
        }
        if (quest.CompletedRunOnce || (completed&&(status!=NotifyQuestUpdate::Stopped||run!=1||quest.Stopped))) return {};
        if (run!=quest.Generation)
        {
            if (!quest.Stopped||status!=NotifyQuestUpdate::Started||
                quest.Generation==UINT32_MAX||run!=quest.Generation+1) return {};
            quest.Prior.push_back(static_cast<const QuestReplayState&>(quest));
            static_cast<QuestReplayState&>(quest)={};
            quest.Generation=run;
        }
        if (quest.Seen&&quest.Targets!=targets) return {};
        quest.Seen=true;quest.Targets=std::move(targets);
        if (instance&&!QuestRunMember::Remember(quest.Members,character,run,*instance)) return {};
        if (status==NotifyQuestUpdate::Started)
        {
            if (quest.Stopped||quest.StartedBy) return {};
            quest.StartedBy=character;
            quest.StartStage=static_cast<uint16_t>(stage);
            quest.StartType=static_cast<uint8_t>(type);
        }
        else if (status==NotifyQuestUpdate::Stopped)
        {
            if (quest.Stopped) return {};
            quest.Stopped=true;
            quest.CompletedRunOnce=completed;
            quest.StopStage=static_cast<uint16_t>(stage);
            quest.StopType=static_cast<uint8_t>(type);
        }
        else if (status==NotifyQuestUpdate::StageUpdate)
        {
            if (quest.Stopped) return {};
            quest.Stages.emplace_back(static_cast<uint16_t>(stage),static_cast<uint8_t>(type));
        }
    }
    return quests;
}

struct QuestPickup
{
    std::string Quest, Reference, Item;
    uint32_t Generation{}, Alias{};
    int32_t Count{};
    uint64_t Character{};
    bool Consumed{};
    std::set<uint64_t> Copies;
};

std::optional<std::vector<QuestPickup>> QuestPickupHistory(
    const std::vector<CampaignJournal::Operation>& ops)
{
    std::vector<QuestPickup> pickups;
    for (const auto& op:ops)
    {
        if (op.Kind!="quest-pickup-v1"&&op.Kind!="quest-consumed-v1"&&
            op.Kind!="quest-copy-v1"&&op.Kind!="quest-consumed-v2") continue;
        QuestPickup item;
        std::istringstream in(op.Payload);
        std::string extra;
        if (!(in>>item.Quest>>item.Generation>>item.Alias>>item.Reference>>item.Item>>item.Count>>item.Character)||
            in>>extra||!item.Generation||item.Count<=0||!item.Character) return {};
        auto found=std::find_if(pickups.begin(),pickups.end(),[&](const auto& p)
        {
            return p.Quest==item.Quest&&p.Generation==item.Generation&&p.Alias==item.Alias;
        });
        if (found==pickups.end())
        {
            if (op.Kind!="quest-pickup-v1") return {};
            pickups.push_back(std::move(item));
            continue;
        }
        if (found->Reference!=item.Reference||found->Item!=item.Item||
            found->Count!=item.Count) return {};
        if (op.Kind=="quest-copy-v1")
        {
            if (item.Count!=1||found->Character==item.Character) return {};
            found->Copies.insert(item.Character);
        }
        else if (op.Kind=="quest-consumed-v2")
        {
            if (item.Count!=1||!found->Copies.contains(item.Character)) return {};
            found->Consumed=true;
        }
        else
        {
            if (found->Character!=item.Character) return {};
            if (op.Kind=="quest-consumed-v1") found->Consumed=true;
        }
    }
    return pickups;
}

bool ShouldRestorePickup(const QuestPickup& item,uint64_t character,uint32_t generation,bool stopped)
{
    return character&&item.Character==character&&generation==1&&item.Generation==generation&&
        !stopped&&!item.Consumed&&item.Count==1;
}

bool ShouldReconcileSpentPickup(const QuestPickup& item,uint64_t character,uint32_t generation)
{
    return character&&(item.Character==character||item.Copies.contains(character))&&item.Consumed&&item.Count==1&&
        generation==1&&item.Generation==generation;
}

// Return only a single unambiguous native inventory stack bearing the
// exact quest alias and reference. A base-form match alone is never enough.
ExtraDataList* FindSpentQuestStack(PlayerCharacter& player,TESBoundObject* base,
    const GameId& questNet,const GameId& refNet,uint32_t alias)
{
    auto* changes=player.GetContainerChanges();
    if (!changes||!changes->entries||!base) return nullptr;
    ExtraDataList* match=nullptr;
    for (auto* entry:*changes->entries)
    {
        if (!entry||entry->form!=base||entry->count<=0||!entry->dataList) continue;
        for (auto* extra:*entry->dataList)
        {
            if (!extra) continue;
            InventoryChangeEvent identity(player.formID,Inventory::Entry{});
            CaptureQuestItem(extra,nullptr,base,identity,true);
            if (identity.QuestId!=questNet||identity.QuestRef!=refNet||identity.QuestAlias!=alias) continue;
            auto* stackCount=Cast<ExtraCount>(extra->GetByType(ExtraDataType::Count));
            if (match||(stackCount&&stackCount->count!=1)) return nullptr;
            match=extra;
        }
    }
    return match;
}

bool ReconcileSpentQuestStack(PlayerCharacter& player,TESBoundObject* base,
    const GameId& questNet,const GameId& refNet,uint32_t alias)
{
    if (!base||!player.GetContainer()||!player.GetContainerChanges()||
        !player.GetContainerChanges()->entries) return false;
    auto* stack=FindSpentQuestStack(player,base,questNet,refNet,alias);
    if (!stack) return false;
    const auto before=player.GetItemCountInInventory(base);
    if (before<=0) return false;
    {
        ScopedInventoryOverride inventory;
        ScopedQuestOverride quest;
        ScopedEquipOverride equip;
        player.RemoveItem(base,1,ITEM_REMOVE_REASON::kRemove,stack,nullptr);
    }
    return player.GetItemCountInInventory(base)==before-1&&
        !FindSpentQuestStack(player,base,questNet,refNet,alias);
}

// Keep attempts local to the current loaded world. A failed native operation
// must not be invoked every time a journal reply arrives.
std::set<std::pair<uint32_t,uint32_t>> attemptedQuestPickups,attemptedQuestConsumption;
uint32_t LocalQuestForm(const std::string& key)
{
    const auto form=TradeProperties::DecodeForm(key);
    if (!form||!form->Valid()) return 0;
    auto* mod=ModManager::Get()->GetByName(form->Plugin.c_str());
    return mod&&mod->IsLoaded()&&(!mod->IsLite()||form->Local<=0xfff)?mod->GetFormId(form->Local):0;
}

// Script-driven movement is observed, not replaced. A missing native
// sleep trigger uses the existing campaign receipt stream. Ordinary stage
// methods are already replayed by ReceiveQuestProgress.
struct SleepTravelObservation
{
    ScriptedTravel::Move Event;
    uint64_t Character{},Campaign{},Epoch{};
    uint32_t QuestForm{},TargetForm{},Instance{};
    bool Arrived{};
    std::chrono::steady_clock::time_point Deadline{};
};
std::map<std::string,SleepTravelObservation> sleepMoves;
std::set<std::string> sleepAttempts;
void ResetScriptedTravel()
{
    sleepMoves.clear();sleepAttempts.clear();
}
void RememberScriptedTravel(ScriptedMoveObservation o)
{
    auto& w=World::Get();
    if (o.Epoch!=dragonLoadEpoch.load(std::memory_order_acquire)||w.IsLoadingSave()||
        !w.GetPartyService().IsInParty()||!w.GetTransport().IsOnline()) return;
    auto* quest=Cast<TESQuest>(TESForm::GetById(o.Quest));
    if (!quest||quest->currentInstanceID!=o.Instance||QuestService::IsNonSyncableQuest(quest)||
        w.GetOverlayService().IsPersonalQuest(o.Quest)) return;
    spdlog::debug("STM native scripted player MoveTo: quest {:X}, stage {}, target {:X}, sleep {}",
        o.Quest,o.Stage,o.Target,o.Sleep);
    if (!o.Sleep||o.WasAtTarget) return;
    auto identity=CampaignAccess::Current(w);
    if (!identity) return;
    ScriptedTravel::Move m{StablePickupKey(o.Quest),StablePickupKey(o.Target),o.Script,o.Stage,
        std::bit_cast<uint32_t>(o.Start),std::bit_cast<uint32_t>(o.End)};
    if (!m.Valid()||sleepMoves.size()>=16) return;
    const auto key=m.Key();
    // Keep the first observation; a subsequent native replay must not
    // replace its arguments or cause a second dispatch.
    sleepMoves.try_emplace(key,SleepTravelObservation{m,identity->Save.Character,identity->Save.Campaign,o.Epoch,
        o.Quest,o.Target,o.Instance,false,std::chrono::steady_clock::now()+std::chrono::seconds(30)});
    nextPull={};
}
void ObserveSleepArrivals(World& w)
{
    if (w.IsLoadingSave()||!SaveWorldReady()) return;
    const auto now=std::chrono::steady_clock::now();
    for (auto& [key,o]:sleepMoves)
    {
        if (o.Arrived||now>=o.Deadline||o.Epoch!=dragonLoadEpoch.load(std::memory_order_acquire)) continue;
        auto* quest=Cast<TESQuest>(TESForm::GetById(o.QuestForm));
        auto* target=Cast<TESObjectREFR>(TESForm::GetById(o.TargetForm));
        if (quest&&quest->currentInstanceID==o.Instance&&AtScriptedTravelTarget(target))
        {o.Arrived=true;nextPull={};}
    }
}
void SendScriptedTravel(World& w,const CampaignIdentity::Stamp& stamp,RequestCampaignPull& request)
{
    if (!request.QuestReceipt.empty()||!w.GetPartyService().IsInParty()) return;
    const auto records=ScriptedTravel::Read(campaignLink.Journal.Operations());
    if (!records) return;
    for (const auto& [key,o]:sleepMoves)
    {
        if (!o.Arrived||o.Campaign!=stamp.Campaign||o.Character!=stamp.Character||
            o.Epoch!=dragonLoadEpoch.load(std::memory_order_acquire)) continue;
        const auto* existing=ScriptedTravel::Find(*records,o.Event);
        if (existing&&existing->Recipient==stamp.Character&&!existing->Arrived&&
            existing->Event.Target==o.Event.Target&&existing->Event.Script==o.Event.Script)
        {
            // The recipient may have slept independently with a different
            // duration; acknowledge the authoritative event only after the
            // same native quest, stage and marker were actually observed.
            request.QuestReceipt=("script-sleep-arrived "+existing->Event.Encode()).c_str();return;
        }
        if (!existing&&
            ScriptedTravel::CurrentStage(campaignLink.Journal.Operations(),o.Event))
        {request.QuestReceipt=("script-sleep-move "+o.Event.Encode()).c_str();return;}
    }
}
template<class CanDispatch>
void ApplyScriptedTravel(World& w,const CampaignIdentity::Stamp& stamp,CanDispatch canDispatch)
{
    if (!w.GetTransport().IsOnline()||!w.GetPartyService().IsInParty()||w.IsLoadingSave()||!SaveWorldReady()) return;
    auto* ui=UI::Get();auto* player=PlayerCharacter::Get();
    if (!ui||!player||ui->BlocksPartyHud()||ui->GetMenuOpen("Sleep/Wait Menu")||
        player->IsInCombat()||!BSInputEnableManager::IsMovementControlsEnabled()) return;
    const auto records=ScriptedTravel::Read(campaignLink.Journal.Operations());
    if (!records) return;
    for (const auto& r:*records)
    {
        if (!ScriptedTravel::MayDispatch(r,stamp.Character,
            ScriptedTravel::CurrentStage(campaignLink.Journal.Operations(),r.Event),
            sleepMoves.contains(r.Event.Key()),sleepAttempts.contains(r.Event.Key()),false)) continue;
        // A saved checkpoint that already applied this revision is not a
        // new instruction to replay a latent native script.
        if (stamp.Applied.Revision>=r.Revision) continue;
        const auto questId=LocalQuestForm(r.Event.Quest),targetId=LocalQuestForm(r.Event.Target);
        auto* quest=questId?Cast<TESQuest>(TESForm::GetById(questId)):nullptr;
        auto* target=targetId?Cast<TESObjectREFR>(TESForm::GetById(targetId)):nullptr;
        if (!quest||!target||quest->IsStopped()||!(quest->flags&TESQuest::RunOnce)||
            (quest->flags&(TESQuest::Failed|TESQuest::Completed|TESQuest::AllowRepeatStages))||
            quest->currentStage!=r.Event.Stage||!quest->IsStageReached(r.Event.Stage)||
            QuestService::IsNonSyncableQuest(quest)||w.GetOverlayService().IsPersonalQuest(questId)||
            !canDispatch(questId,quest->currentInstanceID)||AtScriptedTravelTarget(target)) continue;
        if (!sleepAttempts.insert(r.Event.Key()).second) continue;
        const auto start=std::bit_cast<float>(r.Event.StartBits),end=std::bit_cast<float>(r.Event.EndBits);
        if (!DispatchScriptedSleep(questId,r.Event.Script,start,end))
            spdlog::warn("STM could not dispatch native sleep travel for quest {:X}",questId);
        // No forced MoveTo or synthetic player-controls changes on failure.
        // Only a subsequent real native MoveTo observation confirms arrival.
        break;
    }
}

struct DeferredQuestTurnIn
{
    QuestPickupPermit::Claim Claim;
    uint32_t QuestForm{},BaseForm{},Npc{},Info{},Instance{};
    uint64_t Campaign{},Character{},Nonce{};
    std::chrono::steady_clock::time_point Deadline{};
};
std::optional<DeferredQuestTurnIn> deferredQuestTurnIn;
thread_local bool questDialogueThread{};

std::optional<QuestPickup> FindSharedTurnIn(uint32_t questId,uint32_t baseId)
{
    const auto questKey=StablePickupKey(questId),baseKey=StablePickupKey(baseId);
    if (questKey.empty()||baseKey.empty()) return {};
    const auto items=QuestPickupHistory(campaignLink.Journal.Operations());
    if (!items) return {};
    std::optional<QuestPickup> match;
    for (const auto& item:*items)
        if (item.Quest==questKey&&item.Item==baseKey&&item.Generation==1&&item.Count==1)
        {
            if (match) return {}; // the condition does not distinguish aliases
            match=item;
        }
    return match;
}
bool CanUseSharedTurnIn(World& world,const QuestPickup& item,uint32_t questId,uint32_t baseId,uint64_t character)
{
    auto* player=PlayerCharacter::Get();
    auto* quest=Cast<TESQuest>(TESForm::GetById(questId));
    auto* base=Cast<TESBoundObject>(TESForm::GetById(baseId));
    const auto refId=LocalQuestForm(item.Reference);
    if (!player||!quest||quest->IsStopped()||!(quest->flags&TESQuest::RunOnce)||
        (quest->flags&(TESQuest::Failed|TESQuest::Completed|TESQuest::AllowRepeatStages))||
        !base||!refId) return false;
    auto* ref=quest->GetAliasedRef(item.Alias);
    if (!ref||ref->formID!=refId||!ref->baseForm||ref->baseForm!=base) return false;
    const auto count=player->GetItemCountInInventory(base);
    // A native dialogue must be backed by the submitter's physical exact stack.
    if (!QuestPickupPermit::PhysicalHolder(character,item.Character,item.Copies.contains(character),count)) return false;
    GameId questNet{},refNet{};
    return world.GetModSystem().GetServerModId(questId,questNet)&&
        world.GetModSystem().GetServerModId(refId,refNet)&&
        FindSpentQuestStack(*player,base,questNet,refNet,item.Alias);
}
void ResolveQuestTurnIn(World& world,uint64_t nonce,bool permitted,uint64_t character)
{
    if (!deferredQuestTurnIn||deferredQuestTurnIn->Nonce!=nonce) return;
    const auto pending=std::move(*deferredQuestTurnIn);deferredQuestTurnIn.reset();
    if (!permitted)
    {Utils::ShowHudMessage("Общий предмет уже сдан или сдача занята.");return;}
    if (pending.Character!=character||pending.Campaign!=campaignLink.Campaign||
        pending.Deadline<=std::chrono::steady_clock::now()||
        !world.GetPartyService().IsInParty()||world.IsLoadingSave()||!SaveWorldReady()) return;
    auto* quest=Cast<TESQuest>(TESForm::GetById(pending.QuestForm));
    const auto item=FindSharedTurnIn(pending.QuestForm,pending.BaseForm);
    if (!quest||quest->currentInstanceID!=pending.Instance||!item||item->Consumed||
        world.GetOverlayService().IsPersonalQuest(pending.QuestForm)||
        QuestPickupPermit::Claim{item->Quest,item->Reference,item->Item,item->Alias}.Encode()!=pending.Claim.Encode()||
        !CanUseSharedTurnIn(world,*item,pending.QuestForm,pending.BaseForm,character)) return;
    auto* menu=MenuTopicManager::Get();
    if (!menu) return;
    // The holder alone runs the original dialogue and actual native removal.
    menu->ResumeQuestTurnIn(pending.Npc,pending.Info,pending.QuestForm,pending.BaseForm);
}

void PublishReconciledQuestInventory(World& world,PlayerCharacter& player)
{
    auto view=world.view<FormIdComponent,LocalComponent>();
    for (auto entity:view)
    {
        if (view.get<FormIdComponent>(entity).Id!=player.formID) continue;
        const auto& local=view.get<LocalComponent>(entity);
        RequestInventorySnapshot request{};
        request.ServerId=local.Id;request.OwnershipEpoch=local.OwnershipEpoch;
        request.CurrentInventory=player.GetActorInventory();
        world.GetTransport().Send(request);
        return;
    }
}

void RestoreOwnQuestPickups(World& world,const CampaignJournal::Journal& journal,uint64_t character)
{
    if (!SaveWorldReady()||!world.GetPartyService().IsInParty()) return;
    auto* player=PlayerCharacter::Get();
    auto* cell=player?player->GetParentCellEx():nullptr;
    auto* ui=UI::Get();
    static const BSFixedString dialogue("Dialogue Menu"), container("ContainerMenu"), inventory("InventoryMenu");
    if (!player||!ui||ui->GetMenuOpen(container)||ui->GetMenuOpen(inventory)) return;
    const auto history=ActiveQuestHistory(journal.Operations());
    const auto pickups=QuestPickupHistory(journal.Operations());
    if (!history||!pickups) return;
    for (const auto& item:*pickups)
    {
        auto run=history->find(item.Quest);
        const auto generation=run==history->end()?1u:run->second.Generation;
        const bool stopped=run!=history->end()&&run->second.Stopped;
        const bool restore=ShouldRestorePickup(item,character,generation,stopped);
        const bool reconcile=ShouldReconcileSpentPickup(item,character,generation);
        if (!restore&&!reconcile) continue;
        const auto questId=LocalQuestForm(item.Quest),referenceId=LocalQuestForm(item.Reference),baseId=LocalQuestForm(item.Item);
        if (!questId||!referenceId||!baseId) continue;
        auto* quest=Cast<TESQuest>(TESForm::GetById(questId));
        if (!quest||QuestService::IsNonSyncableQuest(quest)||
            world.GetOverlayService().IsPersonalQuest(questId)) continue;
        if (reconcile)
        {
            // Consume the recorded exact stack as soon as the authoritative
            // removal is known, before replay can clear its alias on stop.
            auto* base=Cast<TESBoundObject>(TESForm::GetById(baseId));
            GameId questNet{},refNet{};
            if (!base||!world.GetModSystem().GetServerModId(questId,questNet)||
                !world.GetModSystem().GetServerModId(referenceId,refNet)) continue;
            if (!FindSpentQuestStack(*player,base,questNet,refNet,item.Alias)||
                !attemptedQuestConsumption.emplace(questId,referenceId).second) continue;
            if (ReconcileSpentQuestStack(*player,base,questNet,refNet,item.Alias))
            {
                PublishReconciledQuestInventory(world,*player);
                spdlog::info("STM reconciled spent quest item {:X} for character {}",referenceId,character);
            }
            else
                spdlog::warn("STM could not reconcile spent quest item {:X}",referenceId);
            continue;
        }
        if (!cell||!cell->IsAttached()||ui->GetMenuOpen(dialogue)||player->IsInCombat()||quest->IsStopped()) continue;
        // Never add a base form as a substitute for a missing reference.
        // A native pickup is possible only if this exact alias is still
        // bound to its original, loaded, available world object.
        auto* ref=quest->GetAliasedRef(item.Alias);
        if (!ref||ref->formID!=referenceId||ref->IsTemporary()||ref->IsDisabled()||ref->IsDeleted()||
            !ref->baseForm||ref->baseForm->formID!=baseId||ref->GetParentCellEx()!=cell||!ref->GetNiNode()) continue;
        if (player->GetItemCountInInventory(ref->baseForm)!=0) continue;
        InventoryChangeEvent check(player->formID,Inventory::Entry{});
        CaptureQuestItem(ref->GetExtraDataList(),ref,ref->baseForm,check);
        GameId questNet{},refNet{};
        if (!world.GetModSystem().GetServerModId(questId,questNet)||
            !world.GetModSystem().GetServerModId(referenceId,refNet)||
            check.QuestId!=questNet||check.QuestRef!=refNet||check.QuestAlias!=item.Alias) continue;
        if (!attemptedQuestPickups.emplace(questId,referenceId).second) continue;
        auto* base=ref->baseForm;
        // The authoritative pickup already exists. Do not send a second
        // inventory delta or create a new campaign operation.
        ScopedInventoryOverride scope;
        player->PickUpObject(ref,1,false,0.f);
        if (player->GetItemCountInInventory(base)==1)
        {
            PublishReconciledQuestInventory(world,*player);
            spdlog::info("STM restored original quest pickup {:X} for character {}",referenceId,character);
        }
        else
            spdlog::warn("STM could not restore original quest pickup {:X}",referenceId);
    }
}

// Session-local suppression of repeated offers. ESS remains the source of
// applied stages; no receipt or save cursor is advanced here.
uint64_t questObservedLoadEpoch{};
struct RepeatJoin
{
    uint64_t Campaign{},Character{};
    uint32_t Run{},Before{};
    std::chrono::steady_clock::time_point RetryAt{};
    bool SawRunning{};
};
std::map<uint32_t,RepeatJoin> repeatJoins;
void ResetQuestReplayForLoad()
{
    const auto epoch=dragonLoadEpoch.load(std::memory_order_acquire);
    if (questObservedLoadEpoch==epoch) return;
    questObservedLoadEpoch=epoch;
    repeatJoins.clear();
    ResetKeywordRewards();
    ResetScriptedTravel();
    uniqueRewardEpoch.store(0,std::memory_order_release);uniqueRewardChoices.clear();
    attemptedQuestPickups.clear();
    attemptedQuestConsumption.clear();
    deferredQuestPickup.reset();
    pendingQuestReceipts.clear();
    deferredQuestTurnIn.reset();
    pendingWord.reset();
    wordBaseline.reset();
    attemptedWords.clear();
    wordAttemptCharacter=0;
    pendingDragonAbsorptions.clear();
    pending={};
    pendingSave={};
    nextPull={};
}

// Start only a repeatable quest with no variable alias definitions to
// resolve. Never replace aliases after startup: fragments/AI may have
// already acted on them. A generation-scoped bind must be confirmed before
// replay of later stages. Keep the guard across a temporary disconnect.
template<class CanDispatch>
void ReconcileRepeatStarts(World& world,const CampaignJournal::Journal& journal,
    uint64_t campaign,uint64_t character,CanDispatch canDispatch)
{
    if (!campaign||!character||!world.GetPartyService().IsInParty()||world.IsLoadingSave()||!SaveWorldReady()) return;
    const auto history=ActiveQuestHistory(journal.Operations());
    if (!history) return;
    const auto now=std::chrono::steady_clock::now();
    for (const auto& [key,run]:*history)
    {
        if (!run.Targets.empty()) continue; // wait for actual local selection; no forced aliases
        const auto id=LocalQuestForm(key);
        auto* quest=id?Cast<TESQuest>(TESForm::GetById(id)):nullptr;
        if (!quest||QuestService::IsNonSyncableQuest(quest)||
            (quest->flags&(TESQuest::RunOnce|TESQuest::Failed|TESQuest::AllowRepeatStages))||
            world.GetOverlayService().IsPersonalQuest(id)) continue;
        const auto instance=quest->currentInstanceID;
        auto pending=repeatJoins.find(id);
        if (pending!=repeatJoins.end()&&QuestRunMember::Bound(run.Members,character,pending->second.Run,instance))
        {
            repeatJoins.erase(pending);
            pending=repeatJoins.end();
        }
        // A server stop or next generation while the native start was
        // pending must not reassign the resulting instance to that new run.
        if (pending!=repeatJoins.end()&&(pending->second.Run!=run.Generation||
            pending->second.Character!=character||pending->second.Campaign!=campaign||run.Stopped))
            continue;
        if (run.Stopped||!run.StartedBy||!QuestRunMember::Instance(run.Members,run.StartedBy,run.Generation)||
            QuestRunMember::Instance(run.Members,character,run.Generation)||!canDispatch(id,instance)) continue;
        const auto targets=QuestService::CaptureQuestTargets(quest);
        if (targets=="!"||targets!=run.Targets) continue;
        if (pending==repeatJoins.end())
        {
            if (quest->IsStopped())
            {
                if (!targets.empty()||!QuestRunMember::CanStartMissing(run.Members,character,run.Generation,instance)) continue;
            }
            else
            {
                // Generation-one late adoption is limited to a real native
                // start with the exact accepted targets and start stage.
                // Never infer a later generation from coincidentally equal aliases.
                if (run.Generation!=1||run.Members.contains(character)||!quest->IsStageReached(run.StartStage)) continue;
            }
            pending=repeatJoins.emplace(id,RepeatJoin{campaign,character,run.Generation,instance,{},!quest->IsStopped()}).first;
        }
        auto& join=pending->second;
        if (quest->IsStopped())
        {
            if (join.SawRunning||instance!=join.Before||!targets.empty()||now<join.RetryAt||
                quest->getState()==TESQuest::State::WaitingPromotion) continue;
            join.RetryAt=now+std::chrono::seconds(5);
            ScopedQuestOverride scope;
            bool started=false;
            quest->EnsureQuestStarted(started,false);
            // Read back on the next confirmed pull; startup may be asynchronous.
            continue;
        }
        if (!join.SawRunning&&instance==join.Before) continue;
        join.SawRunning=true;
        if (!QuestRunMember::CanAttach(run.Members,character,run.Generation,join.Run,instance,true,false)) continue;
        GameId gameId{};
        if (!world.GetModSystem().GetServerModId(id,gameId)) continue;
        RequestQuestUpdate bind{};
        bind.Id=gameId;bind.Stage=quest->currentStage;bind.ClientQuestType=run.StartType;
        bind.Status=RequestQuestUpdate::Started;bind.BindInstance=true;bind.Instance=instance;
        bind.ExpectedRun=join.Run;bind.Targets=targets.c_str();
        world.GetTransport().Send(bind);
    }
}

template<class CanDispatch>
std::vector<NotifyQuestUpdate> MissingActiveQuestStages(World& world,
    const CampaignJournal::Journal& journal,uint64_t character,CanDispatch canDispatch)
{
    std::vector<NotifyQuestUpdate> updates;
    if (!world.GetPartyService().IsInParty()||world.IsLoadingSave()||!SaveWorldReady()) return updates;
    auto* player=PlayerCharacter::Get();
    if (!player) return updates;
    std::set<uint32_t> localQuests;
    for (const auto& objective:player->objectives)
        if (objective.instance&&objective.instance->quest)
            localQuests.insert(objective.instance->quest->formID);
    const auto history=ActiveQuestHistory(journal.Operations());
    const auto physical=QuestPhysicalStage::Read(journal.Operations());
    if (!history||!physical) return updates;
    for (const auto& [key,latest]:*history)
    {
        if (key.ends_with("~legacy")) continue;
        const auto form=TradeProperties::DecodeForm(QuestRunMember::BaseKey(key));
        if (!form||!form->Valid()) continue;
        auto* manager=ModManager::Get();
        auto* mod=manager?manager->GetByName(form->Plugin.c_str()):nullptr;
        if (!mod||!mod->IsLoaded()||(mod->IsLite()&&form->Local>0xFFF)) continue;
        const auto id=mod->GetFormId(form->Local);
        auto* quest=Cast<TESQuest>(TESForm::GetById(id));
        if (!quest||QuestService::IsNonSyncableQuest(quest)||
            world.GetOverlayService().IsPersonalQuest(id)||
            (quest->flags&TESQuest::Failed)||!canDispatch(id,quest->currentInstanceID)) continue;
        const bool sharedStart=latest.CanRestoreStart(character)&&
            (quest->flags&TESQuest::RunOnce)&&
            !(quest->flags&(TESQuest::Completed|TESQuest::Failed|TESQuest::AllowRepeatStages));
        GameId gameId{};
        if (!world.GetModSystem().GetServerModId(id,gameId)) continue;
        if (quest->IsStopped())
        {
            if (sharedStart)
            {
                NotifyQuestUpdate start{};
                start.Id=gameId;start.Stage=QuestPhysicalStage::Has(*physical,key,latest.Generation,latest.StartStage)?0:latest.StartStage;
                start.ClientQuestType=latest.StartType;
                start.Status=NotifyQuestUpdate::Started;start.SharedRun=latest.Generation;
                start.Speaker=world.GetPartyService().GetLeaderPlayerId();
                updates.push_back(start);
            }
            continue; // wait for a real native start before replaying stages
        }
        const auto targets=QuestService::CaptureQuestTargets(quest);
        if (targets=="!") continue;
        const auto instance=quest->currentInstanceID;
        const QuestReplayState* selected=&latest;
        bool bound=QuestRunMember::Bound(latest.Members,character,latest.Generation,instance);
        if (!bound)
            for (auto it=latest.Prior.rbegin();it!=latest.Prior.rend();++it)
                if (QuestRunMember::Bound(latest.Members,character,it->Generation,instance))
                { selected=&*it;bound=true;break; }
        const auto& state=*selected;
        if (targets!=state.Targets) continue;
        if (!(quest->flags&TESQuest::RunOnce)&&!bound) continue;
        if (!bound&&!localQuests.contains(id)&&!sharedStart) continue;
        if (!bound&&(state.Generation!=1||(state.Stopped&&!state.CompletedRunOnce)||
            latest.Members.contains(character))) continue;
        if (state.CompletedRunOnce && (!(quest->flags & TESQuest::RunOnce) ||
            (quest->flags & (TESQuest::Completed|TESQuest::Failed|TESQuest::AllowRepeatStages)))) continue;
        // Replay only recorded native stages. Do not synthesize a stop, reward,
        // completed flag or inventory grant from a completion record.
        auto stages=state.Stages;
        if (bound&&state.StartedBy&&!quest->IsStageReached(state.StartStage))
            stages.insert(stages.begin(),{state.StartStage,state.StartType});
        bool blocked=false;
        for (const auto& [stage,type]:stages)
        {
            // An acknowledged historical arrival is not proof in an older
            // loaded ESS. Only native local progress and fragment completion
            // release this barrier; never synthesize its stage.
            if(QuestPhysicalStage::Blocks(*physical,key,state.Generation,stage,quest->IsStageReached(stage),
                QuestPhysicalStagePending(id,instance,stage))) {blocked=true;break;}
            if (quest->IsStageReached(stage)) continue;
            NotifyQuestUpdate update{};
            update.Id=gameId;update.Stage=stage;update.ClientQuestType=type;
            update.Status=NotifyQuestUpdate::StageUpdate;
            update.SharedRun=state.Generation;
            update.CheckInstance=bound;
            update.ExpectedInstance=bound?instance:0;
            update.Targets=state.Targets.c_str();
            update.Speaker=world.GetPartyService().GetLeaderPlayerId();
            updates.push_back(update);
        }
        // Queue an observed stop after the bound run's stages. Existing
        // native scene and fragment barriers still govern actual execution.
        if (!blocked&&bound&&state.Stopped&&!state.CompletedRunOnce)
        {
            NotifyQuestUpdate stop{};
            stop.Id=gameId;stop.Stage=state.StopStage;stop.ClientQuestType=state.StopType;
            stop.Status=NotifyQuestUpdate::Stopped;stop.SharedRun=state.Generation;
            stop.Targets=state.Targets.c_str();stop.CheckInstance=true;stop.ExpectedInstance=instance;
            stop.Speaker=world.GetPartyService().GetLeaderPlayerId();
            updates.push_back(stop);
        }
    }
    return updates;
}
}

uint32_t ExpectedRepeatRun(uint32_t form) noexcept
{
    const auto it=repeatJoins.find(form);
    return it==repeatJoins.end()?0:it->second.Run;
}

// Called by the world runner after the exact native script call returned.
// Resolve the actor anew: the VM thread never retains a world/actor pointer.
void RecordNativeDragonAbsorption(uint32_t form) noexcept
{
    if (!CampaignRewardPolicy::SupplementDragonSouls) return;
    try
    {
        auto& world=World::Get();
        if (world.IsLoadingSave()||!SaveWorldReady()||
            !world.GetTransport().IsOnline()||!world.GetPartyService().IsInParty()) return;
        const auto identity=CampaignAccess::Current(world);
        if (!identity) return;
        auto* dragon=Cast<Actor>(TESForm::GetById(form));
        if (!dragon||!dragon->IsDragon()||!dragon->IsDead()) return;
        if (dragon->IsTemporary())
        {
            spdlog::debug("STM dragon absorption {:X}: native award observed, dynamic life identity not durable",form);
            return;
        }
        std::vector<InventoryEvidence::PluginSlot> slots;
        auto* mods=ModManager::Get();
        if (!mods) return;
        for (const auto* mod:mods->mods)
        {
            if (!mod||!mod->IsLoaded()) continue;
            const auto* end=std::find(mod->filename,mod->filename+sizeof(mod->filename),'\0');
            if (end!=mod->filename+sizeof(mod->filename))
                slots.push_back({mod->GetId(),mod->IsLite(),std::string(mod->filename,end)});
        }
        const auto stable=InventoryEvidence::Resolve(form,slots);
        if (!stable) return;
        const TradeProperties::FormKey key{stable->Plugin,stable->Local};
        DragonAbsorption::Receipt receipt{key.Encode(),identity->Save.Character};
        if (!receipt.Valid()) return;
        const auto receipts=DragonAbsorption::Read(campaignLink.Journal.Operations());
        if (!receipts||receipts->contains(receipt.Encode())) return;
        if (std::any_of(pendingDragonAbsorptions.begin(),pendingDragonAbsorptions.end(),
                [&](const auto& entry){return entry.Receipt.Encode()==receipt.Encode();})) return;
        uint32_t serverId{};
        const auto view=world.view<FormIdComponent>();
        for (const auto entity:view)
            if (view.get<FormIdComponent>(entity).Id==form)
            {
                const auto id=Utils::GetServerId(entity);
                if (id) serverId=*id;
                break;
            }
        if (pendingDragonAbsorptions.size()>=16) return;
        // A disappeared network entity is not a new identity. The server
        // may already have the exact stable death cohort in its journal.
        pendingDragonAbsorptions.push_back({receipt,identity->Save.Campaign,serverId,
            std::chrono::steady_clock::now()+std::chrono::minutes(2)});
        nextPull={};
        spdlog::info("STM native dragon absorption {:X}, character {}",form,receipt.Character);
    }
    catch (...) {spdlog::warn("STM native dragon absorption observation failed");}
}

uint64_t ScriptedTravelEpoch() noexcept
{return dragonLoadEpoch.load(std::memory_order_acquire);}

bool AtScriptedTravelTarget(TESObjectREFR* target) noexcept
{
    auto* player=PlayerCharacter::Get();
    if (!player||!target||target->IsDeleted()||!target->GetParentCellEx()||!player->GetParentCellEx()) return false;
    auto* world=target->GetWorldSpace();
    if (world?player->GetWorldSpace()!=world:player->GetParentCellEx()!=target->GetParentCellEx()) return false;
    return glm::distance(static_cast<const glm::vec3&>(player->position),
                         static_cast<const glm::vec3&>(target->position))<=128.f;
}

void ObserveScriptedTravel(ScriptedMoveObservation observation) noexcept
{
    try
    {
        if (!World::Exists()||World::Get().IsLoadingSave()) return;
        World::Get().GetRunner().Queue([o=std::move(observation)]() mutable {RememberScriptedTravel(std::move(o));});
    }
    catch (...) {}
}

void QueueQuestItemReceipt(World& world,const RequestInventoryChanges& message) noexcept
{
    try
    {
        if (!world.GetPartyService().IsInParty()||
            world.IsLoadingSave()||!SaveWorldReady()||message.Drop||!message.Item.IsQuestItem||
            !message.QuestId||!message.QuestRef||
            (message.Item.Count!=1&&message.Item.Count!=-1)) return;
        const auto identity=CampaignAccess::LocalIdentity(world);
        if (!identity) return;
        auto& mods=world.GetModSystem();
        QuestPickupPermit::Receipt receipt{
            {StablePickupKey(mods.GetGameId(message.QuestId)),StablePickupKey(mods.GetGameId(message.QuestRef)),
             StablePickupKey(mods.GetGameId(message.Item.BaseId)),message.QuestAlias},
            message.Item.Count,message.QuestInitialEvidence&&message.Item.Count==1};
        if (!receipt.Valid()||QuestPickupPermit::Acknowledged(campaignLink.Journal.Operations(),receipt,identity->Save.Character)) return;
        const auto encoded=receipt.Encode();
        for (const auto& entry:pendingQuestReceipts)
            if (entry.Campaign==identity->Save.Campaign&&entry.Character==identity->Save.Character&&
                entry.Receipt.Encode()==encoded) return;
        if (pendingQuestReceipts.size()>=64)
        {spdlog::warn("STM pending quest item receipt capacity reached");return;}
        pendingQuestReceipts.push_back({std::move(receipt),identity->Save.Campaign,identity->Save.Character});
        nextPull={};
    }
    catch (...) {spdlog::warn("STM could not queue quest item receipt");}
}

void QuestRewardInventoryTouched(TESObjectREFR* owner,uint32_t id) noexcept
{
    if (!owner||owner!=PlayerCharacter::Get()||!id) return;
    try
    {
        std::lock_guard lock(rewardVersionMutex);
        auto it=rewardVersions.find(id);
        if (it!=rewardVersions.end()) it->second=++rewardVersionCounter;
    }
    catch (...) {}
}
QuestRewardAddObservation::QuestRewardAddObservation(TESObjectREFR* owner,TESBoundObject* item,ExtraDataList* extra,
    int32_t count,TESObjectREFR* oldOwner) noexcept
{
    if (!owner||owner!=PlayerCharacter::Get()||!item) return;
    Local=true;
    if (rewardAddDepth++!=0) return;
    QuestRewardInventoryTouched(owner,item->formID);
    try
    {
        const auto epoch=dragonLoadEpoch.load(std::memory_order_acquire);
        const auto quest=NativeUniqueRewardQuest();
        if (!quest||!NativeUniqueRewardStage()||count!=1||oldOwner||extra||
            ScopedInventoryOverride::IsOverriden()||uniqueRewardEpoch.load(std::memory_order_acquire)!=epoch||
            !World::Exists()||World::Get().IsLoadingSave()||!HasRewardKeyword(item)||
            owner->GetItemCountInInventory(item)!=0) return;
        Version=TrackRewardVersion(item->formID);
        Owner=owner;Item=item;Quest=quest;Stage=NativeUniqueRewardStage();
        Instance=NativeUniqueRewardInstance();Epoch=epoch;
    }
    catch (...) {}
}
QuestRewardAddObservation::~QuestRewardAddObservation() noexcept
{
    if (!Local) return;
    --rewardAddDepth;
    if (!Owner||!Item) return;
    try
    {
        if (Epoch!=dragonLoadEpoch.load(std::memory_order_acquire)||!CurrentRewardVersion(Item->formID,Version)||
            !World::Exists()||World::Get().IsLoadingSave()||Owner!=PlayerCharacter::Get()||
            !CleanReward(*PlayerCharacter::Get(),Item)) return;
        World::Get().GetRunner().Queue([quest=Quest,stage=Stage,instance=Instance,item=Item->formID,epoch=Epoch,version=Version]{
            RememberKeywordReward(quest,stage,instance,item,epoch,version);
        });
    }
    catch (...) {}
}
void QuestRewardStageDone(uint32_t quest,uint16_t stage) noexcept
{
    for (auto& [key,r]:keywordObservations)
        if (r.Quest==quest&&r.Key.Stage==stage&&r.Epoch==dragonLoadEpoch.load(std::memory_order_acquire))
            r.FragmentDone=true;
}

bool CaptureUniqueQuestAward(uint32_t) noexcept
{
    // The per-item reward interception was superseded by keyword-based
    // classification. Never suppress a native award using the old catalog.
    // Previously saved DA01 receipts/choices are still read by the recovery path.
    return false;
}

bool DeferSharedQuestTurnIn(uint32_t questId,uint32_t baseId,uint32_t npc,uint32_t info) noexcept
{
    try
    {
        auto& world=World::Get();
        if (!questDialogueThread||!world.GetTransport().IsOnline()||!world.GetPartyService().IsInParty()||
            world.IsLoadingSave()||!SaveWorldReady()||world.GetOverlayService().IsPersonalQuest(questId)) return false;
        const auto identity=CampaignAccess::Current(world);
        if (!identity) return false;
        auto* quest=Cast<TESQuest>(TESForm::GetById(questId));
        if (!quest||!(quest->flags&TESQuest::RunOnce)||QuestService::IsNonSyncableQuest(quest)) return false;
        const auto item=FindSharedTurnIn(questId,baseId);
        if (!item) return false; // unrelated, personal or ambiguous native condition
        if (item->Consumed)
        {Utils::ShowHudMessage("Общий квестовый предмет уже сдан.");return true;}
        const auto character=identity->Save.Character;
        if (!CanUseSharedTurnIn(world,*item,questId,baseId,character))
        {Utils::ShowHudMessage("Не подтверждён исходный квестовый предмет.");return true;}
        const auto now=std::chrono::steady_clock::now();
        if (deferredQuestTurnIn&&deferredQuestTurnIn->Deadline>now) return true;
        deferredQuestTurnIn=DeferredQuestTurnIn{{item->Quest,item->Reference,item->Item,item->Alias},
            questId,baseId,npc,info,quest->currentInstanceID,identity->Save.Campaign,character,0,now+std::chrono::seconds(8)};
        nextPull={};
        return true;
    }
    catch (...) {return false;}
}

bool DeferSharedQuestPickup(TESObjectREFR* ref,uint8_t unk,char processing) noexcept
{
    try
    {
        auto& world=World::Get();
        if (!world.GetTransport().IsOnline()||!world.GetPartyService().IsInParty()||
            world.IsLoadingSave()||!SaveWorldReady()) return false;
        auto pickup=DescribeQuestPickup(world,ref);
        if (!pickup) return false; // personal/non-identified native path
        const auto now=std::chrono::steady_clock::now();
        if (deferredQuestPickup&&deferredQuestPickup->Deadline>now) return true;
        pickup->Unk=unk;pickup->DefaultProcessing=processing;pickup->Deadline=now+std::chrono::seconds(4);
        deferredQuestPickup=std::move(*pickup);nextPull={};
        return true;
    }
    catch (...) {return false;}
}

void QuestService::PollCampaign(const UpdateEvent&) noexcept
{
    ResetQuestReplayForLoad();
    questDialogueThread=true;
    ObserveSleepArrivals(m_world);
    if (CampaignRewardPolicy::SupplementDragonSouls&&!m_world.IsLoadingSave()&&SaveWorldReady()) RefreshCampaignDragonAwards();
    if (!m_world.GetTransport().IsOnline()||!m_world.GetPartyService().IsInParty())
    {
        uniqueRewardEpoch.store(0,std::memory_order_release);uniqueRewardChoices.clear();
        deferredQuestPickup.reset();deferredQuestTurnIn.reset();
        attemptedQuestPickups.clear();
        attemptedQuestConsumption.clear();
        wordBaseline.reset();
        pendingWord.reset(); // a transient peer ID is not durable identity
        // Keep already observed native receipts across a brief transport
        // break. ResetQuestReplayForLoad still clears them on a new ESS.
    }
    if (!m_world.GetTransport().IsOnline()||m_world.IsLoadingSave()||!SaveWorldReady())
    {
        uniqueRewardEpoch.store(0,std::memory_order_release);
        wordBaseline.reset();pendingWord.reset();attemptedWords.clear();wordAttemptCharacter=0;
        deferredQuestPickup.reset();deferredQuestTurnIn.reset();
    }
    if (!m_world.GetTransport().IsOnline()||m_world.IsLoadingSave()||!SaveWorldReady())
    {
        pending={};
        nextPull={};
        return;
    }
    try
    {
        if (campaignLink.Start(false)!=CampaignReplication::Link::Mode::Ready) return;
        const auto now=std::chrono::steady_clock::now();
        const bool newClaim=(deferredQuestPickup && !deferredQuestPickup->Nonce) ||
            (deferredQuestTurnIn && !deferredQuestTurnIn->Nonce);
        if (now<nextPull || (!newClaim && pending.Nonce && now<pendingDeadline)) return;
        nextPull=now+std::chrono::seconds(2);
        const auto credential=CampaignIdentity::ReadCredential(campaignLink.Root);
        const auto stamp=credential ? ReadSave(m_world,*credential,true) : std::nullopt;
        if (!stamp)
        {
            pending={};
            if (!saveWarning) {spdlog::warn("STM campaign replication suspended: missing or incompatible save identity");saveWarning=true;}
            return;
        }
        saveWarning=false;
        pendingLoadEpoch=dragonLoadEpoch.load(std::memory_order_acquire);
        pendingDeadline=now+std::chrono::seconds(8);
        pending={campaignLink.Campaign,++nonce,campaignLink.Profile,campaignLink.Journal.Current()};
        RequestCampaignPull request;
        request.Campaign=pending.Campaign;request.Nonce=pending.Nonce;
        request.Profile=pending.Profile.c_str();request.Revision=pending.Base.Revision;request.Tag=pending.Base.Tag;
        pendingSave=*stamp;
        request.Character=stamp->Character;request.SaveRevision=stamp->Applied.Revision;request.SaveTag=stamp->Applied.Tag;
        request.Credential=credential->Secret.c_str();
        request.Checkpoint=NextCheckpoint(*stamp).c_str();
        SendQuestReceipt(*stamp,request);
        SendScriptedTravel(m_world,*stamp,request);
        SendKeywordRewards(m_world,*stamp,request);
        SendUniqueReward(m_world,*stamp,request);
        ObserveLearnedWord(m_world,stamp->Character,request);
        SendDragonAbsorption(*stamp,request);
        if (deferredQuestPickup)
        {
            if (deferredQuestPickup->Deadline<=std::chrono::steady_clock::now()||deferredQuestPickup->Character!=stamp->Character)
                deferredQuestPickup.reset();
            else
            {
                request.PickupClaim=deferredQuestPickup->Claim.Encode().c_str();
                deferredQuestPickup->Nonce=request.Nonce;
            }
        }
        else if (deferredQuestTurnIn)
        {
            if (deferredQuestTurnIn->Deadline<=std::chrono::steady_clock::now()||
                deferredQuestTurnIn->Character!=stamp->Character||deferredQuestTurnIn->Campaign!=stamp->Campaign)
                deferredQuestTurnIn.reset();
            else
            {
                request.PickupClaim=("turnin "+deferredQuestTurnIn->Claim.Encode()).c_str();
                deferredQuestTurnIn->Nonce=request.Nonce;
            }
        }
        if (!m_world.GetTransport().Send(request)) pending={};
    }
    catch (...) { campaignLink.Fail();spdlog::error("Campaign replica initialization/poll failed"); }
}
void QuestService::OnCampaignRecord(const NotifyCampaignRecord& packet) noexcept
{
    if (!m_world.GetTransport().IsOnline() || campaignLink.State()!=CampaignReplication::Link::Mode::Ready ||
        !pending.Nonce || packet.Nonce!=pending.Nonce || packet.Campaign!=pending.Campaign) return;
    if (m_world.IsLoadingSave()||pendingLoadEpoch!=dragonLoadEpoch.load(std::memory_order_acquire))
    {
        pending={};
        return;
    }
    try
    {
        const auto credential=CampaignIdentity::ReadCredential(campaignLink.Root);
        const auto stamp=credential ? ReadSave(m_world,*credential,false) : std::nullopt;
        if (!stamp || *stamp!=pendingSave) {pending={};return;}
        CampaignReplication::Reply reply;
        reply.Campaign=packet.Campaign;reply.Nonce=packet.Nonce;
        reply.Base={packet.BaseRevision,packet.BaseTag};reply.Target={packet.TargetRevision,packet.TargetTag};
        reply.Total=packet.Total;
        if (packet.State>3 || packet.Row.size()>4096) {campaignLink.Fail();return;}
        reply.State=static_cast<CampaignReplication::Status>(packet.State);
        reply.Row.assign(packet.Row.data(),packet.Row.size());
        // The old connection can still hold the character while the guest
        // reconnects. No reply data is accepted until admission succeeds.
        if (reply.State==CampaignReplication::Status::Unavailable)
        {
            pending={};
            nextPull=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            return;
        }
        auto result=CampaignReplication::Receive(campaignLink.Journal,pending,reply);
        pending={};
        if (result==CampaignJournal::Result::Committed) nextPull={};
        else if (result!=CampaignJournal::Result::Duplicate)
        {
            campaignLink.Fail();
            spdlog::error("Campaign replica refused server history; restart after resolving configuration/history");
        }
        if (result==CampaignJournal::Result::Committed||result==CampaignJournal::Result::Duplicate)
        {
            ResolveQuestPickup(m_world,packet.Nonce,packet.PickupPermit,stamp->Character);
            ResolveQuestTurnIn(m_world,packet.Nonce,packet.PickupPermit,stamp->Character);
        }
        if ((result==CampaignJournal::Result::Committed||result==CampaignJournal::Result::Duplicate)&&
            campaignLink.Journal.Current().Revision==reply.Total)
        {
            // Only after the server confirmed the complete prefix for this save.
            try
            {
                ApplySharedDragonGrants(m_world,campaignLink.Journal,stamp->Character);
                UpdateUniqueRewards(m_world,*stamp);
                UpdateKeywordRewards(m_world,*stamp);
                RestoreLearnedWords(m_world,campaignLink.Journal,stamp->Character);
                RestoreOwnQuestPickups(m_world,campaignLink.Journal,stamp->Character);
                // Reconcile against observed native state, not a permanent
                // "offered" set. Do not enqueue duplicates while the native
                // handler is already waiting on a stage, scene or promotion.
                const auto canDispatch=[this](uint32_t id,uint32_t instance){
                    if (m_heldStages.find(id)!=m_heldStages.end()||
                        m_afterHeldStages.find(id)!=m_afterHeldStages.end()) return false;
                    const auto conflict=m_branchConflicts.find(id);
                    return conflict==m_branchConflicts.end()||conflict->second!=instance;
                };
                ReconcileRepeatStarts(m_world,campaignLink.Journal,stamp->Campaign,stamp->Character,canDispatch);
                for (const auto& update:MissingActiveQuestStages(m_world,campaignLink.Journal,stamp->Character,canDispatch))
                    ReceiveQuestProgress(update);
                ApplyScriptedTravel(m_world,*stamp,canDispatch);
            }
            catch (...) {spdlog::warn("STM active quest catch-up failed");}
        }
    }
    catch (...) {campaignLink.Fail();spdlog::error("Campaign replica import failed");}
}

void QuestService::PollJournal(const UpdateEvent&) noexcept
{
    if (!m_world.GetPartyService().IsInParty() || !m_world.GetTransport().IsConnected())
    {
        m_journalStream = 0;
        m_journalConfirmed = 0;
        m_nextJournalPoll = {};
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < m_nextJournalPoll)
        return;
    m_nextJournalPoll = now + std::chrono::seconds(4);
    RequestQuestJournal pull;
    pull.Stream = m_journalStream;
    pull.Sequence = m_journalConfirmed;
    m_world.GetTransport().Send(pull);
}

void QuestService::OnJournalRecord(const NotifyQuestJournal& record) noexcept
{
    if (!record.Stream || !m_world.GetPartyService().IsInParty() || record.Sequence > record.Total)
        return;
    try
    {
        if (!record.Sequence)
        {
            const auto localCount = QuestJournalDisk::Contiguous(Mirror(), record.Stream);
            if (localCount > record.Total)
                return;
            m_journalStream = record.Stream;
            m_journalConfirmed = localCount;
            m_nextJournalPoll = {};
            return;
        }
        if (record.Stream != m_journalStream || record.Sequence != m_journalConfirmed + 1)
            return;
        if (!QuestJournalDisk::Store(Mirror(), record.Stream, record.Sequence, {record.Row.data(), record.Row.size()}))
        {
            spdlog::error("Cannot store co-op quest journal record {}", record.Sequence);
            return;
        }
        ++m_journalConfirmed;
        if (record.Sequence < record.Total && record.Sequence % 8 == 0)
            m_nextJournalPoll = {};
    }
    catch (const std::exception& e) { spdlog::error("Mirroring co-op quest journal: {}", e.what()); }
}
