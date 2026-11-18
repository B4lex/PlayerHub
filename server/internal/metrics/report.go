package metrics

import (
	"cmp"
	"errors"
	"slices"
	"sort"
	"strconv"
	"time"
)

var ErrUnknownRange = errors.New("unknown range")

type rangeSteps struct {
	name string
	// Zero for 24 hours.
	days int
	// Bucket widths in minutes.
	fineStep, coarseStep int
}

// Ranges are listed shortest first; "all" takes the steps of the shortest that covers the data.
var ranges = []rangeSteps{
	{"24h", 0, 10, 60},
	{"7d", 7, 60, 1440},
	{"30d", 30, 180, 1440},
	{"90d", 90, 360, 1440},
}

const (
	// Connections of one session this close together are one play session.
	playSessionGap = 5 * time.Minute
	topPlayers     = 10
)

// Upper bounds in seconds; the last bucket has none.
var lengthBounds = []float64{300, 900, 1800, 3600, 7200, 14400}

type Report struct {
	Range          string          `json:"range"`
	FromMs         int64           `json:"fromMs"`
	ToMs           int64           `json:"toMs"`
	Summary        Summary         `json:"summary"`
	Fine           FineSeries      `json:"fine"`
	Coarse         CoarseSeries    `json:"coarse"`
	Heatmap        [7][24]*float64 `json:"heatmap"`
	SessionLengths []LengthBucket  `json:"sessionLengths"`
	TopPlayers     []TopPlayer     `json:"topPlayers"`
	Disconnects    []ReasonCount   `json:"disconnects"`
	Refusals       []RefusalSeries `json:"refusals"`
}

type Summary struct {
	PeakPlayers          int      `json:"peakPlayers"`
	PeakAtMs             *int64   `json:"peakAtMs"`
	UniquePlayers        int      `json:"uniquePlayers"`
	NewPlayers           int      `json:"newPlayers"`
	ReturningPlayers     int      `json:"returningPlayers"`
	PlaySeconds          float64  `json:"playSeconds"`
	PlaySessions         int      `json:"playSessions"`
	MedianSessionSeconds *float64 `json:"medianSessionSeconds"`
	ReturnedNextDay      Share    `json:"returnedNextDay"`
	ReturnedWithinWeek   Share    `json:"returnedWithinWeek"`
}

type Share struct {
	Returned int `json:"returned"`
	Eligible int `json:"eligible"`
}

type FineSeries struct {
	StepMinutes  int        `json:"stepMinutes"`
	AtMs         []int64    `json:"atMs"`
	PlayersAvg   []*float64 `json:"playersAvg"`
	PlayersMax   []*float64 `json:"playersMax"`
	InWorldAvg   []*float64 `json:"inWorldAvg"`
	BytesOutAvg  []*float64 `json:"bytesOutAvg"`
	BytesOutPeak []*float64 `json:"bytesOutPeak"`
	BytesInAvg   []*float64 `json:"bytesInAvg"`
	LoopBusy     []*float64 `json:"loopBusy"`
	LoopWorstMs  []*float64 `json:"loopWorstMs"`
}

type CoarseSeries struct {
	StepMinutes int        `json:"stepMinutes"`
	AtMs        []int64    `json:"atMs"`
	Unique      []int      `json:"unique"`
	New         []int      `json:"new"`
	Returning   []int      `json:"returning"`
	PeakPlayers []*float64 `json:"peakPlayers"`
}

type LengthBucket struct {
	// Null for the last bucket.
	MaxSeconds *float64 `json:"maxSeconds"`
	Count      int      `json:"count"`
}

type TopPlayer struct {
	SessionID   string  `json:"sessionId"`
	DisplayName string  `json:"displayName"`
	PlaySeconds float64 `json:"playSeconds"`
	Connections int     `json:"connections"`
}

type ReasonCount struct {
	Reason string `json:"reason"`
	Count  int    `json:"count"`
}

type RefusalSeries struct {
	Reason string `json:"reason"`
	Total  int    `json:"total"`
	Counts []int  `json:"counts"`
}

// Report covers the named range up to now, with days and hours in loc.
func (s *Store) Report(name string, loc *time.Location, now time.Time) (Report, error) {
	now = now.In(loc)
	var from time.Time
	var fineStep, coarseStep int
	if name == "all" {
		earliest, err := s.earliest(now)
		if err != nil {
			return Report{}, err
		}
		chosen := len(ranges) - 1
		for i, r := range ranges {
			if !rangeStart(now, r.days, r.coarseStep).After(earliest) {
				chosen = i
				break
			}
		}
		fineStep, coarseStep = ranges[chosen].fineStep, ranges[chosen].coarseStep
		from = align(earliest, coarseStep)
	} else {
		i := slices.IndexFunc(ranges, func(r rangeSteps) bool { return r.name == name })
		if i < 0 {
			return Report{}, ErrUnknownRange
		}
		fineStep, coarseStep = ranges[i].fineStep, ranges[i].coarseStep
		from = rangeStart(now, ranges[i].days, coarseStep)
	}

	report := Report{Range: name, FromMs: from.UnixMilli(), ToMs: now.UnixMilli()}
	report.Fine.StepMinutes, report.Fine.AtMs = fineStep, bucketStarts(from, now, fineStep)
	report.Coarse.StepMinutes, report.Coarse.AtMs = coarseStep, bucketStarts(from, now, coarseStep)
	if err := s.reportGauges(&report, fineStep < 60, loc); err != nil {
		return Report{}, err
	}
	if err := s.reportPlayers(&report, loc, from, now); err != nil {
		return Report{}, err
	}
	if err := s.reportRefusals(&report); err != nil {
		return Report{}, err
	}
	return report, nil
}

func rangeStart(now time.Time, days, coarseStep int) time.Time {
	if days == 0 {
		return align(now.Add(-23*time.Hour), coarseStep)
	}
	return align(now.AddDate(0, 0, -(days-1)), coarseStep)
}

// The earliest time any table holds, or now when they are empty.
func (s *Store) earliest(now time.Time) (time.Time, error) {
	var at *int64
	err := s.db.QueryRow(`SELECT MIN(at) FROM (
		SELECT MIN(at) AS at FROM samples UNION ALL SELECT MIN(at) FROM hourly
		UNION ALL SELECT MIN(started_at) FROM connections UNION ALL SELECT MIN(first_seen_at) FROM players)`).Scan(&at)
	if err != nil || at == nil {
		return now, err
	}
	return time.UnixMilli(min(*at, now.UnixMilli())).In(now.Location()), nil
}

// align truncates to a step of minutes: sub-hour steps on the clock, hours within the local day,
// a day to local midnight.
func align(t time.Time, minutes int) time.Time {
	switch {
	case minutes < 60:
		return t.Truncate(time.Duration(minutes) * time.Minute)
	case minutes < 1440:
		hours := minutes / 60
		return time.Date(t.Year(), t.Month(), t.Day(), t.Hour()-t.Hour()%hours, 0, 0, 0, t.Location())
	default:
		return time.Date(t.Year(), t.Month(), t.Day(), 0, 0, 0, 0, t.Location())
	}
}

func advance(t time.Time, minutes int) time.Time {
	var next time.Time
	switch {
	case minutes <= 60:
		return t.Add(time.Duration(minutes) * time.Minute)
	case minutes < 1440:
		next = time.Date(t.Year(), t.Month(), t.Day(), t.Hour()+minutes/60, 0, 0, 0, t.Location())
	default:
		next = time.Date(t.Year(), t.Month(), t.Day()+1, 0, 0, 0, 0, t.Location())
	}
	// A daylight saving change can map a local hour back onto the one before.
	if !next.After(t) {
		next = t.Add(time.Duration(minutes) * time.Minute)
	}
	return next
}

func bucketStarts(from, to time.Time, minutes int) []int64 {
	var starts []int64
	for t := from; t.Before(to); t = advance(t, minutes) {
		starts = append(starts, t.UnixMilli())
	}
	return starts
}

// bucketOf gives the bucket holding at, or -1 before the first.
func bucketOf(starts []int64, at int64) int {
	return sort.Search(len(starts), func(i int) bool { return starts[i] > at }) - 1
}

type gaugeRow struct {
	at                                 int64
	samples                            int64
	seconds                            float64
	playersSum, playersMax, inWorldSum int64
	bytesIn, bytesOut                  int64
	outRateMax, busyMs, worstMs        float64
}

type gaugeSum struct {
	gaugeRow
	present bool
}

func (g *gaugeSum) add(row gaugeRow) {
	g.present = true
	g.samples += row.samples
	g.seconds += row.seconds
	g.playersSum += row.playersSum
	g.playersMax = max(g.playersMax, row.playersMax)
	g.inWorldSum += row.inWorldSum
	g.bytesIn += row.bytesIn
	g.bytesOut += row.bytesOut
	g.outRateMax = max(g.outRateMax, row.outRateMax)
	g.busyMs += row.busyMs
	g.worstMs = max(g.worstMs, row.worstMs)
}

func value(present bool, v float64) *float64 {
	if !present {
		return nil
	}
	return &v
}

func (s *Store) reportGauges(report *Report, minutes bool, loc *time.Location) error {
	query := `SELECT at, samples, seconds, players_sum, players_max, in_world_sum, bytes_in, bytes_out, out_rate_max,
		busy_ms, worst_ms FROM hourly WHERE at >= ? AND at < ? ORDER BY at`
	if minutes {
		query = `SELECT at, 1, seconds, players, players, in_world, bytes_in, bytes_out, bytes_out / seconds, busy_ms,
			worst_ms FROM samples WHERE at >= ? AND at < ? ORDER BY at`
	}
	rows, err := s.db.Query(query, report.FromMs, report.ToMs)
	if err != nil {
		return err
	}
	defer rows.Close()
	fine := make([]gaugeSum, len(report.Fine.AtMs))
	coarse := make([]gaugeSum, len(report.Coarse.AtMs))
	var heat [7][24]struct{ players, samples int64 }
	for rows.Next() {
		var row gaugeRow
		if err := rows.Scan(&row.at, &row.samples, &row.seconds, &row.playersSum, &row.playersMax, &row.inWorldSum,
			&row.bytesIn, &row.bytesOut, &row.outRateMax, &row.busyMs, &row.worstMs); err != nil {
			return err
		}
		if i := bucketOf(report.Fine.AtMs, row.at); i >= 0 {
			fine[i].add(row)
		}
		if i := bucketOf(report.Coarse.AtMs, row.at); i >= 0 {
			coarse[i].add(row)
		}
		if row.playersMax > int64(report.Summary.PeakPlayers) {
			report.Summary.PeakPlayers = int(row.playersMax)
			at := row.at
			report.Summary.PeakAtMs = &at
		}
		local := time.UnixMilli(row.at).In(loc)
		cell := &heat[(int(local.Weekday())+6)%7][local.Hour()]
		cell.players += row.playersSum
		cell.samples += row.samples
	}
	if err := rows.Err(); err != nil {
		return err
	}
	f := &report.Fine
	for _, g := range fine {
		samples, seconds := float64(g.samples), g.seconds
		ok := g.present && samples > 0 && seconds > 0
		f.PlayersAvg = append(f.PlayersAvg, value(ok, float64(g.playersSum)/samples))
		f.PlayersMax = append(f.PlayersMax, value(ok, float64(g.playersMax)))
		f.InWorldAvg = append(f.InWorldAvg, value(ok, float64(g.inWorldSum)/samples))
		f.BytesOutAvg = append(f.BytesOutAvg, value(ok, float64(g.bytesOut)/seconds))
		f.BytesOutPeak = append(f.BytesOutPeak, value(ok, g.outRateMax))
		f.BytesInAvg = append(f.BytesInAvg, value(ok, float64(g.bytesIn)/seconds))
		f.LoopBusy = append(f.LoopBusy, value(ok, g.busyMs/(seconds*1000)))
		f.LoopWorstMs = append(f.LoopWorstMs, value(ok, g.worstMs))
	}
	for _, g := range coarse {
		report.Coarse.PeakPlayers = append(report.Coarse.PeakPlayers, value(g.present, float64(g.playersMax)))
	}
	for day := range heat {
		for hour, cell := range heat[day] {
			if cell.samples > 0 {
				report.Heatmap[day][hour] = value(true, float64(cell.players)/float64(cell.samples))
			}
		}
	}
	return nil
}

type connectionRow struct {
	session     uint64
	displayName string
	start, end  int64
	open        bool
	reason      string
}

func localDay(at int64, loc *time.Location, days int) int64 {
	t := time.UnixMilli(at).In(loc)
	return time.Date(t.Year(), t.Month(), t.Day()+days, 0, 0, 0, 0, loc).UnixMilli()
}

func (s *Store) reportPlayers(report *Report, loc *time.Location, from, now time.Time) error {
	fromMs, nowMs := from.UnixMilli(), now.UnixMilli()
	coarse := &report.Coarse
	coarse.Unique = make([]int, len(coarse.AtMs))
	coarse.New = make([]int, len(coarse.AtMs))
	coarse.Returning = make([]int, len(coarse.AtMs))

	// Sessions first seen in the range, with the bucket they were first seen in.
	firstSeen := map[uint64]int64{}
	rows, err := s.db.Query(`SELECT session_id, first_seen_at FROM players WHERE first_seen_at >= ? AND first_seen_at <= ?`,
		fromMs, nowMs)
	if err != nil {
		return err
	}
	for rows.Next() {
		var session uint64
		var at int64
		if err := rows.Scan(&session, &at); err != nil {
			rows.Close()
			return err
		}
		firstSeen[session] = at
		if i := bucketOf(coarse.AtMs, at); i >= 0 {
			coarse.New[i]++
		}
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return err
	}
	report.Summary.NewPlayers = len(firstSeen)

	// Only players first seen since the database existed have their following days recorded.
	createdMs := s.createdAt.UnixMilli()
	var nextDayReturned, weekReturned = map[uint64]bool{}, map[uint64]bool{}
	for session, at := range firstSeen {
		if at < createdMs {
			continue
		}
		if localDay(at, loc, 2) <= nowMs {
			report.Summary.ReturnedNextDay.Eligible++
			nextDayReturned[session] = false
		}
		if localDay(at, loc, 8) <= nowMs {
			report.Summary.ReturnedWithinWeek.Eligible++
			weekReturned[session] = false
		}
	}

	// Two halves, since an OR of the two conditions on ended_at would scan the whole table.
	rows, err = s.db.Query(`SELECT session_id, display_name, started_at, ended_at, 0, end_reason
			FROM connections WHERE ended_at > ? AND started_at <= ?
		UNION ALL
		SELECT session_id, display_name, started_at, ?, 1, ''
			FROM connections WHERE ended_at IS NULL AND started_at <= ?
		ORDER BY session_id, started_at`, fromMs, nowMs, nowMs, nowMs)
	if err != nil {
		return err
	}
	defer rows.Close()
	var connections []connectionRow
	for rows.Next() {
		var c connectionRow
		if err := rows.Scan(&c.session, &c.displayName, &c.start, &c.end, &c.open, &c.reason); err != nil {
			return err
		}
		connections = append(connections, c)
	}
	if err := rows.Err(); err != nil {
		return err
	}

	lastCounted := make([]uint64, len(coarse.AtMs))
	newActive := make([]int, len(coarse.AtMs))
	reasons := map[string]int{}
	players := []TopPlayer{}
	var lengths []float64
	report.SessionLengths = make([]LengthBucket, len(lengthBounds)+1)
	for i := range lengthBounds {
		report.SessionLengths[i].MaxSeconds = &lengthBounds[i]
	}
	addLength := func(seconds float64) {
		lengths = append(lengths, seconds)
		report.SessionLengths[sort.SearchFloat64s(lengthBounds, seconds)].Count++
	}

	for start := 0; start < len(connections); {
		session := connections[start].session
		end := start
		for end < len(connections) && connections[end].session == session {
			end++
		}
		player := TopPlayer{SessionID: strconv.FormatUint(session, 10)}
		var mergedStart, mergedEnd int64
		mergedOpen, merging := false, false
		for _, c := range connections[start:end] {
			player.DisplayName = c.displayName
			player.Connections++
			clippedStart, clippedEnd := max(c.start, fromMs), min(c.end, nowMs)
			player.PlaySeconds += float64(max(0, clippedEnd-clippedStart)) / 1000
			for i := max(0, bucketOf(coarse.AtMs, clippedStart)); i < len(coarse.AtMs) && coarse.AtMs[i] < clippedEnd; i++ {
				// Session IDs start at 1, so zero marks a bucket nobody was counted in yet.
				if lastCounted[i] != session {
					lastCounted[i] = session
					coarse.Unique[i]++
					if seen, ok := firstSeen[session]; ok && bucketOf(coarse.AtMs, seen) == i {
						newActive[i]++
					}
				}
			}
			if !c.open && c.end > fromMs {
				reasons[c.reason]++
			}
			if seen, ok := firstSeen[session]; ok {
				day1 := localDay(seen, loc, 1)
				if _, eligible := nextDayReturned[session]; eligible && c.start < localDay(seen, loc, 2) && c.end >= day1 {
					nextDayReturned[session] = true
				}
				if _, eligible := weekReturned[session]; eligible && c.start < localDay(seen, loc, 8) && c.end >= day1 {
					weekReturned[session] = true
				}
			}
			if merging && c.start-mergedEnd <= playSessionGap.Milliseconds() {
				mergedEnd = max(mergedEnd, c.end)
				mergedOpen = mergedOpen || c.open
				continue
			}
			if merging && !mergedOpen && mergedStart >= fromMs {
				addLength(float64(mergedEnd-mergedStart) / 1000)
			}
			mergedStart, mergedEnd, mergedOpen, merging = c.start, c.end, c.open, true
		}
		if merging && !mergedOpen && mergedStart >= fromMs {
			addLength(float64(mergedEnd-mergedStart) / 1000)
		}
		report.Summary.PlaySeconds += player.PlaySeconds
		players = append(players, player)
		if _, ok := firstSeen[session]; !ok {
			report.Summary.ReturningPlayers++
		}
		start = end
	}
	report.Summary.UniquePlayers = len(players)
	for i := range coarse.Returning {
		coarse.Returning[i] = coarse.Unique[i] - newActive[i]
	}
	for _, returned := range nextDayReturned {
		if returned {
			report.Summary.ReturnedNextDay.Returned++
		}
	}
	for _, returned := range weekReturned {
		if returned {
			report.Summary.ReturnedWithinWeek.Returned++
		}
	}

	report.Summary.PlaySessions = len(lengths)
	if len(lengths) > 0 {
		slices.Sort(lengths)
		median := lengths[len(lengths)/2]
		if len(lengths)%2 == 0 {
			median = (lengths[len(lengths)/2-1] + median) / 2
		}
		report.Summary.MedianSessionSeconds = &median
	}

	slices.SortFunc(players, func(a, b TopPlayer) int {
		return cmp.Or(cmp.Compare(b.PlaySeconds, a.PlaySeconds), cmp.Compare(a.SessionID, b.SessionID))
	})
	report.TopPlayers = players[:min(len(players), topPlayers)]

	report.Disconnects = []ReasonCount{}
	for reason, count := range reasons {
		report.Disconnects = append(report.Disconnects, ReasonCount{reason, count})
	}
	slices.SortFunc(report.Disconnects, func(a, b ReasonCount) int {
		return cmp.Or(cmp.Compare(b.Count, a.Count), cmp.Compare(a.Reason, b.Reason))
	})
	return nil
}

func (s *Store) reportRefusals(report *Report) error {
	rows, err := s.db.Query(`SELECT at, reason, count FROM refusals WHERE at >= ? AND at <= ?`,
		time.UnixMilli(report.FromMs).Truncate(time.Hour).UnixMilli(), report.ToMs)
	if err != nil {
		return err
	}
	defer rows.Close()
	byReason := map[string]*RefusalSeries{}
	for rows.Next() {
		var at int64
		var reason string
		var count int
		if err := rows.Scan(&at, &reason, &count); err != nil {
			return err
		}
		series := byReason[reason]
		if series == nil {
			series = &RefusalSeries{Reason: reason, Counts: make([]int, len(report.Coarse.AtMs))}
			byReason[reason] = series
		}
		series.Total += count
		series.Counts[max(0, bucketOf(report.Coarse.AtMs, at))] += count
	}
	if err := rows.Err(); err != nil {
		return err
	}
	report.Refusals = []RefusalSeries{}
	for _, series := range byReason {
		report.Refusals = append(report.Refusals, *series)
	}
	slices.SortFunc(report.Refusals, func(a, b RefusalSeries) int {
		return cmp.Or(cmp.Compare(b.Total, a.Total), cmp.Compare(a.Reason, b.Reason))
	})
	return nil
}
