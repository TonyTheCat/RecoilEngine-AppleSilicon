/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef _BUILDER_CAI_H_
#define _BUILDER_CAI_H_

#include "MobileCAI.h"
#include "Sim/Units/BuildInfo.h"
#include "System/Misc/BitwiseEnum.h"
#include "System/UnorderedSet.hpp"

#include <vector>

class CUnit;
class CBuilder;
class CFeature;
class CSolidObject;
class CWorldObject;
struct Command;
struct UnitDef;


class CBuilderCAI : public CMobileCAI
{
public:
	CR_DECLARE(CBuilderCAI)
	CBuilderCAI(CUnit* owner);
	CBuilderCAI();
	~CBuilderCAI();

	void PostLoad();

	int GetDefaultCmd(const CUnit* unit, const CFeature* feature);
	void SlowUpdate();

	void FinishCommand();
	void GiveCommandReal(const Command& c, bool fromSynced = true);
	void BuggerOff(const float3& pos, float radius);
	bool TargetInterceptable(const CUnit* unit, float uspeed);

	void ExecuteBuildCmd(Command& c);
	void ExecutePatrol(Command& c);
	void ExecuteFight(Command& c);
	void ExecuteGuard(Command& c);
	void ExecuteStop(Command& c);
	virtual void ExecuteRepair(Command& c);
	virtual void ExecuteCapture(Command& c);
	virtual void ExecuteReclaim(Command& c);
	virtual void ExecuteResurrect(Command& c);
	virtual void ExecuteRestore(Command& c);

	bool ReclaimObject(CSolidObject* o);
	bool ResurrectObject(CFeature* feature);

	bool IsInBuildRange(const CWorldObject* obj) const;
	bool IsInBuildRange(const float3& pos, const float radius) const;
	float GetBuildRange(const float targetRadius) const;

public:
	spring::unordered_set<int> buildOptions;

private:
	enum ReclaimOptions {
		REC_NORESCHECK = 1<<0,
		REC_UNITS      = 1<<1,
		REC_NONREZ     = 1<<2,
		REC_ENEMY      = 1<<3,
		REC_ENEMYONLY  = 1<<4,
		REC_SPECIAL    = 1<<5
	};
	typedef Bitwise::BitwiseEnum<ReclaimOptions> ReclaimOption;

	/**
	 * Known enemies that could shoot a point (gameplay branch, builder
	 * self-preservation): builders on a fight never walk into enemy fire for
	 * an automatic job, and on patrol they also leave it.
	 */
	struct Threats {
		struct Threat {
			float3 pos;
			float range;
			int unitID;
		};
		std::vector<Threat> list;
		bool patrol = false;
		bool builderThreatened = false;

		// what the searches skipped, for the log
		int skipped = 0;
		int firstSkippedID = -1;
		Threat firstSkippedBy = {};

		const Threat* Covering(const float3& pos) const;
	};

private:
	/**
	 * @param pos position where to reclaim
	 * @param radius radius to search for objects to reclaim
	 * @param cmdopts command options
	 * @param recoptions reclaim optioons
	 */
	bool FindReclaimTargetAndReclaim(const float3& pos, float radius, unsigned char cmdopt, ReclaimOption recoptions, Threats* threats = nullptr);
	/**
	 * @param freshOnly reclaims only corpses that have rez progress or all the metal left
	 */
	bool FindResurrectableFeatureAndResurrect(const float3& pos, float radius, unsigned char options, bool freshOnly, Threats* threats = nullptr);

	/**
	 * @param builtOnly skips units that are under construction
	 */
	bool FindRepairTargetAndRepair(const float3& pos, float radius, unsigned char options, bool attackEnemy, bool builtOnly, Threats* threats = nullptr);
	/**
	 * @param pos         position where to search for units to capture
	 * @param radius      radius in which are searched units to capture
	 * @param options     command options
	 * @param healthyOnly only capture units with capture progress or 100% health remaining
	 */
	bool FindCaptureTargetAndCapture(const float3& pos, float radius, unsigned char options, bool healthyOnly);

	int FindReclaimTarget(const float3& pos, float radius, unsigned char cmdopt, ReclaimOption recoptions, float bestStartDist = 1.0e30f, Threats* threats = nullptr) const;

	/// fills threats with the known enemies that could shoot inside the circle, and whether the builder stands under fire
	void CollectThreats(const float3& center, float radius, Threats& threats) const;
	/// true (and counted in threats) when a search has to pass over target: under fire and not workable from where the builder stands
	bool SkipThreatenedTarget(const CWorldObject* target, int targetID, Threats* threats) const;
	/// the PATROL a run of internal FIGHTs from queue index first belongs to, nullptr when they are not a patrol leg
	const Command* NextPatrolPoint(size_t first) const;
	/// drops an automatic fight/patrol job that came under fire
	bool AbandonThreatenedJob(const Command& c);
	/// ends a patrol leg that leads a builder under fire toward the enemy
	bool EndThreatenedPatrolLeg(const Command& c, const Threats& threats);
	void LogThreatSkips(const Threats& threats);
	/// logs line once per builder until the key changes, so a skip seen every SlowUpdate shows up once
	bool ThreatLogDue(int key);

	bool MoveInBuildRange(const CWorldObject* obj, const bool checkMoveTypeForFailed = false);
	bool MoveInBuildRange(const float3& pos, float radius, const bool checkMoveTypeForFailed = false);

	bool IsBuildPosBlocked(const BuildInfo& bi, const CUnit** nanoFrame) const;
	bool IsBuildPosBlocked(const BuildInfo& bi) const {
		const CUnit* u = nullptr;
		return IsBuildPosBlocked(build, &u);
	}

	void CancelRestrictedUnit();
	bool OutOfImmobileRange(const Command& cmd) const;
	/// add a command to reclaim a feature that is blocking our build-site
	void ReclaimFeature(CFeature* f);

	inline float f3Dist(const float3& a, const float3& b) const {
		return range3D ? a.distance(b) : a.distance2D(b);
	}
	inline float f3SqDist(const float3& a, const float3& b) const {
		return range3D ? a.SqDistance(b) : a.SqDistance2D(b);
	}
	//inline float f3Len(const float3& a) const {
	//	return range3D ? a.Length() : a.Length2D();
	//}
	//inline float f3SqLen(const float3& a) const {
	//	return range3D ? a.SqLength() : a.SqLength2D();
	//}

	float GetBuildOptionRadius(const UnitDef* unitdef, int cmdId);

private:
	CBuilder* ownerBuilder;

	bool building;
	BuildInfo build;

	int cachedRadiusId;
	float cachedRadius;

	int buildRetries;
	int randomCounter; ///< used to balance intervals of time intensive ai optimizations

	int lastPC1; ///< helps avoid infinite loops
	int lastPC2;
	int lastPC3;

	bool range3D;

	int lastThreatLogKey; ///< log state only, see ThreatLogDue
};

#endif // _BUILDER_CAI_H_
