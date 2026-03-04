/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "AttackAction.h"

#include "CreatureAI.h"
#include "Event.h"
#include "Group.h"
#include "LastMovementValue.h"
#include "LootObjectStack.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "Unit.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <sstream>

namespace
{
uint8 constexpr PULL_MEMORY_NEARBY_RADIUS = 12;
int32 constexpr PULL_MEMORY_HIGH_RISK_SCORE = 8;
int32 constexpr PULL_MEMORY_MAX_SCORE = 20;

std::string MakePullMemoryKey(uint32 mapId, uint32 creatureEntry)
{
    std::ostringstream key;
    key << "pm:" << mapId << ':' << creatureEntry;
    return key.str();
}

int32 LoadPullDangerScore(uint32 botGuid, uint32 mapId, uint32 creatureEntry)
{
    std::string const key = MakePullMemoryKey(mapId, creatureEntry);
    QueryResult result = PlayerbotsDatabase.Query(
        "SELECT `value` FROM `playerbots_db_store` WHERE `guid` = {} AND `key` = '{}' ORDER BY `id` DESC LIMIT 1",
        botGuid, key);
    if (!result)
        return 0;

    Field* fields = result->Fetch();
    if (!fields)
        return 0;

    std::string const value = fields[0].Get<std::string>();
    if (value.empty())
        return 0;

    return std::max(0, std::atoi(value.c_str()));
}

void SavePullDangerScore(uint32 botGuid, uint32 mapId, uint32 creatureEntry, int32 score)
{
    std::string const key = MakePullMemoryKey(mapId, creatureEntry);
    int32 const clampedScore = std::clamp(score, 0, PULL_MEMORY_MAX_SCORE);

    // Keep a single record per key in db_store for predictable reads.
    PlayerbotsDatabase.Execute(
        "DELETE FROM `playerbots_db_store` WHERE `guid` = {} AND `key` = '{}'",
        botGuid, key);
    PlayerbotsDatabase.Execute(
        "INSERT INTO `playerbots_db_store` (`guid`, `key`, `value`) VALUES ({}, '{}', '{}')",
        botGuid, key, clampedScore);
}

uint8 CountNearbyHostilesAtPull(PlayerbotAI* botAI, Player* bot, Unit* pullTarget, float radius)
{
    if (!botAI || !bot || !pullTarget)
        return 0;

    GuidVector const possibleTargets = botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets")->Get();
    uint8 count = 0;
    for (ObjectGuid const& guid : possibleTargets)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || unit == pullTarget || !unit->IsAlive() || unit->IsPlayer() || !bot->IsHostileTo(unit))
            continue;

        if (ServerFacade::instance().IsDistanceLessOrEqualThan(
                ServerFacade::instance().GetDistance2d(unit, pullTarget),
                radius))
        {
            ++count;
        }
    }

    return count;
}

int32 LearnPullDanger(PlayerbotAI* botAI, Player* bot, Unit* pullTarget)
{
    Creature* creature = pullTarget ? pullTarget->ToCreature() : nullptr;
    if (!botAI || !bot || !creature)
        return 0;

    uint32 const botGuid = uint32(bot->GetGUID().GetCounter());
    uint32 const mapId = bot->GetMapId();
    uint32 const creatureEntry = creature->GetEntry();
    int32 dangerScore = LoadPullDangerScore(botGuid, mapId, creatureEntry);

    uint8 const nearbyHostiles = CountNearbyHostilesAtPull(botAI, bot, pullTarget, float(PULL_MEMORY_NEARBY_RADIUS));
    int32 delta = 0;
    if (nearbyHostiles >= 4)
        delta = 3;
    else if (nearbyHostiles == 3)
        delta = 2;
    else if (nearbyHostiles == 2)
        delta = 1;
    else if (nearbyHostiles == 0)
        delta = -1;

    if (botAI->GetAiObjectContext()->GetValue<bool>("possible adds")->Get())
        ++delta;

    int32 const updatedScore = std::clamp(dangerScore + delta, 0, PULL_MEMORY_MAX_SCORE);
    if (updatedScore != dangerScore)
        SavePullDangerScore(botGuid, mapId, creatureEntry, updatedScore);

    return updatedScore;
}

bool IsDungeonTankLeadPull(PlayerbotAI* botAI, Player* bot, Unit* target)
{
    if (!botAI || !bot || !target || !bot->GetMap() || !bot->GetMap()->IsDungeon() || !bot->GetGroup())
        return false;

    if (!botAI->IsTank(bot) || target->IsInCombat() || target->IsPlayer() || !target->IsAlive())
        return false;

    return bot->IsHostileTo(target);
}

bool IconNeedsRefresh(Group* group, PlayerbotAI* botAI, Player* bot, uint8 icon, Unit* pullTarget)
{
    if (!group || !botAI || !bot)
        return false;

    ObjectGuid const currentGuid = group->GetTargetIcon(icon);
    if (currentGuid.IsEmpty())
        return true;

    Unit* currentTarget = botAI->GetUnit(currentGuid);
    if (!currentTarget || !currentTarget->IsAlive() || !currentTarget->IsInWorld() || currentTarget->IsPlayer() ||
        !bot->IsHostileTo(currentTarget))
    {
        return true;
    }

    if (pullTarget &&
        ServerFacade::instance().IsDistanceGreaterThan(
            ServerFacade::instance().GetDistance2d(currentTarget, pullTarget), 45.0f))
    {
        return true;
    }

    return false;
}

Unit* SelectCcTargetForPull(PlayerbotAI* botAI, Player* bot, Unit* pullTarget)
{
    if (!botAI || !bot || !pullTarget)
        return nullptr;

    Unit* bestTarget = nullptr;
    int32 bestScore = INT_MIN;
    GuidVector const possibleTargets = botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets")->Get();
    for (ObjectGuid const& guid : possibleTargets)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || unit == pullTarget || !unit->IsAlive() || unit->IsPlayer() || !bot->IsHostileTo(unit))
            continue;

        if (ServerFacade::instance().IsDistanceGreaterThan(
                ServerFacade::instance().GetDistance2d(unit, pullTarget), 12.0f))
        {
            continue;
        }

        int32 score = 0;
        if (unit->getPowerType() == POWER_MANA)
            score += 8;

        if (unit->GetCreatureType() == CREATURE_TYPE_HUMANOID)
            score += 4;

        score += int32(unit->GetMaxHealth() / 10000);
        if (score > bestScore)
        {
            bestScore = score;
            bestTarget = unit;
        }
    }

    return bestTarget;
}

void AssignTankPullIcons(PlayerbotAI* botAI, Player* bot, Unit* pullTarget)
{
    Group* group = bot->GetGroup();
    if (!group || !pullTarget)
        return;

    uint8 constexpr ICON_SKULL = 7;
    uint8 constexpr ICON_MOON = 4;

    if (IconNeedsRefresh(group, botAI, bot, ICON_SKULL, pullTarget))
        group->SetTargetIcon(ICON_SKULL, bot->GetGUID(), pullTarget->GetGUID());

    Unit* ccTarget = SelectCcTargetForPull(botAI, bot, pullTarget);
    if (!ccTarget || ccTarget == pullTarget)
        return;

    if (IconNeedsRefresh(group, botAI, bot, ICON_MOON, pullTarget))
        group->SetTargetIcon(ICON_MOON, bot->GetGUID(), ccTarget->GetGUID());
}

bool TryTankRangedPull(PlayerbotAI* botAI, Player* bot, Unit* target)
{
    if (!botAI || !bot || !target || bot->IsWithinMeleeRange(target) || !bot->IsWithinLOSInMap(target))
        return false;

    switch (bot->getClass())
    {
        case CLASS_WARRIOR:
            if (botAI->CanCastSpell("heroic throw", target))
                return botAI->CastSpell("heroic throw", target);
            break;
        case CLASS_PALADIN:
            if (botAI->CanCastSpell("avenger's shield", target))
                return botAI->CastSpell("avenger's shield", target);
            break;
        case CLASS_DRUID:
            if (botAI->CanCastSpell("faerie fire (feral)", target))
                return botAI->CastSpell("faerie fire (feral)", target);
            break;
        case CLASS_DEATH_KNIGHT:
            if (botAI->CanCastSpell("death grip", target))
                return botAI->CastSpell("death grip", target);
            break;
        default:
            break;
    }

    if (botAI->CanCastSpell("shoot", target))
        return botAI->CastSpell("shoot", target);

    return false;
}

bool ShouldWaitForTankPull(PlayerbotAI* botAI, Player* bot, Unit* target)
{
    if (!botAI || !bot || !target || !bot->GetMap() || !bot->GetMap()->IsDungeon() || !bot->GetGroup())
        return false;

    if (botAI->IsTank(bot) || !target->IsCreature())
        return false;

    Unit* mainTankUnit = botAI->GetAiObjectContext()->GetValue<Unit*>("main tank")->Get();
    Player* mainTank = mainTankUnit ? mainTankUnit->ToPlayer() : nullptr;
    if (!mainTank || mainTank == bot || !mainTank->IsAlive() || mainTank->GetMapId() != bot->GetMapId())
        return false;

    // Never block self-defense.
    if (target->GetVictim() == bot || bot->GetVictim() == target)
        return false;

    // Hold DPS until tank has started the pull.
    if (!target->IsInCombat())
        return true;

    // If combat already started, wait until the tank has initial threat/victim lock.
    if (target->GetVictim() == mainTank)
        return false;

    float const tankThreat = target->GetThreatMgr().GetThreat(mainTank);
    return tankThreat <= 0.0f;
}
}

bool AttackAction::Execute(Event /*event*/)
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    if (!target->IsInWorld())
        return false;

    return Attack(target);
}

bool AttackMyTargetAction::Execute(Event /*event*/)
{
    Player* master = GetMaster();
    if (!master)
        return false;

    ObjectGuid guid = master->GetTarget();
    if (!guid)
    {
        if (verbose)
            botAI->TellError("You have no target");

        return false;
    }

    Unit* target = botAI->GetUnit(guid);
    if (!target)
        return false;

    botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Set({guid});

    if (IsDungeonTankLeadPull(botAI, bot, target))
    {
        int32 const dangerScore = LearnPullDanger(botAI, bot, target);
        AssignTankPullIcons(botAI, bot, target);
        if (TryTankRangedPull(botAI, bot, target))
        {
            context->GetValue<ObjectGuid>("pull target")->Set(guid);
            return true;
        }

        // High-risk packs are remembered per dungeon+npc entry. Avoid direct face-pulls.
        if (dangerScore >= PULL_MEMORY_HIGH_RISK_SCORE && !bot->IsWithinMeleeRange(target))
        {
            botAI->TellMasterNoFacing("Holding risky pull: high learned danger for this mob in this dungeon.");
            return false;
        }
    }

    bool result = Attack(target);
    if (result)
        context->GetValue<ObjectGuid>("pull target")->Set(guid);

    return result;
}

bool AttackAction::Attack(Unit* target, bool /*with_pet*/ /*true*/)
{
    Unit* oldTarget = context->GetValue<Unit*>("current target")->Get();
    bool shouldMelee = bot->IsWithinMeleeRange(target) || botAI->IsMelee(bot);

    bool sameTarget = oldTarget == target && bot->GetVictim() == target;
    bool inCombat = botAI->GetState() == BOT_STATE_COMBAT;
    bool sameAttackMode = bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING) == shouldMelee;

    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE ||
        bot->HasUnitState(UNIT_STATE_IN_FLIGHT))
    {
        if (verbose)
            botAI->TellError("I cannot attack in flight");

        return false;
    }

    if (!target)
    {
        if (verbose)
            botAI->TellError("I have no target");

        return false;
    }

    if (!target->IsInWorld())
    {
        if (verbose)
            botAI->TellError(std::string(target->GetName()) + " is no longer in the world.");

        return false;
    }

    // Check if bot OR target is in prohibited zone/area (skip for duels)
    if ((target->IsPlayer() || target->IsPet()) &&
        (!bot->duel || bot->duel->Opponent != target) &&
        (sPlayerbotAIConfig.IsPvpProhibited(bot->GetZoneId(), bot->GetAreaId()) ||
        sPlayerbotAIConfig.IsPvpProhibited(target->GetZoneId(), target->GetAreaId())))
    {
        if (verbose)
            botAI->TellError("I cannot attack other players in PvP prohibited areas.");

        return false;
    }

    if (bot->IsFriendlyTo(target))
    {
        if (verbose)
            botAI->TellError(std::string(target->GetName()) + " is friendly to me.");

        return false;
    }

    if (target->isDead())
    {
        if (verbose)
            botAI->TellError(std::string(target->GetName()) + " is dead.");

        return false;
    }

    if (!bot->IsWithinLOSInMap(target))
    {
        if (verbose)
            botAI->TellError(std::string(target->GetName()) + " is not in my sight.");

        return false;
    }

    if (sameTarget && inCombat && sameAttackMode)
    {
        if (verbose)
            botAI->TellError("I am already attacking " + std::string(target->GetName()) + ".");

        return false;
    }

    if (!bot->IsValidAttackTarget(target))
    {
        if (verbose)
            botAI->TellError("I cannot attack an invalid target.");

        return false;
    }

    if (ShouldWaitForTankPull(botAI, bot, target))
        return false;

    // if (bot->IsMounted() && bot->IsWithinLOSInMap(target))
    // {
    //     WorldPacket emptyPacket;
    //     bot->GetSession()->HandleCancelMountAuraOpcode(emptyPacket);
    // }

    ObjectGuid guid = target->GetGUID();
    bot->SetSelection(target->GetGUID());

        context->GetValue<Unit*>("old target")->Set(oldTarget);

    context->GetValue<Unit*>("current target")->Set(target);
    context->GetValue<LootObjectStack*>("available loot")->Get()->Add(guid);

    LastMovement& lastMovement = AI_VALUE(LastMovement&, "last movement");
    bool moveControlled = bot->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) != NULL_MOTION_TYPE;
    if (lastMovement.priority < MovementPriority::MOVEMENT_COMBAT && bot->isMoving() && !moveControlled)
    {
        AI_VALUE(LastMovement&, "last movement").clear();
        bot->GetMotionMaster()->Clear(false);
        bot->StopMoving();
    }

    if (botAI->CanMove() && !bot->HasInArc(CAST_ANGLE_IN_FRONT, target))
        ServerFacade::instance().SetFacingTo(bot, target);

    botAI->ChangeEngine(BOT_STATE_COMBAT);

    bot->Attack(target, shouldMelee);
    /* prevent pet dead immediately in group */
    // if (bot->GetMap()->IsDungeon() && bot->GetGroup() && !target->IsInCombat())
    // {
    //     with_pet = false;
    // }
    // if (Pet* pet = bot->GetPet())
    // {
    //     if (with_pet)
    //     {
    //         pet->SetReactState(REACT_DEFENSIVE);
    //         pet->SetTarget(target->GetGUID());
    //         pet->GetCharmInfo()->SetIsCommandAttack(true);
    //         pet->AI()->AttackStart(target);
    //     }
    //     else
    //     {
    //         pet->SetReactState(REACT_PASSIVE);
    //         pet->GetCharmInfo()->SetIsCommandFollow(true);
    //         pet->GetCharmInfo()->IsReturning();
    //     }
    // }
    return true;
}

bool AttackDuelOpponentAction::isUseful() { return AI_VALUE(Unit*, "duel target"); }

bool AttackDuelOpponentAction::Execute(Event /*event*/) { return Attack(AI_VALUE(Unit*, "duel target")); }
