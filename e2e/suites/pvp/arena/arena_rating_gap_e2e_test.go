//go:build e2e

// Issue 1: the pre-fix matchmaker anchored a rating window on a team that need not be
// one of the two it picked, then tested both candidates against that anchor rather than
// against each other. Two teams on opposite edges are 2x Arena.MaxRatingDifference apart
// and were paired anyway.
//
// Reproducing it needs teams at different matchmaker ratings, and nothing exposes that:
// there is no GM command to set a team's rating, and writing arena_team directly does
// not reach the team already live in sArenaTeamMgr. Arena.ArenaStartMatchmakerRating is
// read by ArenaTeam::AddMember and re-read by `.reload config`, so rewriting the live
// config between team creations is the only lever. Requires E2E_WORLDSERVER_CONF.
//
// Measured 8 runs each way: the pre-fix worldserver paired the 300-apart teams every
// time, the fixed one never did.
package arena_test

import (
	"fmt"
	"os"
	"regexp"
	"testing"

	_ "github.com/go-sql-driver/mysql"

	"github.com/azerothcore/AzerothGhost/client"
	"github.com/azerothcore/AzerothGhost/e2e/e2eharness"
	"github.com/azerothcore/azerothcore-wotlk/e2e/internal/meta"
)

const maxRatingDifference = 150

func worldserverConfPath(t *testing.T) string {
	t.Helper()
	path := os.Getenv("E2E_WORLDSERVER_CONF")
	if path == "" {
		t.Skip("E2E_WORLDSERVER_CONF not set; this test drives worldserver config to control ratings")
	}
	return path
}

// restoreConfigOnCleanup puts the worldserver config back as it was found, so the
// ratings this test writes do not leak into whatever runs next.
func restoreConfigOnCleanup(t *testing.T, bot *e2eharness.ScenarioBot) {
	t.Helper()
	path := worldserverConfPath(t)
	raw, err := os.ReadFile(path)
	if err != nil {
		e2eharness.Preconditionf(t, "read %s: %v", path, err)
	}
	t.Cleanup(func() {
		if err := os.WriteFile(path, raw, 0o644); err != nil {
			t.Logf("restore %s: %v", path, err)
			return
		}
		bot.World.SendGMCommand(".reload config")
	})
}

// setConfigValue rewrites one `Key = value` line in the live worldserver config and
// makes the server re-read it. Returns once the server has acknowledged the reload.
func setConfigValue(t *testing.T, bot *e2eharness.ScenarioBot, key string, value int) {
	t.Helper()
	path := worldserverConfPath(t)

	raw, err := os.ReadFile(path)
	if err != nil {
		e2eharness.Preconditionf(t, "read %s: %v", path, err)
	}

	re := regexp.MustCompile(`(?m)^` + regexp.QuoteMeta(key) + `\s*=.*$`)
	line := fmt.Sprintf("%s = %d", key, value)
	out := re.ReplaceAllString(string(raw), line)
	if out == string(raw) && !re.MatchString(string(raw)) {
		out = string(raw) + "\n" + line + "\n"
	}
	if err := os.WriteFile(path, []byte(out), 0o644); err != nil {
		e2eharness.Preconditionf(t, "write %s: %v", path, err)
	}

	bot.GM(t, ".reload config")
	bot.FlushWorld(t)
}

// Two teams must never be started against each other more than Arena.MaxRatingDifference
// apart. The pre-fix selection pairs 1850 against 2150, twice the configured gap, because
// both merely fit a window centred on the 2000 team that queued last.
func TestArena_PairIsNeverWiderThanMaxRatingDifference(t *testing.T) {
	meta.Begin(t, meta.TestMeta{
		Tags:     []string{"pvp", "arena", "med", "serial"},
		Runtime:  "med",
		Category: "pvp/arena",
	})

	bots := e2eharness.NewScenario(t, e2eharness.ScenarioOpts{
		Prefix: "ArG",
		Bots: []e2eharness.BotSpec{
			{Role: "lo1", Level: 80}, {Role: "lo2", Level: 80}, // 1850
			{Role: "hi1", Level: 80}, {Role: "hi2", Level: 80}, // 2150
			{Role: "mid1", Level: 80}, {Role: "mid2", Level: 80}, // 2000, queues last
		},
	})
	at := func(n string) *e2eharness.ScenarioBot { return e2eharness.ByRole(t, bots, n) }

	e2eharness.EnableArenaSeason(t, at("lo1"))
	drainQueueOnCleanup(t, bots)
	restoreConfigOnCleanup(t, at("lo1"))
	setConfigValue(t, at("lo1"), "Arena.MaxRatingDifference", maxRatingDifference)

	type team struct {
		leader string
		mmr    int
	}
	teams := []*team{
		{leader: "lo1", mmr: 1850},
		{leader: "hi1", mmr: 2150},
		{leader: "mid1", mmr: 2000},
	}
	others := map[string]string{"lo1": "lo2", "hi1": "hi2", "mid1": "mid2"}

	for _, tm := range teams {
		// Matchmaker rating is what pairs teams, but it only reaches the DB after a match,
		// so the team rating is set to the same value and read back as the evidence that
		// the reload landed. Which of the two start-rating keys the team rating comes from
		// depends on the season, so both are written.
		setConfigValue(t, at("lo1"), "Arena.ArenaStartMatchmakerRating", tm.mmr)
		setConfigValue(t, at("lo1"), "Arena.ArenaStartRating", tm.mmr)
		setConfigValue(t, at("lo1"), "Arena.LegacyArenaStartRating", tm.mmr)

		id := e2eharness.CreateArenaTeam(t, at(tm.leader), at(others[tm.leader]),
			e2eharness.UniqueArenaTeamName("ArG"+tm.leader), client.ArenaTeam2v2)

		if got := e2eharness.ArenaTeamRating(t, at(tm.leader).CharDB, id); got != uint32(tm.mmr) {
			e2eharness.Preconditionf(t, "team %s was created at rating %d, wanted %d: driving "+
				"ratings through the config did not take effect, so this run would compare "+
				"teams that are all at the default rating", tm.leader, got, tm.mmr)
		}
		e2eharness.FormParty(t, at(tm.leader), at(others[tm.leader]))
	}

	battlemaster := e2eharness.TeleportToArenaBattlemaster(t, bots...)

	// The two edge teams wait first. The middle team queues last so that its rating is
	// what the pre-fix code centres its window on.
	for _, tm := range teams {
		at(tm.leader).JoinRatedArena(t, battlemaster, client.ArenaSlot2v2)
		at(tm.leader).WaitBattlefieldStatus(t, client.BattlegroundStatusWaitQueue, 0)
	}

	var invited []*team
	for _, tm := range teams {
		if st, ok := at(tm.leader).TryWaitBattlefieldStatus(
			client.BattlegroundStatusWaitJoin, e2eharness.DefaultBattlefieldTimeout); ok {
			invited = append(invited, tm)
			t.Logf("INVITED %s mmr=%d map=%d rated=%v", tm.leader, tm.mmr, st.MapID, st.IsRated)
		} else {
			t.Logf("NOT INVITED %s mmr=%d", tm.leader, tm.mmr)
		}
	}

	// Three teams fill exactly one arena, so exactly two of ours should be invited. More
	// than that means the bracket also held teams from elsewhere and which of ours faced
	// which can no longer be inferred from the invitations alone.
	if len(invited) > 2 {
		e2eharness.Preconditionf(t, "%d of 3 teams were invited, so the rated bracket was not "+
			"empty when this test started; the pairing cannot be attributed", len(invited))
	}
	if len(invited) < 2 {
		e2eharness.Assertf(t, "no arena started at all: %d teams invited, so the rating rule "+
			"was never exercised", len(invited))
	}

	gap := invited[0].mmr - invited[1].mmr
	if gap < 0 {
		gap = -gap
	}
	if gap > maxRatingDifference {
		e2eharness.Assertf(t, "started an arena between teams %d and %d, %d apart, with "+
			"Arena.MaxRatingDifference at %d", invited[0].mmr, invited[1].mmr, gap, maxRatingDifference)
	}

	t.Logf("PASS %d and %d were paired, %d apart, within %d",
		invited[0].mmr, invited[1].mmr, gap, maxRatingDifference)
}
