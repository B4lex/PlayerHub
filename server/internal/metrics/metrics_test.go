package metrics

import (
	"context"
	"io"
	"log/slog"
	"testing"
	"time"
)

func openStore(t *testing.T) *Store {
	t.Helper()
	s, err := Open("", 365*24*time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { s.Close() })
	return s
}

func at(day, hour, minute int) time.Time {
	return time.Date(2026, time.November, day, hour, minute, 0, 0, time.UTC)
}

type span struct {
	session    uint64
	start, end time.Time
	reason     string
}

func writeConnections(t *testing.T, s *Store, spans []span) {
	t.Helper()
	var events []event
	for i, c := range spans {
		id := uint64(i + 1)
		events = append(events, event{id: id, session: c.session, displayName: "player", at: c.start})
		if c.reason != "" {
			events = append(events, event{id: id, at: c.end, reason: c.reason})
		}
	}
	if err := s.writeEvents(events); err != nil {
		t.Fatal(err)
	}
}

func TestReportPlayers(t *testing.T) {
	s := openStore(t)
	s.createdAt = at(1, 0, 0)
	if err := s.AddPlayers(map[uint64]time.Time{3: time.Date(2026, time.October, 1, 0, 0, 0, 0, time.UTC)}); err != nil {
		t.Fatal(err)
	}
	writeConnections(t, s, []span{
		{1, at(8, 10, 0), at(8, 11, 0), ReasonLeft},
		// Within the gap, so one play session with the one before.
		{1, at(8, 11, 3), at(8, 11, 30), ReasonDropped},
		{1, at(9, 20, 0), time.Time{}, ""},
		{2, at(9, 9, 0), at(9, 9, 10), ReasonKicked},
		{3, at(10, 8, 0), at(10, 9, 0), ReasonTimeout},
	})
	report, err := s.Report("7d", time.UTC, at(10, 12, 0))
	if err != nil {
		t.Fatal(err)
	}
	if report.FromMs != at(4, 0, 0).UnixMilli() || len(report.Coarse.AtMs) != 7 {
		t.Fatalf("range starts %v with %d days", time.UnixMilli(report.FromMs).UTC(), len(report.Coarse.AtMs))
	}
	summary := report.Summary
	if summary.UniquePlayers != 3 || summary.NewPlayers != 2 || summary.ReturningPlayers != 1 {
		t.Fatalf("unique %d, new %d, returning %d", summary.UniquePlayers, summary.NewPlayers, summary.ReturningPlayers)
	}
	for day, want := range map[int][3]int{4: {1, 1, 0}, 5: {2, 1, 1}, 6: {2, 0, 2}} {
		got := [3]int{report.Coarse.Unique[day], report.Coarse.New[day], report.Coarse.Returning[day]}
		if got != want {
			t.Errorf("day %d: unique, new, returning %v, want %v", day, got, want)
		}
	}
	if summary.PlaySessions != 3 || summary.MedianSessionSeconds == nil || *summary.MedianSessionSeconds != 3600 {
		t.Errorf("play sessions %d, median %v", summary.PlaySessions, summary.MedianSessionSeconds)
	}
	for i, want := range []int{0, 1, 0, 1, 1, 0, 0} {
		if report.SessionLengths[i].Count != want {
			t.Errorf("length bucket %d holds %d, want %d", i, report.SessionLengths[i].Count, want)
		}
	}
	if want := 3600.0 + 1620 + 16*3600 + 600 + 3600; summary.PlaySeconds != want {
		t.Errorf("play time %v, want %v", summary.PlaySeconds, want)
	}
	if len(report.TopPlayers) != 3 || report.TopPlayers[0].SessionID != "1" || report.TopPlayers[0].Connections != 3 {
		t.Errorf("top players %+v", report.TopPlayers)
	}
	reasons := map[string]int{}
	for _, r := range report.Disconnects {
		reasons[r.Reason] = r.Count
	}
	if len(reasons) != 4 || reasons[ReasonLeft] != 1 || reasons[ReasonKicked] != 1 {
		t.Errorf("disconnects %v", reasons)
	}
	// Session 2 was first seen yesterday, so its next day has not passed.
	if summary.ReturnedNextDay != (Share{Returned: 1, Eligible: 1}) || summary.ReturnedWithinWeek != (Share{}) {
		t.Errorf("next day %+v, within a week %+v", summary.ReturnedNextDay, summary.ReturnedWithinWeek)
	}

	all, err := s.Report("all", time.UTC, at(10, 12, 0))
	if err != nil {
		t.Fatal(err)
	}
	if all.FromMs != time.Date(2026, time.October, 1, 0, 0, 0, 0, time.UTC).UnixMilli() || all.Coarse.StepMinutes != 1440 ||
		all.Summary.NewPlayers != 3 {
		t.Errorf("all starts %v, step %d, new %d", time.UnixMilli(all.FromMs).UTC(), all.Coarse.StepMinutes,
			all.Summary.NewPlayers)
	}
}

func TestReportGauges(t *testing.T) {
	s := openStore(t)
	write := func(when time.Time, players, inWorld int, bytesOut uint64, busyMs, worstMs float64, refusals map[string]int) {
		t.Helper()
		v := sample{at: when, seconds: 60, players: players, inWorld: inWorld, bytesIn: 600, bytesOut: bytesOut,
			busyMs: busyMs, worstMs: worstMs}
		if err := s.writeSample(v, refusals); err != nil {
			t.Fatal(err)
		}
	}
	write(at(10, 10, 0), 4, 2, 6000, 600, 3, map[string]int{RefusedServerFull: 2})
	write(at(10, 10, 1), 8, 6, 12000, 1200, 7, map[string]int{RefusedServerFull: 1, RefusedProtocolMismatch: 4})
	write(at(10, 10, 20), 1, 1, 60, 0, 0, nil)

	report, err := s.Report("24h", time.UTC, at(10, 12, 0))
	if err != nil {
		t.Fatal(err)
	}
	fine := report.Fine
	i := bucketOf(fine.AtMs, at(10, 10, 0).UnixMilli())
	if *fine.PlayersAvg[i] != 6 || *fine.PlayersMax[i] != 8 || *fine.InWorldAvg[i] != 4 {
		t.Errorf("players avg %v, max %v, in world %v", *fine.PlayersAvg[i], *fine.PlayersMax[i], *fine.InWorldAvg[i])
	}
	if *fine.BytesOutAvg[i] != 150 || *fine.BytesOutPeak[i] != 200 || *fine.BytesInAvg[i] != 10 {
		t.Errorf("out avg %v, out peak %v, in %v", *fine.BytesOutAvg[i], *fine.BytesOutPeak[i], *fine.BytesInAvg[i])
	}
	if *fine.LoopBusy[i] != 0.015 || *fine.LoopWorstMs[i] != 7 {
		t.Errorf("loop busy %v, worst %v", *fine.LoopBusy[i], *fine.LoopWorstMs[i])
	}
	if fine.PlayersAvg[i+1] != nil {
		t.Error("a bucket without samples has a value")
	}
	if report.Summary.PeakPlayers != 8 || *report.Summary.PeakAtMs != at(10, 10, 1).UnixMilli() {
		t.Errorf("peak %d at %v", report.Summary.PeakPlayers, report.Summary.PeakAtMs)
	}
	if cell := report.Heatmap[1][10]; cell == nil || *cell != 13.0/3 {
		t.Errorf("Tuesday 10:00 holds %v", cell)
	}
	if len(report.Refusals) != 2 || report.Refusals[0].Reason != RefusedProtocolMismatch ||
		report.Refusals[1].Total != 3 || report.Refusals[1].Counts[bucketOf(report.Coarse.AtMs, at(10, 10, 0).UnixMilli())] != 3 {
		t.Errorf("refusals %+v", report.Refusals)
	}

	// Longer ranges read the hourly rows.
	week, err := s.Report("7d", time.UTC, at(10, 12, 0))
	if err != nil {
		t.Fatal(err)
	}
	j := bucketOf(week.Fine.AtMs, at(10, 10, 0).UnixMilli())
	if *week.Fine.PlayersAvg[j] != 13.0/3 || *week.Fine.PlayersMax[j] != 8 || *week.Coarse.PeakPlayers[6] != 8 {
		t.Errorf("hourly avg %v, max %v", *week.Fine.PlayersAvg[j], *week.Fine.PlayersMax[j])
	}
}

func TestBucketsFollowLocalDays(t *testing.T) {
	kyiv, err := time.LoadLocation("Europe/Kyiv")
	if err != nil {
		t.Skip(err)
	}
	// Summer time ends on 25 October 2026, so that day has 25 hours.
	now := time.Date(2026, time.October, 27, 15, 0, 0, 0, kyiv)
	days := bucketStarts(align(now.AddDate(0, 0, -4), 1440), now, 1440)
	if len(days) != 5 {
		t.Fatalf("%d days", len(days))
	}
	for _, start := range days {
		if local := time.UnixMilli(start).In(kyiv); local.Hour() != 0 || local.Minute() != 0 {
			t.Errorf("a day starts at %v", local)
		}
	}
	hours := bucketStarts(time.Date(2026, time.October, 25, 0, 0, 0, 0, kyiv), time.Date(2026, time.October, 26, 0, 0, 0, 0, kyiv), 180)
	for i := 1; i < len(hours); i++ {
		if hours[i] <= hours[i-1] {
			t.Fatalf("buckets go back at %d", i)
		}
	}
	if len(hours) != 8 {
		t.Errorf("%d three-hour buckets in the day", len(hours))
	}
}

func TestRecorderClosesConnections(t *testing.T) {
	s := openStore(t)
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	r, err := NewRecorder(s, log)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		r.Run(ctx, func() (Gauges, bool) { return Gauges{}, true })
		close(done)
	}()
	left := r.Connected(7, "left", time.Now())
	r.Connected(8, "stays", time.Now())
	r.Ended(left, time.Now(), ReasonLeft)
	r.Refused(RefusedBanned)
	cancel()
	<-done

	reasons := map[uint64]string{}
	rows, err := s.db.Query(`SELECT session_id, end_reason FROM connections WHERE ended_at IS NOT NULL`)
	if err != nil {
		t.Fatal(err)
	}
	for rows.Next() {
		var session uint64
		var reason string
		rows.Scan(&session, &reason)
		reasons[session] = reason
	}
	rows.Close()
	if reasons[7] != ReasonLeft || reasons[8] != ReasonServerStopped {
		t.Fatalf("reasons %v", reasons)
	}

	// A run that stopped without closing them leaves them ending at its last sample.
	if err := s.writeEvents([]event{{id: 3, session: 9, displayName: "lost", at: at(10, 10, 0)}}); err != nil {
		t.Fatal(err)
	}
	if err := s.writeSample(sample{at: at(10, 10, 5), seconds: 30}, nil); err != nil {
		t.Fatal(err)
	}
	again, err := NewRecorder(s, log)
	if err != nil {
		t.Fatal(err)
	}
	var ended int64
	var reason string
	if err := s.db.QueryRow(`SELECT ended_at, end_reason FROM connections WHERE id = 3`).Scan(&ended, &reason); err != nil {
		t.Fatal(err)
	}
	if ended != at(10, 10, 5).Add(30*time.Second).UnixMilli() || reason != ReasonServerLost {
		t.Errorf("ended %v for %q", time.UnixMilli(ended).UTC(), reason)
	}
	if id := again.Connected(1, "next", time.Now()); id != 4 {
		t.Errorf("next connection ID %d", id)
	}
}
