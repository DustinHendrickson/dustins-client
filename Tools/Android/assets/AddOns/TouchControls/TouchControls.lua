-- Touch Controls: the on-screen controls for playing on a phone or tablet (docs/ANDROID_PORT.md, "Touch and small
-- screens"). Packed in the APK and installed into Interface/AddOns at every start, so it always matches the build.
--
-- The game's native input layer owns every finger (Engine/Source/Os/Android/Input.cpp). This add-on draws the controls
-- and tells it where they are (TouchControls_SetControl): a finger landing on one presses its key, or is the joystick,
-- whatever the UI or the world has under it. Spells are cast by their real key bindings (ACTIONBUTTON1 to 12), so they
-- work in combat and follow the player's bindings; the add-on's own actions are bindings too (Bindings.xml), on keys a
-- phone doesn't type (F13 to F16).
--
-- Two styles, switched in the menu or with /touch: the thumb wheel, whose buttons only show what keys 1 to = cast now
-- (the main bar's page, a stance's bar, a vehicle's) while the game's own action bars are put away; and the default
-- action bars, left where they are for tapping.

BINDING_HEADER_TOUCHCONTROLS = "Touch Controls"
BINDING_NAME_TOUCHCONTROLS_INTERACT = "Interact with nearest"
BINDING_NAME_TOUCHCONTROLS_TARGETNONHOSTILE = "Target nearest friendly or neutral"
BINDING_NAME_TOUCHCONTROLS_TOGGLE = "Show or hide touch controls"
BINDING_NAME_TOUCHCONTROLS_MENU = "Touch controls menu"

local defaults = {
    style = "wheel",    -- "wheel": the thumb wheel; "bars": the default action bars
    hidden = false,
    textScale = 1.15,   -- the game's fonts, as a multiple of their own sizes
    displayChosen = false,  -- whether the interface size for this screen has been set (once, then it's the player's)
}

local db
local controls = {}         -- name -> { frame, key }: the areas the input layer knows
local NUM_WHEEL = 12        -- the whole bar, keys 1 to =

-- The layout, in fractions of the screen's height (sizes keep their shape at any UI scale and screen width).
local CORNER = 0.19         -- the jump button's center, in from the bottom right corner
local JUMP_SIZE = 0.145
local SLOT_SIZE = 0.088     -- the wheel's buttons
local SMALL_SIZE = 0.075    -- the other round buttons
local WHEEL_ROWS = { 0.2, 0.315, 0.43 }  -- the wheel's three arcs, from the jump button's center
local JOYSTICK_RADIUS = 0.11    -- a full push (Input.cpp kJoystickRadius)

-- The art: the minimap's ring around a round icon (SetPortraitToTexture), on a dark disc.
local RING = "Interface\\Minimap\\UI-MINIMAP-BORDER"
local RING_LEFT, RING_RIGHT, RING_TOP, RING_BOTTOM = 95 / 256, 241 / 256, 33 / 256, 179 / 256
local RING_HOLE = 0.88      -- the ring's opening, as a fraction of its size
local DISC = "Interface\\CHARACTERFRAME\\TempPortraitAlphaMask"
local GLOW = "Interface\\Buttons\\IconBorder-GlowRing"

local addon = CreateFrame("Frame", "TouchControlsFrame", UIParent)
addon:SetAllPoints(UIParent)
addon:SetFrameStrata("LOW")

-- The game's frames put away while the wheel is in use (their parent is this hidden frame).
local stash = CreateFrame("Frame")
stash:Hide()

local function ScreenHeight()
    return GetScreenHeight()
end

local function ScreenWidth()
    return GetScreenWidth()
end

-- A frame's area as fractions of the screen from its bottom left, as the input layer takes them.
local function Fractions(frame)
    local left, bottom, width, height = frame:GetRect()

    if not left then
        return nil
    end

    local scale = frame:GetEffectiveScale() / UIParent:GetEffectiveScale()
    local screenWidth = ScreenWidth()
    local screenHeight = ScreenHeight()

    return left * scale / screenWidth, bottom * scale / screenHeight, (left + width) * scale / screenWidth, (bottom + height) * scale / screenHeight
end

local function Register(name, frame, key)
    controls[name] = { frame = frame, key = key }
end

------------------------------------------------------------------------------------------------------------------------
-- Round buttons

local function MakeRound(name, parent)
    local button = CreateFrame("Frame", name, parent)

    button.disc = button:CreateTexture(nil, "BACKGROUND")
    button.disc:SetTexture(DISC)
    button.disc:SetVertexColor(0, 0, 0, 0.6)

    button.icon = button:CreateTexture(nil, "BORDER")

    -- Lit while a finger holds the button.
    button.pressed = button:CreateTexture(nil, "ARTWORK")
    button.pressed:SetTexture(DISC)
    button.pressed:SetBlendMode("ADD")
    button.pressed:SetVertexColor(1, 0.85, 0.4, 0.3)
    button.pressed:Hide()

    button.ring = button:CreateTexture(nil, "OVERLAY")
    button.ring:SetTexture(RING)
    button.ring:SetTexCoord(RING_LEFT, RING_RIGHT, RING_TOP, RING_BOTTOM)
    button.ring:SetAllPoints()

    return button
end

local function SizeRound(button, size)
    button:SetWidth(size)
    button:SetHeight(size)

    local hole = size * RING_HOLE

    for _, texture in ipairs({ button.disc, button.icon, button.pressed }) do
        texture:ClearAllPoints()
        texture:SetPoint("CENTER")
        texture:SetWidth(hole)
        texture:SetHeight(hole)
    end

    if button.glow then
        button.glow:SetWidth(size * 1.45)
        button.glow:SetHeight(size * 1.45)
    end

    if button.text then
        button.text:SetFont(button.text:GetFont(), size * 0.4, "OUTLINE")
    end
end

local function SetRoundIcon(button, path)
    if button.iconPath == path then
        return
    end

    button.iconPath = path

    if path then
        SetPortraitToTexture(button.icon, path)
        button.icon:Show()
    else
        button.icon:Hide()
    end
end

local function PlaceAt(frame, x, y)
    frame:ClearAllPoints()
    frame:SetPoint("CENTER", UIParent, "BOTTOMLEFT", x, y)
end

------------------------------------------------------------------------------------------------------------------------
-- The joystick: an area on the left a thumb can land anywhere in; the base appears where it lands.

local joystickArea = CreateFrame("Frame", "TouchControlsJoystickArea", addon)

local joystickBase = CreateFrame("Frame", nil, joystickArea)
joystickBase.disc = joystickBase:CreateTexture(nil, "BACKGROUND")
joystickBase.disc:SetTexture(DISC)
joystickBase.disc:SetVertexColor(0, 0, 0, 0.35)
joystickBase.disc:SetPoint("CENTER")
joystickBase.ring = joystickBase:CreateTexture(nil, "BORDER")
joystickBase.ring:SetTexture(RING)
joystickBase.ring:SetTexCoord(RING_LEFT, RING_RIGHT, RING_TOP, RING_BOTTOM)
joystickBase.ring:SetAllPoints()

local joystickKnob = MakeRound(nil, joystickArea)
joystickKnob.disc:SetVertexColor(0.85, 0.75, 0.5, 0.5)
joystickKnob:SetFrameLevel(joystickBase:GetFrameLevel() + 2)

local function PlaceJoystick(x, y, pushX, pushY)
    local height = ScreenHeight()
    local radius = JOYSTICK_RADIUS * height
    local centerX = x * ScreenWidth()
    local centerY = y * height

    joystickBase:SetWidth(radius * 2.3)
    joystickBase:SetHeight(radius * 2.3)
    joystickBase.disc:SetWidth(radius * 2.3 * RING_HOLE)
    joystickBase.disc:SetHeight(radius * 2.3 * RING_HOLE)
    PlaceAt(joystickBase, centerX, centerY)

    SizeRound(joystickKnob, radius * 0.95)
    PlaceAt(joystickKnob, centerX + pushX * radius, centerY + pushY * radius)
end

-- Where the base rests, as fractions of the screen (Layout).
local joystickRestX, joystickRestY = 0.14, 0.24

joystickArea:SetScript("OnUpdate", function(self)
    local held, x, y, pushX, pushY = TouchControls_GetJoystick()

    -- Faded through the frame: a texture's own alpha is its vertex color's, which gives the discs their shade.
    if held then
        PlaceJoystick(x, y, pushX, pushY)
        self:SetAlpha(1)
    else
        PlaceJoystick(joystickRestX, joystickRestY, 0, 0)
        self:SetAlpha(0.55)
    end
end)

Register("joystick", joystickArea, "JOYSTICK")

------------------------------------------------------------------------------------------------------------------------
-- The overlay buttons, each pressing a key.

local buttons = {}

local function AddButton(name, icon, key)
    local button = MakeRound("TouchControls" .. name, addon)
    SetRoundIcon(button, icon)
    buttons[name] = button
    Register(name, button, key)
    return button
end

AddButton("jump", "Interface\\Icons\\Ability_Rogue_Sprint", "SPACE")
AddButton("interact", "Interface\\Icons\\INV_Gauntlets_04", "F13")
AddButton("targetEnemy", "Interface\\Icons\\Ability_Hunter_SniperShot", "TAB")
AddButton("targetFriend", "Interface\\Icons\\Spell_Holy_HolyBolt", "F14")
AddButton("chat", "Interface\\Icons\\INV_Letter_15", "ENTER")
AddButton("menu", "Interface\\Icons\\INV_Misc_Gear_01", "F16")
AddButton("toggle", "Interface\\Icons\\Spell_Shadow_DetectInvisibility", "F15")
AddButton("page", nil, nil)

-- The page button shows the wheel's page as a number.
buttons.page.text = buttons.page:CreateFontString(nil, "OVERLAY", "GameFontNormalHuge")
buttons.page.text:SetPoint("CENTER", 0, 1)

------------------------------------------------------------------------------------------------------------------------
-- The wheel: buttons around jump that show the actions keys 1 to = cast; a finger on one presses its binding
-- (ACTIONBUTTON1 to 12), which the game sends to the right bar.

local wheelFrame = CreateFrame("Frame", "TouchControlsWheel", addon)
wheelFrame:SetAllPoints(UIParent)

local slots = {}

for index = 1, NUM_WHEEL do
    local slot = MakeRound("TouchControlsAction" .. index, wheelFrame)

    slot.glow = slot:CreateTexture(nil, "OVERLAY")
    slot.glow:SetTexture(GLOW)
    slot.glow:SetBlendMode("ADD")
    slot.glow:SetVertexColor(1, 0.8, 0.2)
    slot.glow:SetPoint("CENTER")
    slot.glow:Hide()

    slot.shade = slot:CreateTexture(nil, "ARTWORK")
    slot.shade:SetTexture(DISC)
    slot.shade:SetVertexColor(0, 0, 0, 0.6)
    slot.shade:SetAllPoints(slot.icon)
    slot.shade:Hide()

    slot.text = slot:CreateFontString(nil, "OVERLAY", "GameFontHighlightLarge")
    slot.text:SetPoint("CENTER")

    slot.count = slot:CreateFontString(nil, "OVERLAY", "NumberFontNormal")
    slot.count:SetPoint("BOTTOMRIGHT", -2, 2)

    slots[index] = slot
    Register("action" .. index, slot, nil)
end

-- The button keys index would press now: a vehicle's, a stance's or possession's bar, or the main bar.
local function WheelButton(index)
    if VehicleMenuBar and VehicleMenuBar:IsShown() then
        return _G["VehicleMenuBarActionButton" .. index]
    end

    if BonusActionBarFrame and BonusActionBarFrame:IsShown() then
        return _G["BonusActionButton" .. index]
    end

    return _G["ActionButton" .. index]
end

local function FormatCooldown(seconds)
    if seconds >= 3600 then
        return format("%dh", ceil(seconds / 3600))
    elseif seconds >= 60 then
        return format("%dm", ceil(seconds / 60))
    end

    return format("%d", ceil(seconds))
end

local function UpdateSlot(slot, index)
    local button = WheelButton(index)
    local action = button and button.action

    if not action or not HasAction(action) then
        SetRoundIcon(slot, nil)
        slot.glow:Hide()
        slot.shade:Hide()
        slot.text:SetText("")
        slot.count:SetText("")
        slot:SetAlpha(0.45)
        return
    end

    slot:SetAlpha(1)
    SetRoundIcon(slot, GetActionTexture(action))

    -- Usable, out of mana, out of range: the default buttons' colors.
    local usable, noMana = IsUsableAction(action)

    if IsActionInRange(action) == 0 then
        slot.icon:SetVertexColor(0.9, 0.25, 0.25)
    elseif usable then
        slot.icon:SetVertexColor(1, 1, 1)
    elseif noMana then
        slot.icon:SetVertexColor(0.45, 0.45, 1)
    else
        slot.icon:SetVertexColor(0.4, 0.4, 0.4)
    end

    -- A cooldown longer than the global one shades the button and counts down on it.
    local start, duration, enable = GetActionCooldown(action)
    local remaining = (start or 0) + (duration or 0) - GetTime()

    if enable == 1 and duration and duration > 1.5 and remaining > 0 then
        slot.shade:Show()
        slot.text:SetText(FormatCooldown(remaining))
    else
        slot.shade:Hide()
        slot.text:SetText("")
    end

    if IsConsumableAction(action) or IsStackableAction(action) then
        slot.count:SetText(GetActionCount(action))
    else
        slot.count:SetText("")
    end

    if IsCurrentAction(action) or IsAutoRepeatAction(action) then
        slot.glow:Show()
    else
        slot.glow:Hide()
    end
end

------------------------------------------------------------------------------------------------------------------------
-- Controls under the game's windows: a bag, a dialog or the map over a control takes the fingers there instead.

local covering = { "GameMenuFrame", "TouchControlsMenu", "LootFrame", "WorldMapFrame", "InterfaceOptionsFrame",
    "VideoOptionsFrame", "AudioOptionsFrame", "KeyBindingFrame", "MacroFrame", "ChatConfigFrame", "HelpFrame",
    "StaticPopup1", "StaticPopup2", "StaticPopup3", "StaticPopup4", "ItemRefTooltip", "TradeFrame", "MerchantFrame",
    "GossipFrame", "QuestFrame", "TaxiFrame", "BankFrame", "MailFrame", "AuctionFrame", "ReadyCheckFrame" }

local function Over(frame, left, bottom, right, top)
    if not frame or not frame:IsVisible() then
        return false
    end

    local l, b, r, t = Fractions(frame)
    return l and l < right and r > left and b < top and t > bottom
end

local function Covered(left, bottom, right, top)
    for _, name in ipairs(covering) do
        if Over(_G[name], left, bottom, right, top) then
            return true
        end
    end

    for index = 1, NUM_CONTAINER_FRAMES or 13 do
        if Over(_G["ContainerFrame" .. index], left, bottom, right, top) then
            return true
        end
    end

    for _, side in ipairs({ "left", "center", "right", "doublewide", "fullscreen" }) do
        if Over(GetUIPanel(side), left, bottom, right, top) then
            return true
        end
    end

    return false
end

-- Sends every visible, uncovered control's area to the input layer, and drops the others.
local function SyncControls()
    for name, control in pairs(controls) do
        local frame = control.frame
        local sent = false

        if (not db.hidden or name == "toggle") and frame:IsVisible() and control.key then
            local left, bottom, right, top = Fractions(frame)

            if left and not Covered(left, bottom, right, top) then
                TouchControls_SetControl(name, left, bottom, right, top, control.key)
                sent = true
            end
        end

        if not sent then
            TouchControls_RemoveControl(name)
        end
    end
end

------------------------------------------------------------------------------------------------------------------------
-- The menu: the game's menu buttons and bags, large, in a dialog the menu button opens.

local menu = CreateFrame("Frame", "TouchControlsMenu", UIParent)
menu:SetFrameStrata("DIALOG")
menu:SetToplevel(true)
menu:EnableMouse(true)
menu:SetBackdrop({
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 32, edgeSize = 32,
    insets = { left = 11, right = 12, top = 12, bottom = 11 },
})
menu:Hide()
tinsert(UISpecialFrames, "TouchControlsMenu")

local menuHeader = menu:CreateTexture(nil, "ARTWORK")
menuHeader:SetTexture("Interface\\DialogFrame\\UI-DialogBox-Header")
menuHeader:SetWidth(300)
menuHeader:SetHeight(64)
menuHeader:SetPoint("TOP", 0, 12)

local menuTitle = menu:CreateFontString(nil, "ARTWORK", "GameFontNormal")
menuTitle:SetPoint("TOP", menuHeader, "TOP", 0, -14)
menuTitle:SetText(MAINMENU_BUTTON or "Menu")

-- The micro buttons and bags, each row in a holder of its own scale, and the display settings in a third; all keep
-- their size on the screen whatever the interface size.
local function MenuScale()
    return 1.6 * ScreenHeight() / 937
end

local microHolder = CreateFrame("Frame", nil, menu)
local bagHolder = CreateFrame("Frame", nil, menu)
local displayHolder = CreateFrame("Frame", nil, menu)

local styleButton = CreateFrame("Button", "TouchControlsMenuStyle", menu, "UIPanelButtonTemplate")
local closeButton = CreateFrame("Button", "TouchControlsMenuClose", menu, "UIPanelButtonTemplate")

for _, button in ipairs({ styleButton, closeButton }) do
    button:SetWidth(140)
    button:SetHeight(26)
end

------------------------------------------------------------------------------------------------------------------------
-- Display: the interface's size, its text's, and the chat's.

local UI_SCALE_MIN, UI_SCALE_MAX = 0.64, 1.5

-- The size that gives the game's windows room on this screen: a phone's two or three inches get the interface about
-- 640 units high (the tallest windows are 512 under a 104 margin), a tablet's more, towards a desktop's 768 and over.
local function RecommendedScale()
    local _, _, _, _, _, _, heightInches = TouchControls_GetScreen()

    if not heightInches or heightInches <= 0 then
        return 1
    end

    local unitsHigh = min(max(640 + (heightInches - 2.6) * 60, 640), 820)
    return min(max(768 / unitsHigh, UI_SCALE_MIN), UI_SCALE_MAX)
end

local function SetUiScale(scale)
    scale = floor(scale * 100 + 0.5) / 100
    SetCVar("useUiScale", "1")
    SetCVar("uiScale", scale)
end

-- Text: every font object the game names, from the size it was made with.
local fontSizes = {}

local function ApplyTextScale(scale)
    local fonts = {}

    for _, object in pairs(_G) do
        if type(object) == "table" and type(object.GetObjectType) == "function" and type(object.GetFont) == "function" then
            local ok, kind = pcall(object.GetObjectType, object)

            if ok and kind == "Font" then
                fonts[object] = true
            end
        end
    end

    -- All sizes first: a font follows the one it inherits from until it has a size of its own.
    for font in pairs(fonts) do
        if not fontSizes[font] then
            local _, size = font:GetFont()
            fontSizes[font] = size
        end
    end

    for font in pairs(fonts) do
        local file, _, flags = font:GetFont()

        if file and fontSizes[font] then
            font:SetFont(file, fontSizes[font] * scale, flags)
        end
    end
end

local function SetChatSize(size)
    for index = 1, NUM_CHAT_WINDOWS or 10 do
        local frame = _G["ChatFrame" .. index]

        if frame then
            FCF_SetChatWindowFontSize(nil, frame, size)
        end
    end
end

-- A slider with its label and value above it: apply(value) runs once the finger lets go, as resizing the interface
-- under a dragging finger would move the slider away from it.
local updatingSliders = false

local function MakeSlider(name, label, low, high, step, show, apply, live)
    local slider = CreateFrame("Slider", name, displayHolder, "OptionsSliderTemplate")
    slider:SetWidth(300)
    slider:SetHeight(17)
    slider:SetMinMaxValues(low, high)
    slider:SetValueStep(step)
    _G[name .. "Low"]:SetText("")
    _G[name .. "High"]:SetText("")

    local text = _G[name .. "Text"]

    slider:SetScript("OnValueChanged", function(self, value)
        text:SetText(label .. ": |cffffffff" .. show(value) .. "|r")

        if live and not updatingSliders then
            apply(value)
        end
    end)

    if not live then
        slider:SetScript("OnMouseUp", function(self)
            apply(self:GetValue())
        end)
    end

    return slider
end

local Refresh
local refreshAt     -- a second layout once a new interface size has settled (windows size themselves a frame later)

local uiScaleSlider = MakeSlider("TouchControlsMenuUiScale", "Interface size", UI_SCALE_MIN, UI_SCALE_MAX, 0.01,
    function(value) return format("%d%%", value * 100 + 0.5) end,
    function(value)
        SetUiScale(value)
        Refresh()
        refreshAt = GetTime() + 0.3
    end)

local textSlider = MakeSlider("TouchControlsMenuTextScale", "Text size", 1, 1.5, 0.05,
    function(value) return format("%d%%", value * 100 + 0.5) end,
    function(value)
        db.textScale = value
        ApplyTextScale(value)
    end)

local chatSlider = MakeSlider("TouchControlsMenuChatSize", "Chat text", 10, 24, 1,
    function(value) return format("%d", value + 0.5) end,
    function(value)
        SetChatSize(floor(value + 0.5))
    end, true)

local displaySliders = { uiScaleSlider, textSlider, chatSlider }

local function UpdateSliders()
    updatingSliders = true
    uiScaleSlider:SetValue(tonumber(GetCVar("uiScale")) or 1)
    textSlider:SetValue(db.textScale)
    chatSlider:SetValue(select(2, ChatFrame1:GetFont()) or 14)
    updatingSliders = false
end

menu:SetScript("OnShow", UpdateSliders)

-- The game's own UI Scale slider (Video options) reaches as far.
local function WidenGameSlider()
    if ResolutionPanelOptions and ResolutionPanelOptions.uiscale then
        ResolutionPanelOptions.uiscale.maxValue = UI_SCALE_MAX
    end

    if VideoOptionsResolutionPanelUIScaleSlider then
        VideoOptionsResolutionPanelUIScaleSlider:SetMinMaxValues(UI_SCALE_MIN, UI_SCALE_MAX)
    end
end

closeButton:SetText(CLOSE or "Close")
closeButton:SetScript("OnClick", function()
    menu:Hide()
end)

local bags = { "MainMenuBarBackpackButton", "CharacterBag0Slot", "CharacterBag1Slot", "CharacterBag2Slot", "CharacterBag3Slot", "KeyRingButton" }
local bagPoints = {}        -- where the bags were, to put them back
local inMenu = false        -- whether the micro buttons and bags are in the menu now

local function MicroButtonList()
    return MicroButtons or { CharacterMicroButton, SpellbookMicroButton, TalentMicroButton, AchievementMicroButton,
        QuestLogMicroButton, SocialsMicroButton, PVPMicroButton, LFDMicroButton, MainMenuMicroButton, HelpMicroButton }
end

-- A tap on one of them opens its window, and the menu gets out of the way.
local function CloseMenu()
    menu:Hide()
end

for _, name in ipairs(bags) do
    if _G[name] then
        _G[name]:HookScript("OnClick", CloseMenu)
    end
end

for _, button in ipairs(MicroButtonList()) do
    button:HookScript("OnClick", CloseMenu)
end

-- Moves the micro buttons and bags into the menu, a row each, and sizes the menu around them.
local function FillMenu()
    local micro = MicroButtonList()

    for _, button in ipairs(micro) do
        button:SetParent(microHolder)
    end

    -- One row: the chain runs from the character button (a vehicle starts a second line at the social button).
    CharacterMicroButton:ClearAllPoints()
    CharacterMicroButton:SetPoint("BOTTOMLEFT", microHolder, "BOTTOMLEFT", 0, 0)
    SocialsMicroButton:ClearAllPoints()
    SocialsMicroButton:SetPoint("BOTTOMLEFT", QuestLogMicroButton, "BOTTOMRIGHT", -3, 0)

    for _, name in ipairs(bags) do
        local bag = _G[name]

        if bag then
            if not bagPoints[name] then
                local points = {}

                for i = 1, bag:GetNumPoints() do
                    points[i] = { bag:GetPoint(i) }
                end

                bagPoints[name] = { parent = bag:GetParent(), points = points }
            end

            bag:SetParent(bagHolder)
        end
    end

    MainMenuBarBackpackButton:ClearAllPoints()
    MainMenuBarBackpackButton:SetPoint("BOTTOMRIGHT", bagHolder, "BOTTOMRIGHT", 0, 0)

    -- The micro buttons' art is 28 x 58 with the button in its lower 36 (each overlaps the next by 3); the backpack is
    -- 37 square, the other bags 30 with gaps, the key ring 18.
    local microWidth = #micro * 25 + 3
    local bagWidth = 37 + 4 * 32 + 22
    microHolder:SetWidth(microWidth)
    microHolder:SetHeight(40)
    bagHolder:SetWidth(bagWidth)
    bagHolder:SetHeight(37)

    -- The display settings: three sliders, each under its label.
    local sliderRow = 44
    displayHolder:SetWidth(300)
    displayHolder:SetHeight(sliderRow * #displaySliders)

    for index, slider in ipairs(displaySliders) do
        slider:ClearAllPoints()
        slider:SetPoint("TOP", displayHolder, "TOP", 0, -sliderRow * (index - 1) - 20)
    end

    local scale = MenuScale()
    local displayScale = scale * 0.8
    local buttonScale = scale * 0.8
    microHolder:SetScale(scale)
    bagHolder:SetScale(scale)
    displayHolder:SetScale(displayScale)
    styleButton:SetScale(buttonScale)
    closeButton:SetScale(buttonScale)

    local padding = 30
    local gap = 18
    menu:SetWidth(max(microWidth * scale, bagWidth * scale, 300 * displayScale) + padding * 2)
    menu:SetHeight(padding + 40 * scale + gap + 37 * scale + gap + displayHolder:GetHeight() * displayScale + gap + 26 * buttonScale + padding)
    menu:ClearAllPoints()
    menu:SetPoint("CENTER", UIParent, "CENTER", 0, ScreenHeight() * 0.03)

    microHolder:ClearAllPoints()
    microHolder:SetPoint("TOP", menu, "TOP", 0, -padding / scale)
    bagHolder:ClearAllPoints()
    bagHolder:SetPoint("TOP", microHolder, "BOTTOM", 0, -gap / scale)
    displayHolder:ClearAllPoints()
    displayHolder:SetPoint("TOP", menu, "TOP", 0, -(padding + 40 * scale + gap + 37 * scale + gap) / displayScale)
    styleButton:ClearAllPoints()
    styleButton:SetPoint("BOTTOMLEFT", menu, "BOTTOMLEFT", padding / buttonScale, padding / buttonScale)
    closeButton:ClearAllPoints()
    closeButton:SetPoint("BOTTOMRIGHT", menu, "BOTTOMRIGHT", -padding / buttonScale, padding / buttonScale)

    inMenu = true
end

-- Puts the micro buttons and bags back on the main bar.
local function EmptyMenu()
    if not inMenu then
        return
    end

    inMenu = false

    for name, original in pairs(bagPoints) do
        local bag = _G[name]
        bag:SetParent(original.parent)
        bag:ClearAllPoints()

        for _, point in ipairs(original.points) do
            bag:SetPoint(unpack(point))
        end
    end

    wipe(bagPoints)
    VehicleMenuBar_MoveMicroButtons()
end

-- Leaving a vehicle puts the micro buttons back on the main bar: they come back to the menu.
hooksecurefunc("VehicleMenuBar_MoveMicroButtons", function(skinName)
    if not skinName and inMenu then
        FillMenu()
    end
end)

function TouchControls_ToggleMenu()
    if menu:IsShown() then
        menu:Hide()
    else
        menu:Show()
    end
end

------------------------------------------------------------------------------------------------------------------------
-- The game's bars: put away for the wheel, back for the bars style.

local stashed = {}          -- frame -> its parent before

local function Stash(frame)
    if frame and not stashed[frame] then
        stashed[frame] = frame:GetParent()
        frame:SetParent(stash)
    end
end

local function Unstash()
    for frame, parent in pairs(stashed) do
        frame:SetParent(parent)
    end

    wipe(stashed)
end

-- With the action buttons away, the main bar keeps only its experience and reputation bars, which go down to the
-- screen's edge; the stance, pet and totem bars above them grow for fingers.
local SMALL_BARS = { "ShapeshiftBarFrame", "PossessBarFrame", "PetActionBarFrame", "MultiCastActionBarFrame" }

local function PlaceGameBars()
    if InCombatLockdown() then
        return
    end

    local wheel = db.style == "wheel"

    -- With the wheel, the main bar (its experience bar) spans the middle half of the screen, and the stance and pet
    -- buttons (30 high) are a sixteenth of the screen's height.
    local barScale = wheel and min(1, ScreenWidth() * 0.5 / MainMenuBar:GetWidth()) or 1
    MainMenuBar:SetScale(barScale)
    MainMenuBar:ClearAllPoints()
    MainMenuBar:SetPoint("BOTTOM", UIParent, "BOTTOM", 0, wheel and -40 or 0)

    for _, name in ipairs(SMALL_BARS) do
        if _G[name] then
            _G[name]:SetScale(wheel and ScreenHeight() * 0.06 / 30 / barScale or 1)
        end
    end
end

-- The quest tracker goes left of the minimap, under the buffs and above the wheel, instead of down the right edge
-- behind it (UIParent_ManageFramePositions places it, then redraws it for its height).
local function PlaceTracker()
    if not WatchFrame or WatchFrame:IsUserPlaced() or db.style ~= "wheel" then
        return
    end

    local right = -(MinimapCluster and MinimapCluster:GetWidth() or 192) - 12
    WatchFrame:SetPoint("TOPRIGHT", UIParent, "TOPRIGHT", right, -ScreenHeight() * 0.17)
    WatchFrame:SetPoint("BOTTOMRIGHT", UIParent, "BOTTOMRIGHT", right, ScreenHeight() * 0.6)
end

hooksecurefunc("UIParent_ManageFramePositions", function()
    if db then
        PlaceTracker()
    end
end)

------------------------------------------------------------------------------------------------------------------------
-- The layout.

local function PlaceChat()
    if not ChatFrame1 then
        return
    end

    -- Above the joystick's area and under the unit frames, whatever the chat windows were saved with (the game restores
    -- those after the add-ons load, so this is done again when it does).
    ChatFrame1:ClearAllPoints()
    ChatFrame1:SetPoint("BOTTOMLEFT", UIParent, "BOTTOMLEFT", ScreenWidth() * 0.02, ScreenHeight() * 0.5)
    ChatFrame1:SetWidth(ScreenWidth() * 0.3)
    ChatFrame1:SetHeight(ScreenHeight() * 0.22)
end

local function Layout()
    local width = ScreenWidth()
    local height = ScreenHeight()
    local wheel = db.style == "wheel"

    -- The right thumb's corner: jump in it, the wheel around it, the page button in the corner itself. In the bars style
    -- it sits above the bars and left of the right-hand ones.
    local centerX = width - height * (wheel and CORNER or 0.32)
    local centerY = height * (wheel and CORNER or 0.32)

    SizeRound(buttons.jump, height * JUMP_SIZE)
    PlaceAt(buttons.jump, centerX, centerY)

    SizeRound(buttons.page, height * SMALL_SIZE)
    PlaceAt(buttons.page, centerX + height * 0.115, centerY - height * 0.115)

    -- A fan of three rows of four, keys 1 to 4 closest, each row from just past straight up to just past straight left.
    for index, slot in ipairs(slots) do
        local radius = height * WHEEL_ROWS[floor((index - 1) / 4) + 1]
        local angle = math.rad(97.5 + ((index - 1) % 4) * 30)
        SizeRound(slot, height * SLOT_SIZE)
        PlaceAt(slot, centerX + math.cos(angle) * radius, centerY + math.sin(angle) * radius)
    end

    -- Interact and the two targetings in a column left of the wheel, interact lowest.
    local columnX = centerX - height * (wheel and 0.54 or 0.17)
    local columnY = centerY - height * (wheel and 0.09 or 0.05)

    for i, name in ipairs({ "interact", "targetEnemy", "targetFriend" }) do
        SizeRound(buttons[name], height * SMALL_SIZE)
        PlaceAt(buttons[name], columnX, columnY + height * 0.11 * (i - 1))
    end

    -- Chat, the menu and the show / hide toggle at the top, between the unit frames and the minimap.
    for i, name in ipairs({ "chat", "menu", "toggle" }) do
        SizeRound(buttons[name], height * SMALL_SIZE)
        PlaceAt(buttons[name], width * 0.5 + (i - 2) * height * SMALL_SIZE * 1.35, height * (1 - SMALL_SIZE * 0.7))
    end

    -- The joystick's area: the bottom left, clear of the main bar (its experience bar, and the stance and pet bars
    -- that start at its left end).
    local joystickWidth = wheel and width * 0.25 - 8 or width * 0.33
    local joystickBottom = wheel and 0 or (MainMenuBar:GetTop() or 53)

    joystickArea:ClearAllPoints()
    joystickArea:SetPoint("BOTTOMLEFT", UIParent, "BOTTOMLEFT", 0, joystickBottom)
    joystickArea:SetWidth(joystickWidth)
    joystickArea:SetHeight(height * 0.48 - joystickBottom)
    joystickRestX = min(0.14, joystickWidth / width * 0.55)
    joystickRestY = (joystickBottom + height * 0.15) / height

    PlaceChat()

    if InCombatLockdown() then
        return
    end

    if wheel then
        FillMenu()
        Stash(MainMenuBarArtFrame)
        Stash(BonusActionBarFrame)
        Stash(MultiBarBottomLeft)
        Stash(MultiBarBottomRight)
        Stash(MultiBarRight)
        Stash(MultiBarLeft)
        styleButton:SetText("Action bars")
    else
        EmptyMenu()
        Unstash()
        styleButton:SetText("Thumb wheel")
    end

    PlaceGameBars()
    UIParent_ManageFramePositions()
end

-- The page: the bar's, shown on the page button, which pages on (ACTIONPAGE1 to 6, SHIFT-1 to SHIFT-6 by default) to
-- the next page with anything on it.
local function UpdatePage()
    local pages = NUM_ACTIONBAR_PAGES or 6
    local current = GetActionBarPage and GetActionBarPage() or 1
    buttons.page.text:SetText(current)

    local nextPage = 1

    for step = 1, pages do
        local page = (current - 1 + step) % pages + 1
        local used = page == 1

        for slot = 1, NUM_WHEEL do
            used = used or HasAction((page - 1) * 12 + slot)
        end

        if used then
            nextPage = page
            break
        end
    end

    controls.page.key = GetBindingKey("ACTIONPAGE" .. nextPage)
end

-- Into the world on an empty page (keys 1 to = have nothing), the wheel turns to the first page that has something.
-- At login the actions arrive a moment after the world does: tried until some page has one (true), for a while.
local firstPageUntil

local function FirstUsedPage()
    if InCombatLockdown() or not ChangeActionBarPage then
        return false
    end

    local function Used(page)
        for slot = 1, NUM_WHEEL do
            if HasAction((page - 1) * 12 + slot) then
                return true
            end
        end

        return false
    end

    if Used(GetActionBarPage()) then
        return true
    end

    for page = 1, NUM_ACTIONBAR_PAGES or 6 do
        if Used(page) then
            ChangeActionBarPage(page)
            return true
        end
    end

    return false
end

local function ApplyHidden()
    local wheel = db.style == "wheel"

    for name, button in pairs(buttons) do
        if name ~= "toggle" then
            if not db.hidden and (name ~= "page" or wheel) then
                button:Show()
            else
                button:Hide()
            end
        end
    end

    if db.hidden then
        joystickArea:Hide()
    else
        joystickArea:Show()
    end

    if wheel and not db.hidden then
        wheelFrame:Show()
    else
        wheelFrame:Hide()
    end

    buttons.toggle.icon:SetDesaturated(db.hidden)
    buttons.toggle:SetAlpha(db.hidden and 0.6 or 1)
    SyncControls()
end

function TouchControls_Toggle()
    db.hidden = not db.hidden
    ApplyHidden()
end

-- The add-on's actions get their keys once (the player can move them in Key Bindings).
local function EnsureBindings()
    local changed = false

    for key, action in pairs({ F13 = "TOUCHCONTROLS_INTERACT", F14 = "TOUCHCONTROLS_TARGETNONHOSTILE", F15 = "TOUCHCONTROLS_TOGGLE", F16 = "TOUCHCONTROLS_MENU" }) do
        if not GetBindingKey(action) and (GetBindingAction(key) or "") == "" then
            SetBinding(key, action)
            changed = true
        end
    end

    if changed then
        SaveBindings(GetCurrentBindingSet())
    end

    -- The controls press whatever keys these actions are bound to now.
    controls.interact.key = GetBindingKey("TOUCHCONTROLS_INTERACT")
    controls.targetFriend.key = GetBindingKey("TOUCHCONTROLS_TARGETNONHOSTILE")
    controls.toggle.key = GetBindingKey("TOUCHCONTROLS_TOGGLE")
    controls.menu.key = GetBindingKey("TOUCHCONTROLS_MENU")
    controls.targetEnemy.key = GetBindingKey("TARGETNEARESTENEMY") or "TAB"
    controls.jump.key = GetBindingKey("JUMP") or "SPACE"

    for index = 1, NUM_WHEEL do
        controls["action" .. index].key = GetBindingKey("ACTIONBUTTON" .. index)
    end
end

function Refresh()
    Layout()
    EnsureBindings()
    UpdatePage()
    ApplyHidden()
end

local function SetStyle(style)
    if InCombatLockdown() then
        UIErrorsFrame:AddMessage("Touch Controls: after combat.", 1, 0.1, 0.1)
        return
    end

    db.style = style
    Refresh()
end

styleButton:SetScript("OnClick", function()
    menu:Hide()
    SetStyle(db.style == "wheel" and "bars" or "wheel")
end)

-- Pressed buttons light every frame; the wheel follows the actions ten times a second; the areas are resent four times
-- a second (frames move, windows open over them).
local elapsedSinceSlots = 0
local elapsedSinceSync = 0

addon:SetScript("OnUpdate", function(self, elapsed)
    if not db then
        return
    end

    for name, control in pairs(controls) do
        local pressed = control.frame.pressed

        if pressed then
            if TouchControls_IsHeld(name) then
                pressed:Show()
            else
                pressed:Hide()
            end
        end
    end

    elapsedSinceSlots = elapsedSinceSlots + elapsed

    if elapsedSinceSlots > 0.1 and wheelFrame:IsVisible() then
        elapsedSinceSlots = 0

        for index, slot in ipairs(slots) do
            UpdateSlot(slot, index)
        end
    end

    elapsedSinceSync = elapsedSinceSync + elapsed

    if elapsedSinceSync > 0.25 then
        elapsedSinceSync = 0
        SyncControls()

        if firstPageUntil and (FirstUsedPage() or GetTime() > firstPageUntil) then
            firstPageUntil = nil
        end

        if refreshAt and GetTime() > refreshAt then
            refreshAt = nil
            Refresh()

            if WatchFrame_Update then
                WatchFrame_Update(WatchFrame)
            end
        end
    end
end)

addon:RegisterEvent("ADDON_LOADED")
addon:RegisterEvent("PLAYER_ENTERING_WORLD")
addon:RegisterEvent("PLAYER_REGEN_ENABLED")
addon:RegisterEvent("ACTIONBAR_PAGE_CHANGED")
addon:RegisterEvent("ACTIONBAR_SLOT_CHANGED")
addon:RegisterEvent("UPDATE_BINDINGS")
addon:RegisterEvent("UI_SCALE_CHANGED")
addon:RegisterEvent("DISPLAY_SIZE_CHANGED")
addon:RegisterEvent("UPDATE_CHAT_WINDOWS")
addon:RegisterEvent("UNIT_EXITED_VEHICLE")

addon:SetScript("OnEvent", function(self, event, name)
    if event == "ADDON_LOADED" then
        if name ~= "TouchControls" then
            return
        end

        TouchControlsDB = TouchControlsDB or {}
        db = TouchControlsDB

        for key, value in pairs(defaults) do
            if db[key] == nil then
                db[key] = value
            end
        end

        WidenGameSlider()

        if db.textScale ~= 1 then
            ApplyTextScale(db.textScale)
        end

        return
    end

    if not db then
        return
    end

    if event == "PLAYER_ENTERING_WORLD" and db.style == "wheel" then
        firstPageUntil = GetTime() + 15
    end

    -- The first time on this screen: an interface size that fits it, and chat text to match.
    if event == "PLAYER_ENTERING_WORLD" and not db.displayChosen then
        db.displayChosen = true
        SetUiScale(RecommendedScale())
        SetChatSize(16)
        refreshAt = GetTime() + 0.3
    end

    if event == "UPDATE_CHAT_WINDOWS" then
        PlaceChat()
    elseif event == "ACTIONBAR_PAGE_CHANGED" or event == "ACTIONBAR_SLOT_CHANGED" then
        UpdatePage()
    elseif event == "UPDATE_BINDINGS" then
        EnsureBindings()
        UpdatePage()
    elseif event == "UNIT_EXITED_VEHICLE" then
        if name == "player" then
            PlaceGameBars()
        end
    else
        -- Into the world, out of combat again, or the screen changed: anything put off is done now.
        Refresh()
    end
end)

SLASH_TOUCHCONTROLS1 = "/touch"

SlashCmdList.TOUCHCONTROLS = function(message)
    message = strlower(strtrim(message or ""))

    if message == "wheel" or message == "bars" then
        SetStyle(message)
    elseif message == "debug" then
        -- Every control's area, in Logs/Console.txt.
        for name, control in pairs(controls) do
            local left, bottom, right, top = Fractions(control.frame)
            TouchControls_Log(format("control %s key %s visible %s area %s %s %s %s", name, tostring(control.key),
                tostring(control.frame:IsVisible()), tostring(left), tostring(bottom), tostring(right), tostring(top)))
        end

        -- Which slots of each bar page hold something.
        for page = 1, NUM_ACTIONBAR_PAGES or 6 do
            local used = {}

            for slot = 1, 12 do
                if HasAction((page - 1) * 12 + slot) then
                    tinsert(used, slot)
                end
            end

            TouchControls_Log(format("page %d slots %s", page, table.concat(used, " ")))
        end

        TouchControls_Log(format("screen %.0f x %.0f, UIParent scale %.2f, style %s", ScreenWidth(), ScreenHeight(), UIParent:GetEffectiveScale(), db.style))
    elseif message == "hide" or message == "show" then
        db.hidden = message == "hide"
        ApplyHidden()
    else
        DEFAULT_CHAT_FRAME:AddMessage("Touch Controls: /touch wheel, /touch bars (action buttons), /touch hide, /touch show.")
    end
end
