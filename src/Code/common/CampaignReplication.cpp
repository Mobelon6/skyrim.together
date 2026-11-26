#include "CampaignReplication.h"
#include <cstdlib>
#include <fstream>
#include <charconv>
#include <system_error>

namespace CampaignReplication
{
using CampaignJournal::Result;
Reply Serve(const CampaignJournal::Journal& journal, uint64_t campaign, std::string_view profile, const Pull& request)
{
    Reply reply;
    reply.Campaign=request.Campaign; reply.Nonce=request.Nonce; reply.Base=request.Base;
    if (!request.Nonce || !request.Campaign || request.Campaign!=campaign || request.Profile!=profile || !journal.Ready())
        return reply;
    const auto head=journal.Current();
    const auto known=journal.At(request.Base.Revision);
    if (!known || *known!=request.Base)
    {
        reply.State=Status::Conflict; // no history or records disclosed on conflict
        return reply;
    }
    reply.Total=head.Revision;
    if (request.Base==head)
    {
        reply.State=Status::Current; reply.Target=head; return reply;
    }
    const auto next=request.Base.Revision+1;
    auto row=journal.Export(next);
    auto target=journal.At(next);
    if (!row || !target) return reply;
    reply.State=Status::Record;
    reply.Target=*target;
    reply.Row=std::move(*row);
    return reply;
}
Result Receive(CampaignJournal::Journal& journal, const Pull& request, const Reply& reply)
{
    if (!journal.Ready() || !request.Nonce || request.Campaign!=reply.Campaign ||
        request.Nonce!=reply.Nonce || request.Base!=reply.Base || request.Base!=journal.Current())
        return Result::Conflict;
    if (reply.State==Status::Current)
        return reply.Row.empty() && reply.Target==request.Base && reply.Total==request.Base.Revision ? Result::Duplicate : Result::Invalid;
    if (reply.State==Status::Conflict) return Result::Conflict;
    if (reply.State==Status::Unavailable) return Result::StorageError;
    if (reply.State!=Status::Record || request.Base.Revision>=UINT32_MAX-1 ||
        reply.Target.Revision!=request.Base.Revision+1 || reply.Total<reply.Target.Revision)
        return Result::Invalid;
    return journal.ImportExpected(request.Base,reply.Target,reply.Row);
}
Link::Mode Link::Start(bool authority)
{
    if (m_started) return m_mode;
    m_started=true;
#ifdef _WIN32
    const auto* path=_wgetenv(L"STM_CAMPAIGN_ROOT");
#else
    const auto* path=std::getenv("STM_CAMPAIGN_ROOT");
#endif
    if (!path || !*path) return m_mode;
    m_mode=Mode::Failed;
    try
    {
        const std::filesystem::path root(path);
        if (!root.is_absolute()) return m_mode;
        Root=root;
        const auto config=root/"CampaignLink.txt";
        if (!std::filesystem::is_regular_file(config) || std::filesystem::file_size(config)>512) return m_mode;
        std::ifstream input(config);
        std::string id, extra;
        if (!(input>>id>>Profile) || input>>extra || input.bad()) return m_mode;
        const auto parsed=std::from_chars(id.data(),id.data()+id.size(),Campaign);
        if (parsed.ec!=std::errc{} || parsed.ptr!=id.data()+id.size() || !Campaign) return m_mode;
        const auto directory=root/(authority?"authority":"replica");
        if (!Journal.Open(directory,Campaign,Profile,false))
        {
            // Never recreate an existing, damaged history. Authority creation is explicit.
            const auto* create=std::getenv("STM_CAMPAIGN_CREATE");
            if (std::filesystem::exists(directory/std::to_string(Campaign)) ||
                (authority && (!create || std::string_view(create)!="1")) ||
                !Journal.Open(directory,Campaign,Profile,true))
                return m_mode;
        }
        m_mode=Mode::Ready;
    }
    catch (...) { m_mode=Mode::Failed; }
    return m_mode;
}
}
