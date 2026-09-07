//go:build e2e

// Rated arena queueing, driven through the real client path: create arena teams, form
// parties, queue at an Arena Battlemaster, and read SMSG_BATTLEFIELD_STATUS back.
package arena_test

import (
	"testing"
	"time"

	_ "github.com/go-sql-driver/mysql"

	"github.com/azerothcore/AzerothGhost/client"
	"github.com/azerothcore/AzerothGhost/e2e/e2eharness"
	"github.com/azerothcore/azerothcore-wotlk/e2e/internal/meta"
)

// drainQueueOnCleanup takes every bot out of its bracket when the test ends.
//
// An invited group is only erased once its last member is gone, so leaving one queued
// pollutes the rated bracket for the next test in this package until the 60 second
// invite expires.
func drainQueueOnCleanup(t *testing.T, bots []*e2eharness.ScenarioBot) {
	t.Helper()
	t.Cleanup(func() {
		for _, b := range bots {
			b.LeaveBattlefieldQueue(t)
		}
	})
}

// A rated 2v2 queue with two eligible teams must pop for both of them.
func TestArena_RatedQueuePopsForBothTeams(t *testing.T) {
	meta.Begin(t, meta.TestMeta{
		Tags:     []string{"pvp", "arena", "med", "serial"},
		Runtime:  "med",
		Category: "pvp/arena",
	})

	bots := e2eharness.NewScenario(t, e2eharness.ScenarioOpts{
		Prefix: "ArQ",
		Bots: []e2eharness.BotSpec{
			{Role: "a1", Level: 80}, {Role: "a2", Level: 80},
			{Role: "b1", Level: 80}, {Role: "b2", Level: 80},
		},
	})
	a1 := e2eharness.ByRole(t, bots, "a1")
	a2 := e2eharness.ByRole(t, bots, "a2")
	b1 := e2eharness.ByRole(t, bots, "b1")
	b2 := e2eharness.ByRole(t, bots, "b2")

	e2eharness.EnableArenaSeason(t, a1)
	drainQueueOnCleanup(t, bots)

	e2eharness.CreateArenaTeam(t, a1, a2, e2eharness.UniqueArenaTeamName("ArQA"), client.ArenaTeam2v2)
	e2eharness.CreateArenaTeam(t, b1, b2, e2eharness.UniqueArenaTeamName("ArQB"), client.ArenaTeam2v2)
	e2eharness.FormParty(t, a1, a2)
	e2eharness.FormParty(t, b1, b2)

	battlemaster := e2eharness.TeleportToArenaBattlemaster(t, bots...)

	a1.JoinRatedArena(t, battlemaster, client.ArenaSlot2v2)
	b1.JoinRatedArena(t, battlemaster, client.ArenaSlot2v2)

	// A refused rated join is answered with no WAIT_QUEUE, and this waiter names the reason.
	for _, leader := range []*e2eharness.ScenarioBot{a1, b1} {
		leader.WaitBattlefieldStatus(t, client.BattlegroundStatusWaitQueue, 0)
	}

	// Oracle: the matchmaker pairs the two teams and invites both.
	gotA, okA := a1.TryWaitBattlefieldStatus(client.BattlegroundStatusWaitJoin, 30*time.Second)
	gotB, okB := b1.TryWaitBattlefieldStatus(client.BattlegroundStatusWaitJoin, 30*time.Second)
	if !okA || !okB {
		e2eharness.Assertf(t, "rated arena never popped for both teams: a1=%v b1=%v", okA, okB)
	}
	if !gotA.IsRated || !gotB.IsRated {
		e2eharness.Assertf(t, "invite was not flagged rated: a1=%v b1=%v", gotA.IsRated, gotB.IsRated)
	}
	if gotA.ArenaType != client.ArenaTeam2v2 || gotB.ArenaType != client.ArenaTeam2v2 {
		e2eharness.Assertf(t, "invite was not for 2v2: a1=%d b1=%d", gotA.ArenaType, gotB.ArenaType)
	}
	if gotA.MapID != gotB.MapID {
		e2eharness.Assertf(t, "teams were invited to different arena maps: a1=%d b1=%d", gotA.MapID, gotB.MapID)
	}

	t.Logf("PASS rated 2v2 popped for both teams, arenaType=%d map=%d", gotA.ArenaType, gotA.MapID)
}
