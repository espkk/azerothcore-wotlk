//go:build e2e

// Issue 1: the pre-fix matchmaker anchors a rating window on a team that need not be one of
// the two it picks, then tests both candidates against that anchor rather than against each
// other. Two teams sitting on opposite edges of the window are 2x Arena.MaxRatingDifference
// apart and get paired anyway.
//
// Reproducing it needs teams at different matchmaker ratings.
// Arena.ArenaStartMatchmakerRating is read by ArenaTeam::AddMember, and `.reload config`
// re-reads it, so writing the config between team creations gives each team its own rating.
//
// Measured 8 runs each way: the pre-fix worldserver reproduced the 300-apart pair every time,
// the fixed one passed every time.
//
// Requires E2E_WORLDSERVER_CONF pointing at the running worldserver's config file.
package arena_test

import (
	"database/sql"
	"fmt"
	"os"
	"regexp"
	"testing"
	"time"

	_ "github.com/go-sql-driver/mysql"

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

// restoreConfigOnCleanup puts the worldserver config back as it was found, so the ratings this
// test writes do not leak into whatever runs next.
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

// setConfigValue rewrites one `Key = value` line in the live worldserver config and makes the
// server re-read it. Returns once the server has acknowledged the reload.
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

// teamRating reads back the rating the server stored for a freshly created team, so a test
// never silently runs with ratings it did not get.
//
// Matchmaker rating itself cannot be read here: it is only persisted by ArenaTeam::SaveToDB,
// which has not run yet, and is otherwise in memory. But it is seeded by AddMember from
// Arena.ArenaStartMatchmakerRating in the same breath that the team's rating is seeded from
// Arena.ArenaStartRating, and both are picked up by the same `.reload config`. The test sets
// the two configs together, so a matching stored team rating is evidence the reload landed
// and therefore that the matchmaker rating did too.
func teamRating(t *testing.T, db *sql.DB, teamID uint32) uint32 {
	t.Helper()
	var rating uint32
	deadline := time.Now().Add(20 * time.Second)
	for {
		err := db.QueryRow("SELECT rating FROM arena_team WHERE arenaTeamId = ?", teamID).Scan(&rating)
		if err == nil && rating != 0 {
			return rating
		}
		if time.Now().After(deadline) {
			e2eharness.Preconditionf(t, "arena team %d never reported a rating (last %d, err %v)",
				teamID, rating, err)
		}
		time.Sleep(200 * time.Millisecond)
	}
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

	at("lo1").GM(t, ".arena season set state 1")
	at("lo1").GM(t, fmt.Sprintf(".event start %d", arenaTournamentEvent))
	t.Cleanup(func() {
		at("lo1").World.SendGMCommand(fmt.Sprintf(".event stop %d", arenaTournamentEvent))
	})
	restoreConfigOnCleanup(t, at("lo1"))
	setConfigValue(t, at("lo1"), "Arena.MaxRatingDifference", maxRatingDifference)

	suffix := time.Now().UnixNano() % 100000
	type team struct {
		leader string
		other  string
		mmr    int
		id     uint32
	}
	teams := []*team{
		{leader: "lo1", other: "lo2", mmr: 1850},
		{leader: "hi1", other: "hi2", mmr: 2150},
		{leader: "mid1", other: "mid2", mmr: 2000},
	}

	for i, tm := range teams {
		setConfigValue(t, at("lo1"), "Arena.ArenaStartMatchmakerRating", tm.mmr)
		setConfigValue(t, at("lo1"), "Arena.ArenaStartRating", tm.mmr)
		tm.id = setUpTeam(t, at(tm.leader), at(tm.other), fmt.Sprintf("ArG%d%05d", i, suffix))

		if got := teamRating(t, at(tm.leader).CharDB, tm.id); got != uint32(tm.mmr) {
			e2eharness.Preconditionf(t, "team %s was created at rating %d, wanted %d: "+
				"controlling ratings through the config and `.reload config` did not take effect, "+
				"so this run would compare teams that are all at the default rating",
				tm.leader, got, tm.mmr)
		}
		e2eharness.FormParty(t, at(tm.leader), at(tm.other))

		// Invited groups linger in the queue until their last member is gone, so without
		// this a later run finds these teams still waiting and pairs against them.
		teamID := tm.id
		t.Cleanup(func() {
			at("lo1").World.SendGMCommand(fmt.Sprintf(".arena disband %d", teamID))
		})
	}

	spawn := arenaBattlemasterSpawn(t)
	for _, b := range bots {
		e2eharness.GoCreatureGUID(t, b.World, spawn.guid)
		b.FlushWorld(t)
	}

	watchers := map[string]*statusWatcher{}
	for _, tm := range teams {
		w := watchBattlefieldStatus(at(tm.leader))
		defer w.cancel()
		watchers[tm.leader] = w
	}

	// The two edge teams wait first. The middle team queues last so that its rating is what
	// the pre-fix code centres its window on.
	for _, tm := range teams {
		guid := e2eharness.WaitNearbyUnitByEntry(t, at(tm.leader).World, arenaBattlemasterEntry, 20*time.Second)
		sendJoinRatedArena(t, at(tm.leader), guid)
		at(tm.leader).FlushWorld(t)
	}

	var invited []*team
	for _, tm := range teams {
		st, ok := watchers[tm.leader].await(statusWaitJoin, 15*time.Second)
		if ok {
			invited = append(invited, tm)
			t.Logf("INVITED role=%s mmr=%d queueSlot=%d arenaType=%d rated=%v",
				tm.leader, tm.mmr, st.queueSlot, st.arenaType, st.isRated)
		} else {
			t.Logf("NOT INVITED role=%s mmr=%d", tm.leader, tm.mmr)
		}

		// Drain every team, invited or merely queued, before the next run starts. Both
		// members have to leave: an invited group is only erased once its last member is
		// gone, so dropping the leader alone leaves the group sitting in the bracket.
		if last, seen := watchers[tm.leader].last(); seen {
			sendLeaveQueue(t, at(tm.leader), last)
			sendLeaveQueue(t, at(tm.other), last)
			at(tm.leader).FlushWorld(t)
		}
	}

	// Three teams can fill exactly one arena, so exactly two of ours should be invited.
	// More than that means the bracket also held teams from elsewhere, and which of ours
	// faced which can no longer be inferred from the invitations alone.
	if len(invited) > 2 {
		e2eharness.Preconditionf(t, "%d of 3 teams were invited, so the rated bracket was not "+
			"empty when this test started; the pairing cannot be attributed", len(invited))
	}

	for _, a := range invited {
		for _, b := range invited {
			if a == b {
				continue
			}
			gap := a.mmr - b.mmr
			if gap < 0 {
				gap = -gap
			}
			if gap > maxRatingDifference {
				t.Fatalf("started an arena between teams %d and %d, %d apart, with "+
					"Arena.MaxRatingDifference at %d", a.mmr, b.mmr, gap, maxRatingDifference)
			}
		}
	}

	if len(invited) < 2 {
		t.Fatalf("no arena started at all: %d teams invited, so the rating rule was not exercised",
			len(invited))
	}
	t.Logf("PASS %d teams invited, every pair within %d rating", len(invited), maxRatingDifference)
}
