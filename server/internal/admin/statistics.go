package admin

import (
	"errors"
	"net/http"
	"time"
	// The panel names its time zone, which a server image without zoneinfo could not load.
	_ "time/tzdata"

	"playerhub/server/internal/metrics"
)

type statistics struct {
	metrics.Report
	UplinkBytesPerSecond *float64 `json:"uplinkBytesPerSecond"`
}

func (c *Context) statistics(w http.ResponseWriter, r *http.Request) {
	if c.Metrics == nil {
		fail(w, http.StatusNotFound, "this server keeps no metrics")
		return
	}
	query := r.URL.Query()
	name := query.Get("range")
	if name == "" {
		name = "7d"
	}
	zone := query.Get("tz")
	loc := time.UTC
	if zone != "" {
		var err error
		if loc, err = time.LoadLocation(zone); err != nil || zone == "Local" {
			fail(w, http.StatusUnprocessableEntity, "tz must be an IANA time zone such as Europe/Kyiv")
			return
		}
	}
	report, err := c.Metrics.Report(name, loc, time.Now())
	if errors.Is(err, metrics.ErrUnknownRange) {
		fail(w, http.StatusUnprocessableEntity, "range must be 24h, 7d, 30d, 90d, or all")
		return
	}
	if err != nil {
		fail(w, http.StatusInternalServerError, "metrics store failed: "+err.Error())
		return
	}
	result := statistics{Report: report}
	if c.UplinkBytesPerSecond > 0 {
		result.UplinkBytesPerSecond = &c.UplinkBytesPerSecond
	}
	reply(w, http.StatusOK, result)
}
