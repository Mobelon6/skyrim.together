#include <Messages/ServerTimeSettings.h>

void ServerTimeSettings::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    timeModel.Serialize(aWriter);
    aWriter.WriteBits(RestCompleted, 1);
    aWriter.WriteBits(Generation,32);
    aWriter.WriteBits(SleepPhase,2);
    aWriter.WriteBits(Source,32);
    Bed.Serialize(aWriter);
}

void ServerTimeSettings::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    timeModel.Deserialize(aReader);
    uint64_t rest{};
    aReader.ReadBits(rest, 1);
    RestCompleted = rest != 0;
    uint64_t generation{},phase{},source{};
    aReader.ReadBits(generation,32);aReader.ReadBits(phase,2);aReader.ReadBits(source,32);
    Generation=static_cast<uint32_t>(generation);
    SleepPhase=static_cast<uint8_t>(phase);
    Source=static_cast<uint32_t>(source);
    Bed.Deserialize(aReader);
}
