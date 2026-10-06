#include "extension-support/environment/UiApi.hpp"
#include "engine/ui/ImGuiHost.hpp"
#include "imgui.h"
namespace wxl::ui {
const WXL_EnvironmentUiApi* EnvironmentApi() {
    static const WXL_EnvironmentUiApi api = {
        sizeof(WXL_EnvironmentUiApi), IMGUI_VERSION,
        sizeof(ImGuiIO), sizeof(ImGuiStyle), sizeof(ImVec2), sizeof(ImVec4),
        &AddPanel, &AddWindowOwner
    };
    return &api;
}
}
