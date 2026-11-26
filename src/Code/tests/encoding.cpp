#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Allocator.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <optional>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "StringCache.h"
#include "Messages/StringCacheUpdate.h"

#include <catch2/catch.hpp>

#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Structs/Vector2_NetQuantize.h>

#include <TiltedCore/Math.hpp>
#include <TiltedCore/Platform.hpp>

using namespace TiltedPhoques;

TEST_CASE("Encoding factory", "[encoding.factory]")
{
    Buffer buff(1000);

    {
        AuthenticationRequest request;
        request.Token = "TesSt";

        Buffer::Writer writer(&buff);
        request.Serialize(writer);

        Buffer::Reader reader(&buff);

        const ClientMessageFactory factory;
        auto pMessage = factory.Extract(reader);

        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == request.GetOpcode());

        auto pRequest = CastUnique<AuthenticationRequest>(std::move(pMessage));
        REQUIRE(pRequest->Token == request.Token);
    }

    {
        PartyAcceptInviteRequest request;
        request.InviterId = 123456;

        Buffer::Writer writer(&buff);
        request.Serialize(writer);

        Buffer::Reader reader(&buff);

        const ClientMessageFactory factory;
        auto pMessage = factory.Extract(reader);

        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == request.GetOpcode());

        auto pRequest = CastUnique<PartyAcceptInviteRequest>(std::move(pMessage));
        REQUIRE(pRequest->InviterId == request.InviterId);
    }
}

TEST_CASE("Static structures", "[encoding.static]")
{
    GIVEN("GameId")
    {
        GameId sendObjects, recvObjects;
        sendObjects.ModId = 1456987;
        sendObjects.BaseId = 0x789654;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }

    GIVEN("Vector3_NetQuantize")
    {
        Vector3_NetQuantize sendObjects, recvObjects;
        sendObjects.x = 142.56f;
        sendObjects.y = 45687.7f;
        sendObjects.z = -142.56f;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }

    GIVEN("Vector2_NetQuantize")
    {
        Vector2_NetQuantize sendObjects, recvObjects;
        sendObjects.x = 1000.89f;
        sendObjects.y = -485632.75f;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }

    GIVEN("Rotator2_NetQuantize")
    {
        Rotator2_NetQuantize sendObjects, recvObjects;
        sendObjects.x = 1.89f;
        sendObjects.y = TiltedPhoques::Pi * 2.0f;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }

    GIVEN("Rotator2_NetQuantize needing wrap")
    {
        // This test is a bit dangerous as floating errors can lead to sendObjects != recvObjects but the difference is minuscule so we don't care abut such cases
        Rotator2_NetQuantize sendObjects, recvObjects;
        sendObjects.x = -1.87f;
        sendObjects.y = static_cast<float>(TiltedPhoques::Pi) * 18.0f + 3.6f;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendObjects.Serialize(writer);

            Buffer::Reader reader(&buff);
            recvObjects.Deserialize(reader);

            REQUIRE(sendObjects == recvObjects);
        }
    }
}

TEST_CASE("Differential structures", "[encoding.differential]")
{
    GIVEN("Full ActionEvent")
    {
        ActionEvent sendAction, recvAction;

        sendAction.ActionId = 42;
        sendAction.State1 = 6547;
        sendAction.Tick = 48;
        sendAction.ActorId = 12345678;
        sendAction.EventName = "test";
        sendAction.IdleId = 87964;
        sendAction.State2 = 8963;
        sendAction.TargetEventName = "toast";
        sendAction.TargetId = 963741;
        sendAction.Type = 4;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendAction.GenerateDifferential(recvAction, writer);

            Buffer::Reader reader(&buff);
            recvAction.ApplyDifferential(reader);

            REQUIRE(sendAction == recvAction);
        }

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendAction.EventName = "Plot twist !";

            sendAction.GenerateDifferential(recvAction, writer);

            Buffer::Reader reader(&buff);
            recvAction.ApplyDifferential(reader);

            REQUIRE(sendAction == recvAction);
        }
    }

    GIVEN("A single cached event name")
    {
        ActionEvent sendAction, recvAction;

        TP_UNUSED(StringCache::Get().Add("test"))

        sendAction.ActionId = 42;
        sendAction.State1 = 6547;
        sendAction.Tick = 48;
        sendAction.ActorId = 12345678;
        sendAction.EventName = "test";
        sendAction.IdleId = 87964;
        sendAction.State2 = 8963;
        sendAction.TargetEventName = "toast";
        sendAction.TargetId = 963741;
        sendAction.Type = 4;

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendAction.GenerateDifferential(recvAction, writer);

            Buffer::Reader reader(&buff);
            recvAction.ApplyDifferential(reader);

            REQUIRE(sendAction == recvAction);
        }

        {
            Buffer buff(1000);
            Buffer::Writer writer(&buff);

            sendAction.EventName = "Plot twist !";

            sendAction.GenerateDifferential(recvAction, writer);

            Buffer::Reader reader(&buff);
            recvAction.ApplyDifferential(reader);

            REQUIRE(sendAction == recvAction);
        }
    }

    GIVEN("Full Mods")
    {
        Mods sendMods, recvMods;

        Buffer buff(1000);
        Buffer::Writer writer(&buff);

        sendMods.ModList.push_back({"Hello", 42});
        sendMods.ModList.push_back({"Hi", 14});
        sendMods.ModList.push_back({"Test", 8});
        sendMods.ModList.push_back({"Toast", 49});

        sendMods.Serialize(writer);

        Buffer::Reader reader(&buff);
        recvMods.Deserialize(reader);

        REQUIRE(sendMods == recvMods);
    }

    GIVEN("AnimationVariables")
    {
        AnimationVariables vars, recvVars;
 
        vars.Booleans.resize(76);
        String testString("\xDE\xAD\xBE\xEF"
                          "\xDE\xAD\xBE\xEF\x76\xB");
        vars.String_to_VectorBool(testString, vars.Booleans);

        vars.Floats.push_back(1.f);
        vars.Floats.push_back(7.f);
        vars.Floats.push_back(12.f);
        vars.Floats.push_back(0.f);
        vars.Floats.push_back(145.f);
        vars.Floats.push_back(100.f);
        vars.Floats.push_back(-1.f);

        vars.Integers.push_back(0);
        vars.Integers.push_back(12000);
        vars.Integers.push_back(06);
        vars.Integers.push_back(7778);
        vars.Integers.push_back(41104539);

        Buffer buff(1000);
        {
            Buffer::Writer writer(&buff);

            vars.GenerateDiff(recvVars, writer);

            Buffer::Reader reader(&buff);
            recvVars.ApplyDiff(reader);

            REQUIRE(vars.Booleans == recvVars.Booleans);
            REQUIRE(vars.Floats == recvVars.Floats);
            REQUIRE(vars.Integers == recvVars.Integers);
        }

        vars.Booleans.resize(33);
        vars.Booleans[16] = false;
        vars.Booleans[17] = false;
        vars.Booleans[18] = false;
        vars.Booleans[19] = false;
        vars.Floats[3] = 42.f;
        vars.Integers[0] = 18;
        vars.Integers[3] = 0;

        {
            Buffer::Writer writer(&buff);

            vars.GenerateDiff(recvVars, writer);

            Buffer::Reader reader(&buff);
            recvVars.ApplyDiff(reader);

            REQUIRE(vars.Booleans == recvVars.Booleans);
            REQUIRE(vars.Floats == recvVars.Floats);
            REQUIRE(vars.Integers == recvVars.Integers);
        }
    }
}

TEST_CASE("Packets", "[encoding.packets]")
{
    SECTION("AuthenticationRequest")
    {
        Buffer buff(1000);

        AuthenticationRequest sendMessage, recvMessage;
        sendMessage.Token = "TesSt";
        sendMessage.UserMods.ModList.push_back({"Hello", 42});
        sendMessage.UserMods.ModList.push_back({"Hi", 14});
        sendMessage.UserMods.ModList.push_back({"Test", 8});
        sendMessage.UserMods.ModList.push_back({"Toast", 49});

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }

    SECTION("AuthenticationResponse")
    {
        Buffer buff(1000);

        AuthenticationResponse sendMessage, recvMessage;
        sendMessage.Type = AuthenticationResponse::ResponseType::kAccepted;
        sendMessage.UserMods.ModList.push_back({"Hello", 42});
        sendMessage.UserMods.ModList.push_back({"Hi", 14});
        sendMessage.UserMods.ModList.push_back({"Test", 8});
        sendMessage.UserMods.ModList.push_back({"Toast", 49});

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }

    SECTION("AssignCharacterRequest")
    {
        Buffer buff(1000);

        ActionEvent sendAction;
        sendAction.ActionId = 42;
        sendAction.State1 = 6547;
        sendAction.Tick = 48;
        sendAction.ActorId = 12345678;
        sendAction.EventName = "test";
        sendAction.IdleId = 87964;
        sendAction.State2 = 8963;
        sendAction.TargetEventName = "toast";
        sendAction.TargetId = 963741;
        sendAction.Type = 4;

        AssignCharacterRequest sendMessage, recvMessage;
        sendMessage.Cookie = 14523698;
        sendMessage.AppearanceBuffer = "toto";
        sendMessage.CellId.BaseId = 45;
        sendMessage.FormId.ModId = 48;
        sendMessage.ReferenceId.BaseId = 456799;
        sendMessage.ReferenceId.ModId = 4079;
        sendMessage.LatestAction = sendAction;
        sendMessage.Position.x = -452.4f;
        sendMessage.Position.y = 452.4f;
        sendMessage.Position.z = 125452.4f;
        sendMessage.Rotation.x = -1.87f;
        sendMessage.Rotation.y = 45.35f;

        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }

    GIVEN("ClientReferencesMoveRequest")
    {
        ClientReferencesMoveRequest sendMessage;
        sendMessage.SpatialEpoch = 513;
        sendMessage.Tick = 1712345678901;
        auto& move = sendMessage.Updates[1];
        move.OwnershipEpoch = 42;
        move.Position.x = 1234.5f;
        move.Direction = 0.75f;
        move.HasLookAt = true;
        move.LookOffset.x = -320.f;
        move.LookOffset.y = 215.f;
        move.LookOffset.z = -48.f;

        AnimationVariables vars;
        vars.Booleans.resize(76);
        String testString("\xDE\xAD\xBE\xEF\x76\xB");
        vars.String_to_VectorBool(testString, vars.Booleans);

        vars.Floats.push_back(1.f);
        vars.Floats.push_back(7.f);
        vars.Floats.push_back(12.f);
        vars.Floats.push_back(0.f);
        vars.Floats.push_back(145.f);
        vars.Floats.push_back(100.f);
        vars.Floats.push_back(-1.f);

        vars.Integers.push_back(0);
        vars.Integers.push_back(12000);
        vars.Integers.push_back(06);
        vars.Integers.push_back(7778);
        vars.Integers.push_back(41104539);

        move.Variables = vars;

        Buffer buff(1000);
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        const ClientMessageFactory factory;
        auto decoded = factory.Extract(reader);
        REQUIRE(decoded);
        REQUIRE(decoded->GetOpcode() == sendMessage.GetOpcode());
        const auto received = CastUnique<ClientReferencesMoveRequest>(std::move(decoded));
        REQUIRE(received->SpatialEpoch == sendMessage.SpatialEpoch);
        REQUIRE(received->Tick == sendMessage.Tick);
        REQUIRE(received->Updates.size() == 1);
        REQUIRE(received->Updates[1] == sendMessage.Updates[1]);

        ServerReferencesMoveRequest relay;
        relay.SpatialEpoch = 721;
        relay.Tick = sendMessage.Tick;
        relay.Updates[1] = move;
        Buffer response(1000);
        Buffer::Writer responseWriter(&response);
        relay.Serialize(responseWriter);
        Buffer::Reader responseReader(&response);
        const ServerMessageFactory serverFactory;
        auto relayed = serverFactory.Extract(responseReader);
        REQUIRE(relayed);
        REQUIRE(relayed->GetOpcode() == relay.GetOpcode());
        const auto receivedRelay = CastUnique<ServerReferencesMoveRequest>(std::move(relayed));
        REQUIRE(receivedRelay->SpatialEpoch == relay.SpatialEpoch);
        REQUIRE(receivedRelay->Tick == relay.Tick);
        REQUIRE(receivedRelay->Updates.size() == 1);
        REQUIRE(receivedRelay->Updates[1] == relay.Updates[1]);
    }

    GIVEN("RequestActionEvents")
    {
        ActionEvent firstAction;
        firstAction.Tick = 1712345678901;
        firstAction.ActionId = 42;
        firstAction.TargetId = 963741;
        firstAction.State1 = 6547;
        firstAction.EventName = "jumpStandingStart";

        ActionEvent secondAction = firstAction;
        secondAction.Tick += 120;
        secondAction.ActionId = 43;
        secondAction.EventName = "attackStart";
        secondAction.TargetEventName = "toast";

        RequestActionEvents sendMessage, recvMessage;
        sendMessage.Actions[1] = {firstAction, secondAction};
        sendMessage.Actions[7] = {secondAction};

        Buffer buff(1000);
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        recvMessage.DeserializeRaw(reader);

        REQUIRE(sendMessage == recvMessage);
    }

    GIVEN("NotifyActionEvents")
    {
        ActionEvent action;
        action.Tick = 1712345678901;
        action.ActionId = 42;
        action.EventName = "blockStart";

        NotifyActionEvents sendMessage;
        sendMessage.Actions[3] = {action, action};

        Buffer buff(1000);
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        const ServerMessageFactory factory;
        auto pMessage = factory.Extract(reader);

        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == sendMessage.GetOpcode());
        REQUIRE(*CastUnique<NotifyActionEvents>(std::move(pMessage)) == sendMessage);
    }

    GIVEN("RequestInventorySnapshot")
    {
        Inventory::Entry armor;
        armor.BaseId.BaseId = 0x12E46;
        armor.Count = 1;
        armor.ExtraWorn = true;

        Inventory::Entry gold;
        gold.BaseId.BaseId = 0xF;
        gold.Count = 250;

        RequestInventorySnapshot sendMessage;
        sendMessage.ServerId = 0x10042;
        sendMessage.OwnershipEpoch = 7;
        sendMessage.CurrentInventory.Entries = {armor, gold};
        sendMessage.CurrentInventory.CurrentMagicEquipment.RightHandSpell.BaseId = 0x12FCD;

        Buffer buff(1000);
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        const ClientMessageFactory factory;
        auto pMessage = factory.Extract(reader);

        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == sendMessage.GetOpcode());
        REQUIRE(*CastUnique<RequestInventorySnapshot>(std::move(pMessage)) == sendMessage);
    }

    GIVEN("NotifyInventorySnapshot")
    {
        Inventory::Entry armor;
        armor.BaseId.BaseId = 0x12E46;
        armor.Count = 1;
        armor.ExtraWornLeft = true;

        NotifyInventorySnapshot sendMessage;
        sendMessage.ServerId = 0x10042;
        sendMessage.OwnershipEpoch = 7;
        sendMessage.CurrentInventory.Entries = {armor};
        sendMessage.CurrentInventory.CurrentMagicEquipment.Shout.BaseId = 0x13E07;

        Buffer buff(1000);
        Buffer::Writer writer(&buff);
        sendMessage.Serialize(writer);

        Buffer::Reader reader(&buff);

        const ServerMessageFactory factory;
        auto pMessage = factory.Extract(reader);

        REQUIRE(pMessage);
        REQUIRE(pMessage->GetOpcode() == sendMessage.GetOpcode());
        REQUIRE(*CastUnique<NotifyInventorySnapshot>(std::move(pMessage)) == sendMessage);
    }
}

TEST_CASE("StringCache", "[encoding.string_cache]")
{
    SECTION("Messages")
    {
        StringCacheUpdate update;
        update.Values.push_back("Hello");
        update.Values.push_back("Bye");

        Buffer buff(1000);
        Buffer::Writer writer(&buff);
        update.Serialize(writer);

        Buffer::Reader reader(&buff);

        uint64_t trash;
        reader.ReadBits(trash, 8); // pop opcode

        StringCacheUpdate recvUpdate;
        recvUpdate.DeserializeRaw(reader);

        REQUIRE(update == recvUpdate);
    }
}

TEST_CASE("NPC faction snapshots preserve removals and ownership", "[coop-state]")
{
    Faction base{};
    base.Id = GameId(0, 0x5C84E);
    base.Rank = 0;
    Faction removed = base;
    removed.Rank = -1;
    Factions factions;
    factions.NpcFactions.push_back(base);
    factions.ExtraFactions.push_back(removed);

    const auto check = [](const auto& sent) {
        Buffer buffer(4096);
        Buffer::Writer writer(&buffer);
        sent.SerializeRaw(writer);
        writer.WriteBits(0xAD, 8);
        Buffer::Reader reader(&buffer);
        std::decay_t<decltype(sent)> received{};
        received.DeserializeRaw(reader);
        REQUIRE(received == sent);
        uint64_t end = 0;
        reader.ReadBits(end, 8);
        REQUIRE(end == 0xAD);
    };

    AssignCharacterResponse assignment{};
    assignment.OwnershipEpoch = 3;
    assignment.FactionsContent = factions;
    check(assignment);
    NotifyOwnershipTransfer transfer{};
    transfer.OwnershipEpoch = 4;
    transfer.FactionsContent = factions;
    check(transfer);
    RequestFactionsChanges request{};
    request.Changes[0x6D] = {3, factions};
    check(request);
    NotifyFactionsChanges notify{};
    notify.Changes[0x6D] = {4, factions};
    check(notify);
}

TEST_CASE("Inventory snapshots do not record a debt or merge different enchantments", "[coop-state]")
{
    Inventory bag;
    Inventory::Entry sword;
    sword.BaseId = GameId(0, 0x12EB7);
    sword.Count = -1;
    bag.AddOrRemoveEntry(sword);
    REQUIRE(bag.Entries.empty());

    sword.Count = 1;
    Inventory::EffectItem effect;
    effect.EffectId = GameId(0, 10);
    sword.EnchantData.Effects.push_back(effect);
    bag.AddOrRemoveEntry(sword);
    auto second = sword;
    second.EnchantData.Effects.front().EffectId = GameId(0, 11);
    bag.AddOrRemoveEntry(second);
    REQUIRE(bag.Entries.size() == 2);

    sword.Count = -2;
    bag.AddOrRemoveEntry(sword);
    REQUIRE(bag.Entries.size() == 1);
    REQUIRE(bag.Entries.front() == second);
}

TEST_CASE("Health deltas retain the actor ownership epoch", "[coop-state]")
{
    const auto check = [](auto sent) {
        sent.Id = 0xA1;
        sent.OwnershipEpoch = 0x12345;
        sent.DeltaHealth = -12.5f;
        sent.KeepAlive = true;
        Buffer buffer(128);
        Buffer::Writer writer(&buffer);
        sent.SerializeRaw(writer);
        writer.WriteBits(0xAD, 8);
        Buffer::Reader reader(&buffer);
        decltype(sent) received{};
        received.DeserializeRaw(reader);
        REQUIRE(received == sent);
        uint64_t end = 0;
        reader.ReadBits(end, 8);
        REQUIRE(end == 0xAD);
    };
    check(RequestHealthChangeBroadcast{});
    check(NotifyHealthChangeBroadcast{});
}

TEST_CASE("Party dialogue anchors, replies and closure survive packet serialization", "[coop-dialogue]")
{
    const auto check = [](const auto& sent) {
        Buffer buffer(4096);
        Buffer::Writer writer(&buffer);
        sent.SerializeRaw(writer);
        writer.WriteBits(0xA3, 8);
        Buffer::Reader reader(&buffer);
        std::decay_t<decltype(sent)> received;
        received.DeserializeRaw(reader);
        REQUIRE(received == sent);
        uint64_t last = 0;
        reader.ReadBits(last, 8);
        REQUIRE(last == 0xA3);
    };
    PlayerDialogueRequest request;
    request.Anchor.GameInitiated = true;
    request.Anchor.TopicInfo = GameId(17,0x982);
    request.Anchor.TopicQuest = GameId(17,0x959);
    request.Anchor.Session = 0x156;
    request.Anchor.Choice = 3;
    request.Anchor.TargetEntity = 0; // zero is a valid synchronized entity
    request.Anchor.TargetForm = GameId(13, 0x822);
    request.Anchor.Cell = GameId(4, 0x311);
    request.Anchor.WorldSpace = GameId(0, 0x3C);
    request.Anchor.Position = glm::vec3{-8000.f, 42000.f, 260.f};
    request.NpcLine = "I can take you to any hold capital.";
    request.Text = "Choose a destination";
    check(request);
    NotifyPlayerDialogue incoming;
    incoming.SpeakerId = 47;
    incoming.Name = "Guest";
    incoming.Text = request.Text;
    incoming.NpcLine = request.NpcLine;
    incoming.Anchor = request.Anchor;
    check(incoming);
    incoming.Anchor.Cell = {};
    incoming.Text.clear();
    check(incoming);
}

TEST_CASE("Co-op participation, follower decisions and durable journal stream survive transport", "[coop-gameplay]")
{
    const auto check = [](const auto& sent) {
        Buffer buffer(4096);
        Buffer::Writer writer(&buffer);
        sent.SerializeRaw(writer);
        writer.WriteBits(0x9D, 8);
        Buffer::Reader reader(&buffer);
        std::decay_t<decltype(sent)> received;
        received.DeserializeRaw(reader);
        REQUIRE(received == sent);
        uint64_t end{};
        reader.ReadBits(end, 8);
        REQUIRE(end == 0x9D);
    };
    RequestCoopInteraction request;
    request.Event = {Interaction::Hire, 0, 175, 4, GameId(13, 0x4982)};
    check(request);
    NotifyCoopInteraction reply;
    reply.Event = request.Event;
    reply.Event.Kind = Interaction::Granted;
    check(reply);
    RequestQuestJournal pull;
    pull.Stream = 0xfedcba9812345678ULL;
    pull.Sequence = 9100;
    check(pull);
    NotifyQuestJournal record;
    record.Stream = pull.Stream;
    record.Sequence = 9101;
    record.Total = 9200;
    record.Row = "quest-v1\tExample.esp\t4982\tstage\t20";
    check(record);
}
