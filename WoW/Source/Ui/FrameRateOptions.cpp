#include "WoW/Source/Ui/FrameRateOptions.h"
#include "Engine/Source/FrameScript/FrameScript.h"

// Not part of the original client. The page is a VideoOptions panel like Resolution and Effects: its two dropdowns
// are registered controls, so Okay, Cancel and Defaults treat them as they do the game's own. 0 means no cap
// (CGxDevice::WaitForFPSCap); a background cap of 0 leaves the foreground cap in force.
static const char* s_frameRateOptionsLua = R"LUA(
local panel = CreateFrame("Frame", "VideoOptionsFrameRatePanel", VideoOptionsFramePanelContainer)
panel:Hide()
panel.name = "Frame Rate"

local title = panel:CreateFontString("VideoOptionsFrameRatePanelTitle", "ARTWORK", "GameFontNormalLarge")
title:SetPoint("TOPLEFT", 16, -16)
title:SetText("Frame Rate")

local subText = panel:CreateFontString("VideoOptionsFrameRatePanelSubText", "ARTWORK", "GameFontHighlightSmall")
subText:SetPoint("TOPLEFT", title, "BOTTOMLEFT", 0, -8)
subText:SetPoint("RIGHT", -32, 0)
subText:SetHeight(32)
subText:SetJustifyH("LEFT")
subText:SetJustifyV("TOP")
subText:SetNonSpaceWrap(true)
subText:SetText("How many frames a second the game draws. A lower cap saves power and heat; the display's refresh rate is the most it can show.")

local function CreateCapDropDown(name, cvar, label, tooltip, choices, anchor, offsetX, offsetY)
    local dropDown = CreateFrame("Frame", "VideoOptionsFrameRatePanel" .. name, panel, "UIDropDownMenuTemplate")
    dropDown:SetPoint("TOPLEFT", anchor, "BOTTOMLEFT", offsetX, offsetY)
    dropDown.type = CONTROLTYPE_DROPDOWN
    dropDown.cvar = cvar
    dropDown.defaultValue = BlizzardOptionsPanel_GetCVarDefaultSafe(cvar)
    dropDown.value = BlizzardOptionsPanel_GetCVarSafe(cvar)

    local text = dropDown:CreateFontString(dropDown:GetName() .. "Label", "BACKGROUND", "GameFontNormalSmall")
    text:SetPoint("BOTTOMLEFT", dropDown, "TOPLEFT", 21, 0)
    text:SetText(label)

    local function ChoiceText(value)
        for _, choice in ipairs(choices) do
            if choice[1] == tonumber(value) then
                return choice[2]
            end
        end

        return value .. " fps"
    end

    local function Show(value)
        UIDropDownMenu_SetSelectedValue(dropDown, value)
        UIDropDownMenu_SetText(dropDown, ChoiceText(value))
    end

    local function OnClick(button)
        Show(button.value)
        dropDown.newValue = button.value ~= dropDown.value and button.value or nil
    end

    local function Initialize()
        local info = UIDropDownMenu_CreateInfo()
        local selected = tonumber(UIDropDownMenu_GetSelectedValue(dropDown))

        for _, choice in ipairs(choices) do
            info.text = choice[2]
            info.value = choice[1]
            info.func = OnClick
            info.checked = choice[1] == selected and 1 or nil
            UIDropDownMenu_AddButton(info)
        end
    end

    UIDropDownMenu_SetWidth(dropDown, 150)
    UIDropDownMenu_Initialize(dropDown, Initialize)
    Show(dropDown.value)

    dropDown.SetValue = function(self, value)
        BlizzardOptionsPanel_SetCVarSafe(self.cvar, value)
        Show(value)
    end
    dropDown.GetValue = function(self)
        return BlizzardOptionsPanel_GetCVarSafe(self.cvar)
    end
    dropDown.RefreshValue = function(self)
        local value = self:GetValue()
        UIDropDownMenu_Initialize(self, Initialize)
        Show(value)
        self.value = value
        self.newValue = value
    end

    dropDown:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        GameTooltip:SetText(tooltip, nil, nil, nil, nil, 1)
    end)
    dropDown:SetScript("OnLeave", GameTooltip_Hide)

    BlizzardOptionsPanel_RegisterControl(dropDown, panel)
    return dropDown
end

local maxFPS = CreateCapDropDown("MaxFPS", "maxFPS", "Max Frame Rate",
    "The most frames a second the game draws while its window is active.",
    { { 30, "30 fps" }, { 60, "60 fps" }, { 90, "90 fps" }, { 120, "120 fps" }, { 144, "144 fps" }, { 200, "200 fps" }, { 0, "Unlimited" } },
    subText, -16, -24)

CreateCapDropDown("MaxFPSBk", "maxFPSBk", "Background Frame Rate",
    "The most frames a second the game draws while another window is active.",
    { { 10, "10 fps" }, { 30, "30 fps" }, { 60, "60 fps" }, { 120, "120 fps" }, { 0, "Same as Max Frame Rate" } },
    maxFPS, 0, -24)

VideoOptionsPanel_OnLoad(panel)
)LUA";

void FrameRateOptionsCreate() {
    FrameScript_Execute(s_frameRateOptionsLua, "FrameRateOptions.lua", nullptr);
}
