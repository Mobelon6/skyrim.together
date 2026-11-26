#include <TiltedOnlinePCH.h>

#include <Services/ImguiService.h>
#include <Systems/RenderSystemD3D11.h>
#include <d3d11.h>
#include <BSGraphics/BSGraphicsRenderer.h>
#include <cfloat>
#include <imgui/imgui_impl_dx11.h>
#include <imgui/imgui_impl_win32.h>
#include <imgui.h>

// According to imgui documentation we have to do it this way in order to avoid link conflicts with windows.h
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

ImguiService::ImguiService()
    : OnDraw(m_drawSignal)
{
}

ImguiService::~ImguiService() noexcept
{
}

void ImguiService::Create(RenderSystemD3D11* apRenderSystem, HWND aHwnd)
{
    m_context = apRenderSystem->GetDeviceContext();
    m_imDriver.Initialize(static_cast<void*>(aHwnd));

    // init platform
    if (!ImGui_ImplWin32_Init(aHwnd))
        spdlog::error("Failed to initialize Imgui-Win32");

    ImGui_ImplDX11_Init(apRenderSystem->GetDevice(), apRenderSystem->GetDeviceContext());
}

void ImguiService::Render() const
{
    ImGui_ImplDX11_NewFrame();

    ImGui_ImplWin32_NewFrame();
    auto& io = ImGui::GetIO();
    // Post-processing and display-scaling mods may bind an intermediate surface during HUD drawing.
    // ImGui builds its viewport in the pixel coordinates of the surface it actually draws onto.
    ID3D11RenderTargetView* target{};
    if (m_context) m_context->OMGetRenderTargets(1, &target, nullptr);
    if (target)
    {
        ID3D11Resource* resource{};
        target->GetResource(&resource);
        D3D11_TEXTURE2D_DESC desc{};
        ID3D11Texture2D* texture{};
        if (resource && SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture))))
        {
            texture->GetDesc(&desc);
            texture->Release();
        }
        if (resource) resource->Release();
        target->Release();
        if (desc.Width && desc.Height)
        {
            const ImVec2 pixels{static_cast<float>(desc.Width), static_cast<float>(desc.Height)};
            if (io.DisplaySize.x > 0 && io.DisplaySize.y > 0 && io.MousePos.x > -FLT_MAX && io.MousePos.y > -FLT_MAX)
                io.MousePos = {io.MousePos.x * pixels.x / io.DisplaySize.x, io.MousePos.y * pixels.y / io.DisplaySize.y};
            io.DisplaySize = pixels;
        }
    }
    ImGui::NewFrame();

    m_drawSignal.publish();
    ImGui::Render();

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void ImguiService::Reset() const
{
    // TODO: idk how imgui handles this
}

LRESULT ImguiService::WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
}

void ImguiService::RawInputHandler(RAWINPUT& aRawinput)
{
    if (ImGui::GetCurrentContext() == NULL)
        return;

    ImGuiIO& io = ImGui::GetIO();
    if (aRawinput.header.dwType == RIM_TYPEMOUSE)
    {
        const auto mouse = aRawinput.data.mouse;

        if (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)
        {
            io.MouseDown[0] = true;
        }

        if (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)
        {
            io.MouseDown[0] = false;
        }

        if (mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN)
        {
            io.MouseDown[1] = true;
        }

        if (mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)
        {
            io.MouseDown[1] = false;
        }

        if (mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN)
        {
            io.MouseDown[2] = true;
        }

        if (mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP)
        {
            io.MouseDown[2] = false;
        }
    }
}
