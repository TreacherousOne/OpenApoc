#include "game/state/battle/ai/unitaidefault.h"
#include "game/state/aequipment.h"
#include "game/state/battle/ai/aidecision.h"
#include "game/state/battle/ai/aitype.h"
#include "game/state/battle/battleunit.h"
#include "game/state/gamestate.h"
#include <glm/glm.hpp>

namespace OpenApoc
{

namespace
{
static const std::tuple<AIDecision, bool> NULLTUPLE2 = std::make_tuple(AIDecision(), false);
}

// Delay before unit will turn automatically again after doing it once
static const unsigned AUTO_TURN_COOLDOWN = TICKS_PER_TURN;
// Delay before unit will target an enemy automatically again after failing to do so once
static const unsigned AUTO_TARGET_COOLDOWN = TICKS_PER_TURN / 4;

UnitAIDefault::UnitAIDefault() { type = Type::Default; }

void UnitAIDefault::reset(GameState &, BattleUnit &)
{
	ticksAutoTargetAvailable = 0;
	ticksAutoTurnAvailable = 0;
	attackerPosition = {0, 0, 0};
}

void UnitAIDefault::notifyUnderFire(Vec3<int> position) { attackerPosition = position; }

void UnitAIDefault::notifyHit(Vec3<int> position) { attackerPosition = position; }

std::tuple<AIDecision, bool> UnitAIDefault::think(GameState &state, BattleUnit &u, bool interrupt)
{
	static const Vec3<int> NONE = {0, 0, 0};

	bool realTime = state.current_battle->mode == Battle::Mode::RealTime;

	// Default AI should not work in turn based when it's our turn to act
	// We can assume that if it's not our turn then we're interrupting
	// Otherwise no AI would work
	active = realTime || u.owner != state.current_battle->currentActiveOrganisation;

	if (!active)
	{
		return NULLTUPLE2;
	}

	sp<AIAction> action;
	sp<AIMovement> movement;

	// Turn to attacker in real time if we're idle
	if (attackerPosition != NONE && !u.isBusy() && u.isConscious() &&
	    ticksAutoTurnAvailable <= state.gameTime.getTicks())
	{
		movement = mksp<AIMovement>();
		movement->type = AIMovement::Type::Turn;
		movement->targetLocation = u.position + (Vec3<float>)attackerPosition;
		ticksAutoTurnAvailable = state.gameTime.getTicks() + AUTO_TURN_COOLDOWN;
	}

	// Attack or face enemy
	if (u.isConscious() && !u.isAttacking() &&
	    !state.current_battle->visibleEnemies[u.owner].empty() &&
	    (u.missions.empty() || u.missions.front()->type != BattleUnitMission::Type::Snooze))
	{
		auto &enemies = state.current_battle->visibleEnemies[u.owner];
		auto e1 = u.agent->getFirstItemInSlot(AEquipmentSlotType::RightHand);
		auto e2 = u.agent->getFirstItemInSlot(AEquipmentSlotType::LeftHand);
		// Cannot or forbidden to attack:	Turn to enemy
		if (u.fire_permission_mode == BattleUnit::FirePermissionMode::CeaseFire ||
		    ((!e1 || !e1->canFire()) && (!e2 || !e2->canFire())))
		{
			if (ticksAutoTurnAvailable <= state.gameTime.getTicks() && !u.isMoving())
			{
				// Look at focused unit or find closest enemy
				auto targetEnemy = u.focusUnit;
				auto backupEnemy = targetEnemy;
				if (!targetEnemy || !targetEnemy->isConscious() ||
				    enemies.find(targetEnemy) == enemies.end())
				{
					targetEnemy.clear();
					backupEnemy.clear();
					if (realTime) // In TB having focusUnit means we can only attack him
					{
						float minDistance = FLT_MAX;
						for (auto &enemy : enemies)
						{
							// Do not auto-target harmless things
							if (!enemy->isConscious() || enemy->getAIType() == AIType::None)
							{
								continue;
							}
							if (!u.hasLineToUnit(enemy))
							{
								// Track an enemy we can see but can't fire at,
								// In case we can't fire at anybody
								if (u.visibleUnits.find(enemy) != u.visibleUnits.end())
								{
									backupEnemy = enemy;
								}
								continue;
							}
							auto distance = glm::distance(enemy->position, u.position);
							if (distance < minDistance)
							{
								minDistance = distance;
								targetEnemy = enemy;
							}
						}
					}
				}
				if (!targetEnemy && backupEnemy)
				{
					targetEnemy = backupEnemy;
				}
				if (targetEnemy)
				{
					movement = mksp<AIMovement>();
					movement->type = AIMovement::Type::Turn;
					movement->targetLocation = targetEnemy->position;
					ticksAutoTurnAvailable = state.gameTime.getTicks() + AUTO_TURN_COOLDOWN;
				}
			}
		}
		// Can attack and allowed to:		Attack enemy
		else
		{
			if (ticksAutoTargetAvailable <= state.gameTime.getTicks())
			{
				// Find enemy we can attack amongst those visible
				auto targetEnemy = u.focusUnit;
				auto weaponStatus =
				    targetEnemy ? u.canAttackUnit(state, targetEnemy) : WeaponStatus::NotFiring;
				// Ensure we can see and attack focus, if can't attack focus or have no focus - take
				// closest attackable
				if (!targetEnemy || !targetEnemy->isConscious() ||
				    enemies.find(targetEnemy) == enemies.end() ||
				    weaponStatus == WeaponStatus::NotFiring)
				{
					targetEnemy.clear();
					if (realTime) // In TB having focusUnit means we can only attack him
					{
						// Make a list of enemies sorted by distance to them
						std::map<float, StateRef<BattleUnit>> enemiesByDistance;
						for (auto &enemy : enemies)
						{
							// Do not auto-target harmless things
							if (!enemy->isConscious() || enemy->getAIType() == AIType::None)
							{
								continue;
							}
							// Ensure we add every unit
							auto distance = glm::distance(enemy->position, u.position);
							while (enemiesByDistance.find(distance) != enemiesByDistance.end())
							{
								distance += 0.01f;
							}
							enemiesByDistance[distance] = enemy;
						}
						// Pick enemy that is closest and can be attacked
						for (auto &entry : enemiesByDistance)
						{
							weaponStatus = u.canAttackUnit(state, entry.second);
							if (weaponStatus != WeaponStatus::NotFiring)
							{
								targetEnemy = entry.second;
								break;
							}
						}
					}
				}

				// Attack if we can
				if (targetEnemy)
				{
					action = mksp<AIAction>();
					action->type = AIAction::Type::AttackWeaponUnit;
					action->targetUnit = targetEnemy;
					action->weaponStatus = weaponStatus;
				}
				else
				{
					ticksAutoTargetAvailable = state.gameTime.getTicks() + AUTO_TARGET_COOLDOWN;
				}
			}
		}
	}

	// Enzyme random running (only works in real time)
	if (state.current_battle->mode == Battle::Mode::RealTime && u.enzymeDebuffIntensity > 0 &&
	    !u.isMoving() && u.canMove())
	{
		// Move to a random adjacent tile
		auto from = u.tileObject->getOwningTile();
		auto &map = u.tileObject->map;
		auto helper = BattleUnitTileHelper{map, u};
		std::list<Vec3<int>> possiblePositions;
		for (int x = -1; x <= 1; x++)
		{
			for (int y = -1; y <= 1; y++)
			{
				for (int z = -1; z <= 1; z++)
				{
					if (x == 0 && y == 0 && z == 0)
					{
						continue;
					}
					Vec3<int> newPos = (Vec3<int>)u.position + Vec3<int>(x, y, z);
					if (map.tileIsValid(newPos) && helper.canEnterTile(from, map.getTile(newPos)))
					{
						possiblePositions.push_back(newPos);
					}
				}
			}
		}
		if (!possiblePositions.empty())
		{
			auto newPos = listRandomiser(state.rng, possiblePositions);
			movement = mksp<AIMovement>();
			movement->type = AIMovement::Type::Patrol;
			movement->targetLocation = newPos;
			movement->kneelingMode = u.kneeling_mode;
			movement->movementMode = u.agent->isMovementStateAllowed(MovementState::Running)
			                             ? MovementMode::Running
			                             : MovementMode::Walking;
		}
	}

	attackerPosition = NONE;

	return std::make_tuple(AIDecision(action, movement), action || movement);
}
}