#include "aibot.h"

#include <base/log.h>
#include <base/system.h>

#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/shared/linereader.h>
#include <engine/storage.h>
#include <engine/textrender.h>

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
	m_RouteUsesFreeze = false;
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
	m_RouteUsesFreeze = false;
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
	const std::vector<int> &vProgressRoute = m_RaceStarted && !m_vRewardRoute.empty() ? m_vRewardRoute : m_vRoute;
	if(vProgressRoute.size() < 2 || m_LastRewardRouteIndex < 0)
		return 0.0f;
	return 100.0f * std::clamp((float)m_LastRewardRouteIndex / (float)(vProgressRoute.size() - 1), 0.0f, 1.0f);
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
	return IsPathable(Cell, g_Config.m_TcAiBotAllowFreeze != 0);
}

bool CAIBot::IsPathable(int Cell, bool AllowFreeze) const
{
	if(IsSolid(Cell) || IsDeath(Cell) || IsDeepFreeze(Cell))
		return false;
	if(IsFreeze(Cell) && (!AllowFreeze || !CanSurviveFreeze(Cell)))
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

int CAIBot::RewardRouteIndexForCell(int Cell) const
{
	if(Cell < 0 || m_vRewardRoute.empty())
		return -1;

	// Physics does not move one tile at a time. Match the nearest node in a
	// small one-tile radius so a normal jump or fast horizontal move still gets
	// credit for its actual START -> FINISH progress.
	const int CellX = Cell % m_MapWidth;
	const int CellY = Cell / m_MapWidth;
	int BestIndex = -1;
	int BestDistance = 2;
	for(int Index = 0; Index < (int)m_vRewardRoute.size(); ++Index)
	{
		const int RouteCell = m_vRewardRoute[Index];
		const int Distance = std::abs(RouteCell % m_MapWidth - CellX) + std::abs(RouteCell / m_MapWidth - CellY);
		if(Distance < BestDistance || (Distance == BestDistance && BestIndex >= 0 && Index > BestIndex))
		{
			BestDistance = Distance;
			BestIndex = Index;
		}
	}
	return BestIndex;
}

bool CAIBot::BuildRoute(int StartCell, int GoalCell, const char *pGoalName, bool AllowFreeze)
{
	m_GoalCell = GoalCell;
	if(GoalCell < 0)
	{
		m_vRoute.clear();
		m_RouteUsesFreeze = false;
		str_format(m_aStatus, sizeof(m_aStatus), "No %s tile on this map", pGoalName);
		return false;
	}
	if(StartCell < 0 || !IsPathable(StartCell, AllowFreeze))
	{
		m_vRoute.clear();
		m_RouteUsesFreeze = false;
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
			if(!IsPathable(Next, AllowFreeze))
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
		m_RouteUsesFreeze = false;
		str_format(m_aStatus, sizeof(m_aStatus), "A* stopped after %d nodes", Expanded);
		return false;
	}

	m_vRoute.clear();
	for(int Cell = GoalCell; Cell != -1; Cell = vPrevious[Cell])
	m_vRoute.push_back(Cell);
	std::reverse(m_vRoute.begin(), m_vRoute.end());
	m_RouteUsesFreeze = std::any_of(m_vRoute.begin(), m_vRoute.end(), [this](int Cell) { return IsFreeze(Cell); });
	if(m_RaceStarted && m_vRewardRoute.empty())
		m_vRewardRoute = m_vRoute;
	m_RouteIndex = 0;
	str_format(m_aStatus, sizeof(m_aStatus), AllowFreeze ? "A* to %s: safe-freeze fallback, %d nodes" : "A* to %s: freeze-free, %d nodes", pGoalName, (int)m_vRoute.size());
	return true;
}

void CAIBot::ResetEpisode()
{
	m_vRewardRoute.clear();
	m_RouteUsesFreeze = false;
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
	m_ResetRequestedTick = -1000000;
	m_ResetRetries = 0;
	m_ResetSawNoCharacter = false;
}

bool CAIBot::HandleFreeze(int CurrentCell, int Tick)
{
	if(!g_Config.m_TcAiBotResetUnsafeFreeze || m_ResetRequested)
		return m_ResetRequested;

	const bool DeepFreeze = IsDeepFreeze(CurrentCell);
	const bool InFreeze = IsFreeze(CurrentCell) || DeepFreeze;
	if(!InFreeze)
	{
		// A tee that actually made it through a freeze strip must never be
		// punished later by an old timer from the strip it already escaped.
		m_UnsafeFreezeSinceTick = -1;
		return false;
	}

	if(m_UnsafeFreezeSinceTick < 0)
	{
		m_UnsafeFreezeSinceTick = Tick;
		SetStatus(DeepFreeze ? "Deep freeze detected, preparing reset" : "Freeze detected, checking escape route");
	}

	// A solid platform below a normal freeze is only a *possible* escape: the
	// tee still has to fall through the strip. The previous code treated it as
	// safe forever, so a tee stuck in that strip was never reset. Give it a
	// short grace period; if it is still in freeze, restart it normally.
	const bool HasSafeDrop = !DeepFreeze && CanSurviveFreeze(CurrentCell);
	const int ResetDelay = HasSafeDrop ? g_Config.m_TcAiBotSafeFreezeEscapeDelay : g_Config.m_TcAiBotUnsafeFreezeDelay;
	if(Tick - m_UnsafeFreezeSinceTick < ResetDelay)
		return false;

	// Record exactly one failure before asking the server for a normal kill.
	// The no-character handler skips a duplicate penalty while this reset is in flight.
	RegisterFailure();
	m_ResetRequested = true;
	m_ResetRequestedTick = Tick;
	m_ResetRetries = 0;
	m_ResetSawNoCharacter = false;
	GameClient()->SendKill();
	SetStatus(DeepFreeze ? "Deep freeze: reset requested and learned" : "Freeze escape failed: reset requested and learned");
	return true;
}

void CAIBot::FinishResetAtSpawn(int Tick)
{
	ResetEpisode();
	m_vRoute.clear();
	m_RouteIndex = 0;
	m_LastPlanTick = Tick - 10;
	m_LastRewardCell = -1;
	m_LastRewardRouteIndex = -1;
	m_ForceReplan = true;
	SetStatus("Respawn confirmed: rebuilding route");
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
	const std::vector<int> &vProgressRoute = m_RaceStarted && !m_vRewardRoute.empty() ? m_vRewardRoute : m_vRoute;
	if(RouteIndex >= 0 && vProgressRoute.size() > 1)
		Progress = std::clamp((float)RouteIndex / (float)(vProgressRoute.size() - 1), 0.0f, 1.0f);

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
	const int RouteIndex = m_RaceStarted ? RewardRouteIndexForCell(CurrentCell) : RouteIndexForCell(CurrentCell);
	const bool OnRoute = RouteIndex >= 0;

	if(m_FinishCrossed)
		return;

	// A race has two deliberately separate scales:
	// - before START: -100 at the far end of the A* route, increasing to 0;
	// - after START: 0, then +0..+100 by fixed START -> FINISH route percent.
	// The visible reward is therefore never deducted for valid forward movement.
	if(!m_RaceStarted)
	{
		if(Start)
		{
			m_RaceStarted = true;
			m_vRewardRoute.clear();
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

		if(OnRoute && m_vRoute.size() > 1)
		{
			const float Progress = std::clamp((float)RouteIndex / (float)(m_vRoute.size() - 1), 0.0f, 1.0f);
			const float NewReward = -100.0f * (1.0f - Progress);
			const float Delta = NewReward - m_CurrentReward;
			m_CurrentReward = NewReward;
			m_LastTrainingReward = Delta > 0.0f ? Delta : Delta < 0.0f ? -1.0f : 0.0f;
			m_LastRewardRouteIndex = RouteIndex;
			m_RouteIndex = std::max(m_RouteIndex, RouteIndex);
		}
		else
		{
			m_CurrentReward = -100.0f;
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
		m_LastTrainingReward = std::max(20.0f, 100.0f - m_CurrentReward);
		m_CurrentReward = 100.0f;
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

	if(OnRoute && m_vRewardRoute.size() > 1)
	{
		const float Progress = std::clamp((float)RouteIndex / (float)(m_vRewardRoute.size() - 1), 0.0f, 1.0f);
		const float NewReward = 100.0f * Progress;
		const float Gain = std::max(0.0f, NewReward - m_CurrentReward);
		m_CurrentReward = std::max(m_CurrentReward, NewReward);
		m_LastTrainingReward = Gain;
		if(Gain > 0.0f)
		{
			++m_RewardedPathSteps;
			if(IsFreeze(CurrentCell))
				++m_SafeFreezeSteps;
		}

		m_LastRewardRouteIndex = std::max(m_LastRewardRouteIndex, RouteIndex);
		m_BestRaceProgress = std::max(m_BestRaceProgress, Progress);
		m_BestRaceReward = std::max(m_BestRaceReward, m_CurrentReward);
	}
	else
	{
		// Leaving the reward route is bad training data, but it must not make
		// the displayed post-START reward negative or erase earned progress.
		m_LastTrainingReward = -2.0f;
		++m_OffRouteSteps;
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
	// A failed run is negative training data, but the visible post-START scale
	// remains 0..100. The next spawn starts the normal pre-START scale again.
	if(m_RaceStarted)
		m_CurrentReward = std::max(0.0f, m_CurrentReward);
	else
		m_CurrentReward -= 2.0f;
	m_TotalTrainingReward += m_LastTrainingReward;
	if(m_LastProgressCell >= 0)
	{
		++m_FailureCost[m_LastProgressCell];
		const int RouteIndex = m_RaceStarted ? RewardRouteIndexForCell(m_LastProgressCell) : RouteIndexForCell(m_LastProgressCell);
		TrainRewardNet(m_LastTrainingReward, RewardFeatures(m_LastProgressCell, RouteIndex, false, false, false));
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
	// Builds before the fixed 0..100 scale could persist rewards above 100.
	// Keep their failure memory and counters, but migrate display statistics.
	m_BestRaceProgress = std::clamp(m_BestRaceProgress, 0.0f, 1.0f);
	m_BestRaceReward = std::clamp(m_BestRaceReward, 0.0f, 100.0f);
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

void CAIBot::RenderHud() const
{
	if(!g_Config.m_TcAiBotHud || !m_aMapName[0])
		return;

	// The regular HUD uses a 300-unit high screen. Rendering in the same space
	// makes this panel scale with every resolution and avoids covering the DDRace
	// information on the right side of the screen.
	const float Height = 300.0f;
	const float Width = Height * Graphics()->ScreenAspect();
	Graphics()->MapScreen(0.0f, 0.0f, Width, Height);

	const float X = 5.0f;
	const float Y = 54.0f;
	const float BoxWidth = 112.0f;
	const float BoxHeight = 54.0f;
	const float FontSize = 5.0f;
	const float LineHeight = 7.0f;
	Graphics()->DrawRect(X, Y, BoxWidth, BoxHeight, ColorRGBA(0.0f, 0.0f, 0.0f, 0.58f), IGraphics::CORNER_ALL, 4.0f);

	TextRender()->TextColor(g_Config.m_TcAiBot ? ColorRGBA(0.45f, 1.0f, 0.55f, 1.0f) : ColorRGBA(1.0f, 0.75f, 0.25f, 1.0f));
	TextRender()->Text(X + 4.0f, Y + 3.0f, FontSize, g_Config.m_TcAiBot ? "AIBot  ON" : "AIBot  OFF", -1.0f);
	TextRender()->TextColor(TextRender()->DefaultTextColor());

	char aLine[128];
	float LineY = Y + 11.0f;
	const auto RenderLine = [&](const char *pText) {
		TextRender()->Text(X + 4.0f, LineY, FontSize, pText, -1.0f);
		LineY += LineHeight;
	};
	str_format(aLine, sizeof(aLine), "Phase: %s  route: %.1f%%", PhaseName(), RouteProgressPercent());
	RenderLine(aLine);
	str_format(aLine, sizeof(aLine), "Reward: %.1f  best: %.1f", CurrentReward(), BestRaceReward());
	RenderLine(aLine);
	str_format(aLine, sizeof(aLine), "Best route: %.1f%%", BestRaceProgressPercent());
	RenderLine(aLine);
	str_format(aLine, sizeof(aLine), "Attempts: %d  deaths: %d", Episodes(), DeathCount());
	RenderLine(aLine);
	str_format(aLine, sizeof(aLine), "Memory: %d tiles  NN: %d", LearnedFailures(), RewardNetUpdates());
	RenderLine(aLine);
	if(m_ResetRequested)
		str_format(aLine, sizeof(aLine), "Reset pending: retry %d", m_ResetRetries);
	else
		str_format(aLine, sizeof(aLine), m_RouteUsesFreeze ? "A*: freeze fallback  off: %d" : "A*: avoids freeze  off: %d", OffRouteSteps());
	RenderLine(aLine);

	TextRender()->TextColor(TextRender()->DefaultTextColor());
}

void CAIBot::OnRender()
{
	if(!EnsureMap())
		return;
	if(!g_Config.m_TcAiBot)
		return;

	const CNetObj_Character *pCharacter = GameClient()->m_Snap.m_pLocalCharacter;
	if(!pCharacter)
	{
		if(m_HadLocalCharacter)
		{
			if(m_ResetRequested)
			{
				// Keep the reset request alive until the fresh spawn is observed.
				// Some servers respawn instantly and never expose this state, which
				// is handled below as well.
				m_ResetSawNoCharacter = true;
			}
			else if(m_FinishCrossed)
				SaveStats();
			else
				RegisterFailure();
			if(!m_ResetRequested)
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
	if(m_ResetRequested)
	{
		const bool SpawnIsOutsideFreeze = !IsFreeze(CurrentCell) && !IsDeepFreeze(CurrentCell);
		if(m_ResetSawNoCharacter || SpawnIsOutsideFreeze)
		{
			// DDNet servers can replace the character within one snapshot. Accept a
			// fresh safe position as a respawn too, otherwise the old pending flag
			// would permanently suppress ApplyInput after a successful kill.
			FinishResetAtSpawn(Tick);
		}
		else if(Tick - m_ResetRequestedTick >= 25)
		{
			// Keep trying in case the server ignored one packet. Do not train a
			// second failure for a single freeze incident.
			GameClient()->SendKill();
			m_ResetRequestedTick = Tick;
			++m_ResetRetries;
			str_format(m_aStatus, sizeof(m_aStatus), "Reset still pending, retry %d", m_ResetRetries);
			return;
		}
		else
		{
			return;
		}
	}
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
		// Safety comes first: a normal A* plan may not use any freeze tile. A
		// survivable freeze is allowed only as a fallback for maps where there is
		// no way to reach the goal without one.
		if(!BuildRoute(CurrentCell, GoalCell, pGoalName, false) && g_Config.m_TcAiBotAllowFreeze)
			BuildRoute(CurrentCell, GoalCell, pGoalName, true);
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
