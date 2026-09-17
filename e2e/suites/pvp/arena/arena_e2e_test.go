//go:build e2e

package arena_test

import (
	"os"
	"strconv"
	"testing"

	_ "github.com/go-sql-driver/mysql"

	"github.com/azerothcore/AzerothGhost/client"
	"github.com/azerothcore/AzerothGhost/e2e/e2eharness"
	"github.com/azerothcore/azerothcore-wotlk/e2e/internal/meta"
)

// ratedTeam is a 2v2 arena team whose members are also in a party, which a rated join needs.
type ratedTeam struct {
	name   string
	leader *e2eharness.ScenarioBot
}

func newRatedTeam(t *testing.T, namePrefix string, leader, member *e2eharness.ScenarioBot) *ratedTeam {
	t.Helper()
	name := e2eharness.UniqueArenaTeamName(namePrefix)
	e2eharness.CreateArenaTeam(t, leader, member, name, client.ArenaTeam2v2)
	e2eharness.FormParty(t, leader, member)
	return &ratedTeam{name: name, leader: leader}
}

// queue joins the rated 2v2 bracket and returns once the team is in it.
func (tm *ratedTeam) queue(t *testing.T, battlemasterGUID uint64) {
	t.Helper()
	tm.leader.JoinRatedArena(t, battlemasterGUID, client.ArenaSlot2v2)
	tm.leader.WaitBattlefieldStatus(t, client.BattlegroundStatusWaitQueue, 0)
}

// waitInvites reports, per team, whether the matchmaker started an arena for it.
func waitInvites(teams []*ratedTeam) ([]client.BattlefieldStatus, []bool) {
	leaders := make([]*e2eharness.ScenarioBot, len(teams))
	for i, tm := range teams {
		leaders[i] = tm.leader
	}
	return e2eharness.TryWaitBattlefieldStatusEach(
		leaders, client.BattlegroundStatusWaitJoin, e2eharness.DefaultBattlefieldTimeout)
}

// A rated 2v2 queue with two eligible teams must pop for both of them.
func TestArena_RatedQueuePopsForBothTeams(t *testing.T) {
	meta.Begin(t, meta.TestMeta{
		Tags:     []string{"pvp", "arena", "med", "multi_bot", "serial"},
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
	at := func(role string) *e2eharness.ScenarioBot { return e2eharness.ByRole(t, bots, role) }

	e2eharness.EnableArenaSeason(t, at("a1"))

	teamA := newRatedTeam(t, "ArQA", at("a1"), at("a2"))
	teamB := newRatedTeam(t, "ArQB", at("b1"), at("b2"))

	battlemaster := e2eharness.TeleportToArenaBattlemaster(t, bots...)
	teamA.queue(t, battlemaster)
	teamB.queue(t, battlemaster)

	got, ok := waitInvites([]*ratedTeam{teamA, teamB})
	gotA, gotB, okA, okB := got[0], got[1], ok[0], ok[1]
	if !okA || !okB {
		e2eharness.Assertf(t, "rated arena did not pop for both teams: a1=%v b1=%v. If only one "+
			"did, check that the realm is exclusive", okA, okB)
	}
	// Different maps mean each team was paired with one already in the bracket.
	if gotA.MapID != gotB.MapID {
		e2eharness.Preconditionf(t, "the teams were invited to different maps (a1=%d b1=%d), so "+
			"the rated bracket was not empty", gotA.MapID, gotB.MapID)
	}
	if !gotA.IsRated || !gotB.IsRated {
		e2eharness.Assertf(t, "invite was not flagged rated: a1=%v b1=%v", gotA.IsRated, gotB.IsRated)
	}
	if gotA.ArenaType != client.ArenaTeam2v2 || gotB.ArenaType != client.ArenaTeam2v2 {
		e2eharness.Assertf(t, "invite was not for 2v2: a1=%d b1=%d", gotA.ArenaType, gotB.ArenaType)
	}

	t.Logf("PASS rated 2v2 popped for both teams, arenaType=%d map=%d", gotA.ArenaType, gotA.MapID)
}

// maxRatingDifferenceEnv overrides Arena.MaxRatingDifference. Launch worldserver with the
// same value.
const maxRatingDifferenceEnv = "AC_ARENA_MAX_RATING_DIFFERENCE"

// distMaxRatingDifference is Arena.MaxRatingDifference in worldserver.conf.dist.
const distMaxRatingDifference = 150

const ratingBase = 1500

// maxRatingDifference returns the window the realm is taken to run with.
func maxRatingDifference(t *testing.T) int {
	t.Helper()
	raw := os.Getenv(maxRatingDifferenceEnv)
	if raw == "" {
		return distMaxRatingDifference
	}
	d, err := strconv.Atoi(raw)
	if err != nil || d <= 0 {
		e2eharness.Preconditionf(t, "%s=%q is not a positive integer", maxRatingDifferenceEnv, raw)
	}
	return d
}

// PR: https://github.com/azerothcore/azerothcore-wotlk/pull/27548
// Two teams must never be started against each other more than Arena.MaxRatingDifference
// apart. Issue 1 of that PR: two teams two windows apart were paired because both fit the
// window of a third team that queued last.
func TestAC_27548_PairNeverWiderThanMaxRatingDifference(t *testing.T) {
	meta.Begin(t, meta.TestMeta{
		Tags:     []string{"pvp", "arena", "med", "issue", "multi_bot", "serial"},
		Runtime:  "med",
		Issue:    27548,
		Category: "pvp/arena",
	})

	maxDiff := maxRatingDifference(t)

	bots := e2eharness.NewScenario(t, e2eharness.ScenarioOpts{
		Prefix: "ArG",
		Bots: []e2eharness.BotSpec{
			{Role: "lo1", Level: 80}, {Role: "lo2", Level: 80},
			{Role: "hi1", Level: 80}, {Role: "hi2", Level: 80},
			{Role: "mid1", Level: 80}, {Role: "mid2", Level: 80},
		},
	})
	at := func(role string) *e2eharness.ScenarioBot { return e2eharness.ByRole(t, bots, role) }

	e2eharness.EnableArenaSeason(t, at("lo1"))

	type teamAtRating struct {
		*ratedTeam
		rating int
	}
	newTeam := func(namePrefix string, rating int, leader, member string) teamAtRating {
		// Before the team exists: the rating is read once, as a member joins.
		for _, role := range []string{leader, member} {
			e2eharness.SeedMatchmakerRating(t, at(role), client.ArenaTeam2v2, rating)
		}
		return teamAtRating{newRatedTeam(t, namePrefix, at(leader), at(member)), rating}
	}
	// lo and hi are two windows apart, and both are within one window of mid.
	lo := newTeam("ArGlo", ratingBase, "lo1", "lo2")
	hi := newTeam("ArGhi", ratingBase+2*maxDiff, "hi1", "hi2")
	mid := newTeam("ArGmid", ratingBase+maxDiff, "mid1", "mid2")

	battlemaster := e2eharness.TeleportToArenaBattlemaster(t, bots...)

	type invitedTeam struct {
		teamAtRating
		mapID uint32
	}
	collect := func(of ...teamAtRating) []invitedTeam {
		plain := make([]*ratedTeam, len(of))
		for i, tm := range of {
			plain[i] = tm.ratedTeam
		}
		sts, oks := waitInvites(plain)

		var out []invitedTeam
		for i, tm := range of {
			if oks[i] {
				out = append(out, invitedTeam{teamAtRating: tm, mapID: sts[i].MapID})
				t.Logf("INVITED %s rating=%d map=%d rated=%v", tm.name, tm.rating, sts[i].MapID, sts[i].IsRated)
			} else {
				t.Logf("NOT INVITED %s rating=%d", tm.name, tm.rating)
			}
		}
		return out
	}

	// lo and hi queue alone first, where the realm has to refuse to pair them.
	lo.queue(t, battlemaster)
	hi.queue(t, battlemaster)

	if early := collect(lo, hi); len(early) > 0 {
		if len(early) == 2 && early[0].mapID == early[1].mapID {
			e2eharness.Preconditionf(t, "the realm paired %d with %d before the third team queued, "+
				"so its window is wider than %d. Export its value as %s",
				lo.rating, hi.rating, maxDiff, maxRatingDifferenceEnv)
		}
		e2eharness.Preconditionf(t, "%d of the 2 edge teams were invited before the third queued, "+
			"so the rated bracket was not empty", len(early))
	}
	t.Logf("the realm refused %d against %d, %d apart", lo.rating, hi.rating, 2*maxDiff)

	mid.queue(t, battlemaster)
	invited := collect(lo, hi, mid)

	switch len(invited) {
	case 0:
		e2eharness.Preconditionf(t, "no arena started with a team one window from each edge, so "+
			"the realm's window is narrower than %d. Export its value as %s", maxDiff, maxRatingDifferenceEnv)
	case 1:
		e2eharness.Preconditionf(t, "only %s (rating %d) was invited, so it was paired with a team "+
			"already in the bracket", invited[0].name, invited[0].rating)
	case 2:
	default:
		e2eharness.Preconditionf(t, "%d of 3 teams were invited, so the rated bracket also held "+
			"other teams", len(invited))
	}

	if invited[0].mapID != invited[1].mapID {
		e2eharness.Preconditionf(t, "the two invited teams are on different maps (%d and %d), so "+
			"the rated bracket was not empty", invited[0].mapID, invited[1].mapID)
	}

	gap := invited[0].rating - invited[1].rating
	if gap < 0 {
		gap = -gap
	}
	if gap > maxDiff {
		e2eharness.ConfirmedBugf(t, 27548, "the realm refused %d against %d alone, then paired them once %d "+
			"queued: %d apart with a %d window", lo.rating, hi.rating, mid.rating, gap, maxDiff)
	}

	t.Logf("PASS %d and %d were paired, %d apart, within %d",
		invited[0].rating, invited[1].rating, gap, maxDiff)
}
