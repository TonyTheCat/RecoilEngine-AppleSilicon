/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <cassert>

#include "BuilderCAI.h"
#include "ExternalAI/EngineOutHandler.h"
#include "Game/GameHelper.h"
#include "Game/SelectedUnitsHandler.h"
#include "Game/GlobalUnsynced.h"
#include "Map/Ground.h"
#include "Map/MapDamage.h"
#include "Sim/Features/Feature.h"
#include "Sim/Features/FeatureDef.h"
#include "Sim/Features/FeatureHandler.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Misc/GroundBlockingObjectMap.h"
#include "Sim/Misc/QuadField.h"
#include "Sim/Misc/Team.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/MoveTypes/MoveType.h"
#include "Sim/Units/UnitDefHandler.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Units/UnitTypes/Builder.h"
#include "Sim/Units/UnitTypes/Building.h"
#include "Sim/Units/UnitTypes/Factory.h"
#include "Sim/Units/CommandAI/BuilderCaches.h"
#include "Sim/Weapons/Weapon.h"
#include "Sim/Weapons/WeaponDef.h"
#include "System/SpringMath.h"
#include "System/StringUtil.h"
#include "System/EventHandler.h"
#include "System/Exceptions.h"
#include "System/Log/ILog.h"
#include "System/creg/STL_Map.h"
#include "Rendering/Models/3DModel.hpp"

#include "System/Misc/TracyDefs.h"

#define LOG_SECTION_BUILDER_SAFETY "BuilderSafety"
LOG_REGISTER_SECTION_GLOBAL(LOG_SECTION_BUILDER_SAFETY)

// Builder self-preservation (gameplay branch, see GAMEPLAY.md). An enemy
// threatens a point within its longest weapon range that can hit the builder,
// plus THREAT_MARGIN. Ranges are clamped to THREAT_RANGE_CAP: long-range
// artillery reaches everyone, and counting all of it would stop every builder
// near the front. A radar dot never seen in LOS counts as armed with
// THREAT_DEFAULT_RANGE. Smart Builders (bar-widgets) uses the same numbers.
static constexpr float THREAT_MARGIN = 100.0f;
static constexpr float THREAT_RANGE_CAP = 700.0f;
static constexpr float THREAT_DEFAULT_RANGE = 500.0f;

// ThreatLogDue keys, in disjoint ranges (-1 = nothing logged yet): a stretch
// of searches that pass over targets logs once, a kept patrol leg once per
// enemy, a patrol turn once per destination (a leg can be two internal FIGHTs)
static constexpr int THREAT_LOG_SKIPPING = -2;

static int ThreatLogKeepLegKey(int enemyID) {
	return -3 - enemyID;
}
static int ThreatLogTurnKey(const float3& dest) {
	return -100000 - (int(dest.x) / 8) * 4096 - int(dest.z) / 8;
}


CR_BIND_DERIVED(CBuilderCAI ,CMobileCAI , )

CR_REG_METADATA(CBuilderCAI , (
	CR_MEMBER(ownerBuilder),
	CR_MEMBER(building),
	CR_MEMBER(range3D),
	CR_IGNORED(build),
	CR_IGNORED(buildOptions),

	CR_MEMBER(cachedRadiusId),
	CR_MEMBER(cachedRadius),

	CR_MEMBER(buildRetries),
	CR_MEMBER(randomCounter),

	CR_MEMBER(lastPC1),
	CR_MEMBER(lastPC2),
	CR_MEMBER(lastPC3),
	CR_IGNORED(lastThreatLogKey),
	CR_POSTLOAD(PostLoad),
	CR_PREALLOC(GetPreallocContainer)
))

static std::string GetUnitDefBuildOptionToolTip(const UnitDef* ud, bool disabled) {
	RECOIL_DETAILED_TRACY_ZONE;
	std::string tooltip;

	if (disabled) {
		tooltip = "\xff\xff\x22\x22" "DISABLED: " "\xff\xff\xff\xff";
	} else {
		tooltip = "Build: ";
	}

	tooltip += (ud->humanName + " - " + ud->tooltip);
	tooltip += ("\nHealth "      + FloatToString(ud->health,      "%.0f"));
	tooltip += ("\nMetal cost "  + FloatToString(ud->cost.metal,  "%.0f"));
	tooltip += ("\nEnergy cost " + FloatToString(ud->cost.energy, "%.0f"));
	tooltip += ("\nBuild time "  + FloatToString(ud->buildTime,   "%.0f"));

	return tooltip;
}


CBuilderCAI::CBuilderCAI():
	CMobileCAI(),
	ownerBuilder(nullptr),
	building(false),
	cachedRadiusId(0),
	cachedRadius(0),
	buildRetries(0),
	randomCounter(0),
	lastPC1(-1),
	lastPC2(-1),
	lastPC3(-1),
	range3D(true),
	lastThreatLogKey(-1)
{}

CBuilderCAI::CBuilderCAI(CUnit* owner):
	CMobileCAI(owner),
	building(false),
	cachedRadiusId(0),
	cachedRadius(0),
	buildRetries(0),
	randomCounter(0),
	lastPC1(-1),
	lastPC2(-1),
	lastPC3(-1),
	range3D(owner->unitDef->buildRange3D),
	lastThreatLogKey(-1)
{
	ownerBuilder = static_cast<CBuilder*>(owner);

	if (owner->unitDef->canRepair) {
		SCommandDescription c;

		c.id   = CMD_REPAIR;
		c.type = CMDTYPE_ICON_UNIT_OR_AREA;

		c.action    = "repair";
		c.name      = "Repair";
		c.tooltip   = c.name + ": Repairs another unit";
		c.mouseicon = c.name;
		possibleCommands.push_back(commandDescriptionCache.GetPtr(std::move(c)));
	}
	else if (owner->unitDef->canAssist) {
		SCommandDescription c;

		c.id   = CMD_REPAIR;
		c.type = CMDTYPE_ICON_UNIT_OR_AREA;

		c.action    = "assist";
		c.name      = "Assist";
		c.tooltip   = c.name + ": Help build something";
		c.mouseicon = c.name;
		possibleCommands.push_back(commandDescriptionCache.GetPtr(std::move(c)));
	}

	if (owner->unitDef->canReclaim) {
		SCommandDescription c;

		c.id   = CMD_RECLAIM;
		c.type = CMDTYPE_ICON_UNIT_FEATURE_OR_AREA;

		c.action    = "reclaim";
		c.name      = "Reclaim";
		c.tooltip   = c.name + ": Sucks in the metal/energy content of a unit/feature\nand adds it to your storage";
		c.mouseicon = c.name;
		possibleCommands.push_back(commandDescriptionCache.GetPtr(std::move(c)));
	}

	if (owner->unitDef->canRestore && !mapDamage->Disabled()) {
		SCommandDescription c;

		c.id   = CMD_RESTORE;
		c.type = CMDTYPE_ICON_AREA;

		c.action    = "restore";
		c.name      = "Restore";
		c.tooltip   = c.name + ": Restores an area of the map to its original height";
		c.mouseicon = c.name;

		possibleCommands.push_back(commandDescriptionCache.GetPtr(std::move(c)));
	}

	if (owner->unitDef->canResurrect) {
		SCommandDescription c;

		c.id   = CMD_RESURRECT;
		c.type = CMDTYPE_ICON_UNIT_FEATURE_OR_AREA;

		c.action    = "resurrect";
		c.name      = "Resurrect";
		c.tooltip   = c.name + ": Resurrects a unit from a feature";
		c.mouseicon = c.name;
		possibleCommands.push_back(commandDescriptionCache.GetPtr(std::move(c)));
	}
	if (owner->unitDef->canCapture) {
		SCommandDescription c;

		c.id   = CMD_CAPTURE;
		c.type = CMDTYPE_ICON_UNIT_OR_AREA;

		c.action    = "capture";
		c.name      = "Capture";
		c.tooltip   = c.name + ": Captures a unit from the enemy";
		c.mouseicon = c.name;
		possibleCommands.push_back(commandDescriptionCache.GetPtr(std::move(c)));
	}

	for (const auto& bi: ownerBuilder->unitDef->buildOptions) {
		const std::string& name = bi.second;
		const UnitDef* ud = unitDefHandler->GetUnitDefByName(name);

		if (ud == nullptr) {
			string errmsg = "MOD ERROR: loading ";
			errmsg += name.c_str();
			errmsg += " for ";
			errmsg += owner->unitDef->name;
			throw content_error(errmsg);
		}

		{
			SCommandDescription c;

			c.id   = -ud->id; //build options are always negative
			c.type = CMDTYPE_ICON_BUILDING;

			c.action    = "buildunit_" + StringToLower(ud->name);
			c.name      = name;
			c.mouseicon = c.name;
			c.tooltip   = GetUnitDefBuildOptionToolTip(ud, c.disabled = (ud->maxThisUnit <= 0));

			buildOptions.insert(c.id);
			possibleCommands.push_back(commandDescriptionCache.GetPtr(std::move(c)));
		}
	}

	unitHandler.AddBuilderCAI(this);
}

CBuilderCAI::~CBuilderCAI()
{
	RECOIL_DETAILED_TRACY_ZONE;
	CBuilderCaches::RemoveUnitFromReclaimers(owner);
	CBuilderCaches::RemoveUnitFromFeatureReclaimers(owner);
	CBuilderCaches::RemoveUnitFromResurrecters(owner);
	unitHandler.RemoveBuilderCAI(this);
}

void CBuilderCAI::PostLoad()
{
	RECOIL_DETAILED_TRACY_ZONE;
	for (const SCommandDescription* cd: possibleCommands) {
		if (cd->id < 0)
			buildOptions.insert(cd->id);
	}
	if (commandQue.empty())
		return;

	ownerBuilder = static_cast<CBuilder*>(owner);

	const Command& c = commandQue.front();

	if (buildOptions.find(c.GetID()) != buildOptions.end()) {
		build.Parse(c);
		build.pos = CGameHelper::Pos2BuildPos(build, true);
	}
}

float CBuilderCAI::GetBuildRange(const float targetRadius) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	return (ownerBuilder->buildDistance + targetRadius);
}

bool CBuilderCAI::IsInBuildRange(const CWorldObject* obj) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	return IsInBuildRange(obj->pos, obj->buildeeRadius);
}

bool CBuilderCAI::IsInBuildRange(const float3& objPos, const float objRadius) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	const float immDistSqr = f3SqDist(owner->pos, objPos);
	const float buildDist = GetBuildRange(objRadius);

	return (immDistSqr <= (buildDist * buildDist));
}



inline bool CBuilderCAI::MoveInBuildRange(const CWorldObject* obj, const bool checkMoveTypeForFailed)
{
	RECOIL_DETAILED_TRACY_ZONE;
	return MoveInBuildRange(obj->pos, obj->buildeeRadius, checkMoveTypeForFailed);
}

bool CBuilderCAI::MoveInBuildRange(const float3& objPos, float objRadius, const bool checkMoveTypeForFailed)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!IsInBuildRange(objPos, objRadius)) {
		// NOTE:
		//   ignore the fail-check if we are an aircraft, movetype code
		//   is unreliable wrt. setting it correctly and causes (landed)
		//   aircraft to discard orders
		const bool checkFailed = (checkMoveTypeForFailed && !owner->unitDef->IsAirUnit());
		// check if the AMoveType::Failed belongs to the same goal position
		const bool haveFailed = (owner->moveType->progressState == AMoveType::Failed && f3SqDist(owner->moveType->goalPos, objPos) > 1.0f);

		if (checkFailed && haveFailed) {
			// don't call SetGoal() it would reset moveType->progressState
			// and so later code couldn't check if the move command failed
			return false;
		}

		// too far away, start a move command
		SetGoal(objPos, owner->pos, GetBuildRange(objRadius) * 0.9f);
		return false;
	}

	if (owner->unitDef->IsAirUnit()) {
		StopMoveAndKeepPointing(objPos, GetBuildRange(objRadius) * 0.9f, false);
	} else {
		StopMoveAndKeepPointing(owner->moveType->goalPos, GetBuildRange(objRadius) * 0.9f, false);
	}

	return true;
}


bool CBuilderCAI::IsBuildPosBlocked(const BuildInfo& bi, const CUnit** nanoFrame) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	CFeature* feature = nullptr;
	CGameHelper::BuildSquareStatus status = CGameHelper::TestUnitBuildSquare(bi, feature, owner->allyteam, true);

	// buildjob is a feature and it is finished already
	if (feature != nullptr && bi.def->isFeature && bi.def->wreckName == feature->def->name)
		return true;

	// open area, reclaimable feature or movable unit
	if (status != CGameHelper::BUILDSQUARE_BLOCKED)
		return false;

	const CSolidObject* s = nullptr;
	const CUnit* u = nullptr;

	const int2 mins = CSolidObject::GetMapPosStatic(bi.pos, bi.GetXSize(), bi.GetZSize());
	const int2 maxs = mins + int2(bi.GetXSize(), bi.GetZSize());
	for (int z = mins.y; z < maxs.y; ++z) {
		for (int x = mins.x; x < maxs.x; ++x) {
			s = groundBlockingObjectMap.GroundBlocked(float3{
				static_cast<float>(x * SQUARE_SIZE),
				0.0f,
				static_cast<float>(z * SQUARE_SIZE) }
			);

			if (s == nullptr)
				continue;

			// just ourselves, does not count
			if (s == owner)
				continue;

			if (u = dynamic_cast<const CUnit*>(s), u == nullptr)
				continue;

			// figure out if object is soft- or hard-blocking
			if (u->beingBuilt) {
				// we can't or don't want assist finishing the nanoframe
				// if a mobile unit blocks the position, wait until it is
				// finished & moved
				if (!ownerBuilder->CanAssistUnit(u, bi.def))
					continue;

				// unfinished nanoframe, assist it
				if (nanoFrame != nullptr && teamHandler.Ally(owner->allyteam, u->allyteam))
					*nanoFrame = u;

				return false; //be greedy here
			}
		}
	}

	// if a *unit* object is not present, then either
	// there is a feature or the terrain is unsuitable
	// (in the former case feature must be reclaimable)
	if (u == nullptr)
		return (feature == nullptr || !feature->def->reclaimable);

	// unit blocks the pos, can it move away?
	return (u->immobile);
}



inline bool CBuilderCAI::OutOfImmobileRange(const Command& cmd) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	// builder can move
	if (owner->unitDef->canmove)
		return false;

	// not an internal object-targeted command
	if (!cmd.IsInternalOrder() || (cmd.GetNumParams() != 1))
		return false;

	const int objID = cmd.GetParam(0);
	const CWorldObject* obj = unitHandler.GetUnit(objID);

	if (obj == nullptr) {
		// features don't move, but maybe the unit was transported?
		obj = featureHandler.GetFeature(objID - unitHandler.MaxUnits());

		if (obj == nullptr)
			return false;
	}

	switch (cmd.GetID()) {
		case CMD_REPAIR:
		case CMD_RECLAIM:
		case CMD_RESURRECT:
		case CMD_CAPTURE: {
			if (!IsInBuildRange(obj))
				return true;

			break;
		}
	}

	return false;
}


float CBuilderCAI::GetBuildOptionRadius(const UnitDef* ud, int cmdId)
{
	RECOIL_DETAILED_TRACY_ZONE;
	float radius = cachedRadius;

	if (cachedRadiusId != cmdId) {
		radius = ud->GetModelRadius();
		cachedRadius = radius;
		cachedRadiusId = cmdId;
	}

	return radius;
}


void CBuilderCAI::CancelRestrictedUnit()
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (owner->team == gu->myTeam) {
		LOG_L(L_WARNING, "%s: Build failed, unit type limit reached", owner->unitDef->humanName.c_str());
		eventHandler.LastMessagePosition(owner->pos);
	}
	StopMoveAndFinishCommand();
}


void CBuilderCAI::GiveCommandReal(const Command& c, bool fromSynced)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!AllowedCommand(c, fromSynced))
		return;

	// don't guard yourself
	if ((c.GetID() == CMD_GUARD) &&
	    (c.GetNumParams() == 1) && ((int)c.GetParam(0) == owner->id)) {
		return;
	}

	// stop building/reclaiming/... if the new command is not queued, i.e. replaces our current activity
	// FIXME should happen just before CMobileCAI::GiveCommandReal? (the new cmd can still be skipped!)
	if ((c.GetID() != CMD_WAIT) && !(c.GetOpts() & SHIFT_KEY)) {
		if (nonQueingCommands.find(c.GetID()) == nonQueingCommands.end()) {
			building = false;
			static_cast<CBuilder*>(owner)->StopBuild();
		}
	}

	if (buildOptions.find(c.GetID()) != buildOptions.end()) {
		if (c.GetNumParams() < 3)
			return;

		BuildInfo bi;
		bi.pos = c.GetPos(0);

		if (c.GetNumParams() == 4)
			bi.buildFacing = abs((int)c.GetParam(3)) % NUM_FACINGS;

		bi.def = unitDefHandler->GetUnitDefByID(-c.GetID());
		bi.pos = CGameHelper::Pos2BuildPos(bi, true);

		// We are a static building, check if the buildcmd is in range
		if (!owner->unitDef->canmove) {
			if (!IsInBuildRange(bi.pos, GetBuildOptionRadius(bi.def, c.GetID())))
				return;
		}

		const CUnit* nanoFrame = nullptr;

		// check if the buildpos is blocked
		if (IsBuildPosBlocked(bi, &nanoFrame))
			return;

		// if it is a nanoframe help to finish it
		if (nanoFrame != nullptr) {
			Command c2(CMD_REPAIR, c.GetOpts() | INTERNAL_ORDER, nanoFrame->id);
			CMobileCAI::GiveCommandReal(c2, fromSynced);
			CMobileCAI::GiveCommandReal(c, fromSynced);
			return;
		}
	} else {
		if (c.GetID() < 0)
			return;
	}

	CMobileCAI::GiveCommandReal(c, fromSynced);
}


void CBuilderCAI::SlowUpdate()
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (gs->paused) // Commands issued may invoke SlowUpdate when paused
		return;

	if (commandQue.empty()) {
		CMobileCAI::SlowUpdate();
		return;
	}

	if (owner->beingBuilt || owner->IsStunned())
		return;

	Command& c = commandQue.front();

	if (OutOfImmobileRange(c)) {
		FinishCommand();
		return;
	}

	if (AbandonThreatenedJob(c))
		return;

	switch (c.GetID()) {
		case CMD_STOP:      { ExecuteStop(c);      return; }
		case CMD_REPAIR:    { ExecuteRepair(c);    return; }
		case CMD_CAPTURE:   { ExecuteCapture(c);   return; }
		case CMD_GUARD:     { ExecuteGuard(c);     return; }
		case CMD_RECLAIM:   { ExecuteReclaim(c);   return; }
		case CMD_RESURRECT: { ExecuteResurrect(c); return; }
		case CMD_PATROL:    { ExecutePatrol(c);    return; }
		case CMD_FIGHT:     { ExecuteFight(c);     return; }
		case CMD_RESTORE:   { ExecuteRestore(c);   return; }
		default: {
			if (c.GetID() < 0) {
				ExecuteBuildCmd(c);
			} else {
				CMobileCAI::SlowUpdate();
			}

			return;
		}
	}
}


void CBuilderCAI::ReclaimFeature(CFeature* f)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!owner->unitDef->canReclaim || !f->def->reclaimable) {
		// FIXME user shouldn't be able to queue buildings on top of features
		// in the first place (in this case).
		StopMoveAndFinishCommand();
	} else {
		commandQue.push_front(Command(CMD_RECLAIM, 0, f->id + unitHandler.MaxUnits()));
		// this assumes that the reclaim command can never return directly
		// without having reclaimed the target
		SlowUpdate();
	}
}


void CBuilderCAI::FinishCommand()
{
	RECOIL_DETAILED_TRACY_ZONE;
	buildRetries = 0;
	CMobileCAI::FinishCommand();
}


void CBuilderCAI::ExecuteStop(Command& c)
{
	RECOIL_DETAILED_TRACY_ZONE;
	building = false;
	ownerBuilder->StopBuild();
	CMobileCAI::ExecuteStop(c);
}


void CBuilderCAI::ExecuteBuildCmd(Command& c)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (buildOptions.find(c.GetID()) == buildOptions.end())
		return;

	if (inCommand == CMD_STOP) {
		BuildInfo bi;

		// note:
		//   need at least 3 parameters or BuildInfo will fail to parse
		//   this usually indicates a malformed command inserted by Lua
		//   (most common with patrolling pseudo-factory hubs)
		if (!bi.Parse(c)) {
			StopMoveAndFinishCommand();
			return;
		}

		#if 1
		// snap build-position to multiples of SQUARE_SIZE
		bi.pos.x = math::floor(c.GetParam(0) / SQUARE_SIZE) * SQUARE_SIZE;
		bi.pos.z = math::floor(c.GetParam(2) / SQUARE_SIZE) * SQUARE_SIZE;
		#endif

		CFeature* f = nullptr;
		CGameHelper::TestUnitBuildSquare(bi, f, owner->allyteam, true);

		if (f != nullptr) {
			if (!bi.def->isFeature || bi.def->wreckName != f->def->name) {
				ReclaimFeature(f);
			} else {
				StopMoveAndFinishCommand();
			}
			return;
		}

		// <build> is never parsed (except in PostLoad) so just copy it
		build = bi;
		inCommand = c.GetID();

		// One-time early check: if repeating the order (or first run), skip immediately if now blocked
		if (IsBuildPosBlocked(build)) {
			StopMoveAndFinishCommand();
			return;
		}
	}

	// guard against dangling non-build commands
	if (inCommand >= CMD_STOP)
		return;

	assert(build.def != nullptr);
	assert(build.def->id == -c.GetID() && build.def->id != 0);

	float objRadius = build.def->buildeeBuildRadius;
	if (objRadius < 0.f) {
		auto* model = build.def->LoadModel();
		objRadius = std::max(0.f, model->radius);
	}

	if (building) {
		// keep moving until 3D distance to buildPos is LEQ our buildDistance
		MoveInBuildRange(build.pos, objRadius);

		if (ownerBuilder->curBuild == nullptr && !ownerBuilder->terraforming) {
			building = false;
			StopMoveAndFinishCommand();
		}

		return;
	}

	// keep moving until 3D distance to buildPos is LEQ our buildDistance
	if (MoveInBuildRange(build.pos = CGameHelper::Pos2BuildPos(build, true), objRadius, true)) {
		if (IsBuildPosBlocked(build)) {
			StopMoveAndFinishCommand();
			return;
		}

		const auto [allow, drop] = eventHandler.AllowUnitCreation(build.def, owner, &build);
		if (!allow) {
			if (drop)
				StopMoveAndFinishCommand();
			return;
		}

		if (teamHandler.Team(owner->team)->AtUnitLimit())
			return;

		CFeature* f = nullptr;

		bool inWaitStance = false;
		bool limitReached = false;

		if (ownerBuilder->StartBuild(build, f, inWaitStance, limitReached) || (++buildRetries > 30)) {
			building = true;
			return;
		}

		// we can't reliably check if the unit-limit has been reached until
		// the builder has reached the construction site, which is somewhat
		// annoying (since greyed-out icons can still be clicked, etc)
		if (limitReached) {
			CancelRestrictedUnit();
			StopMove();
			return;
		}

		if (f != nullptr && (!build.def->isFeature || build.def->wreckName != f->def->name)) {
			inCommand = CMD_STOP;
			ReclaimFeature(f);
			return;
		}

		if (!inWaitStance) {
			const float xhalf = (((build.buildFacing & 1) == 0) ? build.def->xsize : build.def->zsize)* 0.5f * SQUARE_SIZE;
			const float zhalf = (((build.buildFacing & 1) == 1) ? build.def->xsize : build.def->zsize)* 0.5f * SQUARE_SIZE;

			const float3 mins{build.pos.x - xhalf, build.pos.y, build.pos.z - zhalf};
			const float3 maxs{build.pos.x + xhalf, build.pos.y, build.pos.z + zhalf};

			CGameHelper::BuggerOffRectangle(mins, maxs, true, owner->team, nullptr);

			NonMoving();
			return;
		}

		return;
	}

	if (owner->moveType->progressState == AMoveType::Failed) {
		if (++buildRetries > 5) {
			StopMoveAndFinishCommand();
			return;
		}
	}

	// we are on the way to the buildpos, meanwhile it can happen
	// that another builder already finished our buildcmd or blocked
	// the buildpos with another building (skip our buildcmd then)
	if ((++randomCounter % 5) == 0) {
		if (IsBuildPosBlocked(build)) {
			StopMoveAndFinishCommand();
			return;
		}
	}
}


bool CBuilderCAI::TargetInterceptable(const CUnit* unit, float targetSpeed) {
	RECOIL_DETAILED_TRACY_ZONE;
	// if the target is moving away at a higher speed than we can manage, there is little point in chasing it
	const float maxSpeed = owner->moveType->GetMaxSpeed();
	if (targetSpeed <= maxSpeed)
		return true;
	const float3 unitToPos = unit->pos - owner->pos;
	return (unitToPos.dot2D(unit->speed) <= unitToPos.Length2D() * maxSpeed);
}


void CBuilderCAI::ExecuteRepair(Command& c)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// not all builders are repair-capable by default
	if (!owner->unitDef->canRepair)
		return;

	if (c.GetNumParams() == 1 || c.GetNumParams() == 5) {
		// repair unit
		CUnit* unit = unitHandler.GetUnit(c.GetParam(0));

		if (unit == nullptr) {
			StopMoveAndFinishCommand();
			return;
		}

		if (tempOrder && owner->moveState <= MOVESTATE_MANEUVER) {
			// limit how far away we go when not roaming
			if (LinePointDist(commandPos1, commandPos2, unit->pos) > std::max(500.0f, GetBuildRange(unit->buildeeRadius))) {
				StopMoveAndFinishCommand();
				return;
			}
		}

		if (c.GetNumParams() == 5) {
			const float3& pos = c.GetPos(1);
			const float radius = c.GetParam(4) + 100.0f; // do not walk too far outside repair area

			if ((pos - unit->pos).SqLength2D() > radius * radius ||
				(unit->IsMoving() && ((c.IsInternalOrder() && !TargetInterceptable(unit, unit->speed.Length2D())) || ownerBuilder->curBuild == unit)
				&& !IsInBuildRange(unit))) {
				StopMoveAndFinishCommand();
				return;
			}
		}

		// do not consider units under construction irreparable
		// even if they can be repaired
		bool canRepairUnit = true;
		canRepairUnit &= ((unit->beingBuilt) || (unit->unitDef->repairable && (unit->health < unit->maxHealth)));
		canRepairUnit &= ((unit != owner) || owner->unitDef->canSelfRepair);
		canRepairUnit &= (!unit->soloBuilder || (unit->soloBuilder == owner));
		canRepairUnit &= (!c.IsInternalOrder() || (c.GetOpts() & CONTROL_KEY) || !CBuilderCaches::IsUnitBeingReclaimed(unit, owner));
		canRepairUnit &= (UpdateTargetLostTimer(unit->id) != 0);

		if (canRepairUnit) {
			if (MoveInBuildRange(unit))
				ownerBuilder->SetRepairTarget(unit);
		} else {
			StopMoveAndFinishCommand();
		}
	} else if (c.GetNumParams() == 4) {
		// area repair
		const float3 pos = c.GetPos(0);
		const float radius = c.GetParam(3);

		ownerBuilder->StopBuild();
		if (FindRepairTargetAndRepair(pos, radius, c.GetOpts(), false, (c.GetOpts() & META_KEY))) {
			inCommand = CMD_STOP;
			SlowUpdate();
			return;
		}

		if (!(c.GetOpts() & ALT_KEY))
			StopMoveAndFinishCommand();

	} else {
		StopMoveAndFinishCommand();
	}
}


void CBuilderCAI::ExecuteCapture(Command& c)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// not all builders are capture-capable by default
	if (!owner->unitDef->canCapture)
		return;

	if (c.GetNumParams() == 1 || c.GetNumParams() == 5) {
		// capture unit
		CUnit* unit = unitHandler.GetUnit(c.GetParam(0));

		if (unit == nullptr) {
			StopMoveAndFinishCommand();
			return;
		}

		if (c.GetNumParams() == 5) {
			const float3& pos = c.GetPos(1);
			const float radius = c.GetParam(4) + 100.0f; // do not walk too far outside capture area

			if (((pos - unit->pos).SqLength2D() > (radius * radius) ||
				(ownerBuilder->curCapture == unit && unit->IsMoving() && !IsInBuildRange(unit)))) {
				StopMoveAndFinishCommand();
				return;
			}
		}

		if (unit->unitDef->capturable && unit->team != owner->team && UpdateTargetLostTimer(unit->id)) {
			if (MoveInBuildRange(unit)) {
				ownerBuilder->SetCaptureTarget(unit);
			}
		} else {
			StopMoveAndFinishCommand();
		}
	} else if (c.GetNumParams() == 4) {
		// area capture
		const float3 pos = c.GetPos(0);
		const float radius = c.GetParam(3);

		ownerBuilder->StopBuild();

		if (FindCaptureTargetAndCapture(pos, radius, c.GetOpts(), (c.GetOpts() & META_KEY))) {
			inCommand = CMD_STOP;
			SlowUpdate();
			return;
		}

		if (!(c.GetOpts() & ALT_KEY))
			StopMoveAndFinishCommand();

	} else {
		StopMoveAndFinishCommand();
	}
}


void CBuilderCAI::ExecuteGuard(Command& c)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!owner->unitDef->canGuard)
		return;

	CUnit* guardee = unitHandler.GetUnit(c.GetParam(0));

	if (guardee == nullptr) {
		StopMoveAndFinishCommand();
		return;
	}
	if (guardee == owner) {
		StopMoveAndFinishCommand();
		return;
	}
	if (UpdateTargetLostTimer(guardee->id) == 0) {
		StopMoveAndFinishCommand();
		return;
	}


	if (CBuilder* b = dynamic_cast<CBuilder*>(guardee)) {
		if (b->terraforming) {
			if (MoveInBuildRange(b->terraformCenter, b->terraformRadius * 0.7f)) {
				ownerBuilder->HelpTerraform(b);
			} else {
				StopSlowGuard();
			}
			return;
		} else if (b->curReclaim && !b->curReclaim->detached && owner->unitDef->canReclaim) {
			StopSlowGuard();
			if (!ReclaimObject(b->curReclaim)) {
				StopMove();
			}
			return;
		} else if (b->curResurrect && !b->curResurrect->detached && owner->unitDef->canResurrect) {
			StopSlowGuard();
			if (!ResurrectObject(b->curResurrect)) {
				StopMove();
			}
			return;
		} else {
			ownerBuilder->StopBuild();
		}

		const bool pushRepairCommand =
			(  b->curBuild != nullptr) &&
			(  b->curBuild->soloBuilder == nullptr || b->curBuild->soloBuilder == owner) &&
			(( b->curBuild->beingBuilt && owner->unitDef->canAssist) ||
			( !b->curBuild->beingBuilt && owner->unitDef->canRepair));

		if (pushRepairCommand) {
			StopSlowGuard();

			Command nc(CMD_REPAIR, c.GetOpts(), b->curBuild->id);

			commandQue.push_front(nc);
			inCommand = CMD_STOP;
			SlowUpdate();
			return;
		}
	}

	if (CFactory* fac = dynamic_cast<CFactory*>(guardee)) {
		const bool pushRepairCommand =
			(  fac->curBuild != nullptr) &&
			(  fac->curBuild->soloBuilder == nullptr || fac->curBuild->soloBuilder == owner) &&
			(( fac->curBuild->beingBuilt && owner->unitDef->canAssist) ||
			 (!fac->curBuild->beingBuilt && owner->unitDef->canRepair));

		if (pushRepairCommand) {
			StopSlowGuard();

			commandQue.push_front(Command(CMD_REPAIR, c.GetOpts(), fac->curBuild->id));
			inCommand = CMD_STOP;
			// SlowUpdate();
			return;
		}
	}

	if (!(c.GetOpts() & CONTROL_KEY) && CBuilderCaches::IsUnitBeingReclaimed(guardee, owner))
		return;

	const float3 pos    = guardee->pos;
	const float  radius = (guardee->immobile) ? guardee->buildeeRadius : guardee->buildeeRadius * 0.8f; // in case of mobile units reduce radius a bit

	if (MoveInBuildRange(pos, radius)) {
		StartSlowGuard(guardee->moveType->GetMaxSpeed());

		const bool pushRepairCommand =
			(  guardee->health < guardee->maxHealth) &&
			(  guardee->soloBuilder == nullptr || guardee->soloBuilder == owner) &&
			(( guardee->beingBuilt && owner->unitDef->canAssist) ||
			 (!guardee->beingBuilt && owner->unitDef->canRepair));

		if (pushRepairCommand) {
			StopSlowGuard();

			commandQue.push_front(Command(CMD_REPAIR, c.GetOpts(), guardee->id));
			inCommand = CMD_STOP;
			return;
		}

		NonMoving();
	} else {
		StopSlowGuard();
	}
}


void CBuilderCAI::ExecuteReclaim(Command& c)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// not all builders are reclaim-capable by default
	if (!owner->unitDef->canReclaim)
		return;

	if (c.GetNumParams() == 1 || c.GetNumParams() == 5) {
		const int signedId = (int) c.GetParam(0);

		if (signedId < 0) {
			LOG_L(L_WARNING, "Trying to reclaim unit or feature with id < 0 (%i), aborting.", signedId);
			return;
		}

		const unsigned int uid = signedId;

		const bool checkForBetterTarget = ((++randomCounter % 5) == 0);
		if (checkForBetterTarget && c.IsInternalOrder() && (c.GetNumParams() >= 5)) {
			// regular check if there is a closer reclaim target
			CSolidObject* obj;

			if (uid >= unitHandler.MaxUnits()) {
				obj = featureHandler.GetFeature(uid - unitHandler.MaxUnits());
			} else {
				obj = unitHandler.GetUnit(uid);
			}

			if (obj) {
				const float3& pos = c.GetPos(1);
				const float radius = c.GetParam(4);
				const float curdist = pos.SqDistance2D(obj->pos);

				const bool recUnits = !!(c.GetOpts() & META_KEY);
				const bool recEnemyOnly = (c.GetOpts() & META_KEY) && (c.GetOpts() & CONTROL_KEY);
				const bool recSpecial = !!(c.GetOpts() & CONTROL_KEY);

				ReclaimOption recopt = REC_NORESCHECK;
				if (recUnits)     recopt |= REC_UNITS;
				if (recEnemyOnly) recopt |= REC_ENEMYONLY;
				if (recSpecial)   recopt |= REC_SPECIAL;

				// a fight/patrol job does not switch to a closer target under fire
				Threats threats;
				Threats* jobThreats = nullptr;
				if (tempOrder && !owner->immobile) {
					threats.patrol = (NextPatrolPoint(1) != nullptr);
					CollectThreats(pos, radius + owner->pos.distance2D(pos), threats);
					jobThreats = &threats;
				}

				const int rid = FindReclaimTarget(pos, radius, c.GetOpts(), recopt, curdist, jobThreats);
				if ((rid > 0) && (rid != uid)) {
					StopMoveAndFinishCommand();
					CBuilderCaches::RemoveUnitFromReclaimers(owner);
					CBuilderCaches::RemoveUnitFromFeatureReclaimers(owner);
					return;
				}
			}
		}

		if (uid >= unitHandler.MaxUnits()) { // reclaim feature
			CFeature* feature = featureHandler.GetFeature(uid - unitHandler.MaxUnits());

			if (feature != nullptr) {
				bool featureBeingResurrected = CBuilderCaches::IsFeatureBeingResurrected(feature->id, owner);
				featureBeingResurrected &= c.IsInternalOrder();

				if (featureBeingResurrected || !ReclaimObject(feature)) {
					StopMoveAndFinishCommand();
					CBuilderCaches::RemoveUnitFromFeatureReclaimers(owner);
				} else {
					CBuilderCaches::AddUnitToFeatureReclaimers(owner);
				}
			} else {
				StopMoveAndFinishCommand();
				CBuilderCaches::RemoveUnitFromFeatureReclaimers(owner);
			}

			CBuilderCaches::RemoveUnitFromReclaimers(owner);
		} else { // reclaim unit
			CUnit* unit = unitHandler.GetUnit(uid);

			if (unit != nullptr && c.GetNumParams() == 5) {
				const float3& pos = c.GetPos(1);
				const float radius = c.GetParam(4) + 100.0f; // do not walk too far outside reclaim area

				const bool outOfReclaimRange =
					(pos.SqDistance2D(unit->pos) > radius * radius) ||
					(ownerBuilder->curReclaim == unit && unit->IsMoving() && !IsInBuildRange(unit));
				const bool busyAlliedBuilder =
					unit->unitDef->builder &&
					!unit->commandAI->commandQue.empty() &&
					teamHandler.Ally(owner->allyteam, unit->allyteam);

				if (outOfReclaimRange || busyAlliedBuilder) {
					StopMoveAndFinishCommand();
					CBuilderCaches::RemoveUnitFromReclaimers(owner);
					CBuilderCaches::RemoveUnitFromFeatureReclaimers(owner);
					return;
				}
			}

			if (unit != nullptr && unit != owner && unit->unitDef->reclaimable && UpdateTargetLostTimer(unit->id) && unit->AllowedReclaim(owner)) {
				if (!ReclaimObject(unit)) {
					StopMoveAndFinishCommand();
				} else {
					CBuilderCaches::AddUnitToReclaimers(owner);
				}
			} else {
				CBuilderCaches::RemoveUnitFromReclaimers(owner);
				StopMoveAndFinishCommand();
			}

			CBuilderCaches::RemoveUnitFromFeatureReclaimers(owner);
		}
	} else if (c.GetNumParams() == 4) {
		// area reclaim
		const float3 pos = c.GetPos(0);
		const float radius = c.GetParam(3);
		const bool recUnits = !!(c.GetOpts() & META_KEY);
		const bool recEnemyOnly = (c.GetOpts() & META_KEY) && (c.GetOpts() & CONTROL_KEY);
		const bool recSpecial = !!(c.GetOpts() & CONTROL_KEY);

		CBuilderCaches::RemoveUnitFromReclaimers(owner);
		CBuilderCaches::RemoveUnitFromFeatureReclaimers(owner);
		ownerBuilder->StopBuild();

		ReclaimOption recopt = REC_NORESCHECK;
		if (recUnits)     recopt |= REC_UNITS;
		if (recEnemyOnly) recopt |= REC_ENEMYONLY;
		if (recSpecial)   recopt |= REC_SPECIAL;

		if (FindReclaimTargetAndReclaim(pos, radius, c.GetOpts(), recopt)) {
			inCommand = CMD_STOP;
			SlowUpdate();
			return;
		}

		if (!(c.GetOpts() & ALT_KEY))
			StopMoveAndFinishCommand();

	} else {
		// wrong number of parameters
		CBuilderCaches::RemoveUnitFromReclaimers(owner);
		CBuilderCaches::RemoveUnitFromFeatureReclaimers(owner);
		StopMoveAndFinishCommand();
	}
}


bool CBuilderCAI::ResurrectObject(CFeature *feature) {
	RECOIL_DETAILED_TRACY_ZONE;
	if (MoveInBuildRange(feature, true)) {
		ownerBuilder->SetResurrectTarget(feature);
		return true;
	}

	return (owner->moveType->progressState != AMoveType::Failed);
}


void CBuilderCAI::ExecuteResurrect(Command& c)
{
	RECOIL_DETAILED_TRACY_ZONE;
	// not all builders are resurrect-capable by default
	if (!owner->unitDef->canResurrect)
		return;

	if (c.GetNumParams() == 1 || c.GetNumParams() == 5) {
		const unsigned int id = (unsigned int) c.GetParam(0);

		if (id >= unitHandler.MaxUnits()) { // resurrect feature
			CFeature* feature = featureHandler.GetFeature(id - unitHandler.MaxUnits());

			if (feature && feature->udef != nullptr) {
				if ((c.IsInternalOrder() && !(c.GetOpts() & CONTROL_KEY) && CBuilderCaches::IsFeatureBeingReclaimed(feature->id, owner)) ||
					!ResurrectObject(feature)) {
					CBuilderCaches::RemoveUnitFromResurrecters(owner);
					StopMoveAndFinishCommand();
				}
				else {
					CBuilderCaches::AddUnitToResurrecters(owner);
				}
			} else {
				CBuilderCaches::RemoveUnitFromResurrecters(owner);

				if (ownerBuilder->lastResurrected && unitHandler.GetUnitUnsafe(ownerBuilder->lastResurrected) != nullptr && owner->unitDef->canRepair) {
					// resurrection finished, start repair (by overwriting the current order)
					if (c.GetNumParams() == 5) {
						float3 pos = c.GetPos(1);
						float radius = c.GetParam(4);
						c = Command(CMD_REPAIR, c.GetOpts() | INTERNAL_ORDER, ownerBuilder->lastResurrected, pos);
						c.PushParam(radius);
					} else {
						c = Command(CMD_REPAIR, c.GetOpts() | INTERNAL_ORDER, ownerBuilder->lastResurrected);
					}
					ownerBuilder->lastResurrected = 0;
					inCommand = CMD_STOP;
					SlowUpdate();
					return;
				}

				StopMoveAndFinishCommand();
			}
		} else { // resurrect unit
			CBuilderCaches::RemoveUnitFromResurrecters(owner);
			StopMoveAndFinishCommand();
		}
	} else if (c.GetNumParams() == 4) {
		// area resurrect
		const float3 pos = c.GetPos(0);
		const float radius = c.GetParam(3);

		if (FindResurrectableFeatureAndResurrect(pos, radius, c.GetOpts(), (c.GetOpts() & META_KEY))) {
			inCommand = CMD_STOP;
			SlowUpdate();
			return;
		}

		if (!(c.GetOpts() & ALT_KEY))
			StopMoveAndFinishCommand();

	} else {
		// wrong number of parameters
		CBuilderCaches::RemoveUnitFromResurrecters(owner);
		StopMoveAndFinishCommand();
	}
}


void CBuilderCAI::ExecutePatrol(Command& c)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!owner->unitDef->canPatrol)
		return;

	if (c.GetNumParams() < 3)
		return;

	Command temp(CMD_FIGHT, c.GetOpts() | INTERNAL_ORDER, c.GetPos(0));

	commandQue.push_back(c);
	commandQue.pop_front();
	commandQue.push_front(temp);
	Command tmpC(CMD_PATROL);
	eoh->CommandFinished(*owner, tmpC);
	SlowUpdate();
}


void CBuilderCAI::ExecuteFight(Command& c)
{
	RECOIL_DETAILED_TRACY_ZONE;
	assert(c.IsInternalOrder() || owner->unitDef->canFight);

	if (tempOrder) {
		tempOrder = false;
		inCommand = CMD_FIGHT;
	}
	if (c.GetNumParams() < 3) {
		LOG_L(L_ERROR, "[BuilderCAI::%s][f=%d][id=%d][#c.params=%d min=3]", __func__, gs->frameNum, owner->id, c.GetNumParams());
		return;
	}

	if (c.GetNumParams() >= 6) {
		if (inCommand == CMD_STOP)
			commandPos1 = c.GetPos(3);

	} else {
		// Some hackery to make sure the line (commandPos1,commandPos2) is NOT
		// rotated (only shortened) if we reach this because the previous return
		// fight command finished by the 'if((curPos-pos).SqLength2D()<(64*64)){'
		// condition, but is actually updated correctly if you click somewhere
		// outside the area close to the line (for a new command).
		if (f3SqDist(owner->pos, commandPos1 = ClosestPointOnLine(commandPos1, commandPos2, owner->pos)) > Square(96.0f))
			commandPos1 = owner->pos;
	}

	float3 pos = c.GetPos(0);
	if (inCommand == CMD_STOP) {
		inCommand = CMD_FIGHT;
		commandPos2 = pos;
	}

	float3 curPosOnLine = ClosestPointOnLine(commandPos1, commandPos2, owner->pos);

	if (c.GetNumParams() >= 6)
		pos = curPosOnLine;

	if (pos != owner->moveType->goalPos)
		SetGoal(pos, owner->pos);

	const UnitDef* ownerDef = owner->unitDef;

	const bool resurrectMode = !!(c.GetOpts() & ALT_KEY);
	const bool reclaimEnemyMode = !!(c.GetOpts() & META_KEY);
	const bool reclaimEnemyOnlyMode = (c.GetOpts() & CONTROL_KEY) && (c.GetOpts() & META_KEY);

	ReclaimOption recopt;
	if (resurrectMode       ) recopt |= REC_NONREZ;
	if (reclaimEnemyMode    ) recopt |= REC_ENEMY;
	if (reclaimEnemyOnlyMode) recopt |= REC_ENEMYONLY;

	const float searchRadius = (owner->immobile ? 0.0f : (300.0f * owner->moveState)) + ownerBuilder->buildDistance;

	// self-preservation: a builder that can walk away does (turrets cannot)
	Threats threats;
	Threats* searchThreats = nullptr;
	if (!owner->immobile) {
		threats.patrol = (c.IsInternalOrder() && NextPatrolPoint(0) != nullptr);
		CollectThreats(curPosOnLine, searchRadius + owner->pos.distance2D(curPosOnLine), threats);
		searchThreats = &threats;

		if (EndThreatenedPatrolLeg(c, threats))
			return;
	}

	// Priority 1: Repair
	if (!reclaimEnemyOnlyMode && (ownerDef->canRepair || ownerDef->canAssist) && FindRepairTargetAndRepair(curPosOnLine, searchRadius, c.GetOpts(), true, resurrectMode, searchThreats)){
		LogThreatSkips(threats);
		tempOrder = true;
		inCommand = CMD_STOP;

		if (lastPC1 != gs->frameNum) {  //avoid infinite loops
			lastPC1 = gs->frameNum;
			SlowUpdate();
		}

		return;
	}

	// Priority 2: Resurrect (optional)
	if (!reclaimEnemyOnlyMode && resurrectMode && ownerDef->canResurrect && FindResurrectableFeatureAndResurrect(curPosOnLine, searchRadius, c.GetOpts(), false, searchThreats)) {
		LogThreatSkips(threats);
		tempOrder = true;
		inCommand = CMD_STOP;

		if (lastPC2 != gs->frameNum) {  //avoid infinite loops
			lastPC2 = gs->frameNum;
			SlowUpdate();
		}

		return;
	}

	// Priority 3: Reclaim / reclaim non resurrectable (optional) / reclaim enemy units (optional)
	if (ownerDef->canReclaim && FindReclaimTargetAndReclaim(curPosOnLine, searchRadius, c.GetOpts(), recopt, searchThreats)) {
		LogThreatSkips(threats);
		tempOrder = true;
		inCommand = CMD_STOP;

		if (lastPC3 != gs->frameNum) {  //avoid infinite loops
			lastPC3 = gs->frameNum;
			SlowUpdate();
		}

		return;
	}

	LogThreatSkips(threats);

	if (f3SqDist(owner->pos, pos) < Square(64.0f)) {
		StopMoveAndFinishCommand();
		return;
	}

	if (owner->HaveTarget() && owner->moveType->progressState != AMoveType::Done) {
		StopMove();
	} else {
		SetGoal(owner->moveType->goalPos, owner->pos);
	}
}


void CBuilderCAI::ExecuteRestore(Command& c)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!owner->unitDef->canRestore)
		return;

	if (inCommand == CMD_RESTORE) {
		if (!ownerBuilder->terraforming)
			StopMoveAndFinishCommand();

		return;
	}

	if (owner->unitDef->canRestore) {
		const float3 pos(c.GetParam(0), CGround::GetHeightReal(c.GetParam(0), c.GetParam(2)), c.GetParam(2));
		const float radius = std::min(c.GetParam(3), 200.0f);

		if (MoveInBuildRange(pos, radius * 0.7f)) {
			ownerBuilder->StartRestore(pos, radius);
			inCommand = CMD_RESTORE;
		}
	}
}


int CBuilderCAI::GetDefaultCmd(const CUnit* pointed, const CFeature* feature)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (pointed != nullptr) {
		if (!teamHandler.Ally(gu->myAllyTeam, pointed->allyteam)) {
			if (owner->unitDef->canAttack && (owner->maxRange > 0.0f))
				return CMD_ATTACK;

			if (owner->unitDef->canReclaim && pointed->unitDef->reclaimable)
				return CMD_RECLAIM;
		} else {
			const bool canAssistPointed = ownerBuilder->CanAssistUnit(pointed);
			const bool canRepairPointed = ownerBuilder->CanRepairUnit(pointed);

			if (canAssistPointed)
				return CMD_REPAIR;
			if (canRepairPointed)
				return CMD_REPAIR;

			if (pointed->CanTransport(owner))
				return CMD_LOAD_ONTO;
			if (owner->unitDef->canGuard)
				return CMD_GUARD;
		}
	}

	if (feature != nullptr) {
		if (owner->unitDef->canResurrect && feature->udef != nullptr)
			return CMD_RESURRECT;

		if (owner->unitDef->canReclaim && feature->def->reclaimable)
			return CMD_RECLAIM;
	}

	return CMD_MOVE;
}


bool CBuilderCAI::ReclaimObject(CSolidObject* object) {
	RECOIL_DETAILED_TRACY_ZONE;
	if (MoveInBuildRange(object)) {
		ownerBuilder->SetReclaimTarget(object);
		return true;
	}

	return (owner->moveType->progressState != AMoveType::Failed);
}


int CBuilderCAI::FindReclaimTarget(const float3& pos, float radius, unsigned char cmdopt, ReclaimOption recoptions, float bestStartDist, Threats* threats) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	const bool noResCheck   = recoptions & REC_NORESCHECK;
	const bool recUnits     = recoptions & REC_UNITS;
	const bool recNonRez    = recoptions & REC_NONREZ;
	const bool recEnemy     = recoptions & REC_ENEMY;
	const bool recEnemyOnly = recoptions & REC_ENEMYONLY;
	const bool recSpecial   = recoptions & REC_SPECIAL;

	const CSolidObject* best = nullptr;
	float bestDist = bestStartDist;
	bool stationary = false;
	int rid = -1;

	if (recUnits || recEnemy || recEnemyOnly) {
		QuadFieldQuery qfQuery;
		quadField.GetUnitsExact(qfQuery, pos, radius, false);

		for (const CUnit* u: *qfQuery.units) {
			if (u == owner)
				continue;
			if (!u->unitDef->reclaimable)
				continue;
			if (!((!recEnemy && !recEnemyOnly) || !teamHandler.Ally(owner->allyteam, u->allyteam)))
				continue;
			if (!(u->losStatus[owner->allyteam] & (LOS_INRADAR|LOS_INLOS)))
				continue;

			// reclaim stationary targets first
			if (u->IsMoving() && stationary)
				continue;

			// do not reclaim friendly builders that are busy
			if (u->unitDef->builder && teamHandler.Ally(owner->allyteam, u->allyteam) && !u->commandAI->commandQue.empty())
				continue;

			const float dist = f3SqDist(u->pos, owner->pos);
			if (dist < bestDist || (!stationary && !u->IsMoving())) {
				if (owner->immobile && !IsInBuildRange(u))
					continue;

				if (SkipThreatenedTarget(u, u->id, threats))
					continue;

				if (!stationary && !u->IsMoving())
					stationary = true;

				bestDist = dist;
				best = u;
			}
		}
		if (best != nullptr)
			rid = best->id;
	}

	if ((!best || !stationary) && !recEnemyOnly) {
		best = nullptr;
		const CTeam* team = teamHandler.Team(owner->team);
		QuadFieldQuery qfQuery;
		quadField.GetFeaturesExact(qfQuery, pos, radius, false);
		bool metal = false;

		for (const CFeature* f: *qfQuery.features) {
			if (!f->def->reclaimable)
				continue;
			if (!recSpecial && !f->def->autoreclaim)
				continue;

			if (recNonRez && f->udef != nullptr)
				continue;

			if (recSpecial && metal && f->defResources.metal <= 0.0)
				continue;

			const float dist = f3SqDist(f->pos, owner->pos);

			if ((dist < bestDist || (recSpecial && !metal && f->defResources.metal > 0.0)) &&
				(noResCheck ||
				((f->defResources.metal  > 0.0f) && (team->res.metal  < team->resStorage.metal)) ||
				((f->defResources.energy > 0.0f) && (team->res.energy < team->resStorage.energy)))
			) {
				if (!f->IsInLosForAllyTeam(owner->allyteam))
					continue;

				if (!owner->unitDef->canmove && !IsInBuildRange(f))
					continue;

				if (CBuilderCaches::IsFeatureBeingResurrected(f->id, owner))
					continue;

				if (SkipThreatenedTarget(f, unitHandler.MaxUnits() + f->id, threats))
					continue;

				metal |= (recSpecial && !metal && f->defResources.metal > 0.0f);

				bestDist = dist;
				best = f;
			}
		}

		if (best != nullptr)
			rid = unitHandler.MaxUnits() + best->id;
	}

	return rid;
}


/******************************************************************************/
//
//  Area searches
//

bool CBuilderCAI::FindReclaimTargetAndReclaim(const float3& pos, float radius, unsigned char cmdopt, ReclaimOption recoptions, Threats* threats)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const int rid = FindReclaimTarget(pos, radius, cmdopt, recoptions, 1.0e30f, threats);

	if (rid < 0)
		return false;

	// FIGHT commands always resource check
	if (!(recoptions & REC_NORESCHECK))
		PushOrUpdateReturnFight();

	Command c(CMD_RECLAIM, cmdopt | INTERNAL_ORDER, rid, pos);
	c.PushParam(radius);
	commandQue.push_front(c);
	return true;
}


bool CBuilderCAI::FindResurrectableFeatureAndResurrect(
	const float3& pos,
	float radius,
	unsigned char options,
	bool freshOnly,
	Threats* threats
) {
	RECOIL_DETAILED_TRACY_ZONE;
	QuadFieldQuery qfQuery;
	quadField.GetFeaturesExact(qfQuery, pos, radius, false);

	const CFeature* best = nullptr;
	float bestDist = 1.0e30f;

	for (const CFeature* f: *qfQuery.features) {
		if (f->udef == nullptr)
			continue;

		if (!f->IsInLosForAllyTeam(owner->allyteam))
			continue;

		if (freshOnly && f->reclaimLeft < 1.0f)
			continue;

		const float dist = f3SqDist(f->pos, owner->pos);
		if (dist < bestDist) {
			// dont lock-on to units outside of our reach (for immobile builders)
			if (owner->immobile && !IsInBuildRange(f))
				continue;

			if (!(options & CONTROL_KEY) && CBuilderCaches::IsFeatureBeingReclaimed(f->id, owner))
				continue;

			if (SkipThreatenedTarget(f, unitHandler.MaxUnits() + f->id, threats))
				continue;

			bestDist = dist;
			best = f;
		}
	}

	if (best != nullptr) {
		Command c(CMD_RESURRECT, options | INTERNAL_ORDER, unitHandler.MaxUnits() + best->id, pos);
		c.PushParam(radius);
		commandQue.push_front(c);
		return true;
	}

	return false;
}


bool CBuilderCAI::FindCaptureTargetAndCapture(
	const float3& pos,
	float radius,
	unsigned char options,
	bool healthyOnly
) {
	RECOIL_DETAILED_TRACY_ZONE;
	QuadFieldQuery qfQuery;
	quadField.GetUnitsExact(qfQuery, pos, radius, false);

	const CUnit* best = nullptr;
	float bestDist = 1.0e30f;
	bool stationary = false;

	const bool ctrlOpt = (options & CONTROL_KEY);

	for (const CUnit* unit: *qfQuery.units) {
		const bool isAlliedUnit = teamHandler.Ally(owner->allyteam, unit->allyteam);
		const bool isVisibleUnit = (unit->losStatus[owner->allyteam] & (LOS_INRADAR | LOS_INLOS));
		const bool isCapturableUnit = !unit->beingBuilt && unit->unitDef->capturable;

		const bool isEnemyTarget = ((ctrlOpt && owner->team != unit->team) || !isAlliedUnit);
		const bool isValidTarget = ((unit != owner) && isVisibleUnit && isCapturableUnit);

		if (isEnemyTarget && isValidTarget) {
			// capture stationary targets first
			if (unit->IsMoving() && stationary)
				continue;

			if (healthyOnly && unit->health < unit->maxHealth && unit->captureProgress <= 0.0f)
				continue;

			const float dist = f3SqDist(unit->pos, owner->pos);

			if (dist < bestDist || (!stationary && !unit->IsMoving())) {
				if (!owner->unitDef->canmove && !IsInBuildRange(unit))
					continue;

				stationary |= (!stationary && !unit->IsMoving());

				bestDist = dist;
				best = unit;
			}
		}
	}

	if (best != nullptr) {
		commandQue.push_front(Command(CMD_CAPTURE, options | INTERNAL_ORDER, best->id));
		return true;
	}

	return false;
}


bool CBuilderCAI::FindRepairTargetAndRepair(
	const float3& pos,
	float radius,
	unsigned char options,
	bool attackEnemy,
	bool builtOnly,
	Threats* threats
) {
	RECOIL_DETAILED_TRACY_ZONE;
	QuadFieldQuery qfQuery;
	quadField.GetUnitsExact(qfQuery, pos, radius, false);
	const CUnit* bestUnit = nullptr;

	const float maxSpeed = owner->moveType->GetMaxSpeed();
	float unitSpeed = 0.0f;
	float bestDist = 1.0e30f;

	bool haveEnemy = false;
	bool trySelfRepair = false;
	bool stationary = false;

	for (const CUnit* unit: *qfQuery.units) {
		if (teamHandler.Ally(owner->allyteam, unit->allyteam)) {
			if (!haveEnemy && (unit->health < unit->maxHealth)) {
				// don't help allies build unless set on roam
				if (unit->beingBuilt && owner->team != unit->team && (owner->moveState != MOVESTATE_ROAM))
					continue;

				// don't help factories produce units when set on hold pos
				if (unit->beingBuilt && unit->moveDef != nullptr && (owner->moveState == MOVESTATE_HOLDPOS))
					continue;

				// don't assist or repair if can't assist or repair
				if (!ownerBuilder->CanAssistUnit(unit) && !ownerBuilder->CanRepairUnit(unit))
					continue;

				if (unit == owner) {
					trySelfRepair = true;
					continue;
				}
				// repair stationary targets first
				if (unit->IsMoving() && stationary)
					continue;

				if (builtOnly && unit->beingBuilt)
					continue;

				float dist = f3SqDist(unit->pos, owner->pos);

				// avoid targets that are faster than our max speed
				if (unit->IsMoving()) {
					unitSpeed = unit->speed.Length2D();
					dist *= (1.0f + std::max(unitSpeed - maxSpeed, 0.0f));
				}
				if (dist < bestDist || (!stationary && !unit->IsMoving())) {
					// dont lock-on to units outside of our reach (for immobile builders)
					if ((owner->immobile || (unit->IsMoving() && !TargetInterceptable(unit, unitSpeed))) && !IsInBuildRange(unit))
						continue;

					// don't repair stuff that's being reclaimed
					if (!(options & CONTROL_KEY) && CBuilderCaches::IsUnitBeingReclaimed(unit, owner))
						continue;

					if (SkipThreatenedTarget(unit, unit->id, threats))
						continue;

					stationary |= (!stationary && !unit->IsMoving());

					bestDist = dist;
					bestUnit = unit;
				}
			}
		} else {
			if (unit->IsNeutral())
				continue;

			if (!attackEnemy || !owner->unitDef->canAttack || (owner->maxRange <= 0) )
				continue;

			if (!(unit->losStatus[owner->allyteam] & (LOS_INRADAR | LOS_INLOS)))
				continue;

			const float dist = f3SqDist(unit->pos, owner->pos);

			if ((dist < bestDist) || !haveEnemy) {
				if (owner->immobile && ((dist - unit->buildeeRadius) > owner->maxRange))
					continue;

				bestUnit = unit;
				bestDist = dist;
				haveEnemy = true;
			}
		}
	}

	if (bestUnit == nullptr) {
		if (!trySelfRepair || !owner->unitDef->canSelfRepair || (owner->health >= owner->maxHealth))
			return false;

		bestUnit = owner;
	}

	if (!haveEnemy) {
		if (attackEnemy)
			PushOrUpdateReturnFight();

		Command c(CMD_REPAIR, options | INTERNAL_ORDER, bestUnit->id, pos);
		c.PushParam(radius);
		commandQue.push_front(c);
	} else {
		PushOrUpdateReturnFight(); // attackEnemy must be true
		commandQue.push_front(Command(CMD_ATTACK, options | INTERNAL_ORDER, bestUnit->id));
	}

	return true;
}



/******************************************************************************/
//
//  Self-preservation (gameplay branch)
//

const CBuilderCAI::Threats::Threat* CBuilderCAI::Threats::Covering(const float3& pos) const
{
	for (const Threat& t: list) {
		if (pos.SqDistance2D(t.pos) < Square(t.range + THREAT_MARGIN))
			return &t;
	}

	return nullptr;
}


void CBuilderCAI::CollectThreats(const float3& center, float radius, Threats& threats) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	threats.list.clear();

	QuadFieldQuery qfQuery;
	quadField.GetUnitsExact(qfQuery, center, radius + THREAT_RANGE_CAP + THREAT_MARGIN, false);

	for (const CUnit* u: *qfQuery.units) {
		if (teamHandler.Ally(owner->allyteam, u->allyteam) || u->IsNeutral() || u->beingBuilt)
			continue;

		const uint8_t los = u->losStatus[owner->allyteam];

		if (!(los & (LOS_INLOS | LOS_INRADAR)))
			continue;

		float range = THREAT_DEFAULT_RANGE;

		// a type seen in LOS is known: count only its weapons that can hit the builder
		if (los & (LOS_INLOS | LOS_PREVLOS)) {
			range = 0.0f;

			for (const CWeapon* w: u->weapons) {
				if (w->weaponDef->isShield || w->weaponDef->interceptor != 0)
					continue;
				if ((w->onlyTargetCategory & owner->category) == 0)
					continue;

				range = std::max(range, w->range);
			}

			if (range <= 0.0f)
				continue;
		}

		threats.list.push_back({u->pos, std::min(range, THREAT_RANGE_CAP), u->id});
	}

	threats.builderThreatened = (threats.Covering(owner->pos) != nullptr);
}


// A target under fire is only worked from where the builder already stands,
// and on patrol only while the builder itself is out of fire.
bool CBuilderCAI::SkipThreatenedTarget(const CWorldObject* target, int targetID, Threats* threats) const
{
	if (threats == nullptr)
		return false;

	const Threats::Threat* t = threats->Covering(target->pos);

	if (t == nullptr)
		return false;
	if (IsInBuildRange(target) && !(threats->patrol && threats->builderThreatened))
		return false;

	if (threats->skipped++ == 0) {
		threats->firstSkippedID = targetID;
		threats->firstSkippedBy = *t;
	}

	return true;
}


// ExecutePatrol pushes an internal FIGHT for each leg and PushOrUpdateReturnFight
// may put a 6-param one in front of it; the PATROL behind them is the next leg.
const Command* CBuilderCAI::NextPatrolPoint(size_t first) const
{
	for (size_t i = first; i < commandQue.size(); ++i) {
		const Command& c = commandQue[i];

		if (c.GetID() == CMD_PATROL)
			return &c;
		if (c.GetID() != CMD_FIGHT || !c.IsInternalOrder())
			return nullptr;
	}

	return nullptr;
}


// An automatic job a fight or patrol search started (tempOrder, 5 params) is
// dropped as soon as that search would pass over its target (see
// SkipThreatenedTarget), so the job and the search never disagree: a job on a
// target out of fire goes on even with the builder under fire, it leads away.
bool CBuilderCAI::AbandonThreatenedJob(const Command& c)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (owner->immobile || !tempOrder || !c.IsInternalOrder() || c.GetNumParams() != 5)
		return false;
	if (c.GetID() != CMD_REPAIR && c.GetID() != CMD_RECLAIM && c.GetID() != CMD_RESURRECT)
		return false;

	const int maxUnits = static_cast<int>(unitHandler.MaxUnits());
	const int targetID = static_cast<int>(c.GetParam(0));
	const bool isFeature = (targetID >= maxUnits);
	const CWorldObject* target = isFeature?
		static_cast<const CWorldObject*>(featureHandler.GetFeature(targetID - maxUnits)):
		static_cast<const CWorldObject*>(unitHandler.GetUnit(targetID));

	// a target that is gone is the job's own business
	if (target == nullptr)
		return false;

	Threats threats;
	threats.patrol = (NextPatrolPoint(1) != nullptr);
	CollectThreats(owner->pos, owner->pos.distance2D(target->pos), threats);

	if (!SkipThreatenedTarget(target, targetID, &threats))
		return false;

	const Threats::Threat* t = &threats.firstSkippedBy;
	const char* why = IsInBuildRange(target)? "the builder and its target are under fire": "its target is under fire, out of build range";

	LOG_SL(LOG_SECTION_BUILDER_SAFETY, L_NOTICE,
		"builder %d (%s) at (%.0f, %.0f) on %s drops its automatic %s of %s %d at (%.0f, %.0f): %s (enemy %d at (%.0f, %.0f), range %.0f)",
		owner->id, owner->unitDef->name.c_str(), owner->pos.x, owner->pos.z, threats.patrol? "patrol": "fight",
		(c.GetID() == CMD_REPAIR)? "repair": ((c.GetID() == CMD_RECLAIM)? "reclaim": "resurrect"),
		isFeature? "feature": "unit", isFeature? targetID - maxUnits: targetID, target->pos.x, target->pos.z,
		why, t->unitID, t->pos.x, t->pos.z, t->range);

	ownerBuilder->StopBuild();
	CBuilderCaches::RemoveUnitFromReclaimers(owner);
	CBuilderCaches::RemoveUnitFromFeatureReclaimers(owner);
	CBuilderCaches::RemoveUnitFromResurrecters(owner);
	StopMoveAndFinishCommand();
	return true;
}


// A builder under fire on a patrol leg that leads toward the enemy turns back:
// the leg ends, and the next patrol point takes it away. When that point lies
// toward the enemy as well, turning would only flip between the two legs, so
// the leg goes on.
bool CBuilderCAI::EndThreatenedPatrolLeg(const Command& c, const Threats& threats)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!threats.patrol || !threats.builderThreatened)
		return false;

	const Threats::Threat* t = threats.Covering(owner->pos);
	const Command* nextPatrol = NextPatrolPoint(0);
	const float3 legEnd = (c.GetNumParams() >= 6)? c.GetPos(3): c.GetPos(0);
	const float3 nextEnd = nextPatrol->GetPos(0);
	const float here = owner->pos.SqDistance2D(t->pos);

	if (legEnd.SqDistance2D(t->pos) >= here)
		return false;

	if (nextEnd.SqDistance2D(t->pos) < here) {
		if (ThreatLogDue(ThreatLogKeepLegKey(t->unitID))) {
			LOG_SL(LOG_SECTION_BUILDER_SAFETY, L_NOTICE,
				"builder %d (%s) at (%.0f, %.0f) is under fire on patrol (enemy %d at (%.0f, %.0f), range %.0f), but both patrol points (%.0f, %.0f) and (%.0f, %.0f) lie toward it: keeps its leg",
				owner->id, owner->unitDef->name.c_str(), owner->pos.x, owner->pos.z, t->unitID, t->pos.x, t->pos.z, t->range,
				legEnd.x, legEnd.z, nextEnd.x, nextEnd.z);
		}
		return false;
	}

	if (ThreatLogDue(ThreatLogTurnKey(nextEnd))) {
		LOG_SL(LOG_SECTION_BUILDER_SAFETY, L_NOTICE,
			"builder %d (%s) at (%.0f, %.0f) is under fire on patrol (enemy %d at (%.0f, %.0f), range %.0f): turns back from (%.0f, %.0f) toward (%.0f, %.0f)",
			owner->id, owner->unitDef->name.c_str(), owner->pos.x, owner->pos.z, t->unitID, t->pos.x, t->pos.z, t->range,
			legEnd.x, legEnd.z, nextEnd.x, nextEnd.z);
	}

	StopMoveAndFinishCommand();
	return true;
}


// once per stretch of searches that pass over something: a builder walking
// past wrecks under fire would otherwise log at every step
void CBuilderCAI::LogThreatSkips(const Threats& threats)
{
	if (threats.skipped == 0) {
		if (lastThreatLogKey == THREAT_LOG_SKIPPING)
			lastThreatLogKey = -1;

		return;
	}
	if (!ThreatLogDue(THREAT_LOG_SKIPPING))
		return;

	const int maxUnits = static_cast<int>(unitHandler.MaxUnits());
	const bool isFeature = (threats.firstSkippedID >= maxUnits);
	const Threats::Threat& t = threats.firstSkippedBy;

	LOG_SL(LOG_SECTION_BUILDER_SAFETY, L_NOTICE,
		"builder %d (%s) at (%.0f, %.0f) on %s passes over %d target(s) under fire, first %s %d (enemy %d at (%.0f, %.0f), range %.0f)",
		owner->id, owner->unitDef->name.c_str(), owner->pos.x, owner->pos.z, threats.patrol? "patrol": "fight", threats.skipped,
		isFeature? "feature": "unit", isFeature? threats.firstSkippedID - maxUnits: threats.firstSkippedID,
		t.unitID, t.pos.x, t.pos.z, t.range);
}


bool CBuilderCAI::ThreatLogDue(int key)
{
	if (key == lastThreatLogKey)
		return false;

	lastThreatLogKey = key;
	return true;
}


void CBuilderCAI::BuggerOff(const float3& pos, float radius) {
	RECOIL_DETAILED_TRACY_ZONE;
	if (owner->unitDef->IsStaticBuilderUnit())
		return;

	CMobileCAI::BuggerOff(pos, radius);
}

