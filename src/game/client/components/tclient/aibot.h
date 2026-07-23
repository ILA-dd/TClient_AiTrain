#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_AIBOT_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_AIBOT_H

#include <game/client/component.h>

#include <array>
#include <unordered_map>
#include <vector>

class CAIBot : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;
	void OnShutdown() override;
	void OnRender() override;

	// Called immediately before the client sends the local player's input.
	// Returns true only when the bot replaced the movement input.
	bool ApplyInput(struct CNetObj_PlayerInput &Input);

	void ForceReplan();
	void ClearLearning();
	const char *Status() const { return m_aStatus; }
	int PlannedNodes() const { return (int)m_vRoute.size(); }
	int LearnedFailures() const { return (int)m_FailureCost.size(); }
	int RouteProgressNodes() const { return m_LastRewardRouteIndex + 1; }
	float RouteProgressPercent() const;
	float CurrentReward() const { return m_CurrentReward; }
	float LastTrainingReward() const { return m_LastTrainingReward; }
	float TotalTrainingReward() const { return m_TotalTrainingReward; }
	float RewardNetEstimate() const { return m_RewardNetEstimate; }
	float BestRaceProgressPercent() const { return 100.0f * m_BestRaceProgress; }
	float BestRaceReward() const { return m_BestRaceReward; }
	int Episodes() const { return m_Episodes; }
	int StartCount() const { return m_StartCount; }
	int FinishCount() const { return m_FinishCount; }
	int DeathCount() const { return m_DeathCount; }
	int RewardedPathSteps() const { return m_RewardedPathSteps; }
	int OffRouteSteps() const { return m_OffRouteSteps; }
	int RewardNetUpdates() const { return m_RewardNetUpdates; }
	const char *PhaseName() const;

private:
	bool EnsureMap();
	bool BuildRoute(int StartCell, int GoalCell, const char *pGoalName);
	bool IsWalkable(int Cell) const;
	bool IsSolid(int Cell) const;
	bool IsDeath(int Cell) const;
	bool IsFreeze(int Cell) const;
	bool IsDeepFreeze(int Cell) const;
	bool IsStart(int Cell) const;
	bool IsFinish(int Cell) const;
	bool IsLocalFrozen(int Tick) const;
	bool CanSurviveFreeze(int Cell) const;
	int FindStart() const;
	int FindFinish() const;
	int RouteIndexForCell(int Cell) const;
	int RawTile(int Cell) const;
	int FrontRawTile(int Cell) const;
	int CellFromPos(const vec2 &Pos) const;
	vec2 CellCenter(int Cell) const;
	void ResetEpisode();
	void UpdateReward(int CurrentCell, int Tick);
	bool HandleFreeze(int CurrentCell, int Tick);
	std::array<float, 6> RewardFeatures(int Cell, int RouteIndex, bool OnRoute, bool Safe, bool Finished) const;
	float PredictReward(const std::array<float, 6> &Features) const;
	void TrainRewardNet(float TrainingReward, const std::array<float, 6> &Features);
	void RegisterFailure();
	void LoadLearning();
	void SaveLearning() const;
	void LoadStats();
	void SaveStats() const;
	void MaybeSaveProgress(int Tick, bool Force = false);
	void SetStatus(const char *pStatus);

	std::vector<int> m_vRoute;
	std::unordered_map<int, int> m_FailureCost;
	int m_MapWidth = 0;
	int m_MapHeight = 0;
	int m_GoalCell = -1;
	int m_RouteIndex = 0;
	int m_LastPlanTick = -1000000;
	int m_LastCell = -1;
	int m_LastProgressCell = -1;
	int m_LastRewardCell = -1;
	int m_LastRewardRouteIndex = -1;
	int m_LastRewardTick = -1000000;
	int m_LastPersistenceTick = -1000000;
	int m_UnsafeFreezeSinceTick = -1;
	int m_PostFinishTicks = 0;
	int m_Episodes = 0;
	int m_StartCount = 0;
	int m_FinishCount = 0;
	int m_DeathCount = 0;
	int m_RewardedPathSteps = 0;
	int m_OffRouteSteps = 0;
	int m_SafeFreezeSteps = 0;
	int m_RewardNetUpdates = 0;
	float m_CurrentReward = -1.0f;
	float m_LastTrainingReward = 0.0f;
	float m_TotalTrainingReward = 0.0f;
	float m_RewardNetEstimate = 0.0f;
	float m_BestRaceProgress = 0.0f;
	float m_BestRaceReward = 0.0f;
	std::array<float, 6> m_aRewardNetWeights = {};
	bool m_HadLocalCharacter = false;
	bool m_EpisodeActive = false;
	bool m_ResetRequested = false;
	bool m_RaceStarted = false;
	bool m_FinishCrossed = false;
	bool m_ForceReplan = true;
	char m_aMapName[128] = {};
	char m_aStatus[128] = "Waiting for a map";
};

#endif
