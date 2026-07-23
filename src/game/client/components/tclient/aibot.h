#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_AIBOT_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_AIBOT_H

#include <game/client/component.h>

#include <unordered_map>
#include <vector>

class CAIBot : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;
	void OnRender() override;

	// Called immediately before the client sends the local player's input.
	// Returns true only when the bot replaced the movement input.
	bool ApplyInput(struct CNetObj_PlayerInput &Input);

	void ForceReplan();
	void ClearLearning();
	const char *Status() const { return m_aStatus; }
	int PlannedNodes() const { return (int)m_vRoute.size(); }
	int LearnedFailures() const { return (int)m_FailureCost.size(); }

private:
	bool EnsureMap();
	bool BuildRoute(int StartCell);
	bool IsWalkable(int Cell) const;
	bool IsSolid(int Cell) const;
	bool IsDeath(int Cell) const;
	bool IsFreeze(int Cell) const;
	bool IsDeepFreeze(int Cell) const;
	bool CanSurviveFreeze(int Cell) const;
	int FindFinish() const;
	int RawTile(int Cell) const;
	int FrontRawTile(int Cell) const;
	int CellFromPos(const vec2 &Pos) const;
	vec2 CellCenter(int Cell) const;
	void RegisterFailure();
	void LoadLearning();
	void SaveLearning() const;
	void SetStatus(const char *pStatus);

	std::vector<int> m_vRoute;
	std::unordered_map<int, int> m_FailureCost;
	int m_MapWidth = 0;
	int m_MapHeight = 0;
	int m_RouteIndex = 0;
	int m_LastPlanTick = -1000000;
	int m_LastCell = -1;
	int m_LastProgressCell = -1;
	bool m_HadLocalCharacter = false;
	bool m_ForceReplan = true;
	char m_aMapName[128] = {};
	char m_aStatus[128] = "Waiting for a map";
};

#endif
