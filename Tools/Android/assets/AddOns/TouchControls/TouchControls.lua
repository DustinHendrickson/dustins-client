-- Touch Controls: the on-screen controls for playing on a phone or tablet (docs/ANDROID_PORT.md, "Touch and small
-- screens"). Packed in the APK and installed into Interface/AddOns at every start, so it always matches the build.
--
-- The game's native input layer owns every finger (Engine/Source/Os/Android/Input.cpp). This add-on draws the controls
-- and tells it where they are (TouchControls_SetControl): a finger landing on one presses its key, or is the joystick,
-- whatever the UI or the world has under it. Spells are cast by their real key bindings (ACTIONBUTTON1 to 12), so they
-- work in combat and follow the player's bindings; the add-on's own actions are bindings too (Bindings.xml), on keys a
-- phone doesn't type (F14 to F19; F13 is Print Screen to the game, the same key code).
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
BINDING_NAME_TOUCHCONTROLS_BAGS = "Touch controls: all bags"
BINDING_NAME_TOUCHCONTROLS_RADIAL = "Touch controls: actions"
BINDING_NAME_TOUCHCONTROLS_ASSIGN = "Touch controls: put a spell on the held button"
BINDING_NAME_TOUCHCONTROLS_DISMOUNT = "Dismount or leave the vehicle"
BINDING_NAME_TOUCHCONTROLS_FOLLOW = "Follow target"
BINDING_NAME_TOUCHCONTROLS_MODIFIER = "Touch controls: Shift / Ctrl / Alt for the next tap"
BINDING_NAME_TOUCHCONTROLS_CHAT = "Hide or show the chat"
BINDING_NAME_TOUCHCONTROLS_EMOTES = "Touch controls: emotes"

local defaults = {
    style = "wheel",    -- "wheel": the thumb wheel; "bars": the default action bars
    hidden = false,
    textScale = 1.15,   -- the game's fonts, as a multiple of their own sizes
    displayChosen = false,  -- whether the interface size for this screen has been set (once, then it's the player's)
    autoLootChosen = false, -- whether auto loot has been turned on (once, likewise)
    touchSettingsChosen = false,    -- whether the interface options suited to touch have been set (once, likewise)
    helpShown = false,  -- whether the touch guide has opened by itself (once, at the first time in the world)
    frameRate = 60,     -- 30 saves the battery
    cameraSpeed = 1,    -- the camera drag's turn per pixel
    cameraInvert = false,
    wheelCount = 12,    -- the wheel's buttons on screen (WHEEL_LAYOUTS)
    lockButtons = false,    -- holding a wheel button doesn't open the spellbook
    emoteButton = true,     -- the emotes button beside the targeting buttons
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
local PAGE_SIZE = 0.1       -- the page button, in the corner beside jump
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
-- (The ring's crop: its circle sits 2.6 texels above the middle of the art's square, so the crop starts that much higher.)
local RING_LEFT, RING_RIGHT, RING_TOP, RING_BOTTOM = 95 / 256, 241 / 256, 30.4 / 256, 176.4 / 256
local RING_HOLE = 0.88      -- the ring's opening, as a fraction of its size
local ICON_TRIM = 0.07      -- an icon's frame, as a fraction of its size
local ICON_SIZE = 0.85      -- a trimmed icon, as a fraction of its button (its cut then ends at the ring's edge)
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

    -- The trimmed icon's round cut reaches past its square: a little smaller, it stays inside the ring.
    button.icon:SetWidth(size * ICON_SIZE)
    button.icon:SetHeight(size * ICON_SIZE)

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
        -- The portrait is the whole icon cut round, its light frame included: the frame's edge is trimmed off.
        SetPortraitToTexture(button.icon, path)
        button.icon:SetTexCoord(ICON_TRIM, 1 - ICON_TRIM, ICON_TRIM, 1 - ICON_TRIM)
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
AddButton("jump", "Interface\\Icons\\INV_Boots_08", "SPACE", "round")
AddButton("interact", "Interface\\Icons\\Ability_Hunter_BeastSoothe", "F18")
AddButton("targetEnemy", "Interface\\Icons\\Ability_Hunter_SniperShot", "TAB")
AddButton("targetFriend", "Interface\\Icons\\Achievement_Reputation_01", "F14")
AddButton("emote", "Interface\\Icons\\Spell_Holy_Silence", nil)
AddButton("chat", "Interface\\Icons\\INV_Letter_15", "ENTER")
AddButton("character", nil, "C")
AddButton("bags", "Interface\\Icons\\INV_Misc_Bag_08", "F19")
AddButton("actions", "Interface\\Icons\\Ability_Rogue_Disguise", "CTRL-F16")
AddButton("modifier", "Interface\\Icons\\INV_Misc_Key_03", "CTRL-F18")
AddButton("leave", "Interface\\Icons\\Spell_Shadow_SacrificialShield", "CTRL-F14", "round")

-- The modifier button shows the latched modifier, and glows while one waits for a finger.
buttons.modifier.text = buttons.modifier:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
buttons.modifier.text:SetPoint("CENTER", 0, 0)
buttons.modifier.glow = buttons.modifier:CreateTexture(nil, "OVERLAY")
buttons.modifier.glow:SetTexture("Interface\\Buttons\\IconBorder-GlowRing")
buttons.modifier.glow:SetBlendMode("ADD")
buttons.modifier.glow:SetPoint("CENTER")
buttons.modifier.glow:Hide()

local MODIFIER_LABELS = { SHIFT = "Shift", CTRL = "Ctrl", ALT = "Alt" }
local MODIFIER_NEXT = { SHIFT = "CTRL", CTRL = "ALT", ALT = nil }

-- Off, Shift, Ctrl, Alt, off: for the next tap, hold or drag on the UI or the world (socketing, splitting stacks,
-- linking to chat, the dressing room, item flyouts).
function TouchControls_CycleModifier()
    local current = TouchControls_GetLatch()
    TouchControls_SetLatch(current and MODIFIER_NEXT[current] or (not current and "SHIFT") or nil)
end

local function UpdateModifier()
    local latch = TouchControls_GetLatch()
    buttons.modifier.text:SetText(latch and MODIFIER_LABELS[latch] or "")

    if latch then
        buttons.modifier.glow:Show()
        buttons.modifier.icon:SetAlpha(0.35)
    else
        buttons.modifier.glow:Hide()
        buttons.modifier.icon:SetAlpha(1)
    end
end
controls.actions.holdAction = "TOUCHCONTROLS_RADIAL"
controls.emote.holdAction = "TOUCHCONTROLS_EMOTES"
AddButton("menu", "Interface\\Icons\\INV_Misc_Gear_01", "F16")
AddButton("toggle", "Interface\\Icons\\INV_Misc_Eye_01", "F15")
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
    controls["action" .. index].holdAction = "TOUCHCONTROLS_ASSIGN"
    controls["action" .. index].wheel = true
end

------------------------------------------------------------------------------------------------------------------------
-- The actions list: things a keyboard does with a hotkey, as big rows in a panel dropped down from the actions button. Each presses its
-- action's binding (a key of the add-on's own where the player has none); a tap anywhere else closes it.

local radialEntries = {
    { name = "radialSit", action = "SITORSTAND", label = "Sit / stand", icon = "Interface\\Icons\\Spell_Nature_Sleep", spare = "ALT-F14" },
    { name = "radialSheathe", action = "TOGGLESHEATH", label = "Sheathe", icon = "Interface\\Icons\\INV_Sword_04", spare = "ALT-F15" },
    { name = "radialWalk", action = "TOGGLERUN", label = "Walk / run", icon = "Interface\\Icons\\Ability_Tracking", spare = "ALT-F16" },
    { name = "radialMap", action = "TOGGLEWORLDMAP", label = "Map", icon = "Interface\\Icons\\INV_Misc_Map_01", spare = "ALT-F17" },
    { name = "radialSelf", action = "TARGETSELF", label = "Target self", icon = "Interface\\Icons\\Spell_Holy_PowerWordShield", spare = "ALT-F18" },
    { name = "radialFollow", action = "TOUCHCONTROLS_FOLLOW", label = "Follow", icon = "Interface\\Icons\\Ability_Rogue_Sprint", spare = "CTRL-F15" },
    { name = "radialAssist", action = "ASSISTTARGET", label = "Assist", icon = "Interface\\Icons\\Ability_Hunter_MarkedForDeath", spare = "SHIFT-F14" },
    { name = "radialFocus", action = "FOCUSTARGET", label = "Set focus", icon = "Interface\\Icons\\Ability_Hunter_SniperShot", spare = "SHIFT-F15" },
    { name = "radialTargetFocus", action = "TARGETFOCUS", label = "Target focus", icon = "Interface\\Icons\\Spell_Holy_MindVision", spare = "SHIFT-F16" },
    { name = "radialReply", action = "REPLY", label = "Reply", icon = "Interface\\Icons\\INV_Letter_15", spare = "SHIFT-F17" },
    { name = "radialStopAttack", action = "STOPATTACK", label = "Stop attack", icon = "Interface\\Icons\\Ability_Rogue_FeignDeath", spare = "SHIFT-F18" },
    { name = "radialPetAttack", action = "PETATTACK", label = "Pet attack", icon = "Interface\\Icons\\Ability_GhoulFrenzy", spare = "SHIFT-F19" },
    { name = "radialSkull", action = "RAIDTARGET8", label = "Mark skull", icon = "Interface\\TargetingFrame\\UI-RaidTargetingIcon_8", spare = "CTRL-SHIFT-F14" },
    { name = "radialNameplates", action = "ALLNAMEPLATES", label = "Nameplates", icon = "Interface\\Icons\\INV_Misc_Note_02", spare = "CTRL-SHIFT-F15" },
    { name = "radialDismount", action = "TOUCHCONTROLS_DISMOUNT", label = "Dismount / leave", icon = "Interface\\Icons\\Ability_Mount_RidingHorse", spare = "CTRL-F14" },
    { name = "radialScreenshot", action = "SCREENSHOT", label = "Screenshot", icon = "Interface\\Icons\\INV_Misc_Spyglass_03", spare = "ALT-F19" },
    { name = "radialChat", action = "TOUCHCONTROLS_CHAT", label = "Hide / show chat", icon = "Interface\\Icons\\INV_Misc_Note_01", spare = "CTRL-SHIFT-F16" },
}

-- Behind the ring, the whole screen: a tap on it closes the ring (and nothing reaches the world meanwhile).
local radialCatcher = CreateFrame("Frame", "TouchControlsRadial", UIParent)
radialCatcher:SetAllPoints(UIParent)
radialCatcher:SetFrameStrata("DIALOG")
radialCatcher:EnableMouse(true)
radialCatcher:Hide()

local radialShade = radialCatcher:CreateTexture(nil, "BACKGROUND")
radialShade:SetAllPoints()
radialShade:SetTexture(0, 0, 0, 0.25)

-- The panel the rows sit in, dropped down from the actions button.
local radialPanel = CreateFrame("Frame", nil, radialCatcher)
radialPanel:SetBackdrop({
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 32, edgeSize = 24,
    insets = { left = 8, right = 8, top = 8, bottom = 8 },
})
radialPanel:EnableMouse(true)


-- Each action a row: its icon and its name, the whole row the button, lit while a finger is on it.
for _, entry in ipairs(radialEntries) do
    local row = CreateFrame("Frame", "TouchControls" .. entry.name, radialPanel)
    row:SetBackdrop({ bgFile = "Interface\\Tooltips\\UI-Tooltip-Background", edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border", tile = true, tileSize = 16, edgeSize = 12, insets = { left = 3, right = 3, top = 3, bottom = 3 } })
    row:SetBackdropColor(0.12, 0.12, 0.12, 0.95)
    row:SetBackdropBorderColor(0.6, 0.55, 0.45)

    row.icon = MakeRound(nil, row)
    SetRoundIcon(row.icon, entry.icon)
    row.label = row:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
    row.label:SetPoint("LEFT", row.icon, "RIGHT", 8, 0)
    row.label:SetText(entry.label)

    row.pressed = row:CreateTexture(nil, "HIGHLIGHT")
    row.pressed:SetTexture(1, 0.82, 0.3, 0.25)
    row.pressed:SetPoint("TOPLEFT", 3, -3)
    row.pressed:SetPoint("BOTTOMRIGHT", -3, 3)
    row.pressed:SetDrawLayer("OVERLAY")
    row.pressed:Hide()

    entry.button = row
    Register(entry.name, row, nil, "lift")
    controls[entry.name].radial = true
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

local covering = { "GameMenuFrame", "TouchControlsMenu", "TouchControlsHelp", "TouchControlsEmotes", "LootFrame", "WorldMapFrame", "InterfaceOptionsFrame",
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

local radialOpen = false     -- the actions ring is showing (it pauses the other controls)

-- Sends every visible, uncovered control's area to the input layer, and drops the others.
local function SyncControls()
    GatherOpen()

    local cursorFull = GetCursorInfo() and true or false

    for _, slot in ipairs(slots) do
        slot:EnableMouse(cursorFull)
    end

    for name, control in pairs(controls) do
        local frame = control.frame
        local sent = false

        -- With the actions ring open only its buttons count; with a spell or item on the cursor the wheel's buttons
        -- are left to the UI, to take it (a tap or a drop on one puts it there).
        local paused = (radialOpen and not control.radial) or (control.wheel and GetCursorInfo())

        if (not db.hidden or name == "toggle") and frame:IsVisible() and control.key and not paused then
            local left, bottom, right, top = Fractions(frame)

            if left and (control.radial or not Covered(left, bottom, right, top)) then
                -- Holding a wheel button opens the spellbook to put a spell on it, unless the buttons are locked.
                local hold = control.holdAction and not (control.wheel and db.lockButtons) and GetBindingKey(control.holdAction) or nil
                TouchControls_SetControl(name, left, bottom, right, top, control.key, control.flags, hold)
                sent = true
            end
        end

        if not sent then
            TouchControls_RemoveControl(name)
        end
    end
end

------------------------------------------------------------------------------------------------------------------------
-- The actions ring, opened by the actions button (a tap or a hold).

local function LayoutRadial()
    local height = ScreenHeight()
    local rowHeight = height * 0.1
    local rowWidth = height * 0.44
    local gap = height * 0.018
    local padding = height * 0.03

    -- The rows of the actions there are keys for, two columns.
    local shown = {}

    for _, entry in ipairs(radialEntries) do
        if controls[entry.name].key then
            tinsert(shown, entry)
            entry.button:Show()
        else
            entry.button:Hide()
        end
    end

    -- Two columns, three once there are many: rows no more than six.
    local columns = #shown > 12 and 3 or 2
    local rows = ceil(#shown / columns)
    radialPanel:SetWidth(rowWidth * columns + gap * (columns - 1) + padding * 2)
    radialPanel:SetHeight(rows * rowHeight + (rows - 1) * gap + padding * 2)
    radialPanel:ClearAllPoints()
    radialPanel:SetPoint("TOP", buttons.actions, "BOTTOM", 0, -gap)
    radialPanel:SetClampedToScreen(true)

    for index, entry in ipairs(shown) do
        local column, line = (index - 1) % columns, floor((index - 1) / columns)
        local row = entry.button
        row:SetWidth(rowWidth)
        row:SetHeight(rowHeight)
        row:ClearAllPoints()
        row:SetPoint("TOPLEFT", radialPanel, "TOPLEFT", padding + column * (rowWidth + gap), -padding - line * (rowHeight + gap))
        SizeRound(row.icon, rowHeight * 0.78)
        row.icon:ClearAllPoints()
        row.icon:SetPoint("LEFT", row, "LEFT", rowHeight * 0.12, 0)
    end
end

local function CloseRadial()
    radialOpen = false
    radialCatcher:Hide()
    SyncControls()
end

function TouchControls_ToggleRadial()
    if radialOpen then
        CloseRadial()
        return
    end

    radialOpen = true
    LayoutRadial()
    radialCatcher:Show()
    SyncControls()
end

radialCatcher:SetScript("OnMouseUp", CloseRadial)

------------------------------------------------------------------------------------------------------------------------
-- The emotes, opened by the emotes button beside the targeting buttons (a tap or a hold): a panel of the game's emotes,
-- each a button. A tap does the emote (at the target, if any) and closes it; a tap anywhere else closes it. While it is
-- open it covers the other controls (the covering list). In a block: the main chunk is near Lua's 200 locals.

do
    local EMOTES = { "WAVE", "HELLO", "BYE", "THANK", "BOW", "CHEER", "DANCE", "LAUGH", "APPLAUD", "SALUTE",
        "POINT", "FLEX", "KISS", "CRY", "ROAR", "CHICKEN", "YES", "NO", "KNEEL", "RUDE" }
    local COLUMNS = 5

    local catcher = CreateFrame("Frame", "TouchControlsEmotes", UIParent)
    catcher:SetAllPoints(UIParent)
    catcher:SetFrameStrata("DIALOG")
    catcher:EnableMouse(true)
    catcher:Hide()

    local shade = catcher:CreateTexture(nil, "BACKGROUND")
    shade:SetAllPoints()
    shade:SetTexture(0, 0, 0, 0.25)

    local panel = CreateFrame("Frame", nil, catcher)
    panel:SetBackdrop({
        bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
        edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
        tile = true, tileSize = 32, edgeSize = 24,
        insets = { left = 8, right = 8, top = 8, bottom = 8 },
    })
    panel:EnableMouse(true)

    local emoteButtons = {}

    for index, token in ipairs(EMOTES) do
        local button = CreateFrame("Button", "TouchControlsEmote" .. index, panel, "UIPanelButtonTemplate")
        button:SetText("/" .. strlower(token))
        button:SetScript("OnClick", function()
            catcher:Hide()
            DoEmote(token)
        end)
        emoteButtons[index] = button
    end

    catcher:SetScript("OnMouseUp", function()
        catcher:Hide()
    end)

    catcher:SetScript("OnShow", function()
        SyncControls()
    end)

    catcher:SetScript("OnHide", function()
        SyncControls()
    end)

    function TouchControls_ToggleEmotes()
        if catcher:IsShown() then
            catcher:Hide()
            return
        end

        -- Buttons as big as the actions list's rows are tall, in rows of five.
        local height = ScreenHeight()
        local buttonHeight = height * 0.1
        local buttonWidth = height * 0.3
        local gap = height * 0.018
        local padding = height * 0.03
        local rows = ceil(#EMOTES / COLUMNS)
        local scale = buttonHeight / 22

        panel:SetWidth(COLUMNS * buttonWidth + (COLUMNS - 1) * gap + padding * 2)
        panel:SetHeight(rows * buttonHeight + (rows - 1) * gap + padding * 2)
        panel:ClearAllPoints()
        panel:SetPoint("CENTER", UIParent, "CENTER", 0, 0)

        for index, button in ipairs(emoteButtons) do
            local column, row = (index - 1) % COLUMNS, floor((index - 1) / COLUMNS)
            button:SetScale(scale)
            button:SetWidth(buttonWidth / scale)
            button:SetHeight(22)
            button:ClearAllPoints()
            button:SetPoint("TOPLEFT", panel, "TOPLEFT", (padding + column * (buttonWidth + gap)) / scale, -(padding + row * (buttonHeight + gap)) / scale)
        end

        catcher:Show()
    end
end

------------------------------------------------------------------------------------------------------------------------
-- Putting spells on the wheel. Holding a wheel button opens the spellbook for it: a tap on a spell puts the spell on
-- that button. And with a spell or item on the cursor (dragged from the spellbook or the bags) the wheel's buttons take
-- a drop or a tap (SyncControls leaves them to the UI then).

local assignAction          -- the action slot the spellbook is choosing for
local lastHeldSlot          -- the wheel button a finger was last on (the one the hold was on)
local assignBanner, assignOverlays

local function EndAssign()
    assignAction = nil

    if assignBanner then
        assignBanner:Hide()

        for _, overlay in ipairs(assignOverlays) do
            overlay:Hide()
        end
    end
end

local function WheelAction(index)
    local button = WheelButton(wheelOffset + index)
    return button and button.action
end

local function SetupAssign()
    if assignBanner or not SpellBookFrame then
        return
    end

    assignBanner = CreateFrame("Frame", nil, SpellBookFrame)
    assignBanner:SetFrameLevel(SpellBookFrame:GetFrameLevel() + 20)
    assignBanner:SetPoint("BOTTOMLEFT", SpellBookFrame, "TOPLEFT", 20, -12)
    assignBanner:SetPoint("BOTTOMRIGHT", SpellBookFrame, "TOPRIGHT", -40, -12)
    assignBanner:SetHeight(30)
    assignBanner:SetBackdrop({ bgFile = "Interface\\Tooltips\\UI-Tooltip-Background", edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border", tile = true, tileSize = 16, edgeSize = 12, insets = { left = 3, right = 3, top = 3, bottom = 3 } })
    assignBanner:SetBackdropColor(0, 0, 0, 0.9)
    assignBanner.text = assignBanner:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    assignBanner.text:SetPoint("LEFT", 12, 0)
    assignBanner.text:SetPoint("RIGHT", assignBanner, "RIGHT", -128, 0)
    assignBanner.text:SetJustifyH("LEFT")

    -- Empties the button instead.
    assignBanner.clear = CreateFrame("Button", "TouchControlsAssignClear", assignBanner, "UIPanelButtonTemplate")
    assignBanner.clear:SetWidth(120)
    assignBanner.clear:SetHeight(24)
    assignBanner.clear:SetPoint("RIGHT", -4, 0)
    assignBanner.clear:SetText("Clear button")
    assignBanner.clear:SetScript("OnClick", function()
        if assignAction then
            PickupAction(assignAction)
            ClearCursor()
        end

        EndAssign()
        HideUIPanel(SpellBookFrame)
    end)

    -- Over each spell, a button that takes the tap instead of casting it.
    assignOverlays = {}

    for index = 1, SPELLS_PER_PAGE or 12 do
        local spellButton = _G["SpellButton" .. index]

        if spellButton then
            -- The spell's icon and its name beside it, for a finger.
            local overlay = CreateFrame("Button", nil, spellButton)
            overlay:SetPoint("TOPLEFT", spellButton, "TOPLEFT", -4, 4)
            overlay:SetPoint("BOTTOMRIGHT", spellButton, "BOTTOMRIGHT", spellButton:GetWidth() * 3.4, -4)
            overlay:SetFrameLevel(spellButton:GetFrameLevel() + 10)
            overlay:SetHighlightTexture("Interface\\Buttons\\ButtonHilight-Square", "ADD")
            overlay:EnableMouse(true)
            overlay:RegisterForClicks("LeftButtonUp", "RightButtonUp")
            overlay:SetScript("OnClick", function()
                local id = SpellBook_GetSpellID(spellButton:GetID())
                local book = SpellBookFrame.bookType

                -- An empty slot (past the tab's last spell) has an id too, of another tab's spell or of none; the game
                -- disables it (IsEnabled gives 0, not nil, for a disabled button).
                if not assignAction or not id or id > (MAX_SPELLS or 1024) or spellButton:IsEnabled() ~= 1 or
                        not GetSpellName(id, book) or IsPassiveSpell(id, book) then
                    return
                end

                PickupSpell(id, book)
                PlaceAction(assignAction)
                ClearCursor()
                EndAssign()
                HideUIPanel(SpellBookFrame)
            end)
            overlay:Hide()
            assignOverlays[index] = overlay
        end
    end

    SpellBookFrame:HookScript("OnHide", EndAssign)

    -- Turning the page or tab: only slots with a spell take a tap.
    hooksecurefunc("SpellButton_UpdateButton", function(button)
        local overlay = assignOverlays[button:GetID()]

        if overlay then
            if assignAction and button:IsEnabled() == 1 then
                overlay:Show()
            else
                overlay:Hide()
            end
        end
    end)
end

function TouchControls_AssignStart()
    if not lastHeldSlot then
        return
    end

    SetupAssign()
    assignAction = WheelAction(lastHeldSlot)

    if not assignAction or not assignBanner then
        return
    end

    if not SpellBookFrame:IsShown() then
        ToggleSpellBook(BOOKTYPE_SPELL)
    end

    assignBanner.text:SetText(format("Tap a spell for button %d", lastHeldSlot))
    assignBanner:Show()

    for index, overlay in ipairs(assignOverlays) do
        if _G["SpellButton" .. index]:IsEnabled() == 1 then
            overlay:Show()
        else
            overlay:Hide()
        end
    end
end

-- A spell or item on the cursor goes on the wheel button it's dropped or tapped on. The buttons take the mouse only then
-- (SyncControls): otherwise a drag off one turns the camera, which a frame under the mouse would stop.
for index, slot in ipairs(slots) do

    local function Place()
        local action = WheelAction(index)

        if action and GetCursorInfo() then
            PlaceAction(action)
        end
    end

    slot:SetScript("OnMouseUp", Place)
    slot:SetScript("OnReceiveDrag", Place)
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

-- The dialog's own background is see-through; at most of the screen the world and the controls behind would show.
local menuFill = menu:CreateTexture(nil, "BACKGROUND")
menuFill:SetTexture(0.04, 0.04, 0.04, 0.94)
menuFill:SetPoint("TOPLEFT", 11, -12)
menuFill:SetPoint("BOTTOMRIGHT", -12, 11)

local menuHeader = menu:CreateTexture(nil, "ARTWORK")
menuHeader:SetTexture("Interface\\DialogFrame\\UI-DialogBox-Header")
menuHeader:SetWidth(300)
menuHeader:SetHeight(64)
menuHeader:SetPoint("TOP", 0, 12)

local menuTitle = menu:CreateFontString(nil, "ARTWORK", "GameFontNormal")
menuTitle:SetPoint("TOP", menuHeader, "TOP", 0, -14)
menuTitle:SetText("Touch Controls")

-- The micro buttons and bags, each row in a holder of its own scale, and the display settings in a third; all keep
-- their size on the screen whatever the interface size.
local function MenuScale()
    return 1.6 * ScreenHeight() / 937
end

-- Two framed panels in the style of the game's option windows: the game's menu and the bags, and the settings.
local function MakePanel(title)
    local panel = CreateFrame("Frame", nil, menu)
    panel:SetBackdrop({
        bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 16, edgeSize = 16,
        insets = { left = 4, right = 4, top = 4, bottom = 4 },
    })
    panel:SetBackdropColor(0.1, 0.1, 0.1, 0.9)
    panel:SetBackdropBorderColor(0.6, 0.55, 0.45)
    panel.title = panel:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
    panel.title:SetText(title)
    return panel
end

local gamePanel = MakePanel(MAINMENU_BUTTON or "Menu")
local settingsPanel = MakePanel(SETTINGS or "Settings")

local bagsTitle = gamePanel:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
bagsTitle:SetText(BAGSLOTTEXT or "Bags")

-- What a tap on the bags does (the bag sheet below).
local bagsHint = gamePanel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
bagsHint:SetJustifyH("LEFT")
bagsHint:SetText("Tap a bag to open it, take it off, or put a different bag in its place.")

local microHolder = CreateFrame("Frame", nil, menu)
local bagHolder = CreateFrame("Frame", nil, menu)
local displayHolder = CreateFrame("Frame", nil, menu)

-- A name under each of the game's menu buttons, in the menu's order (MicroButtonList).
local MICRO_LABELS = { "Character", "Spells", "Talents", "Achievements", "Quests", "Social", "PvP", "Dungeons", "Game menu", "Help" }
local microLabels = {}

local styleButton = CreateFrame("Button", "TouchControlsMenuStyle", menu, "UIPanelButtonTemplate")
local helpButton = CreateFrame("Button", "TouchControlsMenuHelp", menu, "UIPanelButtonTemplate")
local defaultsButton = CreateFrame("Button", "TouchControlsMenuDefaults", menu, "UIPanelButtonTemplate")
local closeButton = CreateFrame("Button", "TouchControlsMenuClose", menu, "UIPanelButtonTemplate")
local menuButtons = { styleButton, helpButton, defaultsButton, closeButton }

for _, button in ipairs(menuButtons) do
    button:SetWidth(112)
    button:SetHeight(26)
end

defaultsButton:SetText(DEFAULTS or "Defaults")
helpButton:SetText("How to play")

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

-- Lock buttons: holding a wheel button doesn't open the spellbook (SyncControls sends no hold key).
local lockCheck = CreateFrame("CheckButton", "TouchControlsMenuLock", displayHolder, "UICheckButtonTemplate")
TouchControlsMenuLockText:SetText("Lock buttons")
lockCheck:SetScript("OnClick", function(self)
    db.lockButtons = self:GetChecked() and true or false
end)

-- The emotes button beside the targeting buttons (TouchControlsMenuEmote; no local, the main chunk is near Lua's 200).
CreateFrame("CheckButton", "TouchControlsMenuEmote", displayHolder, "UICheckButtonTemplate")
TouchControlsMenuEmoteText:SetText("Emote button")
TouchControlsMenuEmote:SetScript("OnClick", function(self)
    db.emoteButton = self:GetChecked() and true or false
    Refresh()
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
    lockCheck:SetChecked(db.lockButtons)
    TouchControlsMenuEmote:SetChecked(db.emoteButton)
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

-- The backpack and the key ring only open: a tap does that, and the menu gets out of the way.
MainMenuBarBackpackButton:HookScript("OnClick", CloseMenu)
KeyRingButton:HookScript("OnClick", CloseMenu)

-- A bag slot in the menu: a tap over it opens the bag sheet, with what can be done to that slot in buttons. The game's
-- own button under it still shows the bag; holding shows its tooltip.
local BAG_NAMES = { BACKPACK_TOOLTIP or "Backpack", "Bag 1", "Bag 2", "Bag 3", "Bag 4", KEYRING or "Key Ring" }
local bagLabels = {}        -- under each bag in the menu: its name and slots
local bagCovers = {}        -- over each of the four bag slots, by bag (1 - 4)
local bagSheet, UpdateBagLabels

-- In a block of its own: the main chunk is near Lua's 200 locals.
do

bagSheet = CreateFrame("Frame", "TouchControlsBagSheet", menu)
bagSheet:SetBackdrop({
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 32, edgeSize = 32,
    insets = { left = 11, right = 12, top = 12, bottom = 11 },
})
bagSheet:EnableMouse(true)
bagSheet:Hide()

local bagSheetFill = bagSheet:CreateTexture(nil, "BACKGROUND")
bagSheetFill:SetTexture(0.06, 0.06, 0.06, 0.97)
bagSheetFill:SetPoint("TOPLEFT", 11, -12)
bagSheetFill:SetPoint("BOTTOMRIGHT", -12, 11)

-- Behind the sheet, over the rest of the menu: a tap there puts the sheet away.
local bagSheetShade = CreateFrame("Button", nil, menu)
bagSheetShade:SetAllPoints(menu)
bagSheetShade:Hide()
bagSheetShade:SetScript("OnClick", function() bagSheet:Hide() end)
local bagSheetShadeFill = bagSheetShade:CreateTexture(nil, "BACKGROUND")
bagSheetShadeFill:SetAllPoints()
bagSheetShadeFill:SetTexture(0, 0, 0, 0.55)

bagSheet:SetScript("OnShow", function() bagSheetShade:Show() end)
bagSheet:SetScript("OnHide", function() bagSheetShade:Hide() end)
menu:HookScript("OnHide", function() bagSheet:Hide() end)

local bagSheetIcon = bagSheet:CreateTexture(nil, "ARTWORK")
bagSheetIcon:SetWidth(36)
bagSheetIcon:SetHeight(36)
bagSheetIcon:SetPoint("TOPLEFT", 24, -24)

local bagSheetTitle = bagSheet:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
bagSheetTitle:SetPoint("TOPLEFT", bagSheetIcon, "TOPRIGHT", 10, -1)
bagSheetTitle:SetJustifyH("LEFT")

local bagSheetInfo = bagSheet:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
bagSheetInfo:SetPoint("TOPLEFT", bagSheetTitle, "BOTTOMLEFT", 0, -4)
bagSheetInfo:SetJustifyH("LEFT")

local function SheetButton(name, text)
    local button = CreateFrame("Button", name, bagSheet, "UIPanelButtonTemplate")
    button:SetWidth(150)
    button:SetHeight(30)
    button:SetText(text)
    return button
end

local bagOpen = SheetButton("TouchControlsBagSheetOpen", "Open")
local bagRemove = SheetButton("TouchControlsBagSheetRemove", "Take off")
local bagDone = SheetButton("TouchControlsBagSheetClose", CLOSE or "Close")

-- Why a bag can't come off, or where it goes, beside Take off.
local bagRemoveNote = bagSheet:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
bagRemoveNote:SetPoint("LEFT", bagRemove, "RIGHT", 10, 0)
bagRemoveNote:SetJustifyH("LEFT")

local bagChoicesTitle = bagSheet:CreateFontString(nil, "OVERLAY", "GameFontNormal")
bagChoicesTitle:SetJustifyH("LEFT")

-- The bags in the player's bags, one button each, to put in this slot.
local bagChoices = {}
local BAG_CHOICE_COLUMNS, BAG_CHOICE_ROWS = 2, 3
local BAG_CHOICE_WIDTH, BAG_CHOICE_HEIGHT = 230, 40

local function BagChoice(index)
    if bagChoices[index] then
        return bagChoices[index]
    end

    local choice = CreateFrame("Button", nil, bagSheet)
    choice:SetWidth(BAG_CHOICE_WIDTH)
    choice:SetHeight(BAG_CHOICE_HEIGHT)
    choice:SetBackdrop({ bgFile = "Interface\\Tooltips\\UI-Tooltip-Background", edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border", tile = true, tileSize = 16, edgeSize = 12, insets = { left = 3, right = 3, top = 3, bottom = 3 } })
    choice:SetBackdropColor(0.15, 0.15, 0.15, 1)
    choice:SetBackdropBorderColor(0.6, 0.55, 0.45)
    choice:SetHighlightTexture("Interface\\Buttons\\UI-Listbox-Highlight2", "ADD")
    choice.icon = choice:CreateTexture(nil, "ARTWORK")
    choice.icon:SetWidth(30)
    choice.icon:SetHeight(30)
    choice.icon:SetPoint("LEFT", 5, 0)
    choice.text = choice:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
    choice.text:SetPoint("LEFT", choice.icon, "RIGHT", 8, 0)
    choice.text:SetPoint("RIGHT", -6, 0)
    choice.text:SetJustifyH("LEFT")
    bagChoices[index] = choice
    return choice
end

-- A general bag (not this one) with a free space, for a bag taken off: 0 is the backpack.
local function FreeBagFor(bag)
    for other = 0, NUM_BAG_SLOTS do
        if other ~= bag then
            local free, family = GetContainerNumFreeSlots(other)

            if free and free > 0 and family == 0 then
                return other
            end
        end
    end
end

-- The bags in the player's bags that can go in a bag slot (bags and quivers).
local function BagsInBags()
    local found = {}

    for bag = 0, NUM_BAG_SLOTS do
        for slot = 1, GetContainerNumSlots(bag) do
            local link = GetContainerItemLink(bag, slot)

            if link then
                local name, _, quality, _, _, _, _, _, equip, texture = GetItemInfo(link)

                if equip == "INVTYPE_BAG" or equip == "INVTYPE_QUIVER" then
                    tinsert(found, { bag = bag, slot = slot, name = name, quality = quality, texture = texture })
                end
            end
        end
    end

    return found
end

local sheetBag             -- the bag (1 - 4) the sheet is for

local function FillBagSheet()
    local bag = sheetBag
    local inventory = ContainerIDToInventoryID(bag)
    local texture = GetInventoryItemTexture("player", inventory)
    local link = GetInventoryItemLink("player", inventory)
    local slots = GetContainerNumSlots(bag)
    local free = GetContainerNumFreeSlots(bag)

    bagSheetIcon:SetTexture(texture or "Interface\\PaperDoll\\UI-PaperDoll-Slot-Bag")

    if link then
        local name, _, quality = GetItemInfo(link)
        local color = ITEM_QUALITY_COLORS[quality or 1]
        bagSheetTitle:SetText(name or BAG_NAMES[bag + 1])
        bagSheetTitle:SetTextColor(color.r, color.g, color.b)
        bagSheetInfo:SetText(format("%s: %d slots, %d free", BAG_NAMES[bag + 1], slots, free))
    else
        bagSheetTitle:SetText(BAG_NAMES[bag + 1] .. ": empty slot")
        bagSheetTitle:SetTextColor(NORMAL_FONT_COLOR.r, NORMAL_FONT_COLOR.g, NORMAL_FONT_COLOR.b)
        bagSheetInfo:SetText("No bag here yet.")
    end

    -- Open and Take off only with a bag in the slot; it comes off only empty, into a free space in another bag.
    if link then
        bagOpen:Show()
        bagRemove:Show()
    else
        bagOpen:Hide()
        bagRemove:Hide()
    end

    bagRemoveNote:SetText("")

    if link then
        if free < slots then
            bagRemove:Disable()
            bagRemoveNote:SetText("Empty this bag first.")
        elseif not FreeBagFor(bag) then
            bagRemove:Disable()
            bagRemoveNote:SetText("No free space in your other bags.")
        else
            bagRemove:Enable()
        end
    end

    -- The bags that can go here: with one here already, a choice swaps them.
    local choices = BagsInBags()
    local top = link and -116 or -80
    bagChoicesTitle:ClearAllPoints()
    bagChoicesTitle:SetPoint("TOPLEFT", bagSheet, "TOPLEFT", 24, top)

    if #choices == 0 then
        bagChoicesTitle:SetText(link and "No other bags in your bags to swap in." or "No bags in your bags to put here.")
    else
        bagChoicesTitle:SetText(link and "Swap for one of your bags:" or "Put one of your bags here:")
    end

    local shown = min(#choices, BAG_CHOICE_COLUMNS * BAG_CHOICE_ROWS)

    for index, choice in ipairs(bagChoices) do
        choice:Hide()
    end

    for index = 1, shown do
        local item = choices[index]
        local choice = BagChoice(index)
        local column, row = (index - 1) % BAG_CHOICE_COLUMNS, floor((index - 1) / BAG_CHOICE_COLUMNS)
        local color = ITEM_QUALITY_COLORS[item.quality or 1]
        choice.item = item
        choice.icon:SetTexture(item.texture)
        choice.text:SetText(item.name)
        choice.text:SetTextColor(color.r, color.g, color.b)
        choice:ClearAllPoints()
        choice:SetPoint("TOPLEFT", bagSheet, "TOPLEFT", 24 + column * (BAG_CHOICE_WIDTH + 8), top - 22 - row * (BAG_CHOICE_HEIGHT + 6))
        choice:Show()
    end

    local rows = ceil(shown / BAG_CHOICE_COLUMNS)
    bagSheet:SetWidth(48 + BAG_CHOICE_COLUMNS * BAG_CHOICE_WIDTH + 8)
    bagSheet:SetHeight(-top + 22 + rows * (BAG_CHOICE_HEIGHT + 6) + 64)
end

local function ShowBagSheet(bag)
    sheetBag = bag
    -- The sheet's buttons as big as the menu's own.
    bagSheet:SetScale(ScreenHeight() * 0.065 / 30)
    bagSheet:SetFrameLevel(menu:GetFrameLevel() + 20)
    bagSheetShade:SetFrameLevel(menu:GetFrameLevel() + 19)
    bagSheet:ClearAllPoints()
    bagSheet:SetPoint("CENTER", menu, "CENTER", 0, 0)
    FillBagSheet()
    bagSheet:Show()
end

bagOpen:SetPoint("TOPLEFT", bagSheet, "TOPLEFT", 24, -72)
bagRemove:SetPoint("LEFT", bagOpen, "RIGHT", 10, 0)
bagRemoveNote:ClearAllPoints()
bagRemoveNote:SetPoint("LEFT", bagRemove, "RIGHT", 10, 0)
bagDone:SetPoint("BOTTOMRIGHT", bagSheet, "BOTTOMRIGHT", -22, 20)

bagOpen:SetScript("OnClick", function()
    local bag = sheetBag
    menu:Hide()
    ToggleBag(bag)
end)

bagRemove:SetScript("OnClick", function()
    local into = FreeBagFor(sheetBag)

    if into and not CursorHasItem() then
        PickupBagFromSlot(ContainerIDToInventoryID(sheetBag))

        if into == 0 then
            PutItemInBackpack()
        else
            PutItemInBag(ContainerIDToInventoryID(into))
        end
    end

    -- Whatever the game refused goes back where it was, not stuck on the finger.
    if CursorHasItem() then
        ClearCursor()
    end
end)

bagDone:SetScript("OnClick", function() bagSheet:Hide() end)

-- A bag chosen: it goes in this slot, and the one there (if any) takes its place in the bags.
local function ChooseBag(choice)
    local item = choice.item

    if item and not CursorHasItem() then
        PickupContainerItem(item.bag, item.slot)
        PutItemInBag(ContainerIDToInventoryID(sheetBag))
    end

    if CursorHasItem() then
        ClearCursor()
    end
end

for index = 1, BAG_CHOICE_COLUMNS * BAG_CHOICE_ROWS do
    BagChoice(index):SetScript("OnClick", ChooseBag)
end

-- The game moves bags a moment later: the sheet and the labels follow.
local bagWatch = CreateFrame("Frame")
bagWatch:RegisterEvent("BAG_UPDATE")
bagWatch:RegisterEvent("UNIT_INVENTORY_CHANGED")
bagWatch:RegisterEvent("ITEM_LOCK_CHANGED")

function UpdateBagLabels()
    for index, label in pairs(bagLabels) do
        local bag = index - 1
        local slots

        if index == 1 then
            slots = GetContainerNumSlots(0)
        elseif index <= 5 then
            slots = GetInventoryItemTexture("player", ContainerIDToInventoryID(bag)) and GetContainerNumSlots(bag)
        end

        if index == 6 then
            -- The game shows the key ring only once there are keys.
            label:SetText(KeyRingButton:IsShown() and BAG_NAMES[index] or "")
        elseif slots then
            label:SetText(format("%s\n|cffffffff%d slots|r", BAG_NAMES[index], slots))
        else
            label:SetText(format("%s\n|cff808080empty|r", BAG_NAMES[index]))
        end
    end
end

bagWatch:SetScript("OnEvent", function()
    if menu:IsShown() then
        UpdateBagLabels()

        if bagSheet:IsShown() then
            FillBagSheet()
        end
    end
end)

menu:HookScript("OnShow", UpdateBagLabels)

for bag = 1, NUM_BAG_SLOTS do
    local cover = CreateFrame("Button", nil, menu)
    cover:SetScript("OnClick", function(self)
        -- A bag on the finger already (picked up some other way) goes straight in, as with the game's own slot.
        if CursorHasItem() then
            PutItemInBag(ContainerIDToInventoryID(bag))
        else
            ShowBagSheet(bag)
        end
    end)
    -- A hold shows the slot's tooltip, as the game's button would.
    cover:SetScript("OnEnter", function(self)
        local slot = _G["CharacterBag" .. (bag - 1) .. "Slot"]
        GameTooltip:SetOwner(self, "ANCHOR_TOP")

        if not GameTooltip:SetInventoryItem("player", slot:GetID()) then
            GameTooltip:SetText(EQUIP_CONTAINER, 1, 1, 1)
        end
    end)
    cover:SetScript("OnLeave", function() GameTooltip:Hide() end)
    cover:Hide()
    bagCovers[bag] = cover
end
end

for _, button in ipairs(MicroButtonList()) do
    button:HookScript("OnClick", CloseMenu)
end

-- Moves the micro buttons and bags into the menu, a row each, and sizes the menu around them.
local function FillMenu()
    local micro = MicroButtonList()
    local width = ScreenWidth()
    local height = ScreenHeight()

    -- Where they were, to put them back (EmptyMenu).
    local function Keep(name, frame)
        if not bagPoints[name] then
            local points = {}

            for i = 1, frame:GetNumPoints() do
                points[i] = { frame:GetPoint(i) }
            end

            bagPoints[name] = { parent = frame:GetParent(), points = points }
        end
    end

    -- The menu buttons in a grid of five by two, each over its name: their art is 28 x 58 with the button in its lower 36.
    local cellWidth, cellHeight, labelHeight = 40, 50, 9

    for index, button in ipairs(micro) do
        local column, row = (index - 1) % 5, floor((index - 1) / 5)
        Keep(button:GetName(), button)
        button:SetParent(microHolder)
        button:ClearAllPoints()
        button:SetPoint("BOTTOM", microHolder, "TOPLEFT", (column + 0.5) * cellWidth, -(row + 1) * cellHeight + labelHeight)

        local label = microLabels[index] or microHolder:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
        microLabels[index] = label
        label:SetFont(label:GetFont(), 5.5)
        label:SetText(MICRO_LABELS[index] or "")
        label:ClearAllPoints()
        label:SetPoint("TOP", microHolder, "TOPLEFT", (column + 0.5) * cellWidth, -(row + 1) * cellHeight + labelHeight - 1)
    end

    microHolder:SetWidth(5 * cellWidth)
    microHolder:SetHeight(2 * cellHeight)

    -- The bags in a row, each over its name and slots: the backpack 37 square, the bags 30, the key ring 18 wide.
    local bagCell, bagLabelHeight = 56, 24

    for index, name in ipairs(bags) do
        local bag = _G[name]

        if bag then
            Keep(name, bag)
            bag:SetParent(bagHolder)
            bag:ClearAllPoints()
            bag:SetPoint("BOTTOM", bagHolder, "BOTTOMLEFT", (index - 0.5) * bagCell, bagLabelHeight + 2)

            local label = bagLabels[index] or bagHolder:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
            bagLabels[index] = label
            label:SetFont(label:GetFont(), 9)
            label:ClearAllPoints()
            label:SetPoint("TOP", bagHolder, "BOTTOMLEFT", (index - 0.5) * bagCell, bagLabelHeight)

            -- The four bag slots: a tap anywhere in the cell, name included, opens the sheet.
            local cover = bagCovers[index - 1]

            if cover then
                cover:SetParent(bagHolder)
                cover:SetFrameLevel(bag:GetFrameLevel() + 5)
                cover:ClearAllPoints()
                cover:SetPoint("BOTTOMLEFT", bagHolder, "BOTTOMLEFT", (index - 1) * bagCell, 0)
                cover:SetWidth(bagCell)
                cover:SetHeight(40 + bagLabelHeight)
                cover:Show()
            end
        end
    end

    bagHolder:SetWidth(#bags * bagCell)
    bagHolder:SetHeight(40 + bagLabelHeight)
    UpdateBagLabels()

    -- The settings: sliders, each under its label, then two columns of check boxes.
    local sliderRow = 44
    displayHolder:SetWidth(300)
    displayHolder:SetHeight(sliderRow * #displaySliders + 88)

    for index, slider in ipairs(displaySliders) do
        slider:ClearAllPoints()
        slider:SetPoint("TOP", displayHolder, "TOP", 0, -sliderRow * (index - 1) - 20)
    end

    local checksTop = -sliderRow * #displaySliders - 4
    batteryCheck:ClearAllPoints()
    batteryCheck:SetPoint("TOPLEFT", displayHolder, "TOPLEFT", 0, checksTop)
    glowCheck:ClearAllPoints()
    glowCheck:SetPoint("TOPLEFT", displayHolder, "TOPLEFT", 0, checksTop - 28)
    invertCheck:ClearAllPoints()
    invertCheck:SetPoint("TOPLEFT", displayHolder, "TOP", 10, checksTop)
    lockCheck:ClearAllPoints()
    lockCheck:SetPoint("TOPLEFT", displayHolder, "TOP", 10, checksTop - 28)
    TouchControlsMenuEmote:ClearAllPoints()
    TouchControlsMenuEmote:SetPoint("TOPLEFT", displayHolder, "TOPLEFT", 0, checksTop - 56)

    -- Most of the screen, below the top row of buttons: two panels side by side, the buttons along the bottom.
    local menuWidth, menuHeight = width * 0.86, height * 0.84
    local margin = height * 0.035      -- the dialog's border to the panels, and between them
    local inset = height * 0.025       -- a panel's edge to what's in it
    local titleHeight = height * 0.055 -- a panel's title
    local buttonScale = height * 0.065 / 26
    local buttonsHeight = 26 * buttonScale + margin
    local panelWidth = (menuWidth - margin * 3) / 2
    local panelHeight = menuHeight - margin * 2 - buttonsHeight
    local inner = panelWidth - inset * 2

    menu:SetWidth(menuWidth)
    menu:SetHeight(menuHeight)
    menu:ClearAllPoints()
    menu:SetPoint("CENTER", UIParent, "CENTER", 0, -height * 0.04)

    for i, panel in ipairs({ gamePanel, settingsPanel }) do
        panel:SetWidth(panelWidth)
        panel:SetHeight(panelHeight)
        panel:ClearAllPoints()
        panel:SetPoint("TOPLEFT", menu, "TOPLEFT", margin + (i - 1) * (panelWidth + margin), -margin)
        panel:SetFrameLevel(menu:GetFrameLevel() + 1)
        panel.title:ClearAllPoints()
        panel.title:SetPoint("TOPLEFT", panel, "TOPLEFT", inset, -inset)
    end

    for _, holder in ipairs({ microHolder, bagHolder, displayHolder }) do
        holder:SetFrameLevel(menu:GetFrameLevel() + 3)
    end

    -- The game panel: the menu grid, then the bags under their title, then the hint.
    local gridHeight = panelHeight * 0.5
    local bagsHeight = panelHeight * 0.17
    local microScale = min(inner / microHolder:GetWidth(), gridHeight / microHolder:GetHeight())
    local bagScale = min(inner / bagHolder:GetWidth(), bagsHeight / bagHolder:GetHeight())
    microHolder:SetScale(microScale)
    bagHolder:SetScale(bagScale)

    local y = inset + titleHeight
    microHolder:ClearAllPoints()
    microHolder:SetPoint("TOP", gamePanel, "TOP", 0, -y / microScale)
    y = y + microHolder:GetHeight() * microScale + inset
    bagsTitle:ClearAllPoints()
    bagsTitle:SetPoint("TOPLEFT", gamePanel, "TOPLEFT", inset, -y)
    y = y + titleHeight
    bagHolder:ClearAllPoints()
    bagHolder:SetPoint("TOP", gamePanel, "TOP", 0, -y / bagScale)
    y = y + bagHolder:GetHeight() * bagScale + inset * 0.6
    bagsHint:ClearAllPoints()
    bagsHint:SetPoint("TOPLEFT", gamePanel, "TOPLEFT", inset, -y)
    bagsHint:SetWidth(inner)

    -- The settings panel: the settings as big as fit under its title.
    local displayScale = min(inner / displayHolder:GetWidth(), (panelHeight - titleHeight - inset * 2) / displayHolder:GetHeight())
    displayHolder:SetScale(displayScale)
    displayHolder:ClearAllPoints()
    displayHolder:SetPoint("TOP", settingsPanel, "TOP", 0, -(inset + titleHeight) / displayScale)

    for _, button in ipairs(menuButtons) do
        button:SetScale(buttonScale)
    end

    styleButton:ClearAllPoints()
    styleButton:SetPoint("BOTTOMLEFT", menu, "BOTTOMLEFT", margin / buttonScale, margin / buttonScale)
    closeButton:ClearAllPoints()
    closeButton:SetPoint("BOTTOMRIGHT", menu, "BOTTOMRIGHT", -margin / buttonScale, margin / buttonScale)
    -- How to play and Defaults between the two, a third of the way in from each side.
    helpButton:ClearAllPoints()
    helpButton:SetPoint("BOTTOM", menu, "BOTTOMLEFT", menuWidth * 0.37 / buttonScale, margin / buttonScale)
    defaultsButton:ClearAllPoints()
    defaultsButton:SetPoint("BOTTOM", menu, "BOTTOMLEFT", menuWidth * 0.63 / buttonScale, margin / buttonScale)

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
    bagSheet:Hide()

    for _, cover in pairs(bagCovers) do
        cover:Hide()
    end

    VehicleMenuBar_MoveMicroButtons()
end

-- Leaving a vehicle puts the micro buttons back on the main bar: they come back to the menu. Getting in puts them on
-- the vehicle's bar, by the first of each line: the rest go back to their chain behind it.
-- Getting in or out of a vehicle moves the micro buttons to its bar or the main bar: they come back to the menu.
hooksecurefunc("VehicleMenuBar_MoveMicroButtons", function(skinName)
    if inMenu then
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

-- Where the touch layout keeps the main bar (Layout's numbers, for PlaceGameBars): its y, and the right edge the class
-- bars above it must stop at (the targeting column's left).
local mainBarY = -40
local classBarsRight

local function PlaceGameBars()
    if InCombatLockdown() then
        return
    end

    local wheel = db.style == "wheel"
    local width, height = ScreenWidth(), ScreenHeight()

    -- With the wheel, the main bar (its experience bar) spans the middle half of the screen, from the joystick's edge,
    -- with its top a little above the screen's bottom (clear of the system's gesture bar), where the stance, pet and
    -- totem bars start.
    local barScale = wheel and min(1, width * 0.5 / MainMenuBar:GetWidth()) or 1
    mainBarY = wheel and floor(height * 0.035 / barScale - 53) or 0
    MainMenuBar:SetScale(barScale)
    MainMenuBar:ClearAllPoints()
    MainMenuBar:SetPoint("BOTTOM", UIParent, "BOTTOM", 0, mainBarY)

    -- The stance, pet and totem buttons (30 high) a finger's size, as long as their widest bar (the pet bar, 509 wide)
    -- fits between the joystick and the targeting column.
    local size = height * 0.07 / 30

    if wheel and classBarsRight then
        size = min(size, (classBarsRight - width * 0.25 - 40 * barScale) / 509)
    end

    for _, name in ipairs(SMALL_BARS) do
        if _G[name] then
            _G[name]:SetScale(wheel and size / barScale or 1)
        end
    end

    -- Getting out of a vehicle the main bar has: a finger's size too.
    if MainMenuBarVehicleLeaveButton then
        MainMenuBarVehicleLeaveButton:SetScale(wheel and height * 0.09 / 32 / barScale or 1)
    end

    -- A vehicle's own bar: as wide as half the screen at most, its action buttons see-through (the wheel casts them).
    if VehicleMenuBar then
        VehicleMenuBar:SetScale(wheel and min(1, width * 0.5 / 970) or 1)
    end

    if VehicleMenuBarActionButtonFrame then
        VehicleMenuBarActionButtonFrame:SetAlpha(wheel and 0 or 1)
    end
end

-- The pet bar goes above whichever class bar is showing (stances, totems, possession), in its own row: the game places
-- it beside the stance bar by the stance bar's screen position, which with the main bar moved puts it under the wheel.
-- Done when the game has placed it (its slide over), out of combat.
local function PlacePetBar()
    local bar = PetActionBarFrame

    if db.style ~= "wheel" or InCombatLockdown() or not bar or not bar:IsShown() or (bar.slideTimer and bar.timeToSlide and bar.slideTimer < bar.timeToSlide) then
        return
    end

    local below

    for _, name in ipairs({ "MultiCastActionBarFrame", "PossessBarFrame", "ShapeshiftBarFrame" }) do
        if _G[name] and _G[name]:IsShown() then
            below = _G[name]
            break
        end
    end

    local _, relativeTo = bar:GetPoint(1)

    if below and relativeTo ~= below then
        bar:ClearAllPoints()
        bar:SetPoint("BOTTOMLEFT", below, "TOPLEFT", 0, 4)
    end
end

-- The quest tracker goes left of the minimap, under the buffs and above the wheel, instead of down the right edge
-- behind it (UIParent_ManageFramePositions places it, then redraws it for its height).
local function PlaceTracker()
    if not WatchFrame or WatchFrame:IsUserPlaced() or db.style ~= "wheel" then
        return
    end

    -- A block beside the minimap, down to just above the highest touch button under it (the screen's right side under
    -- the minimap is left to the camera). The tracker shows only the quests that fit its height, so it takes all of
    -- that room: a fixed short block held one quest.
    local height = ScreenHeight()
    local right = -(MinimapCluster and MinimapCluster:GetWidth() or 192) - 12
    local trackerRight = UIParent:GetWidth() + right
    local trackerLeft = trackerRight - WatchFrame:GetWidth()
    local bottom = height * 0.3
    local uiScale = UIParent:GetEffectiveScale()

    for _, list in ipairs({ slots, buttons }) do
        for _, frame in pairs(list) do
            local left, frameBottom, width, frameHeight = frame:GetRect()

            if left and frame:IsShown() then
                local scale = frame:GetEffectiveScale() / uiScale

                if (left + width) * scale > trackerLeft and left * scale < trackerRight then
                    bottom = max(bottom, (frameBottom + frameHeight) * scale + 8)
                end
            end
        end
    end

    WatchFrame:SetPoint("TOPRIGHT", UIParent, "TOPRIGHT", right, -height * 0.15)
    WatchFrame:SetPoint("BOTTOMRIGHT", UIParent, "BOTTOMRIGHT", right, min(bottom, height * 0.68))

    -- A vehicle's seats, left of the tracker rather than over the wheel.
    if VehicleSeatIndicator then
        VehicleSeatIndicator:ClearAllPoints()
        VehicleSeatIndicator:SetPoint("TOPRIGHT", WatchFrame, "TOPLEFT", -8, 0)
    end
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
    SizeRound(buttons.page, height * PAGE_SIZE)
    PlaceAt(buttons.page, width - height * 0.065, height * 0.065)

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
    -- A column clear of the wheel's reach, above the stance, pet and totem bars along the bottom.
    local columnX = centerX - height * (reach + SMALL_SIZE * 0.5 + 0.05)
    classBarsRight = columnX - height * SMALL_SIZE * 0.6

    for i, name in ipairs({ "interact", "targetEnemy", "targetFriend", "leave" }) do
        SizeRound(buttons[name], height * SMALL_SIZE)

        if wheel then
            PlaceAt(buttons[name], columnX, height * (0.2 + 0.1 * (i - 1)))
        else
            PlaceAt(buttons[name], centerX - height * 0.17, centerY - height * 0.05 + height * 0.11 * (i - 1))
        end
    end

    -- The emotes button, left of the targeting buttons (shown as the menu's option has it: ApplyHidden).
    SizeRound(buttons.emote, height * SMALL_SIZE)

    if wheel then
        PlaceAt(buttons.emote, columnX - height * SMALL_SIZE * 1.6, height * 0.3)
    else
        PlaceAt(buttons.emote, centerX - height * (0.17 + SMALL_SIZE * 1.6), centerY + height * 0.06)
    end

    -- Chat, the character panel, the bags, the menu and the show / hide toggle at the top, between the unit frames and
    -- the minimap.
    local topRow = { "chat", "character", "bags", "actions", "modifier", "menu", "toggle" }

    for i, name in ipairs(topRow) do
        SizeRound(buttons[name], height * SMALL_SIZE)
        PlaceAt(buttons[name], width * 0.5 + (i - (#topRow + 1) / 2) * height * SMALL_SIZE * 1.35, height * (1 - SMALL_SIZE * 0.7))
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

    -- Pages numbered plainly, 1 on: with fewer buttons than keys a bar page is several of them, one after another.
    -- Every tap goes to the next, none skipped, so the number always says where the wheel is.
    buttons.page.text:SetText((OtherBar() and 0 or (current - 1) * groups) + group)

    -- On a vehicle's bar the page doesn't count: the page button only steps through its groups.
    if OtherBar() then
        nextGroup, pendingPage, pendingGroup = group % groups + 1, nil, nil
        controls.page.key = groups > 1 and GetBindingKey("TOUCHCONTROLS_NEXTGROUP") or nil
        return
    end

    local position = ((current - 1) * groups + group) % (pages * groups)
    local targetPage, targetGroup = floor(position / groups) + 1, position % groups + 1

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
            -- The page button with the wheel only; leave in a vehicle only; emotes when the option has it.
            if not db.hidden and (name ~= "page" or wheel) and (name ~= "leave" or (CanExitVehicle and CanExitVehicle()))
                    and (name ~= "emote" or db.emoteButton) then
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

        -- F13 is Print Screen to the game (the same key code): interact's binding there from before moves.
        if GetBindingAction("F13") == "TOUCHCONTROLS_INTERACT" then
            SetBinding("F13")
        end

        -- Keys the game names with their modifiers in its own order (ALT-CTRL-SHIFT-): a binding saved as SHIFT-CTRL-
        -- never matches a key press. Those from before move to their CTRL-SHIFT- spares below.
        for _, key in ipairs({ "SHIFT-CTRL-F14", "SHIFT-CTRL-F15", "SHIFT-CTRL-F16" }) do
            if (GetBindingAction(key) or "") ~= "" then
                SetBinding(key)
            end
        end

        -- The ring's actions the player has no key for get a key of the add-on's own.
        for _, entry in ipairs(radialEntries) do
            if not GetBindingKey(entry.action) and (GetBindingAction(entry.spare) or "") == "" then
                SetBinding(entry.spare, entry.action)
            end
        end

        for key, action in pairs({ F18 = "TOUCHCONTROLS_INTERACT", F14 = "TOUCHCONTROLS_TARGETNONHOSTILE", F15 = "TOUCHCONTROLS_TOGGLE", F16 = "TOUCHCONTROLS_MENU", F17 = "TOUCHCONTROLS_NEXTGROUP", F19 = "TOUCHCONTROLS_BAGS",
            ["CTRL-F14"] = "TOUCHCONTROLS_DISMOUNT", ["CTRL-F15"] = "TOUCHCONTROLS_FOLLOW", ["CTRL-F16"] = "TOUCHCONTROLS_RADIAL",
            ["CTRL-F17"] = "TOUCHCONTROLS_ASSIGN", ["CTRL-F18"] = "TOUCHCONTROLS_MODIFIER", ["CTRL-F19"] = "TOUCHCONTROLS_EMOTES" }) do
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
    controls.bags.key = GetBindingKey("TOUCHCONTROLS_BAGS")
    controls.actions.key = GetBindingKey("TOUCHCONTROLS_RADIAL")
    controls.modifier.key = GetBindingKey("TOUCHCONTROLS_MODIFIER")
    controls.leave.key = GetBindingKey("TOUCHCONTROLS_DISMOUNT")
    controls.emote.key = GetBindingKey("TOUCHCONTROLS_EMOTES")

    for _, entry in ipairs(radialEntries) do
        controls[entry.name].key = GetBindingKey(entry.action)
    end
    controls.character.key = GetBindingKey("TOGGLECHARACTER0")
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

                if control.wheel then
                    lastHeldSlot = tonumber(strsub(name, 7))
                end
            else
                pressed:Hide()
            end
        end
    end

    UpdateModifier()

    -- A ring button pressed and let go (however quickly): its action is done, and the ring closes.
    if radialOpen then
        local done = false

        for _, entry in ipairs(radialEntries) do
            done = TouchControls_WasPressed(entry.name) or done
        end

        if done then
            CloseRadial()
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
            ShowGroup(1)
        end

        -- The leave button while there's a vehicle to leave.
        local canLeave = not db.hidden and CanExitVehicle and CanExitVehicle() and true or false

        if canLeave ~= buttons.leave:IsShown() then
            if canLeave then
                buttons.leave:Show()
            else
                buttons.leave:Hide()
            end
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
        if db.style == "wheel" and not InCombatLockdown() and MainMenuBar:IsShown() and not MainMenuBar.busy and floor(select(5, MainMenuBar:GetPoint(1)) + 0.5) ~= mainBarY then
            PlaceGameBars()
        end

        PlacePetBar()

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

------------------------------------------------------------------------------------------------------------------------
-- Bags a finger can drag by their frame: each bag goes back where it was put whenever the game lays the open ones out.

local bagPlaces = {}        -- bag id -> left, top where it was put

local function PlaceMovedBags()
    for index = 1, NUM_CONTAINER_FRAMES or 13 do
        local frame = _G["ContainerFrame" .. index]
        local place = frame and frame:IsShown() and bagPlaces[frame:GetID()]

        if place then
            frame:ClearAllPoints()
            frame:SetPoint("TOPLEFT", UIParent, "BOTTOMLEFT", place[1], place[2])
        end
    end
end

for index = 1, NUM_CONTAINER_FRAMES or 13 do
    local frame = _G["ContainerFrame" .. index]

    if frame then
        frame:SetMovable(true)
        frame:SetClampedToScreen(true)
        frame:RegisterForDrag("LeftButton")
        frame:SetScript("OnDragStart", function(self)
            self:StartMoving()
        end)
        frame:SetScript("OnDragStop", function(self)
            self:StopMovingOrSizing()
            bagPlaces[self:GetID()] = { self:GetLeft(), self:GetTop() }
        end)
    end
end

hooksecurefunc("updateContainerFrameAnchors", PlaceMovedBags)

------------------------------------------------------------------------------------------------------------------------
-- Defaults: every touch and display option back as a new install has them, after a yes.

-- Interface options for a finger, which has no Shift and no hover: item tooltips always compare with what's equipped,
-- talents are planned before they're learned (a tap only previews), and every buff shows as its own icon.
local function TouchSettings()
    SetCVar("alwaysCompareItems", "1")
    SetCVar("previewTalents", "1")
    SetCVar("consolidateBuffs", "0")
end

local function ResetDefaults()
    for key, value in pairs(defaults) do
        db[key] = value
    end

    -- The one-time settings are made again now, not at the next login.
    db.displayChosen, db.autoLootChosen, db.graphicsChosen, db.glowChosen, db.touchSettingsChosen = true, true, true, true, true
    TouchSettings()
    wheelOffset = 0
    wipe(bagPlaces)

    SetUiScale(RecommendedScale())
    ApplyTextScale(db.textScale)
    SetChatSize(16)
    SetCVar("ffxGlow", "0")
    TouchControls_SetFrameRate(db.frameRate)
    TouchControls_SetCamera(db.cameraSpeed, db.cameraInvert)

    for _, mode in ipairs(renderModes) do
        if mode.height <= 1080 then
            pendingRenderMode, pendingRenderAt = mode, GetTime() + 0.5
            break
        end
    end

    Refresh()
    refreshAt = GetTime() + 0.3
    UpdateSliders()
end

StaticPopupDialogs["TOUCHCONTROLS_DEFAULTS"] = {
    text = "Put every touch and display option back to its default?",
    button1 = YES or "Yes",
    button2 = NO or "No",
    OnAccept = ResetDefaults,
    timeout = 0,
    whileDead = 1,
    hideOnEscape = 1,
}

defaultsButton:SetScript("OnClick", function()
    StaticPopup_Show("TOUCHCONTROLS_DEFAULTS")
end)

-- The character button shows the player's face.
local function UpdatePortrait()
    SetPortraitTexture(buttons.character.icon, "player")
    buttons.character.icon:Show()
end

-- Menus stay open until a choice, Cancel or Back: a finger leaving one (it leaves the screen after every lift) would
-- otherwise start its two-second close, and a submenu reached by a hold on its arrow would be gone before the tap.
hooksecurefunc("UIDropDownMenu_StartCounting", function(frame)
    while frame and frame.parent do
        frame = frame.parent
    end

    if frame then
        frame.showTimer = nil
        frame.isCounting = nil
    end
end)

------------------------------------------------------------------------------------------------------------------------
-- How to play: a guide to the touch controls, a section at a time, picked from a list on the left. It opens by itself
-- the first time in the world; the menu's How to play button and /touch help bring it back.

local HELP = {
    { title = "Moving and the camera", text = {
        "|cffffd200Move|r: put your left thumb anywhere in the lower left and push. The further you push, the faster you go.",
        "|cffffd200Autorun|r: double-tap the joystick. Move again, or double-tap, to stop.",
        "|cffffd200Look around|r: drag anywhere on the world with your right thumb. Your character turns with the camera.",
        "|cffffd200Zoom|r: pinch with two fingers on the world.",
        "|cffffd200Select|r: tap a creature or player. |cffffd200Talk, loot or attack|r: hold your finger on it.",
        "|cffffd200Jump|r: the big button in the bottom right corner.",
    } },
    { title = "Spells and the wheel", text = {
        "The buttons around jump are your action bar. |cffffd200Tap|r one to use it.",
        "|cffffd200The number button|r in the corner turns to the next page of your bar: 1, 2, 3... and back to 1.",
        "|cffffd200Put a spell on a button|r: hold the button. The spellbook opens; tap the spell you want there, or |cffffd200Clear button|r at its top to empty it.",
        "You can also drag a spell or item from the spellbook or your bags and drop it on a button.",
        "|cffffd200Lock buttons|r (in the menu) stops a long press from opening the spellbook.",
        "|cffffd200Buttons on screen|r (in the menu): show 12, 8, 6 or 4. Fewer are bigger; the number button steps through the rest.",
        "Interact (the open hand) loots, talks to or uses the nearest thing. The two target buttons pick the nearest enemy, or friend.",
    } },
    { title = "The top bar", text = {
        "|cffffd200Letter|r: chat. |cffffd200Portrait|r: your character. |cffffd200Bag|r: all your bags.",
        "|cffffd200Mask|r: actions that are only keys on a keyboard (sit, sheathe, walk, map, assist, focus, reply, marks...), and hiding or showing the chat.",
        "|cffffd200Emotes|r: the face left of the target buttons opens the emotes (/wave, /thank, /dance...); tap one. The menu can hide it.",
        "|cffffd200Key|r: Shift, Ctrl or Alt for your next tap (see Shift, Ctrl and Alt).",
        "|cffffd200Gear|r: the menu. Every game window, your bag slots and the settings.",
        "|cffffd200Eye|r: hide the touch controls, for screenshots and cutscenes. Tap it again to bring them back.",
        "|cffffd200Back|r (swipe in from the screen's edge) closes windows and opens the game menu, like Escape.",
    } },
    { title = "Reading and right-clicking", text = {
        "|cffffd200Read|r an item, spell or buff: hold your finger on it. Its tooltip shows while you hold, and follows your finger as you slide.",
        "|cffffd200Right-click|r: hold, then lift where you started. That uses an item, buys it, opens a menu...",
        "|cffffd200Changed your mind?|r Slide your finger away before you lift, and nothing happens.",
        "Item tooltips always compare with what you're wearing.",
        "|cffffd200Scroll|r a list, the chat or a window: drag up or down on it.",
    } },
    { title = "Shift, Ctrl and Alt", text = {
        "Tap the |cffffd200key button|r on the top bar: Shift, again for Ctrl, again for Alt, again for off.",
        "It glows while it waits. Your |cffffd200next|r tap, hold or drag uses it, then it turns off.",
        "|cffffd200Shift + tap|r a stack: split it. At a vendor: buy several. With chat open: link the item.",
        "|cffffd200Shift + hold and lift|r on an item with sockets: socket gems.",
        "|cffffd200Ctrl + tap|r an item: try it on in the dressing room.",
        "|cffffd200Alt + tap|r a slot on your character: choose from the items for that slot.",
    } },
    { title = "Bags and items", text = {
        "The |cffffd200bag button|r on the top bar opens and closes all your bags.",
        "|cffffd200Move a bag|r: drag it by its frame.",
        "|cffffd200Move an item|r: tap it to pick it up, tap where it should go. Or drag it there.",
        "|cffffd200Use or equip|r: hold the item, then lift.",
        "|cffffd200Bag slots|r: open the menu and tap one of Bag 1 to Bag 4. Its sheet has Open, Take off (the bag has to be empty), and your spare bags: tap one to put it in that slot, or to swap it for the one there.",
    } },
    { title = "Menus and settings", text = {
        "|cffffd200Menus|r (hold a portrait, a name in chat...) stay open until you pick something, tap Cancel, or go Back.",
        "Entries with an |cffffd200arrow|r have more inside: hold the entry to open them, then tap your choice.",
        "|cffffd200The menu|r (gear) has every game window, your bag slots and the settings:",
        "Interface size, text size and chat text; render resolution and glow (lower for a cooler, longer-lasting phone); camera speed and invert; buttons on screen; battery saver.",
        "|cffffd200Action bars|r switches between the wheel and the game's own bars. |cffffd200Defaults|r puts every touch setting back.",
    } },
}

local help = CreateFrame("Frame", "TouchControlsHelp", UIParent)
help:SetFrameStrata("DIALOG")
help:SetToplevel(true)
help:EnableMouse(true)
help:SetBackdrop({
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 32, edgeSize = 32,
    insets = { left = 11, right = 12, top = 12, bottom = 11 },
})
help:Hide()
tinsert(UISpecialFrames, "TouchControlsHelp")

local helpFill = help:CreateTexture(nil, "BACKGROUND")
helpFill:SetTexture(0.04, 0.04, 0.04, 0.94)
helpFill:SetPoint("TOPLEFT", 11, -12)
helpFill:SetPoint("BOTTOMRIGHT", -12, 11)

local helpHeader = help:CreateTexture(nil, "ARTWORK")
helpHeader:SetTexture("Interface\\DialogFrame\\UI-DialogBox-Header")
helpHeader:SetWidth(320)
helpHeader:SetHeight(64)
helpHeader:SetPoint("TOP", 0, 12)

local helpTitle = help:CreateFontString(nil, "ARTWORK", "GameFontNormal")
helpTitle:SetPoint("TOP", helpHeader, "TOP", 0, -14)
helpTitle:SetText("How to play with touch")

-- The page: a framed panel with the section's title and its lines.
local helpPage = CreateFrame("Frame", nil, help)
helpPage:SetBackdrop({
    bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
    edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
    tile = true, tileSize = 16, edgeSize = 16,
    insets = { left = 4, right = 4, top = 4, bottom = 4 },
})
helpPage:SetBackdropColor(0.1, 0.1, 0.1, 0.9)
helpPage:SetBackdropBorderColor(0.6, 0.55, 0.45)

local helpPageTitle = helpPage:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
local helpLines = {}

local helpSections = {}
local helpClose = CreateFrame("Button", "TouchControlsHelpClose", help, "UIPanelButtonTemplate")
helpClose:SetText(CLOSE or "Close")
helpClose:SetScript("OnClick", function()
    help:Hide()
end)

local helpShowing = 1

local function ShowHelpSection(index)
    helpShowing = index
    local section = HELP[index]
    helpPageTitle:SetText(section.title)

    for i, line in ipairs(helpLines) do
        line:SetText(section.text[i] or "")
    end

    for i, button in ipairs(helpSections) do
        if i == index then
            button:LockHighlight()
        else
            button:UnlockHighlight()
        end
    end
end

for index, section in ipairs(HELP) do
    local button = CreateFrame("Button", "TouchControlsHelpSection" .. index, help, "UIPanelButtonTemplate")
    button:SetText(section.title)
    button:SetScript("OnClick", function()
        ShowHelpSection(index)
    end)
    helpSections[index] = button
end

for index = 1, 7 do
    local line = helpPage:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
    line:SetJustifyH("LEFT")
    line:SetJustifyV("TOP")
    helpLines[index] = line
end

-- Most of the screen: the sections down the left, the page on the right, Close under it.
local function LayoutHelp()
    local width, height = ScreenWidth(), ScreenHeight()
    local helpWidth, helpHeight = width * 0.86, height * 0.84
    local margin = height * 0.035
    local listWidth = helpWidth * 0.28
    local buttonScale = height * 0.06 / 26
    local inset = height * 0.025

    help:SetWidth(helpWidth)
    help:SetHeight(helpHeight)
    help:ClearAllPoints()
    help:SetPoint("CENTER", UIParent, "CENTER", 0, -height * 0.04)

    -- The section buttons, evenly down the left, a finger tall each.
    local rowHeight = (helpHeight - margin * 3 - 26 * buttonScale) / #helpSections

    for index, button in ipairs(helpSections) do
        button:SetScale(buttonScale)
        button:SetWidth(listWidth / buttonScale)
        button:SetHeight(min(rowHeight * 0.82, height * 0.09) / buttonScale)
        button:ClearAllPoints()
        button:SetPoint("TOPLEFT", help, "TOPLEFT", margin / buttonScale, -(margin + (index - 1) * rowHeight) / buttonScale)
    end

    local pageWidth = helpWidth - listWidth - margin * 3
    local pageHeight = helpHeight - margin * 3 - 26 * buttonScale
    helpPage:SetWidth(pageWidth)
    helpPage:SetHeight(pageHeight)
    helpPage:ClearAllPoints()
    helpPage:SetPoint("TOPLEFT", help, "TOPLEFT", margin * 2 + listWidth, -margin)

    helpPageTitle:ClearAllPoints()
    helpPageTitle:SetPoint("TOPLEFT", helpPage, "TOPLEFT", inset, -inset)

    -- Each line in a row of its own, wrapped to the page.
    local lineTop = inset + height * 0.07
    local lineHeight = (pageHeight - lineTop - inset) / #helpLines

    for index, line in ipairs(helpLines) do
        line:SetFont(line:GetFont(), height * 0.03)
        line:SetWidth(pageWidth - inset * 2)
        line:SetHeight(lineHeight)
        line:ClearAllPoints()
        line:SetPoint("TOPLEFT", helpPage, "TOPLEFT", inset, -(lineTop + (index - 1) * lineHeight))
    end

    helpClose:SetScale(buttonScale)
    helpClose:SetWidth(140)
    helpClose:SetHeight(26)
    helpClose:ClearAllPoints()
    helpClose:SetPoint("BOTTOMRIGHT", help, "BOTTOMRIGHT", -margin / buttonScale, margin / buttonScale)
end

function TouchControls_ShowHelp()
    menu:Hide()
    LayoutHelp()
    ShowHelpSection(helpShowing)
    help:Show()
end

helpButton:SetScript("OnClick", TouchControls_ShowHelp)

-- A vehicle's aim buttons work while pressed, and a finger's hold is a hover with no button down: holding one aims
-- (the finger's arrival and departure are its enter and leave).
for _, aim in ipairs({ { "VehicleMenuBarPitchUpButton", "VehicleAimUpStart", "VehicleAimUpStop" }, { "VehicleMenuBarPitchDownButton", "VehicleAimDownStart", "VehicleAimDownStop" } }) do
    local button = _G[aim[1]]

    if button then
        button:HookScript("OnEnter", function()
            _G[aim[2]]()
        end)
        button:HookScript("OnLeave", function()
            _G[aim[3]]()
        end)
    end
end

-- A totem slot's arrow (choosing its totem) shows only while the mouse is over the slot, which a finger leaves at once:
-- it stays, so a hold on the slot then a tap on the arrow opens the choice.
if MultiCastFlyoutFrameOpenButton_Show then
    local lastType, lastParent

    hooksecurefunc("MultiCastFlyoutFrameOpenButton_Show", function(self, type, parent)
        lastType, lastParent = type, parent
    end)

    hooksecurefunc("MultiCastFlyoutFrameOpenButton_Hide", function(self, force)
        if not force and lastType and lastParent and lastParent:IsVisible() and not self:IsShown() then
            MultiCastFlyoutFrameOpenButton_Show(self, lastType, lastParent)
        end
    end)
end

-- The game's own tooltip spot is the bottom right corner, under the wheel and the right thumb: tooltips that use it
-- (units in the world, action buttons, the minimap...) go to the top middle instead, under the top row of buttons,
-- where nothing covers them and no finger does.
hooksecurefunc("GameTooltip_SetDefaultAnchor", function(tooltip, parent)
    if not db then
        return
    end

    tooltip:ClearAllPoints()
    tooltip:SetPoint("TOP", UIParent, "TOP", 0, -ScreenHeight() * (SMALL_SIZE * 1.5 + 0.02))
end)

-- A choice in a submenu hides only the submenu (the mouse leaving would close the rest, which it no longer does): the
-- whole menu goes with it.
hooksecurefunc("UIDropDownMenuButton_OnClick", function(self)
    if not self.keepShownOnClick and not self.hasArrow then
        CloseDropDownMenus()
    end
end)

-- UIParent hides for the full-screen world map, cinematics and Alt-Z, and this frame with it: its controls go too, so
-- taps reach what is showing, and come back with it.
------------------------------------------------------------------------------------------------------------------------
-- Hiding the chat: its windows, tabs and buttons (the friends counter too) go under a hidden frame, where the chat code's own Show calls (docking,
-- fading, a new tab) can't bring them back and messages still arrive. The typing box of each window stays on the
-- screen, where it was, so chat still opens and sends; it hides again once the message is sent. Kept in
-- TouchControlsDB.chatHidden. (In a block: the main chunk is near Lua's 200 locals.)

do
    local stash = CreateFrame("Frame")
    stash:Hide()
    local parents = {}      -- frame -> its parent before

    local function ChatParts()
        local parts = { GeneralDockManager, ChatFrameMenuButton, FriendsMicroButton, CombatLogQuickButtonFrame_Custom }

        for index = 1, NUM_CHAT_WINDOWS or 10 do
            tinsert(parts, _G["ChatFrame" .. index])
            tinsert(parts, _G["ChatFrame" .. index .. "Tab"])
        end

        return parts
    end

    function TouchControls_SetChatHidden(hidden)
        if hidden then
            for _, frame in pairs(ChatParts()) do
                if not parents[frame] then
                    parents[frame] = frame:GetParent()
                    frame:SetParent(stash)
                end
            end

            for index = 1, NUM_CHAT_WINDOWS or 10 do
                local editBox = _G["ChatFrame" .. index .. "EditBox"]

                if editBox and not parents[editBox] then
                    parents[editBox] = editBox:GetParent()
                    editBox:SetParent(UIParent)

                    if editBox ~= ACTIVE_CHAT_EDIT_BOX then
                        editBox:Hide()
                    end
                end
            end
        else
            for frame, parent in pairs(parents) do
                frame:SetParent(parent)
            end

            wipe(parents)

            -- The typing box shows as the chat style has it (always, faded, for the "IM" style).
            if ChatEdit_GetLastActiveWindow and GetCVar("chatStyle") == "im" then
                local editBox = ChatEdit_GetLastActiveWindow()

                if editBox then
                    editBox:Show()
                end
            end
        end
    end

    function TouchControls_ToggleChat()
        TouchControlsDB.chatHidden = not TouchControlsDB.chatHidden
        TouchControls_SetChatHidden(TouchControlsDB.chatHidden)
    end

    -- A sent or cancelled message leaves the typing box out, faded, in the "IM" chat style: away while the chat is.
    hooksecurefunc("ChatEdit_DeactivateChat", function(editBox)
        if TouchControlsDB and TouchControlsDB.chatHidden then
            editBox:Hide()
        end
    end)

    -- A window opened while hidden, and the combat log's buttons, which load with it.
    hooksecurefunc("FCF_OpenNewWindow", function()
        if TouchControlsDB and TouchControlsDB.chatHidden then
            TouchControls_SetChatHidden(true)
        end
    end)

    local watch = CreateFrame("Frame")
    watch:RegisterEvent("PLAYER_ENTERING_WORLD")
    watch:RegisterEvent("ADDON_LOADED")
    watch:SetScript("OnEvent", function(self, event, name)
        if TouchControlsDB and TouchControlsDB.chatHidden and (event == "PLAYER_ENTERING_WORLD" or name == "Blizzard_CombatLog") then
            TouchControls_SetChatHidden(true)
        end
    end)
end

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
addon:RegisterEvent("UNIT_PORTRAIT_UPDATE")

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

    -- The guide, the first time in the world.
    if event == "PLAYER_ENTERING_WORLD" and not db.helpShown then
        db.helpShown = true
        TouchControls_ShowHelp()
    end

    if event == "PLAYER_ENTERING_WORLD" and not db.touchSettingsChosen then
        db.touchSettingsChosen = true
        TouchSettings()
    end

    if event == "PLAYER_ENTERING_WORLD" and not db.displayChosen then
        db.displayChosen = true
        SetUiScale(RecommendedScale())
        SetChatSize(16)
        refreshAt = GetTime() + 0.3
    end

    if event == "PLAYER_ENTERING_WORLD" or (event == "UNIT_PORTRAIT_UPDATE" and name == "player") then
        UpdatePortrait()

        if event == "UNIT_PORTRAIT_UPDATE" then
            return
        end
    end

    if event == "UPDATE_CHAT_WINDOWS" then
        PlaceChat()
    elseif event == "ACTIONBAR_PAGE_CHANGED" then
        -- To the group the page button was going to, or the page's first.
        local group = pendingPage == GetActionBarPage() and pendingGroup or 1
        wheelOffset = (group - 1) * db.wheelCount
        SetWheelKeys()
        UpdatePage()
    elseif event == "UPDATE_BONUS_ACTIONBAR" then
        -- A form, stealth or possession: its bar from its first keys.
        ShowGroup(1)
    elseif event == "ACTIONBAR_SLOT_CHANGED" then
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
    elseif message == "help" then
        TouchControls_ShowHelp()
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
        DEFAULT_CHAT_FRAME:AddMessage("Touch Controls: /touch help, /touch wheel, /touch bars (action buttons), /touch hide, /touch show.")
    end
end
