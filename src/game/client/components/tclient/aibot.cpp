#include "aibot.h"

#include <base/log.h>
#include <base/system.h>

#include <engine/client.h>
#include <engine/shared/config.h>
#include <engine/shared/linereader.h>
#include <engine/storage.h>

#include <game/client/components/controls.h>
#include <game/client/gameclient.h>
#include <game/collision.h>
#include <game/mapitems.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <queue>
#include <string>
#include <vector>

namespace
{
constexpr int TILE_SIZE = 32;

struct SOpenNode
{
	int m_Cell;
	int m_GScore;
	int m_FScore;

	bool operator<(const SOpenNode &Other) const
	{
		return m_FScore > Other.m_FScore;
	}
};

bool IsNormalFreeze(int Tile)
{
	return Tile == TILE_FREEZE || Tile == TILE_LFREEZE;
}

bool IsDeepFreezeTile(int Tile)
{
	return Tile == TILE_DFREEZE;
}

bool IsDeathTile(int Tile)
{
	return Tile == TILE_DEATH;
}
}

void CAIBot::OnReset()
{
	m_vRoute.clear();
	m_FailureCost.clear();
	m_MapWidth = 0;
	m_MapHeight = 0;
	m_GoalCell = -1;
	m_RouteIndex = 0;
	m_LastPlanTick = -1000000;
	m_LastPersistenceTick = -1000000;
	m_UnsafeFreezeSinceTick = -1;
	m_LastCell = -1;
	m_LastProgressCell = -1;
	m_Episodes = 0;
	m_StartCount = 0;
	m_FinishCount = 0;
	m_DeathCount = 0;
	m_RewardedPathSteps = 0;
	m_OffRouteSteps = 0;
	m_SafeFreezeSteps = 0;
	m_RewardNetUpdates = 0;
	m_TotalTrainingReward = 0.0f;
	m_RewardNetEstimate = 0.0f;
	m_BestRaceProgress = 0.0f;
	m_BestRaceReward = 0.0f;
	m_aRewardNetWeights.fill(0.0f);
	ResetEpisode();
	m_HadLocalCharacter = false;
	m_ResetRequested = false;
	m_ForceReplan = true;
	m_aMapName[0] = '\0';
	SetStatus("Waiting for a map");
}

void CAIBot::OnShutdown()
{
	// A normal client close must not discard route penalties or reward-network
	// updates earned since the last periodic autosave.
	SaveLearning();
	SaveStats();
}

void CAIBot::SetStatus(const char *pStatus)
{
	str_copy(m_aStatus, pStatus, sizeof(m_aStatus));
}

bool CAIBot::EnsureMap()
{
	if(!Collision() || Collision()->GetWidth() <= 0 || Collision()->GetHeight() <= 0 || !GameClient()->Map())
		return false;

	const char *pMapName = GameClient()->Map()->BaseName();
	if(str_comp(m_aMapName, pMapName) == 0 && m_MapWidth == Collision()->GetWidth() && m_MapHeight == Collision()->GetHeight())
		return true;

	SaveLearning();
	SaveStats();
	str_copy(m_aMapName, pMapName, sizeof(m_aMapName));
	m_MapWidth = Collision()->GetWidth();
	m_MapHeight = Collision()->GetHeight();
	m_vRoute.clear();
	m_FailureCost.clear();
	m_GoalCell = -1;
	m_RouteIndex = 0;
	m_LastPersistenceTick = -1000000;
	m_LastCell = -1;
	m_LastProgressCell = -1;
	m_Episodes = 0;
	m_StartCount = 0;
	m_FinishCount = 0;
	m_DeathCount = 0;
	m_RewardedPathSteps = 0;
	m_OffRouteSteps = 0;
	m_SafeFreezeSteps = 0;
	m_RewardNetUpdates = 0;
	m_TotalTrainingReward = 0.0f;
	m_RewardNetEstimate = 0.0f;
	m_BestRaceProgress = 0.0f;
	m_BestRaceReward = 0.0f;
	m_aRewardNetWeights.fill(0.0f);
	ResetEpisode();
	m_ForceReplan = true;
	LoadLearning();
	LoadStats();
	SetStatus("Map loaded, planning route");
	return true;
}

int CAIBot::CellFromPos(const vec2 &Pos) const
{
	if(m_MapWidth <= 0 || m_MapHeight <= 0)
		return -1;
	const int X = std::clamp((int)Pos.x / TILE_SIZE, 0, m_MapWidth - 1);
	const int Y = std::clamp((int)Pos.y / TILE_SIZE, 0, m_MapHeight - 1);
	return Y * m_MapWidth + X;
}

vec2 CAIBot::CellCenter(int Cell) const
{
	return vec2((float)((Cell % m_MapWidth) * TILE_SIZE + TILE_SIZE / 2), (float)((Cell / m_MapWidth) * TILE_SIZE + TILE_SIZE / 2));
}

float CAIBot::RouteProgressPercent() const
{
	if(m_vRoute.size() < 2 || m_LastRewardRouteIndex < 0)
		return 0.0f;
	return 100.0f * std::clamp((float)m_LastRewardRouteIndex / (float)(m_vRoute.size() - 1), 0.0f, 1.0f);
}

int CAIBot::RawTile(int Cell) const
{
	if(Cell < 0 || Cell >= m_MapWidth * m_MapHeight)
		return TILE_DEATH;
	return Collision()->GetTileIndex(Cell);
}

int CAIBot::FrontRawTile(int Cell) const
{
	if(Cell < 0 || Cell >= m_MapWidth * m_MapHeight)
		return TILE_DEATH;
	return Collision()->GetFrontTileIndex(Cell);
}

bool CAIBot::IsSolid(int Cell) const
{
	const vec2 Pos = CellCenter(Cell);
	return Collision()->IsSolid((int)Pos.x, (int)Pos.y) != 0;
}

bool CAIBot::IsDeath(int Cell) const
{
	return IsDeathTile(RawTile(Cell)) || IsDeathTile(FrontRawTile(Cell));
}

bool CAIBot::IsFreeze(int Cell) const
{
	return IsNormalFreeze(RawTile(Cell)) || IsNormalFreeze(FrontRawTile(Cell));
}

bool CAIBot::IsDeepFreeze(int Cell) const
{
	return IsDeepFreezeTile(RawTile(Cell)) || IsDeepFreezeTile(FrontRawTile(Cell));
}

bool CAIBot::IsStart(int Cell) const
{
	return RawTile(Cell) == TILE_START || FrontRawTile(Cell) == TILE_START;
}

bool CAIBot::IsFinish(int Cell) const
{
	return RawTile(Cell) == TILE_FINISH || FrontRawTile(Cell) == TILE_FINISH;
}

bool CAIBot::IsLocalFrozen(int Tick) const
{
	const int LocalId = GameClient()->m_aLocalIds[g_Config.m_ClDummy];
	if(LocalId < 0 || LocalId >= MAX_CLIENTS)
		return false;
	const auto &ClientInfo = GameClient()->m_aClients[LocalId];
	return ClientInfo.m_DeepFrozen || ClientInfo.m_FreezeEnd == -1 || ClientInfo.m_FreezeEnd > Tick;
}

bool CAIBot::CanSurviveFreeze(int Cell) const
{
	const int X = Cell % m_MapWidth;
	const int Y = Cell / m_MapWidth;
	const int MaxFallTiles = g_Config.m_TcAiBotFreezeDropTiles;

	for(int Drop = 1; Drop <= MaxFallTiles && Y + Drop < m_MapHeight; ++Drop)
	{
		const int Below = (Y + Drop) * m_MapWidth + X;
		if(IsDeath(Below) || IsDeepFreeze(Below))
			return false;
		if(IsSolid(Below))
			return true;
	}
	return false;
}

bool CAIBot::IsWalkable(int Cell) const
{
	if(IsSolid(Cell) || IsDeath(Cell) || IsDeepFreeze(Cell))
		return false;
	if(IsFreeze(Cell) && (!g_Config.m_TcAiBotAllowFreeze || !CanSurviveFreeze(Cell)))
		return false;
	return true;
}

int CAIBot::FindStart() const
{
	const int NumCells = m_MapWidth * m_MapHeight;
	for(int Cell = 0; Cell < NumCells; ++Cell)
	{
		if(IsStart(Cell))
			return Cell;
	}
	return -1;
}

int CAIBot::FindFinish() const
{
	const int NumCells = m_MapWidth * m_MapHeight;
	for(int Cell = 0; Cell < NumCells; ++Cell)
	{
		if(IsFinish(Cell))
			return Cell;
	}
	return -1;
}

int CAIBot::RouteIndexForCell(int Cell) const
{
	if(Cell < 0 || m_vRoute.empty())
		return -1;

	const int Start = std::max(0, m_LastRewardRouteIndex - 1);
	for(int Index = Start; Index < (int)m_vRoute.size(); ++Index)
	{
		if(m_vRoute[Index] == Cell)
			return Index;
	}
	return -1;
}

bool CAIBot::BuildRoute(int StartCell, int GoalCell, const char *pGoalName)
{
	m_GoalCell = GoalCell;
	if(GoalCell < 0)
	{
		m_vRoute.clear();
		str_format(m_aStatus, sizeof(m_aStatus), "No %s tile on this map", pGoalName);
		return false;
	}
	if(StartCell < 0 || !IsWalkable(StartCell))
	{
		m_vRoute.clear();
		SetStatus("Current position is not pathable");
		return false;
	}

	const int NumCells = m_MapWidth * m_MapHeight;
	const int Infinity = std::numeric_limits<int>::max() / 4;
	std::vector<int> vCost(NumCells, Infinity);
	std::vector<int> vPrevious(NumCells, -1);
	std::priority_queue<SOpenNode> Open;

	auto Heuristic = [this, GoalCell](int Cell) {
		const int Dx = std::abs((Cell % m_MapWidth) - (GoalCell % m_MapWidth));
		const int Dy = std::abs((Cell / m_MapWidth) - (GoalCell / m_MapWidth));
		return (Dx + Dy) * 10;
	};

	vCost[StartCell] = 0;
	Open.push({StartCell, 0, Heuristic(StartCell)});
	int Expanded = 0;

	while(!Open.empty() && Expanded < g_Config.m_TcAiBotMaxNodes)
	{
		const SOpenNode Current = Open.top();
		Open.pop();
		if(Current.m_GScore != vCost[Current.m_Cell])
			continue;
		if(Current.m_Cell == GoalCell)
			break;

		++Expanded;
		const int X = Current.m_Cell % m_MapWidth;
		const int Y = Current.m_Cell / m_MapWidth;
		const int aDx[] = {-1, 1, 0, 0};
		const int aDy[] = {0, 0, -1, 1};
		for(int Direction = 0; Direction < 4; ++Direction)
		{
			const int NextX = X + aDx[Direction];
			const int NextY = Y + aDy[Direction];
			if(NextX < 0 || NextX >= m_MapWidth || NextY < 0 || NextY >= m_MapHeight)
				continue;
			const int Next = NextY * m_MapWidth + NextX;
			if(!IsWalkable(Next))
				continue;

			int StepCost = 10;
			if(IsFreeze(Next))
				StepCost += g_Config.m_TcAiBotFreezePenalty;
			if(const auto It = m_FailureCost.find(Next); It != m_FailureCost.end())
				StepCost += It->second * 25;

			const int NextCost = Current.m_GScore + StepCost;
			if(NextCost >= vCost[Next])
				continue;

			vCost[Next] = NextCost;
			vPrevious[Next] = Current.m_Cell;
			Open.push({Next, NextCost, NextCost + Heuristic(Next)});
		}
	}

	if(vPrevious[GoalCell] == -1 && GoalCell != StartCell)
	{
		m_vRoute.clear();
		str_format(m_aStatus, sizeof(m_aStatus), "A* stopped after %d nodes", Expanded);
		return false;
	}

	m_vRoute.clear();
	for(int Cell = GoalCell; Cell != -1; Cell = vPrevious[Cell])
	m_vRoute.push_back(Cell);
	std::reverse(m_vRoute.begin(), m_vRoute.end());
	m_RouteIndex = 0;
	str_format(m_aStatus, sizeof(m_aStatus), "A* to %s: %d nodes, %d checked", pGoalName, (int)m_vRoute.size(), Expanded);
	return true;
}

void CAIBot::ResetEpisode()
{
	m_LastRewardCell = -1;
	m_LastRewardRouteIndex = -1;
	m_LastRewardTick = -1000000;
	m_PostFinishTicks = 0;
	m_CurrentReward = -1.0f;
	m_LastTrainingReward = 0.0f;
	m_EpisodeActive = false;
	m_RaceStarted = false;
	m_FinishCrossed = false;
	m_UnsafeFreezeSinceTick = -1;
	m_ResetRequested = false;
}

bool CAIBot::HandleFreeze(int CurrentCell, int Tick)
{
	if(!g_Config.m_TcAiBotResetUnsafeFreeze || m_ResetRequested)
		return m_ResetRequested;

	// Do not kill a tee that has entered a freeze strip with a reachable solid
	// platform underneath. It can fall through, land safely and unfreeze there.
	if(IsFreeze(CurrentCell))
	{
		if(CanSurviveFreeze(CurrentCell))
		{
			m_UnsafeFreezeSinceTick = -1;
			return false;
		}
		if(m_UnsafeFreezeSinceTick < 0)
		{
			m_UnsafeFreezeSinceTick = Tick;
			SetStatus("Unsafe freeze detected, preparing reset");
		}
	}

	if(m_UnsafeFreezeSinceTick < 0 || !IsLocalFrozen(Tick) || Tick - m_UnsafeFreezeSinceTick < g_Config.m_TcAiBotUnsafeFreezeDelay)
		return false;

	// Record exactly one failure before asking the server for a normal kill.
	// The no-character handler skips a duplicate penalty while this reset is in flight.
	RegisterFailure();
	m_ResetRequested = true;
	GameClient()->SendKill();
	SetStatus("Unsafe freeze: reset requested and learned");
	return true;
}

const char *CAIBot::PhaseName() const
{
	if(m_FinishCrossed)
		return "finished";
	if(m_RaceStarted)
		return "racing";
	return "to start";
}

std::array<float, 6> CAIBot::RewardFeatures(int Cell, int RouteIndex, bool OnRoute, bool Safe, bool Finished) const
{
	float Progress = 0.0f;
	if(RouteIndex >= 0 && m_vRoute.size() > 1)
		Progress = std::clamp((float)RouteIndex / (float)(m_vRoute.size() - 1), 0.0f, 1.0f);

	return {1.0f, Progress, OnRoute ? 1.0f : 0.0f, Safe ? 1.0f : 0.0f, Finished ? 1.0f : 0.0f, IsFreeze(Cell) ? 1.0f : 0.0f};
}

float CAIBot::PredictReward(const std::array<float, 6> &Features) const
{
	float Sum = 0.0f;
	for(size_t Index = 0; Index < Features.size(); ++Index)
		Sum += m_aRewardNetWeights[Index] * Features[Index];
	return std::tanh(Sum);
}

void CAIBot::TrainRewardNet(float TrainingReward, const std::array<float, 6> &Features)
{
	const float Prediction = PredictReward(Features);
	const float Target = std::clamp(TrainingReward / 5.0f, -1.0f, 1.0f);
	const float Gradient = 0.03f * (Target - Prediction) * (1.0f - Prediction * Prediction);
	for(size_t Index = 0; Index < Features.size(); ++Index)
		m_aRewardNetWeights[Index] += Gradient * Features[Index];

	m_RewardNetEstimate = PredictReward(Features);
	++m_RewardNetUpdates;
}

void CAIBot::UpdateReward(int CurrentCell, int Tick)
{
	if(Tick == m_LastRewardTick || CurrentCell < 0)
		return;
	m_LastRewardTick = Tick;

	if(!m_EpisodeActive)
	{
		m_EpisodeActive = true;
		++m_Episodes;
	}

	const bool Safe = IsWalkable(CurrentCell);
	const bool Start = IsStart(CurrentCell);
	const bool Finish = IsFinish(CurrentCell);
	const int RouteIndex = RouteIndexForCell(CurrentCell);
	const bool OnRoute = RouteIndex >= 0;

	if(m_FinishCrossed)
		return;

	// A race has two different objectives. Before START, the displayed reward
	// remains negative and approaches zero only while the tee follows A* to the
	// start tile. On START, it is reset to zero and the finish run begins.
	if(!m_RaceStarted)
	{
		if(Start)
		{
			m_RaceStarted = true;
			m_CurrentReward = 0.0f;
			m_LastTrainingReward = 0.0f;
			m_LastRewardCell = -1;
			m_LastRewardRouteIndex = -1;
			++m_StartCount;
			m_ForceReplan = true;
			SaveStats();
			SetStatus("Start crossed: reward reset to zero, planning finish");
			return;
		}

		if(CurrentCell == m_LastRewardCell)
			return;

		if(OnRoute)
		{
			const int Remaining = (int)m_vRoute.size() - RouteIndex - 1;
			m_CurrentReward = -(float)std::max(1, Remaining + 1);
			m_LastTrainingReward = m_LastRewardRouteIndex >= 0 && RouteIndex > m_LastRewardRouteIndex ? 0.25f : -0.25f;
			m_LastRewardRouteIndex = RouteIndex;
			m_RouteIndex = std::max(m_RouteIndex, RouteIndex);
		}
		else
		{
			m_CurrentReward = std::min(-1.0f, m_CurrentReward - 2.0f);
			m_LastTrainingReward = -2.0f;
			++m_OffRouteSteps;
			m_ForceReplan = true;
		}

		m_LastRewardCell = CurrentCell;
		m_TotalTrainingReward += m_LastTrainingReward;
		TrainRewardNet(m_LastTrainingReward, RewardFeatures(CurrentCell, RouteIndex, OnRoute, Safe, false));
		return;
	}

	if(Finish)
	{
		m_FinishCrossed = true;
		m_LastTrainingReward = 20.0f;
		m_CurrentReward += 100.0f;
		m_BestRaceProgress = 1.0f;
		m_BestRaceReward = std::max(m_BestRaceReward, m_CurrentReward);
		m_TotalTrainingReward += m_LastTrainingReward;
		++m_FinishCount;
		TrainRewardNet(m_LastTrainingReward, RewardFeatures(CurrentCell, RouteIndex, OnRoute, Safe, true));
		SaveStats();
		SetStatus("Finish crossed: terminal reward +100");
		return;
	}

	if(CurrentCell == m_LastRewardCell)
		return;

	if(OnRoute)
	{
		if(m_LastRewardRouteIndex < 0)
		{
			// A route rebuild establishes a baseline; it must not reward standing
			// on the same tile or let the bot farm replans.
			m_LastTrainingReward = 0.0f;
		}
		else if(RouteIndex == m_LastRewardRouteIndex + 1 && Safe)
		{
			const float Progress = m_vRoute.size() > 1 ? (float)RouteIndex / (float)(m_vRoute.size() - 1) : 1.0f;
			m_LastTrainingReward = 1.0f + 8.0f * Progress;
			m_CurrentReward += m_LastTrainingReward;
			++m_RewardedPathSteps;
			if(IsFreeze(CurrentCell))
				++m_SafeFreezeSteps;
		}
		else if(RouteIndex > m_LastRewardRouteIndex + 1)
		{
			m_LastTrainingReward = -3.0f;
			m_CurrentReward -= 3.0f;
			++m_OffRouteSteps;
		}
		else
		{
			m_LastTrainingReward = -1.0f;
			m_CurrentReward -= 1.0f;
		}

		m_LastRewardRouteIndex = RouteIndex;
		m_RouteIndex = std::max(m_RouteIndex, RouteIndex);
		if(m_vRoute.size() > 1)
			m_BestRaceProgress = std::max(m_BestRaceProgress, (float)RouteIndex / (float)(m_vRoute.size() - 1));
		m_BestRaceReward = std::max(m_BestRaceReward, m_CurrentReward);
	}
	else
	{
		m_CurrentReward -= 4.0f;
		m_LastTrainingReward = -4.0f;
		++m_OffRouteSteps;
		m_LastRewardRouteIndex = -1;
		m_ForceReplan = true;
	}

	m_LastRewardCell = CurrentCell;
	m_TotalTrainingReward += m_LastTrainingReward;
	TrainRewardNet(m_LastTrainingReward, RewardFeatures(CurrentCell, RouteIndex, OnRoute, Safe, false));
}

void CAIBot::RegisterFailure()
{
	++m_DeathCount;
	m_LastTrainingReward = m_RaceStarted ? -10.0f : -2.0f;
	m_CurrentReward -= m_RaceStarted ? 10.0f : 2.0f;
	m_TotalTrainingReward += m_LastTrainingReward;
	if(m_LastProgressCell >= 0)
	{
		++m_FailureCost[m_LastProgressCell];
		TrainRewardNet(m_LastTrainingReward, RewardFeatures(m_LastProgressCell, RouteIndexForCell(m_LastProgressCell), false, false, false));
	}
	SaveLearning();
	SaveStats();
	m_ForceReplan = true;
	str_format(m_aStatus, sizeof(m_aStatus), "Death learned at tile %d", m_LastProgressCell);
}

void CAIBot::LoadLearning()
{
	if(!m_aMapName[0])
		return;

	char aFilename[IO_MAX_PATH_LENGTH];
	str_format(aFilename, sizeof(aFilename), "aibot/%s.learn", m_aMapName);
	CLineReader Reader;
	if(!Reader.OpenFile(Storage()->OpenFile(aFilename, IOFLAG_READ, IStorage::TYPE_SAVE)))
		return;

	while(const char *pLine = Reader.Get())
	{
		int Cell = -1;
		int Cost = 0;
		if(std::sscanf(pLine, "%d %d", &Cell, &Cost) == 2 && Cell >= 0 && Cell < m_MapWidth * m_MapHeight && Cost > 0)
			m_FailureCost[Cell] = Cost;
	}
}

void CAIBot::SaveLearning() const
{
	if(!m_aMapName[0])
		return;

	Storage()->CreateFolder("aibot", IStorage::TYPE_SAVE);
	char aFilename[IO_MAX_PATH_LENGTH];
	str_format(aFilename, sizeof(aFilename), "aibot/%s.learn", m_aMapName);
	IOHANDLE File = Storage()->OpenFile(aFilename, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!File)
	{
		log_error("aibot", "Failed to save learning data to '%s'", aFilename);
		return;
	}

	for(const auto &[Cell, Cost] : m_FailureCost)
	{
		char aLine[64];
		str_format(aLine, sizeof(aLine), "%d %d\n", Cell, Cost);
		io_write(File, aLine, str_length(aLine));
	}
	io_close(File);
}

void CAIBot::LoadStats()
{
	if(!m_aMapName[0])
		return;

	char aFilename[IO_MAX_PATH_LENGTH];
	str_format(aFilename, sizeof(aFilename), "aibot/%s.stats", m_aMapName);
	CLineReader Reader;
	if(!Reader.OpenFile(Storage()->OpenFile(aFilename, IOFLAG_READ, IStorage::TYPE_SAVE)))
		return;

	while(const char *pLine = Reader.Get())
	{
		int Version = 0;
		if(std::sscanf(pLine, "v%d %d %d %d %d %d %d %d %d %f %f", &Version, &m_Episodes, &m_StartCount, &m_FinishCount, &m_DeathCount, &m_RewardedPathSteps, &m_OffRouteSteps, &m_SafeFreezeSteps, &m_RewardNetUpdates, &m_BestRaceProgress, &m_BestRaceReward) == 11 && Version == 3)
			continue;
		if(std::sscanf(pLine, "v%d %d %d %d %d %d %d %d %d", &Version, &m_Episodes, &m_StartCount, &m_FinishCount, &m_DeathCount, &m_RewardedPathSteps, &m_OffRouteSteps, &m_SafeFreezeSteps, &m_RewardNetUpdates) == 9 && Version == 2)
			continue;
		if(std::sscanf(pLine, "v%d %d %d %d %d %d %d %d", &Version, &m_Episodes, &m_FinishCount, &m_DeathCount, &m_RewardedPathSteps, &m_OffRouteSteps, &m_SafeFreezeSteps, &m_RewardNetUpdates) == 8 && Version == 1)
			continue;
		if(std::sscanf(pLine, "reward %f", &m_TotalTrainingReward) == 1)
			continue;
		std::sscanf(pLine, "net %f %f %f %f %f %f", &m_aRewardNetWeights[0], &m_aRewardNetWeights[1], &m_aRewardNetWeights[2], &m_aRewardNetWeights[3], &m_aRewardNetWeights[4], &m_aRewardNetWeights[5]);
	}
}

void CAIBot::SaveStats() const
{
	if(!m_aMapName[0])
		return;

	Storage()->CreateFolder("aibot", IStorage::TYPE_SAVE);
	char aFilename[IO_MAX_PATH_LENGTH];
	str_format(aFilename, sizeof(aFilename), "aibot/%s.stats", m_aMapName);
	IOHANDLE File = Storage()->OpenFile(aFilename, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!File)
	{
		log_error("aibot", "Failed to save stats to '%s'", aFilename);
		return;
	}

	char aLine[256];
	str_format(aLine, sizeof(aLine), "v3 %d %d %d %d %d %d %d %d %.6f %.6f\n", m_Episodes, m_StartCount, m_FinishCount, m_DeathCount, m_RewardedPathSteps, m_OffRouteSteps, m_SafeFreezeSteps, m_RewardNetUpdates, m_BestRaceProgress, m_BestRaceReward);
	io_write(File, aLine, str_length(aLine));
	str_format(aLine, sizeof(aLine), "reward %.6f\n", m_TotalTrainingReward);
	io_write(File, aLine, str_length(aLine));
	str_format(aLine, sizeof(aLine), "net %.8f %.8f %.8f %.8f %.8f %.8f\n", m_aRewardNetWeights[0], m_aRewardNetWeights[1], m_aRewardNetWeights[2], m_aRewardNetWeights[3], m_aRewardNetWeights[4], m_aRewardNetWeights[5]);
	io_write(File, aLine, str_length(aLine));
	io_close(File);
}

void CAIBot::MaybeSaveProgress(int Tick, bool Force)
{
	// Saving once every five seconds keeps the current map's learned penalties
	// and reward net durable without writing on every simulation tick.
	if(!m_aMapName[0] || (!Force && Tick - m_LastPersistenceTick < 250))
		return;
	m_LastPersistenceTick = Tick;
	SaveLearning();
	SaveStats();
}

void CAIBot::OnRender()
{
	if(!EnsureMap())
		return;

	const CNetObj_Character *pCharacter = GameClient()->m_Snap.m_pLocalCharacter;
	if(!pCharacter)
	{
		if(m_HadLocalCharacter)
		{
			if(m_FinishCrossed)
				SaveStats();
			else if(!m_ResetRequested)
				RegisterFailure();
			ResetEpisode();
		}
		m_HadLocalCharacter = false;
		return;
	}

	m_HadLocalCharacter = true;
	const int CurrentCell = CellFromPos(vec2((float)pCharacter->m_X, (float)pCharacter->m_Y));
	m_LastCell = CurrentCell;
	if(CurrentCell >= 0)
		m_LastProgressCell = CurrentCell;

	const int Tick = Client()->GameTick(g_Config.m_ClDummy);
	if(HandleFreeze(CurrentCell, Tick))
	{
		MaybeSaveProgress(Tick, true);
		return;
	}
	if((m_ForceReplan || m_vRoute.empty()) && Tick - m_LastPlanTick >= 10)
	{
		m_LastPlanTick = Tick;
		m_ForceReplan = false;
		int GoalCell = m_RaceStarted ? FindFinish() : FindStart();
		const char *pGoalName = m_RaceStarted ? "finish" : "start";
		if(GoalCell < 0 && !m_RaceStarted)
		{
			// Non-race maps do not contain START. They still get a usable route,
			// but their reward begins from the current position.
			m_RaceStarted = true;
			m_CurrentReward = 0.0f;
			GoalCell = FindFinish();
			pGoalName = "finish";
			SetStatus("No start tile: racing from current position");
		}
		BuildRoute(CurrentCell, GoalCell, pGoalName);
	}
	UpdateReward(CurrentCell, Tick);
	MaybeSaveProgress(Tick);
}

bool CAIBot::ApplyInput(CNetObj_PlayerInput &Input)
{
	if(!g_Config.m_TcAiBot || !EnsureMap())
		return false;
	if(m_FinishCrossed || m_ResetRequested)
		return false;

	const CNetObj_Character *pCharacter = GameClient()->m_Snap.m_pLocalCharacter;
	if(!pCharacter || m_vRoute.empty())
		return false;

	const vec2 Position((float)pCharacter->m_X, (float)pCharacter->m_Y);
	const int CurrentCell = CellFromPos(Position);
	if(CurrentCell < 0)
		return false;

	while(m_RouteIndex + 1 < (int)m_vRoute.size() && m_vRoute[m_RouteIndex] == CurrentCell)
		++m_RouteIndex;
	if(m_RouteIndex >= (int)m_vRoute.size())
		return false;

	// Aim ahead of the next A* node. A single vertical node has no horizontal
	// intent by itself, which used to leave the tee standing under ledges.
	int AimIndex = m_RouteIndex;
	bool NeedJump = false;
	for(int Index = m_RouteIndex; Index < (int)m_vRoute.size() && Index <= m_RouteIndex + 4; ++Index)
	{
		if(CellCenter(m_vRoute[Index]).y < Position.y - 8.0f)
		{
			NeedJump = true;
			AimIndex = std::min(Index + 1, (int)m_vRoute.size() - 1);
			break;
		}
		AimIndex = Index;
	}

	const vec2 Target = CellCenter(m_vRoute[AimIndex]);
	const vec2 Delta = Target - Position;
	Input.m_Direction = Delta.x > 6.0f ? 1 : Delta.x < -6.0f ? -1 : 0;
	const int DirectionCell = CurrentCell + Input.m_Direction;
	if(Input.m_Direction != 0 && IsSolid(DirectionCell))
		NeedJump = true;

	// A held jump fires only once. Alternating short press/release windows lets
	// the tee jump again after landing and makes wall-jumps possible.
	const int Tick = Client()->GameTick(g_Config.m_ClDummy);
	Input.m_Jump = NeedJump && ((Tick / 3) % 2 == 0) ? 1 : 0;
	Input.m_TargetX = (int)Delta.x;
	Input.m_TargetY = (int)Delta.y;
	if(!Input.m_TargetX && !Input.m_TargetY)
		Input.m_TargetX = 1;

	// A straight-up A* segment needs a real wall target. The old fixed right-side
	// aim made the tee fail every climb whose hookable wall was on the left. Ray
	// cast a small upward fan and use the first actual collision as hook target.
	vec2 HookAim(0.0f, 0.0f);
	const bool NeedsTallClimb = g_Config.m_TcAiBotUseHook && NeedJump && Delta.y < -32.0f;
	if(NeedsTallClimb)
	{
		const float PreferredSide = Input.m_Direction != 0 ? (float)Input.m_Direction : (Delta.x < 0.0f ? -1.0f : 1.0f);
		const std::array<vec2, 8> aHookRays = {
			vec2(PreferredSide * 220.0f, -260.0f),
			vec2(PreferredSide * 140.0f, -330.0f),
			vec2(PreferredSide * 70.0f, -370.0f),
			vec2(-PreferredSide * 220.0f, -260.0f),
			vec2(-PreferredSide * 140.0f, -330.0f),
			vec2(-PreferredSide * 70.0f, -370.0f),
			vec2(300.0f, -180.0f),
			vec2(-300.0f, -180.0f),
		};
		for(const vec2 &Ray : aHookRays)
		{
			vec2 CollisionPos;
			if(Collision()->IntersectLine(Position, Position + Ray, &CollisionPos, nullptr) && distance(Position, CollisionPos) > 24.0f)
			{
				HookAim = CollisionPos - Position;
				break;
			}
		}
	}

	const bool NeedHook = HookAim.x != 0.0f || HookAim.y != 0.0f;
	Input.m_Hook = NeedHook && Tick % 12 < 10 ? 1 : 0;
	if(NeedHook)
	{
		// Moving toward the wall while hooking prevents a vertical climb from
		// becoming an aim-only swing with no horizontal control.
		Input.m_Direction = HookAim.x > 8.0f ? 1 : HookAim.x < -8.0f ? -1 : Input.m_Direction;
		Input.m_TargetX = (int)HookAim.x;
		Input.m_TargetY = (int)HookAim.y;
	}
	return true;
}

void CAIBot::ForceReplan()
{
	m_ForceReplan = true;
	SetStatus("Route rebuild requested");
}

void CAIBot::ClearLearning()
{
	m_FailureCost.clear();
	m_Episodes = 0;
	m_StartCount = 0;
	m_FinishCount = 0;
	m_DeathCount = 0;
	m_RewardedPathSteps = 0;
	m_OffRouteSteps = 0;
	m_SafeFreezeSteps = 0;
	m_RewardNetUpdates = 0;
	m_TotalTrainingReward = 0.0f;
	m_RewardNetEstimate = 0.0f;
	m_aRewardNetWeights.fill(0.0f);
	ResetEpisode();
	SaveLearning();
	SaveStats();
	ForceReplan();
	SetStatus("Reward network and map learning cleared");
}
