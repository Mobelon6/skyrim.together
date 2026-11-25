"""Compile the production wire epoch policy and test stale movement ordering."""
from pathlib import Path
import subprocess, sys, tempfile
root=Path(sys.argv[1]).resolve()
source=r'''
#include "SpatialEpoch.h"
#include "SpatialWire.h"
#include <cassert>
int main()
{
    using namespace SpatialEpoch;
    Counter client,server,peer;
    assert(client.Matches(0)&&server.Matches(0));
    assert(client.Next()&&client.Value()==1);
    assert(!server.Matches(client.Value())); // movement overtook reliable cell report
    assert(server.Advance(client.Value())&&server.Matches(client.Value()));
    assert(!server.Advance(1)&&!server.Advance(0));
    Sample sample;
    assert(sample.Observe(1,1,100)==Order::Newest);
    assert(sample.Observe(1,1,90)==Order::Historical&&sample.Tick==100);
    assert(sample.Observe(1,1,100)==Order::Duplicate);
    assert(sample.Observe(1,1,101)==Order::Newest);
    const auto old=client.Value();
    assert(client.Next()&&server.Advance(client.Value()));
    assert(!server.Matches(old)&&!client.Matches(old));
    assert(sample.Observe(1,2,1)==Order::Newest); // new location, clock may have reset
    assert(sample.Observe(2,2,0)==Order::Newest); // new owner
    assert(sample.Observe(2,2,0)==Order::Duplicate);
    assert(peer.Matches(0)); // receiver generation independent from sender
    assert(server.Advance(5)&&!server.Advance(4)); // rejected reports may leave gaps
    assert(server.Advance(UINT64_MAX)&&!server.Next());
    client.Reset(); assert(client.Matches(0)&&client.Next());
    assert(Finite(1,-1,0)&&!Finite(NAN,0,0)&&!Finite(0,INFINITY,0));
    static_assert(SpatialWire::Location(kEnterExteriorCellRequest));
    static_assert(SpatialWire::Location(kEnterInteriorCellRequest));
    static_assert(SpatialWire::Location(kShiftGridCellRequest));
    static_assert(SpatialWire::Stamped(kClientReferencesMoveRequest));
    static_assert(SpatialWire::Stamped(kServerReferencesMoveRequest));
    static_assert(!SpatialWire::Stamped(kAuthenticationRequest));
    static_assert(!SpatialWire::Stamped(kAuthenticationResponse));
    static_assert(!SpatialWire::Stamped(kRequestPartyTrade));
    static_assert(!SpatialWire::Stamped(kNotifyCampaignRecord));
}
'''
with tempfile.TemporaryDirectory() as d:
    src=Path(d)/"test.cpp";src.write_text(source);exe=Path(d)/"test"
    subprocess.run(["g++","-std=c++20","-Wall","-Wextra","-Werror","-I",str(root/"Code/common"),
                    "-I",str(root/"Code/encoding"),str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
def read(path): return (root/path).read_text()
server=read("Code/server/Services/PlayerService.cpp")
assert server.count("AdvanceSpatialEpoch(message.SpatialEpoch)")==3
assert server.index("HomeVisit::CanEnter")<server.rindex("AdvanceSpatialEpoch(message.SpatialEpoch)")
char=read("Code/server/Services/CharacterService.cpp")
assert "message.SpatialEpoch!=acMessage.pPlayer->LocationEpoch()" in char
assert "owner.IsCurrentOwner(acMessage.pPlayer,movement.OwnershipEpoch)" in char
assert "relay.SpatialEpoch=pPlayer->LocationEpoch()" in char
assert "LastMovement.Observe" in char and "Order::Historical" in char
client=read("Code/client/Services/Generic/CharacterService.cpp")
assert "movement.OwnershipEpoch=localComponent.OwnershipEpoch" in client
assert "OwnershipEpoch!=movement.OwnershipEpoch" in client
transport=read("Code/client/Services/Generic/TransportService.cpp")
assert "m_spatialEpoch.Next()" in transport and "m_spatialEpoch.Reset()" in transport
assert "m_spatialEpoch.Matches(pMessage->SpatialEpoch)" in transport
msg=read("Code/encoding/Messages/Message.cpp")
assert msg.count("Serialization::WriteVarInt(aWriter, SpatialEpoch)")==2
for direction in ("Client","Server"):
    assert "ptr->SpatialEpoch=Serialization::ReadVarInt(aReader)" in read("Code/encoding/Messages/"+direction+"MessageFactory.cpp")
print("PASS reliable location fence, stale movement and owner generations, reconnect, wire integration")
