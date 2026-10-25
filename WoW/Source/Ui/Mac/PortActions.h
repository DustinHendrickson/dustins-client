#ifndef WOW_SOURCE_UI_MAC_PORTACTIONS_H
#define WOW_SOURCE_UI_MAC_PORTACTIONS_H

// Actions the ports' controls give a button of their own, shared by the Switch's SwitchControls and Android's
// TouchControls add-ons (WoW/Source/Ui/Switch, WoW/Source/Ui/Android).

// Right-clicks the nearest thing a right-click would act on: a lootable or skinnable body, an NPC with something to
// offer, or a usable object, preferring what is in front of the player; targets it if it is a unit. False if nothing
// is in reach. Runs as the game, not as add-on code (which may not interact or target).
bool PortInteractNearest();

// Targets the next living friendly or neutral unit, cycling as Tab does through enemies. Runs as the game.
void PortTargetNonHostile(bool reverse);

#endif
