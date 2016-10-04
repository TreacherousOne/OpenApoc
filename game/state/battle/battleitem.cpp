#include "game/state/battle/battleitem.h"
#include "framework/framework.h"
#include "framework/logger.h"
#include "game/state/aequipment.h"
#include "game/state/battle/battle.h"
#include "game/state/gamestate.h"
#include "game/state/tileview/collision.h"
#include "game/state/tileview/tile.h"
#include "game/state/tileview/tileobject_battleitem.h"
#include "game/state/tileview/tileobject_battlemappart.h"
#include "game/state/tileview/tileobject_battleunit.h"
#include "game/state/tileview/tileobject_shadow.h"
#include <cmath>

namespace OpenApoc
{
void BattleItem::die(GameState &, bool violently)
{
	if (violently)
	{
		// FIXME: Explode if nessecary
	}
	auto this_shared = shared_from_this();
	auto b = battle.lock();
	if (b)
		b->items.remove(this_shared);
	this->tileObject->removeFromMap();
	this->shadowObject->removeFromMap();
	this->tileObject.reset();
	this->shadowObject.reset();
}

void BattleItem::handleCollision(GameState &state, Collision &c)
{
	// FIXME: Proper damage
	std::ignore = c;
	die(state, true);
}

void BattleItem::setPosition(const Vec3<float> &pos)
{
	this->position = pos;
	if (!this->tileObject)
	{
		LogError("setPosition called on item with no tile object");
	}
	else
	{
		this->tileObject->setPosition(pos);
	}

	if (!this->shadowObject)
	{
		LogError("setPosition called on item with no shadow object");
	}
	else
	{
		this->shadowObject->setPosition(pos);
	}
}

Collision BattleItem::checkItemCollision(Vec3<float> previousPosition, Vec3<float> nextPosition)
{
	Collision c = tileObject->map.findCollision(previousPosition, nextPosition, {});
	if (c && ownerInvulnerableTicks > 0 && c.obj->getType() == TileObject::Type::Unit &&
	    item->ownerAgent->unit == std::static_pointer_cast<TileObjectBattleUnit>(c.obj)->getUnit())
	{
		return {};
	}
	return c;
}

void BattleItem::update(GameState &state, unsigned int ticks)
{
	if (supported)
	{
		return;
	}

	if (ownerInvulnerableTicks > 0)
	{
		ownerInvulnerableTicks -= ticks;
	}
	int remainingTicks = ticks;

	auto previousPosition = position;
	auto newPosition = position;

	while (remainingTicks-- > 0)
	{
		velocity.z -= FALLING_ACCELERATION_ITEM;
		newPosition += this->velocity / (float)TICK_SCALE / VELOCITY_SCALE_BATTLE;
	}

	// Check if new position is valid
	// FIXME: Collide with units but not with us
	bool collision = false;
	auto c = checkItemCollision(previousPosition, newPosition);
	if (c)
	{
		collision = true;
		// If colliding with anything but ground, bounce back once
		switch (c.obj->getType())
		{
			case TileObject::Type::Unit:
			case TileObject::Type::LeftWall:
			case TileObject::Type::RightWall:
			case TileObject::Type::Feature:
				if (!bounced)
				{
					// If bounced do not try to find support this time
					collision = false;
					bounced = true;
					newPosition = previousPosition;
					velocity.x = -velocity.x / 4;
					velocity.y = -velocity.y / 4;
					velocity.z = std::abs(velocity.z / 4);
				}
				else
				{
					// Let item fall so that it can collide with scenery if falling on top of it
					newPosition = {previousPosition.x, previousPosition.y,
					               std::min(newPosition.z, previousPosition.z)};
				}
				break;
			case TileObject::Type::Ground:
			{
				setPosition({c.position.x, c.position.y, c.position.z});
				if (findSupport(true, true))
				{
					return;
				}
				// Some objects have buggy voxelmaps and items collide with them but no support is
				// given
				// In this case, just ignore the collision and let the item fall further
			}
			break;
			default:
				LogError("What the hell is this item colliding with? Value %d", (int)c.obj->getType());
				break;
		}
	}

	// If moved but did not find support - check if within level bounds and set position
	if (newPosition != previousPosition)
	{
		auto mapSize = this->tileObject->map.size;

		// Collision with ceiling
		if (newPosition.z >= mapSize.z)
		{
			collision = true;
			newPosition.z = mapSize.z - 0.01f;
			velocity = {0.0f, 0.0f, 0.0f};
		}
		// Collision with map edge
		if (newPosition.x < 0 || newPosition.y < 0 || newPosition.y >= mapSize.y ||
		    newPosition.x >= mapSize.x || newPosition.y >= mapSize.y)
		{
			collision = true;
			velocity.x = -velocity.x / 4;
			velocity.y = -velocity.y / 4;
			velocity.z = 0;
			newPosition = previousPosition;
		}
		// Fell below 0???
		if (newPosition.z < 0)
		{
			LogError("Item fell off the end of the world!?");
			die(state, false);
			return;
		}
		setPosition(newPosition);
	}

	if (collision)
	{
		findSupport();
	}
}

bool BattleItem::findSupport(bool emitSound, bool forced)
{
	if (supported)
		return true;
	auto tile = tileObject->getOwningTile();
	auto obj = tile->getItemSupportingObject();
	if (!obj)
	{
		return false;
	}
	auto restingPosition =
	    obj->getPosition() + Vec3<float>{0.0f, 0.0f, (float)obj->type->height / 40.0f};
	if (!forced && position.z > restingPosition.z)
	{
		return false;
	}

	supported = true;
	bounced = false;
	velocity = {0.0f, 0.0f, 0.0f};
	obj->supportedItems.push_back(shared_from_this());
	if (position != restingPosition)
	{
		setPosition(restingPosition);
	}

	// Emit sound
	if (emitSound && tile->objectDropSfx)
	{
		fw().soundBackend->playSample(tile->objectDropSfx, getPosition(), 0.25f);
	}
	return true;
}

} // namespace OpenApoc
