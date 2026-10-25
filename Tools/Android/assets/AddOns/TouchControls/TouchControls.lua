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
BINDING_NAME_TOUCHCONTROLS_NEXTGROUP = "Touch controls: next buttons"

local defaults = {
    style = "wheel",    -- "wheel": the thumb wheel; "bars": the default action bars
    hidden = false,
    textScale = 1.15,   -- the game's fonts, as a multiple of their own sizes
    displayChosen = false,  -- whether the interface size for this screen has been set (once, then it's the player's)
    autoLootChosen = false, -- whether auto loot has been turned on (once, likewise)
    frameRate = 60,     -- 30 saves the battery
    cameraSpeed = 1,    -- the camera drag's turn per pixel
    cameraInvert = false,
    wheelCount = 12,    -- the wheel's buttons on screen (WHEEL_LAYOUTS)
    graphicsChosen = false, -- whether the phone's graphics defaults have been set (once, then they're the player's)
    glowChosen = false, -- whether the full-screen glow has been turned off (once, likewise)
}

local db
local controls = {}         -- name -> { frame, key }: the areas the input layer knows
local NUM_WHEEL = 12        -- the whole bar, keys 1 to =

-- The layout, in fractions of the screen's height (sizes keep their shape at any UI scale and screen width).
local CORNER = 0.16         -- the jump button's center, in from the bottom right corner
local JUMP_SIZE = 0.14
local SMALL_SIZE = 0.072    -- the other round buttons
-- The wheel packs into the corner, so the screen's right side above it is the world's, for the right thumb to turn
-- the camera on. How many buttons it shows is an option: fewer are bigger, with more room between them, and the rest
-- of the bar is a press of the page button away. Each layout's button size and arcs (radius from jump's center, how
-- many, and from which angle to which, 90 straight up), every button clear of its neighbours by a fingertip.
local WHEEL_LAYOUTS = {
    [12] = { slot = 0.075, rows = { { radius = 0.24, count = 5 }, { radius = 0.37, count = 7 } } },
    [8] = { slot = 0.09, rows = { { radius = 0.25, count = 3 }, { radius = 0.37, count = 5 } } },
    [6] = { slot = 0.105, rows = { { radius = 0.25, count = 3 }, { radius = 0.385, count = 3, from = 116, to = 164 } } },
    [4] = { slot = 0.12, rows = { { radius = 0.3, count = 4 } } },
}
local WHEEL_SIZES = { 4, 6, 8, 12 }
local WHEEL_FROM, WHEEL_TO = 95, 185

-- The keys the wheel shows: those after this many (a group of the bar, with fewer buttons than keys).
local wheelOffset = 0
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

-- flags for the input layer: "round" (the circle in the frame), "lift" (pressed when the finger lifts, so dragging
-- off the button turns the camera instead of pressing it).
local function Register(name, frame, key, flags)
    controls[name] = { frame = frame, key = key, flags = flags }
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

    -- The textures sized to the ring's opening (SizeRound).
    button.holeTextures = { button.disc, button.icon, button.pressed }

    return button
end

local function SizeRound(button, size)
    button:SetWidth(size)
    button:SetHeight(size)

    local hole = size * RING_HOLE

    for _, texture in ipairs(button.holeTextures) do
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

-- Where it was last placed, so a resting or still joystick isn't laid out again every frame.
local placedX, placedY, placedPushX, placedPushY, placedHeight

joystickArea:SetScript("OnUpdate", function(self)
    local held, x, y, pushX, pushY = TouchControls_GetJoystick()

    if not held then
        x, y, pushX, pushY = joystickRestX, joystickRestY, 0, 0
    end

    local height = ScreenHeight()

    if x ~= placedX or y ~= placedY or pushX ~= placedPushX or pushY ~= placedPushY or height ~= placedHeight then
        placedX, placedY, placedPushX, placedPushY, placedHeight = x, y, pushX, pushY, height
        PlaceJoystick(x, y, pushX, pushY)
    end

    -- Faded through the frame: a texture's own alpha is its vertex color's, which gives the discs their shade.
    self:SetAlpha(held and 1 or 0.55)
end)

Register("joystick", joystickArea, "JOYSTICK")

------------------------------------------------------------------------------------------------------------------------
-- The overlay buttons, each pressing a key.

local buttons = {}

local function AddButton(name, icon, key, flags)
    local button = MakeRound("TouchControls" .. name, addon)
    SetRoundIcon(button, icon)
    buttons[name] = button
    Register(name, button, key, flags or "round,lift")
    return button
end

-- Jump jumps the moment it's touched (a drag from it still turns the camera after); the rest press on lift.
AddButton("jump", "Interface\\Icons\\Ability_Rogue_Sprint", "SPACE", "round")
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
    Register("action" .. index, slot, nil, "round,lift")
end

-- The button keys index would press now: a vehicle's, a stance's or possession's bar, or the main bar.
local function WheelButton(index)
    -- ActionButtonDown's choice: the vehicle has the first six keys, the rest go on to the bars.
    if VehicleMenuBar and VehicleMenuBar:IsShown() and index <= (VEHICLE_MAX_ACTIONBUTTONS or 6) then
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
    "VideoOptionsFrame", "AudioOptionsFrame", "KeyBindingFrame", "MacroFrame", "MacroPopupFrame", "ChatConfigFrame",
    "HelpFrame", "StaticPopup1", "StaticPopup2", "StaticPopup3", "StaticPopup4", "ItemRefTooltip", "TradeFrame",
    "MerchantFrame", "GossipFrame", "QuestFrame", "TaxiFrame", "BankFrame", "MailFrame", "OpenMailFrame",
    "AuctionFrame", "ReadyCheckFrame", "DropDownList1", "DropDownList2", "StackSplitFrame", "ColorPickerFrame",
    "GearManagerDialog", "LFDDungeonReadyDialog", "LFDDungeonReadyPopup", "LFDRoleCheckPopup", "GroupLootFrame1",
    "GroupLootFrame2", "GroupLootFrame3", "GroupLootFrame4" }
local panelSides = { "left", "center", "right", "doublewide", "fullscreen" }

-- The open windows' areas, gathered once a sync (four numbers each).
local openAreas = {}

local function GatherOpen()
    local count = 0

    local function Add(frame)
        if frame and frame:IsVisible() then
            local left, bottom, right, top = Fractions(frame)

            if left then
                openAreas[count + 1], openAreas[count + 2], openAreas[count + 3], openAreas[count + 4] = left, bottom, right, top
                count = count + 4
            end
        end
    end

    for _, name in ipairs(covering) do
        Add(_G[name])
    end

    for index = 1, NUM_CONTAINER_FRAMES or 13 do
        Add(_G["ContainerFrame" .. index])
    end

    for _, side in ipairs(panelSides) do
        Add(GetUIPanel(side))
    end

    for index = count + 1, #openAreas do
        openAreas[index] = nil
    end
end

local function Covered(left, bottom, right, top)
    for index = 1, #openAreas, 4 do
        if openAreas[index] < right and openAreas[index + 2] > left and openAreas[index + 1] < top and openAreas[index + 3] > bottom then
            return true
        end
    end

    return false
end

-- Sends every visible, uncovered control's area to the input layer, and drops the others.
local function SyncControls()
    GatherOpen()

    for name, control in pairs(controls) do
        local frame = control.frame
        local sent = false

        if (not db.hidden or name == "toggle") and frame:IsVisible() and control.key then
            local left, bottom, right, top = Fractions(frame)

            if left and not Covered(left, bottom, right, top) then
                TouchControls_SetControl(name, left, bottom, right, top, control.key, control.flags)
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

-- Render resolution: the screen's shape at the sizes the device offers, the largest (the screen's own) first. Fewer
-- pixels is the most a phone saves, and on a small screen it shows little.
local function RenderModes()
    local modes = {}

    for _, text in ipairs({ GetScreenResolutions() }) do
        local width, height = strmatch(text, "(%d+)x(%d+)")

        if width then
            tinsert(modes, { text = text, height = tonumber(height) })
        end
    end

    sort(modes, function(a, b) return a.height > b.height end)
    return modes
end

local renderModes = RenderModes()
local pendingRenderMode, pendingRenderAt     -- a render resolution to switch to a moment from now

-- The slider runs from the smallest (1) to the screen's own (the last).
local function RenderMode(value)
    return renderModes[#renderModes - floor(value + 0.5) + 1]
end

local function SetRenderMode(mode)
    if mode and mode.text ~= GetCVar("gxResolution") then
        SetCVar("gxResolution", mode.text)
        RestartGx()
    end
end

local renderSlider = MakeSlider("TouchControlsMenuRender", "Render resolution", 1, max(#renderModes, 1), 1,
    function(value)
        local mode = RenderMode(value)
        return mode and format("%d%% (%s)", mode.height / renderModes[1].height * 100 + 0.5, mode.text) or "-"
    end,
    function(value)
        SetRenderMode(RenderMode(value))
    end)

local cameraSlider = MakeSlider("TouchControlsMenuCamera", "Camera speed", 0.3, 3, 0.1,
    function(value) return format("%d%%", value * 100 + 0.5) end,
    function(value)
        db.cameraSpeed = floor(value * 10 + 0.5) / 10
        TouchControls_SetCamera(db.cameraSpeed, db.cameraInvert)
    end, true)

-- Buttons on screen: fewer are bigger, the rest of the bar a press of the page button away.
local wheelSlider = MakeSlider("TouchControlsMenuWheel", "Buttons on screen", 1, #WHEEL_SIZES, 1,
    function(value) return tostring(WHEEL_SIZES[floor(value + 0.5)]) end,
    function(value)
        local count = WHEEL_SIZES[floor(value + 0.5)]

        if count and count ~= db.wheelCount then
            db.wheelCount = count
            wheelOffset = 0
            Refresh()
        end
    end, true)

local displaySliders = { uiScaleSlider, textSlider, chatSlider, renderSlider, cameraSlider, wheelSlider }

-- Battery saver: the game held to 30 frames a second.
local batteryCheck = CreateFrame("CheckButton", "TouchControlsMenuBattery", displayHolder, "UICheckButtonTemplate")
TouchControlsMenuBatteryText:SetText("Battery saver (30 fps)")
batteryCheck:SetScript("OnClick", function(self)
    db.frameRate = self:GetChecked() and 30 or 60
    TouchControls_SetFrameRate(db.frameRate)
end)

-- The full-screen glow: its copies of the frame cost a phone's GPU about two thirds of its frame rate at 1080 lines.
local glowCheck = CreateFrame("CheckButton", "TouchControlsMenuGlow", displayHolder, "UICheckButtonTemplate")
TouchControlsMenuGlowText:SetText("Glow (slower)")
glowCheck:SetScript("OnClick", function(self)
    SetCVar("ffxGlow", self:GetChecked() and "1" or "0")
end)

local invertCheck = CreateFrame("CheckButton", "TouchControlsMenuInvert", displayHolder, "UICheckButtonTemplate")
TouchControlsMenuInvertText:SetText("Invert camera")
invertCheck:SetScript("OnClick", function(self)
    db.cameraInvert = self:GetChecked() and true or false
    TouchControls_SetCamera(db.cameraSpeed, db.cameraInvert)
end)

local function UpdateSliders()
    updatingSliders = true
    uiScaleSlider:SetValue(tonumber(GetCVar("uiScale")) or 1)
    textSlider:SetValue(db.textScale)
    chatSlider:SetValue(select(2, ChatFrame1:GetFont()) or 14)

    for index, mode in ipairs(renderModes) do
        if mode.text == GetCVar("gxResolution") then
            renderSlider:SetValue(#renderModes - index + 1)
        end
    end

    batteryCheck:SetChecked(db.frameRate == 30)
    invertCheck:SetChecked(db.cameraInvert)
    glowCheck:SetChecked(GetCVar("ffxGlow") == "1")
    cameraSlider:SetValue(db.cameraSpeed)

    for index, count in ipairs(WHEEL_SIZES) do
        if count == db.wheelCount then
            wheelSlider:SetValue(index)
        end
    end
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
    displayHolder:SetHeight(sliderRow * #displaySliders + 60)

    for index, slider in ipairs(displaySliders) do
        slider:ClearAllPoints()
        slider:SetPoint("TOP", displayHolder, "TOP", 0, -sliderRow * (index - 1) - 20)
    end

    -- Two columns of check boxes under the sliders: the battery and the glow, the camera's invert.
    local checksTop = -sliderRow * #displaySliders - 4
    batteryCheck:ClearAllPoints()
    batteryCheck:SetPoint("TOPLEFT", displayHolder, "TOPLEFT", 0, checksTop)
    glowCheck:ClearAllPoints()
    glowCheck:SetPoint("TOPLEFT", displayHolder, "TOPLEFT", 0, checksTop - 28)
    invertCheck:ClearAllPoints()
    invertCheck:SetPoint("TOPLEFT", displayHolder, "TOP", 10, checksTop)

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

    -- A short block beside the minimap: the screen's right side under it is left to the camera.
    local right = -(MinimapCluster and MinimapCluster:GetWidth() or 192) - 12
    WatchFrame:SetPoint("TOPRIGHT", UIParent, "TOPRIGHT", right, -ScreenHeight() * 0.15)
    WatchFrame:SetPoint("BOTTOMRIGHT", UIParent, "BOTTOMRIGHT", right, ScreenHeight() * 0.68)
end

------------------------------------------------------------------------------------------------------------------------
-- The layout.

local function PlaceChat()
    if not ChatFrame1 then
        return
    end

    -- Above the joystick's area and under the unit frames, whatever the chat windows were saved with (the game restores
    -- those after the add-ons load, so this is done again when it does).
    -- Clear of a camera hole on that side (its side buttons hang off its left edge).
    local _, insetLeft = TouchControls_GetScreen()
    ChatFrame1:ClearAllPoints()
    ChatFrame1:SetPoint("BOTTOMLEFT", UIParent, "BOTTOMLEFT", ScreenWidth() * (0.02 + (insetLeft or 0)), ScreenHeight() * 0.5)
    ChatFrame1:SetWidth(ScreenWidth() * 0.3)
    ChatFrame1:SetHeight(ScreenHeight() * 0.22)
end

-- The game places the tracker and a chat window that was never dragged on every pass of its frame manager (stances,
-- pets, bags, vehicles): they go back where the touch layout has them after it.
hooksecurefunc("UIParent_ManageFramePositions", function()
    if db then
        PlaceTracker()
        PlaceChat()
    end
end)

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

    -- The page button in the corner itself, below and right of jump.
    SizeRound(buttons.page, height * SMALL_SIZE)
    PlaceAt(buttons.page, centerX + height * 0.1, centerY - height * 0.1)

    -- The wheel's arcs in the chosen size, the first keys on the inner one; the buttons past it hidden.
    local layout = WHEEL_LAYOUTS[db.wheelCount] or WHEEL_LAYOUTS[12]
    local reach = 0
    local index = 1

    for _, row in ipairs(layout.rows) do
        local from, to = row.from or WHEEL_FROM, row.to or WHEEL_TO
        reach = max(reach, row.radius + layout.slot * 0.5)

        for i = 1, row.count do
            local slot = slots[index]
            local angle = math.rad(row.count > 1 and from + (to - from) * (i - 1) / (row.count - 1) or (from + to) / 2)
            SizeRound(slot, height * layout.slot)
            PlaceAt(slot, centerX + math.cos(angle) * height * row.radius, centerY + math.sin(angle) * height * row.radius)
            slot:Show()
            index = index + 1
        end
    end

    for i = index, NUM_WHEEL do
        slots[i]:Hide()
    end

    -- Interact and the two targetings in a row along the bottom, clear of the wheel's widest reach, interact nearest
    -- the thumb. In the bars style, a column left of jump.
    for i, name in ipairs({ "interact", "targetEnemy", "targetFriend" }) do
        SizeRound(buttons[name], height * SMALL_SIZE)

        if wheel then
            PlaceAt(buttons[name], centerX - height * (reach + SMALL_SIZE * 0.5 + 0.04 + 0.1 * (i - 1)), height * 0.07)
        else
            PlaceAt(buttons[name], centerX - height * 0.17, centerY - height * 0.05 + height * 0.11 * (i - 1))
        end
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

    -- The menu's buttons aren't protected: it's ready even after a login in combat.
    if wheel then
        FillMenu()
    end

    if InCombatLockdown() then
        return
    end

    -- The form, stealth and possession bar stays on the main bar, below the screen's edge and see-through: its own
    -- OnUpdate shows and hides it, which a hidden parent would stop, leaving keys 1 to = on it for good.
    if BonusActionBarFrame then
        BonusActionBarFrame:SetAlpha(wheel and 0 or 1)
    end

    if wheel then
        Stash(MainMenuBarArtFrame)
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
    PlaceTracker()
end

-- The wheel's buttons press the keys of the group it shows (ACTIONBUTTON1 to 12 from the offset).
local function SetWheelKeys()
    for index = 1, NUM_WHEEL do
        local key = wheelOffset + index
        controls["action" .. index].key = key <= NUM_WHEEL and GetBindingKey("ACTIONBUTTON" .. key) or nil
    end
end

-- The page: the bar's, shown on the page button, which pages on (ACTIONPAGE1 to 6, SHIFT-1 to SHIFT-6 by default) to
-- the next page with anything on it.
-- Whether keys 1 to = are on a vehicle's bar, which no page changes (a form's or possession's bar takes the place of
-- page 1 only, GroupUsed).
local function OtherBar()
    return VehicleMenuBar and VehicleMenuBar:IsShown()
end

-- The bar's keys in groups the wheel's size (12 keys in 2 groups of 6, or 3 of 4, ...).
local function WheelGroups()
    return ceil(NUM_WHEEL / db.wheelCount)
end

-- Whether a group of a page has anything on it. In a stance, form or possession, page 1 is that bar
-- (ActionButton_CalculateAction's bonus pages, after the six).
local function GroupUsed(page, group)
    local offset = GetBonusBarOffset()
    local base = (page == 1 and offset > 0) and ((NUM_ACTIONBAR_PAGES or 6) + offset - 1) * 12 or (page - 1) * 12

    for key = (group - 1) * db.wheelCount + 1, min(group * db.wheelCount, NUM_WHEEL) do
        if HasAction(base + key) then
            return true
        end
    end

    return false
end

-- The group the page button goes to next when it is on this page (nil: another page, through its key).
local nextGroup
-- The page the page button turns the bar to, and the group to show once it has.
local pendingPage, pendingGroup

-- The page button: the next group with anything in it, this page's or the next pages' (page 1's first group whatever
-- is in it). Within the page, its binding (TOUCHCONTROLS_NEXTGROUP) moves the wheel along; to another page, that
-- page's binding (ACTIONPAGE1 to 6, SHIFT-1 to SHIFT-6 by default) turns the bar, a key press so it works in combat.
local function UpdatePage()
    local pages = NUM_ACTIONBAR_PAGES or 6
    local groups = WheelGroups()
    local current = GetActionBarPage and GetActionBarPage() or 1
    local group = floor(wheelOffset / db.wheelCount) + 1

    buttons.page.text:SetText(groups > 1 and format("%d%s", current, strsub("abc", group, group)) or current)

    -- On a vehicle's bar the page doesn't count: the page button only steps through its groups.
    if OtherBar() then
        nextGroup, pendingPage, pendingGroup = group % groups + 1, nil, nil
        controls.page.key = groups > 1 and GetBindingKey("TOUCHCONTROLS_NEXTGROUP") or nil
        return
    end

    local here = (current - 1) * groups + group - 1
    local targetPage, targetGroup = 1, 1

    for step = 1, pages * groups do
        local position = (here + step) % (pages * groups)
        local page, candidate = floor(position / groups) + 1, position % groups + 1

        if (page == 1 and candidate == 1) or GroupUsed(page, candidate) then
            targetPage, targetGroup = page, candidate
            break
        end
    end

    if targetPage == current then
        nextGroup, pendingPage, pendingGroup = targetGroup, nil, nil
        controls.page.key = GetBindingKey("TOUCHCONTROLS_NEXTGROUP")
    else
        nextGroup, pendingPage, pendingGroup = nil, targetPage, targetGroup
        controls.page.key = GetBindingKey("ACTIONPAGE" .. targetPage)
    end
end

-- The wheel to a group of the bar's keys.
local function ShowGroup(group)
    wheelOffset = (group - 1) * db.wheelCount
    SetWheelKeys()
    UpdatePage()
end

function TouchControls_NextGroup()
    if nextGroup then
        ShowGroup(nextGroup)
    end
end

-- Into the world on an empty page (keys 1 to = have nothing), the wheel turns to the first page that has something.
-- At login the actions arrive a moment after the world does: tried until some page has one (true), for a while.
local firstPageUntil

local function FirstUsedPage()
    if InCombatLockdown() or not ChangeActionBarPage then
        return false
    end

    -- In a stance or form, keys 1 to = are its bar's, which follow page 1 only.
    if GetBonusBarOffset() > 0 then
        return true
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
            wheelOffset = 0
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

-- The add-on's actions get keys for the session where the player hasn't given them any (Key Bindings can move them).
-- Never saved: the bindings come from the server a moment after login, and a set saved before they arrive holds
-- nothing but these, which unbinds every default key (movement, Enter, the mouse on the world) for the account.
local ensuringBindings = false

local function EnsureBindings()
    -- SetBinding signals UPDATE_BINDINGS, which comes back here.
    if ensuringBindings then
        return
    end

    -- Account (1) or character (2) once the bindings have loaded, and out of combat; until then UPDATE_BINDINGS or
    -- PLAYER_REGEN_ENABLED brings this back.
    local set = GetCurrentBindingSet()

    if (set == 1 or set == 2) and not InCombatLockdown() then
        ensuringBindings = true

        for key, action in pairs({ F13 = "TOUCHCONTROLS_INTERACT", F14 = "TOUCHCONTROLS_TARGETNONHOSTILE", F15 = "TOUCHCONTROLS_TOGGLE", F16 = "TOUCHCONTROLS_MENU", F17 = "TOUCHCONTROLS_NEXTGROUP" }) do
            if not GetBindingKey(action) and (GetBindingAction(key) or "") == "" then
                SetBinding(key, action)
            end
        end

        ensuringBindings = false
    end

    -- The controls press whatever keys these actions are bound to now.
    controls.interact.key = GetBindingKey("TOUCHCONTROLS_INTERACT")
    controls.targetFriend.key = GetBindingKey("TOUCHCONTROLS_TARGETNONHOSTILE")
    controls.toggle.key = GetBindingKey("TOUCHCONTROLS_TOGGLE")
    controls.menu.key = GetBindingKey("TOUCHCONTROLS_MENU")
    controls.targetEnemy.key = GetBindingKey("TARGETNEARESTENEMY") or "TAB"
    controls.jump.key = GetBindingKey("JUMP") or "SPACE"

    -- A double tap on the joystick presses autorun's key (NUMLOCK by default).
    TouchControls_SetAutorunKey(GetBindingKey("TOGGLEAUTORUN"))

    SetWheelKeys()
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
local elapsedSinceInsets = 0
local lastOtherBar
local lastInsetLeft

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

        for index = 1, db.wheelCount do
            UpdateSlot(slots[index], wheelOffset + index)
        end

        -- A vehicle's bar takes keys 1 to = whatever the page: the page button rests, unless the wheel shows the bar in
        -- groups it steps through. Getting in or out repages.
        local otherBar = OtherBar() and true or false
        buttons.page:SetAlpha(otherBar and WheelGroups() == 1 and 0.35 or 1)

        if otherBar ~= lastOtherBar then
            lastOtherBar = otherBar
            UpdatePage()
        end
    end

    elapsedSinceSync = elapsedSinceSync + elapsed

    if elapsedSinceSync > 0.25 then
        elapsedSinceSync = 0
        SyncControls()

        if firstPageUntil and (FirstUsedPage() or GetTime() > firstPageUntil) then
            firstPageUntil = nil
        end

        -- Turning the phone end over end moves a camera hole to the other side, with no event for it.
        elapsedSinceInsets = elapsedSinceInsets + 0.25

        if elapsedSinceInsets >= 1 then
            elapsedSinceInsets = 0
            local _, insetLeft = TouchControls_GetScreen()
            local _, _, _, chatX = ChatFrame1:GetPoint(1)

            -- The game puts a chat window back where it was saved (dragged before) after the add-on places it.
            if insetLeft ~= lastInsetLeft or not chatX or abs(chatX - ScreenWidth() * (0.02 + (insetLeft or 0))) > 1 then
                lastInsetLeft = insetLeft
                PlaceChat()
            end
        end

        -- Out of a loading screen or a vehicle the main bar slides back to its own place; the touch one is restored once
        -- it has finished.
        if db.style == "wheel" and not InCombatLockdown() and MainMenuBar:IsShown() and not MainMenuBar.busy and select(5, MainMenuBar:GetPoint(1)) ~= -40 then
            PlaceGameBars()
        end

        if pendingRenderAt and GetTime() > pendingRenderAt and not InCombatLockdown() then
            SetRenderMode(pendingRenderMode)
            pendingRenderMode, pendingRenderAt = nil, nil
        end

        if refreshAt and GetTime() > refreshAt then
            refreshAt = nil
            Refresh()
        end
    end
end)

-- UIParent hides for the full-screen world map, cinematics and Alt-Z, and this frame with it: its controls go too, so
-- taps reach what is showing, and come back with it.
addon:SetScript("OnHide", function()
    TouchControls_ClearControls()
end)

addon:SetScript("OnShow", function()
    if db then
        SyncControls()
    end
end)

addon:RegisterEvent("ADDON_LOADED")
addon:RegisterEvent("PLAYER_ENTERING_WORLD")
addon:RegisterEvent("PLAYER_REGEN_ENABLED")
addon:RegisterEvent("ACTIONBAR_PAGE_CHANGED")
addon:RegisterEvent("ACTIONBAR_SLOT_CHANGED")
addon:RegisterEvent("UPDATE_BONUS_ACTIONBAR")
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
        TouchControls_SetFrameRate(db.frameRate)
        TouchControls_SetCamera(db.cameraSpeed, db.cameraInvert)

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
    -- Looting a body takes everything with one tap, as the Switch port does; once (Interface options can turn it off).
    if event == "PLAYER_ENTERING_WORLD" and not db.autoLootChosen then
        db.autoLootChosen = true
        SetCVar("autoLootDefault", "1")
    end

    -- The first time: a phone's graphics. The render resolution at most 1080 high (a sharper screen shows little more
    -- for the battery and heat), and lighter weather and particles; the view distance and ground clutter are already
    -- modest by default.
    if event == "PLAYER_ENTERING_WORLD" and not db.graphicsChosen and not InCombatLockdown() then
        db.graphicsChosen = true

        if tonumber(GetCVar("weatherDensity")) == 2 then
            SetCVar("weatherDensity", "1")
        end

        if tonumber(GetCVar("particleDensity")) == 1 then
            SetCVar("particleDensity", "0.8")
        end

        -- A moment after the world is in: restarting the device during the loading is asking for trouble.
        for _, mode in ipairs(renderModes) do
            if mode.height <= 1080 then
                pendingRenderMode, pendingRenderAt = mode, GetTime() + 2
                break
            end
        end
    end

    -- The full-screen glow off, once: on a phone's GPU it takes most of the frame rate (22 frames a second against 63
    -- at 1080 lines on a Pixel 7 Pro); the menu turns it back on.
    if event == "PLAYER_ENTERING_WORLD" and not db.glowChosen then
        db.glowChosen = true
        SetCVar("ffxGlow", "0")
    end

    if event == "PLAYER_ENTERING_WORLD" and not db.displayChosen then
        db.displayChosen = true
        SetUiScale(RecommendedScale())
        SetChatSize(16)
        refreshAt = GetTime() + 0.3
    end

    if event == "UPDATE_CHAT_WINDOWS" then
        PlaceChat()
    elseif event == "ACTIONBAR_PAGE_CHANGED" then
        -- To the group the page button was going to, or the page's first.
        local group = pendingPage == GetActionBarPage() and pendingGroup or 1
        wheelOffset = (group - 1) * db.wheelCount
        SetWheelKeys()
        UpdatePage()
    elseif event == "ACTIONBAR_SLOT_CHANGED" or event == "UPDATE_BONUS_ACTIONBAR" then
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

-- A tap skips an in-game cinematic, as Escape does (movies are skipped by the engine: CSimpleMovieFrame).
if CinematicFrame then
    CinematicFrame:HookScript("OnMouseUp", function()
        StopCinematic()
    end)
end

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
