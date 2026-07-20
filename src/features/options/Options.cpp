// WarcraftXL Interface Options integration for the 3.3.5 client.
// Copyright (C) 2026 WarcraftXL. GPLv3 (see source headers).

#include "runtime/LuaBindings.hpp"
#include "runtime/ModuleInstall.hpp"

namespace wxl::features::options
{
    namespace
    {
        void InstallModule()
        {
            wxl::runtime::lua::RegisterScript("wxl-options", R"lua(
                if _G.WarcraftXLOptionsInstalled then return end

                local categories = _G.WARCRAFTXL_OPTIONS_CATEGORIES or {}
                _G.WARCRAFTXL_OPTIONS_CATEGORIES = categories

                local list
                local tab
                local displayed = {}

                local function runForAll(method)
                    for _, category in ipairs(categories) do
                        local callback = category[method]
                        if type(callback) == "function" then
                            pcall(callback, category)
                        end
                    end
                end

                local function hideStockSpacers()
                    if InterfaceOptionsFrameTab1TabSpacer then
                        InterfaceOptionsFrameTab1TabSpacer:Hide()
                    end
                    if InterfaceOptionsFrameTab2TabSpacer1 then
                        InterfaceOptionsFrameTab2TabSpacer1:Hide()
                    end
                    if InterfaceOptionsFrameTab2TabSpacer2 then
                        InterfaceOptionsFrameTab2TabSpacer2:Hide()
                    end
                end

                local function displayPanel(panel)
                    if not panel or not InterfaceOptionsFramePanelContainer then return end
                    InterfaceOptionsList_DisplayPanel(panel)
                end

                local function selectButton(button)
                    if not button or not button.element then return end
                    OptionsList_ClearSelection(InterfaceOptionsFrameCategories,
                        InterfaceOptionsFrameCategories.buttons)
                    OptionsList_ClearSelection(InterfaceOptionsFrameAddOns,
                        InterfaceOptionsFrameAddOns.buttons)
                    OptionsList_ClearSelection(list, list.buttons)
                    OptionsList_SelectButton(list, button)
                    displayPanel(button.element)
                end

                local function toggleButton(button)
                    local element = button and button.element
                    if not element then return end
                    element.collapsed = not element.collapsed
                    for _, category in ipairs(categories) do
                        if category.parent == element.name then
                            category.hidden = element.collapsed and true or false
                        end
                    end
                    if list and list.update then list:update() end
                end

                local function configureButtons()
                    if not list or not list.buttons then return end
                    local buttonHeight = list.buttons[1]:GetHeight()
                    local buttonCount = math.floor((list:GetHeight() - 8) / buttonHeight)
                    for index = #list.buttons + 1, buttonCount do
                        local button = CreateFrame("Button", list:GetName() .. "Button" .. index,
                            list, "OptionsListButtonTemplate")
                        button:SetPoint("TOPLEFT", list.buttons[index - 1], "BOTTOMLEFT")
                        table.insert(list.buttons, button)
                    end
                    list.buttonHeight = buttonHeight
                    for _, button in ipairs(list.buttons) do
                        button.toggleFunc = function() toggleButton(button) end
                        button:SetScript("OnClick", function(self, mouseButton)
                            PlaySound("igMainMenuOptionCheckBoxOn")
                            if mouseButton == "RightButton" then
                                if self.element and self.element.hasChildren then
                                    OptionsListButtonToggle_OnClick(self.toggle)
                                end
                                return
                            end
                            selectButton(self)
                        end)
                    end
                end

                local function updateList()
                    if not list or not list.buttons then return end
                    local scroll = list.scrollFrame
                    local offset = FauxScrollFrame_GetOffset(scroll)
                    for key in pairs(displayed) do displayed[key] = nil end
                    for _, category in ipairs(categories) do
                        if not category.hidden then
                            table.insert(displayed, category)
                        end
                    end

                    local buttonCount = #list.buttons
                    local categoryCount = #displayed
                    if categoryCount > buttonCount and not scroll:IsShown() then
                        OptionsList_DisplayScrollBar(list)
                    elseif categoryCount <= buttonCount and scroll:IsShown() then
                        OptionsList_HideScrollBar(list)
                    end
                    FauxScrollFrame_Update(scroll, categoryCount, buttonCount,
                        list.buttons[1]:GetHeight())

                    local selection = list.selection
                    if selection then
                        OptionsList_ClearSelection(list, list.buttons)
                    end
                    for index = 1, buttonCount do
                        local button = list.buttons[index]
                        local element = displayed[index + offset]
                        if element then
                            OptionsList_DisplayButton(button, element)
                            if selection == element and not list.selection then
                                OptionsList_SelectButton(list, button)
                            end
                        else
                            button.element = nil
                            OptionsList_HideButton(button)
                        end
                    end
                    if selection then list.selection = selection end
                end

                local function showWXLTab()
                    if not list or not InterfaceOptionsFrame then return end
                    if InterfaceOptionsFrame.selectedTab ~= 3 then
                        list:Hide()
                        return
                    end
                    InterfaceOptionsFrameCategories:Hide()
                    InterfaceOptionsFrameAddOns:Hide()
                    hideStockSpacers()
                    list:Show()
                    updateList()

                    if list.selection then
                        displayPanel(list.selection)
                    else
                        for _, button in ipairs(list.buttons) do
                            if button.element then
                                selectButton(button)
                                break
                            end
                        end
                    end
                end

                local function findCategory(panel)
                    local name = type(panel) == "string" and panel or nil
                    for _, category in ipairs(categories) do
                        if category == panel or (name and category.name == name) then
                            return category
                        end
                    end
                end

                function _G.WarcraftXL_AddOptionsCategory(frame, position)
                    if not frame or not frame.name then return nil end
                    for _, category in ipairs(categories) do
                        if category == frame then return frame end
                    end

                    frame.okay = frame.okay or function() end
                    frame.cancel = frame.cancel or function() end
                    frame.default = frame.default or function() end
                    frame.refresh = frame.refresh or function() end

                    if frame.parent then
                        for index, parent in ipairs(categories) do
                            if parent.name == frame.parent then
                                if not parent.hasChildren then
                                    parent.hasChildren = true
                                    parent.collapsed = true
                                end
                                frame.hidden = parent.collapsed and true or false
                                local insertAt = index + 1
                                while categories[insertAt] and
                                      categories[insertAt].parent == frame.parent do
                                    insertAt = insertAt + 1
                                end
                                table.insert(categories, insertAt, frame)
                                if list then updateList() end
                                return frame
                            end
                        end
                    end

                    if position then
                        table.insert(categories, position, frame)
                    else
                        local insertAt
                        local lowered = string.lower(frame.name)
                        for index, category in ipairs(categories) do
                            if not category.parent and lowered < string.lower(category.name) then
                                insertAt = index
                                break
                            end
                        end
                        table.insert(categories, insertAt or (#categories + 1), frame)
                    end
                    if list then updateList() end
                    return frame
                end

                function _G.WarcraftXL_OpenToCategory(panel)
                    local category = findCategory(panel)
                    if not category or not tab then return false end
                    if category.parent then
                        for _, parent in ipairs(categories) do
                            if parent.name == category.parent and parent.collapsed then
                                parent.collapsed = false
                                for _, child in ipairs(categories) do
                                    if child.parent == parent.name then child.hidden = false end
                                end
                                break
                            end
                        end
                    end
                    if not InterfaceOptionsFrame:IsShown() then
                        InterfaceOptionsFrame:Show()
                    end
                    tab:Click()
                    updateList()
                    for _, button in ipairs(list.buttons) do
                        if button.element == category then
                            selectButton(button)
                            return true
                        end
                    end
                    displayPanel(category)
                    list.selection = category
                    return true
                end

                local function installInterface()
                    if list then return true end
                    if not InterfaceOptionsFrame or not InterfaceOptionsFrameCategories or
                       not InterfaceOptionsFrameAddOns or not InterfaceOptionsFramePanelContainer or
                       not OptionsList_OnLoad or not PanelTemplates_SetNumTabs then
                        return false
                    end

                    list = CreateFrame("Frame", "InterfaceOptionsFrameWXL",
                        InterfaceOptionsFrame, "OptionsFrameListTemplate")
                    list:SetWidth(175)
                    list:SetHeight(429)
                    list:SetPoint("TOPLEFT", InterfaceOptionsFrame, "TOPLEFT", 22, -40)
                    list.labelText = "WXL"
                    list.update = updateList
                    list:Hide()
                    configureButtons()

                    tab = CreateFrame("Button", "InterfaceOptionsFrameTab3",
                        InterfaceOptionsFrame, "OptionsFrameTabButtonTemplate")
                    tab:SetID(3)
                    tab:SetText("WXL")
                    tab:SetPoint("TOPLEFT", InterfaceOptionsFrameTab2, "TOPRIGHT", -16, 0)
                    tab:SetScript("OnClick", function(self)
                        PlaySound("igCharacterInfoTab")
                        PanelTemplates_Tab_OnClick(self, InterfaceOptionsFrame)
                        showWXLTab()
                    end)

                    local selected = InterfaceOptionsFrame.selectedTab or 1
                    PanelTemplates_SetNumTabs(InterfaceOptionsFrame, 3)
                    InterfaceOptionsFrameTab1:Show()
                    InterfaceOptionsFrameTab2:Show()
                    tab:Show()
                    PanelTemplates_SetTab(InterfaceOptionsFrame, selected)

                    InterfaceOptionsFrame:HookScript("OnShow", function()
                        PanelTemplates_SetNumTabs(InterfaceOptionsFrame, 3)
                        tab:Show()
                        updateList()
                        showWXLTab()
                        runForAll("refresh")
                    end)
                    InterfaceOptionsFrameOkay:HookScript("OnClick", function()
                        runForAll("okay")
                    end)
                    InterfaceOptionsFrameCancel:HookScript("OnClick", function()
                        runForAll("cancel")
                    end)

                    if hooksecurefunc then
                        hooksecurefunc("InterfaceOptionsFrame_TabOnClick", showWXLTab)
                        hooksecurefunc("InterfaceOptionsListButton_OnClick", function()
                            if list and list.buttons then
                                OptionsList_ClearSelection(list, list.buttons)
                            end
                        end)
                        hooksecurefunc("InterfaceOptionsFrame_SetAllToDefaults", function()
                            runForAll("default")
                            runForAll("refresh")
                        end)
                    end

                    updateList()
                    showWXLTab()
                    _G.WarcraftXLOptionsInstalled = true
                    return true
                end

                if not installInterface() then
                    local loader = CreateFrame("Frame")
                    local elapsed = 0
                    loader:SetScript("OnUpdate", function(self, delta)
                        elapsed = elapsed + delta
                        if elapsed < 0.1 then return end
                        elapsed = 0
                        if installInterface() then
                            self:SetScript("OnUpdate", nil)
                        end
                    end)
                end
            )lua");
        }

        struct Registrar
        {
            Registrar()
            {
                wxl::runtime::modules::Register("wxl-options", &InstallModule);
            }
        } g_registrar;
    }
}
