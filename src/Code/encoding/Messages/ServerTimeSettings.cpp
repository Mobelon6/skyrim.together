#include <Messages/ServerTimeSettings.h>

void ServerTimeSettings::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    timeModel.Serialize(aWriter);
    aWriter.WriteBits(RestCompleted, 1);
}

void ServerTimeSettings::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    timeModel.Deserialize(aReader);
    uint64_t rest{};
    aReader.ReadBits(rest, 1);
    RestCompleted = rest != 0;
}
