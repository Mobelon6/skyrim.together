#include <OverlayProc.hpp>

#include <charconv>
#include <windows.h>

namespace TiltedPhoques
{
    OverlayProc::OverlayProc(const std::function<OverlayRenderProcessHandler* ()>& aFactory) noexcept
        : m_pRenderProcess(aFactory())
    {
    }

    CefRefPtr<CefRenderProcessHandler> OverlayProc::GetRenderProcessHandler()
    {
        return m_pRenderProcess;
    }

    namespace
    {
        DWORD WINAPI ExitWhenParentStops(void* parentHandle) noexcept
        {
            const auto result = WaitForSingleObject(parentHandle, INFINITE);
            CloseHandle(parentHandle);

            // The CEF browser process is already gone. A normal Chromium
            // shutdown may be waiting for IPC and never complete.
            if (result == WAIT_OBJECT_0)
                TerminateProcess(GetCurrentProcess(), 0);

            return 0;
        }

        void WatchBrowserProcess(CefCommandLine& args)
        {
            const auto pid = args.GetSwitchValue("pid").ToString();
            DWORD parentId = 0;
            const auto end = pid.data() + pid.size();
            const auto parsed = std::from_chars(pid.data(), end, parentId);
            if (parsed.ec != std::errc{} || parsed.ptr != end || parentId == 0 || parentId == GetCurrentProcessId())
                return;

            const auto parent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parentId);
            if (!parent)
            {
                // The browser may already have exited before this worker started.
                if (GetLastError() == ERROR_INVALID_PARAMETER)
                    TerminateProcess(GetCurrentProcess(), 0);

                return;
            }

            FILETIME parentCreated, selfCreated, exit, kernel, user;
            if (!GetProcessTimes(parent, &parentCreated, &exit, &kernel, &user) ||
                !GetProcessTimes(GetCurrentProcess(), &selfCreated, &exit, &kernel, &user))
            {
                CloseHandle(parent);
                return;
            }

            // A process created after this worker is a recycled PID.
            if (CompareFileTime(&parentCreated, &selfCreated) > 0 ||
                WaitForSingleObject(parent, 0) == WAIT_OBJECT_0)
            {
                CloseHandle(parent);
                TerminateProcess(GetCurrentProcess(), 0);
                return;
            }

            const auto watcher = CreateThread(nullptr, 0, ExitWhenParentStops, parent, 0, nullptr);
            if (watcher)
                CloseHandle(watcher);
            else
                CloseHandle(parent);
        }
    }

    int UIMain(const char*, const HINSTANCE aInstance, const std::function<OverlayRenderProcessHandler* ()>& acFactory) noexcept
    {
        auto pArgs = CefCommandLine::CreateCommandLine();
        pArgs->InitFromString(GetCommandLineW());
        WatchBrowserProcess(*pArgs);

        CefMainArgs mainArgs(aInstance);
        CefRefPtr<OverlayProc> App = new OverlayProc(acFactory);

        return CefExecuteProcess(mainArgs, App, nullptr);
    }
}
