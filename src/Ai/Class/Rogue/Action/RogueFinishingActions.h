/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_ROGUEFINISHINGACTIONS_H
#define _PLAYERBOT_ROGUEFINISHINGACTIONS_H

#include "GenericSpellActions.h"

class PlayerbotAI;

class CastEviscerateAction : public CastMeleeSpellAction
{
public:
    CastEviscerateAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "eviscerate") {}
};

class CastSliceAndDiceAction : public CastMeleeSpellAction
{
public:
    CastSliceAndDiceAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "slice and dice") {}
};

class CastExposeArmorAction : public CastDebuffSpellAction
{
public:
    CastExposeArmorAction(PlayerbotAI* botAI) : CastDebuffSpellAction(botAI, "expose armor", false, 25.0f) {}
};

class CastRuptureAction : public CastDebuffSpellAction
{
public:
    CastRuptureAction(PlayerbotAI* botAI) : CastDebuffSpellAction(botAI, "rupture", true, 3.0f) {}

    Unit* GetTarget() override
    {
        Unit* comboTarget = bot->GetComboTarget();
        if (comboTarget &&
            comboTarget->IsAlive() &&
            bot->GetComboTargetGUID() == comboTarget->GetGUID() &&
            bot->IsWithinMeleeRange(comboTarget))
        {
            return comboTarget;
        }

        return CastDebuffSpellAction::GetTarget();
    }

    bool isUseful() override
    {
        Unit* target = GetTarget();
        if (!target)
            return false;

        if (bot->GetComboPoints(target) < 3)
            return false;

        return CastDebuffSpellAction::isUseful();
    }
};

class CastKidneyShotAction : public CastMeleeSpellAction
{
public:
    CastKidneyShotAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "kidney shot") {}
};

#endif
