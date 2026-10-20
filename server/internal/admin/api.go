package admin

import (
	"crypto/rand"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"mime"
	"net/http"
	"strconv"
	"time"

	"playerhub/server/internal/auth"
	"playerhub/server/internal/hub"
	"playerhub/server/internal/loadtest"
	"playerhub/server/internal/playback"
	"playerhub/server/internal/recording"
	"playerhub/server/internal/session"
	"playerhub/server/internal/validate"
	"playerhub/server/internal/wire/pb"
)

func (c *Context) Routes(mux *http.ServeMux) {
	mux.HandleFunc("GET /api/state", func(w http.ResponseWriter, _ *http.Request) { reply(w, http.StatusOK, c.State()) })

	mux.HandleFunc("GET /api/recordings", func(w http.ResponseWriter, _ *http.Request) {
		reply(w, http.StatusOK, c.recordings())
	})
	mux.HandleFunc("GET /api/recordings/{id}", c.getRecording)
	mux.HandleFunc("POST /api/recordings", c.startRecording)
	mux.HandleFunc("POST /api/recordings/{id}/stop", func(w http.ResponseWriter, r *http.Request) {
		info, err := c.Captures.StopRecording(r.PathValue("id"), "stopped")
		if err != nil {
			fail(w, http.StatusNotFound, fmt.Sprintf("recording %s is not being captured", r.PathValue("id")))
			return
		}
		reply(w, http.StatusOK, info)
	})
	mux.HandleFunc("DELETE /api/recordings/{id}", func(w http.ResponseWriter, r *http.Request) {
		id := r.PathValue("id")
		if c.Captures.IsActive(id) {
			fail(w, http.StatusConflict, "stop the capture before deleting its recording")
			return
		}
		if err := c.Store.Delete(id); err != nil {
			fail(w, http.StatusNotFound, "no recording "+id)
			return
		}
		c.OnChange()
		w.WriteHeader(http.StatusNoContent)
	})

	mux.HandleFunc("POST /api/sessions", c.issueSession)
	mux.HandleFunc("PUT /api/players/{id}/hidden", c.setHidden)
	mux.HandleFunc("POST /api/players/{id}/kick", func(w http.ResponseWriter, r *http.Request) {
		slot, err := strconv.ParseUint(r.PathValue("id"), 10, 32)
		if err != nil || !c.Connections.Kick(uint32(slot)) {
			fail(w, http.StatusNotFound, "player is not connected")
			return
		}
		w.WriteHeader(http.StatusNoContent)
	})
	mux.HandleFunc("POST /api/players/{id}/ban", c.ban)
	mux.HandleFunc("DELETE /api/bans/{id}", func(w http.ResponseWriter, r *http.Request) {
		id, err := strconv.ParseUint(r.PathValue("id"), 10, 64)
		lifted := false
		if err == nil {
			if lifted, err = c.Sessions.Lift(id, time.Now()); err != nil {
				fail(w, http.StatusInternalServerError, "session store failed: "+err.Error())
				return
			}
		}
		if !lifted {
			fail(w, http.StatusNotFound, "no ban "+r.PathValue("id"))
			return
		}
		c.OnChange()
		w.WriteHeader(http.StatusNoContent)
	})
	mux.HandleFunc("POST /api/exclusions", func(w http.ResponseWriter, r *http.Request) { c.setExcluded(w, r, true) })
	mux.HandleFunc("DELETE /api/exclusions", func(w http.ResponseWriter, r *http.Request) { c.setExcluded(w, r, false) })

	mux.HandleFunc("POST /api/playbacks", c.openPlayback)
	mux.HandleFunc("POST /api/playbacks/{id}/play", c.withPlayback(func(s *playback.Session, _ *http.Request) error {
		s.Play()
		return nil
	}))
	mux.HandleFunc("POST /api/playbacks/{id}/pause", c.withPlayback(func(s *playback.Session, _ *http.Request) error {
		s.Pause()
		return nil
	}))
	mux.HandleFunc("POST /api/playbacks/{id}/seek", c.withPlayback(func(s *playback.Session, r *http.Request) error {
		var request struct {
			PositionMs *float64 `json:"positionMs"`
		}
		if err := decode(r, &request); err != nil {
			return err
		}
		if request.PositionMs == nil || !finite(*request.PositionMs) || *request.PositionMs < 0 {
			return errInvalid("positionMs must be a finite number >= 0")
		}
		s.Seek(*request.PositionMs)
		return nil
	}))
	mux.HandleFunc("PATCH /api/playbacks/{id}", c.withPlayback(func(s *playback.Session, r *http.Request) error {
		var request struct {
			Rate *float64 `json:"rate"`
			Loop *bool    `json:"loop"`
		}
		if err := decode(r, &request); err != nil {
			return err
		}
		if err := s.Configure(request.Rate, request.Loop); err != nil {
			return errInvalid(err.Error())
		}
		return nil
	}))
	mux.HandleFunc("DELETE /api/playbacks/{id}", func(w http.ResponseWriter, r *http.Request) {
		if err := c.Playbacks.Close(r.PathValue("id")); err != nil {
			fail(w, http.StatusNotFound, "no playback "+r.PathValue("id"))
			return
		}
		w.WriteHeader(http.StatusNoContent)
	})

	mux.HandleFunc("POST /api/batches", c.createBatch)
	mux.HandleFunc("POST /api/batches/{id}/pause", c.withBatch((*loadtest.Batch).Pause))
	mux.HandleFunc("POST /api/batches/{id}/resume", c.withBatch((*loadtest.Batch).Resume))
	mux.HandleFunc("DELETE /api/batches/{id}", func(w http.ResponseWriter, r *http.Request) {
		if err := c.Batches.Close(r.PathValue("id")); err != nil {
			fail(w, http.StatusNotFound, "no load batch "+r.PathValue("id"))
			return
		}
		w.WriteHeader(http.StatusNoContent)
	})
}

func (c *Context) getRecording(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	var info recording.Info
	found := false
	for _, live := range c.Captures.ActiveInfos() {
		if live.ID == id {
			info, found = live, true
		}
	}
	if !found {
		var err error
		if info, err = c.Store.Info(id); err != nil {
			fail(w, http.StatusNotFound, "no recording "+id)
			return
		}
	}
	loaded, err := c.Store.Load(id)
	if err != nil {
		recordingFailure(w, id, err)
		return
	}
	reply(w, http.StatusOK, struct {
		recording.Info
		Markers []recording.Marker `json:"markers"`
	}{info, recording.Markers(loaded)})
}

func (c *Context) startRecording(w http.ResponseWriter, r *http.Request) {
	var request struct {
		PlayerID string `json:"playerId"`
		Label    string `json:"label"`
	}
	if err := decode(r, &request); err != nil {
		invalid(w, err)
		return
	}
	if len(request.Label) > recording.MaximumLabelLength {
		fail(w, http.StatusUnprocessableEntity, fmt.Sprintf("label is longer than %d characters", recording.MaximumLabelLength))
		return
	}
	slot, err := strconv.ParseUint(request.PlayerID, 10, 32)
	var initial []*pb.ClientMessage
	var name string
	connected := false
	if err == nil {
		c.Loop.Call(func(h *hub.Hub) {
			if h.Player(uint32(slot)) != nil {
				connected = true
				name = h.Name(uint32(slot))
				initial = h.InitialMessages(uint32(slot))
			}
		})
	}
	if !connected {
		fail(w, http.StatusNotFound, "player is not connected")
		return
	}
	info, err := c.Captures.Start(uint32(slot), name, request.Label, initial, time.Now())
	switch {
	case errors.Is(err, recording.ErrAlreadyCapturing):
		fail(w, http.StatusConflict, err.Error())
	case err != nil:
		fail(w, http.StatusInternalServerError, "cannot write recording: "+err.Error())
	default:
		reply(w, http.StatusCreated, info)
	}
}

func (c *Context) issueSession(w http.ResponseWriter, r *http.Request) {
	var request struct {
		DisplayName string `json:"displayName"`
		Token       []byte `json:"token"`
	}
	if err := decode(r, &request); err != nil {
		invalid(w, err)
		return
	}
	if validate.DisplayName(request.DisplayName) != nil {
		fail(w, http.StatusUnprocessableEntity, fmt.Sprintf("displayName must be %d to %d letters, digits, or underscores",
			validate.MinimumDisplayNameBytes, validate.MaximumDisplayNameBytes))
		return
	}
	if len(request.Token) != 0 && len(request.Token) != validate.SessionTokenBytes {
		fail(w, http.StatusUnprocessableEntity, fmt.Sprintf("token must be empty or %d bytes", validate.SessionTokenBytes))
		return
	}
	session, issued, err := c.Sessions.Authenticate(request.Token, request.DisplayName, time.Now(), nil)
	switch {
	case errors.Is(err, auth.ErrDisplayNameTaken):
		fail(w, http.StatusConflict, "display name taken")
		return
	case errors.Is(err, auth.ErrBanned):
		fail(w, http.StatusForbidden, "session is banned")
		return
	case err != nil:
		fail(w, http.StatusInternalServerError, "session store failed: "+err.Error())
		return
	}
	status, token := http.StatusOK, request.Token
	if issued != nil {
		status, token = http.StatusCreated, issued
	}
	reply(w, status, struct {
		SessionID   string `json:"sessionId"`
		DisplayName string `json:"displayName"`
		Token       []byte `json:"token"`
	}{strconv.FormatUint(session.ID, 10), session.DisplayName, token})
}

func (c *Context) ban(w http.ResponseWriter, r *http.Request) {
	var request struct {
		Address *bool `json:"address"`
	}
	if err := decode(r, &request); err != nil {
		invalid(w, err)
		return
	}
	if request.Address == nil {
		invalid(w, errInvalid("expected {\"address\": true|false}"))
		return
	}
	slot, err := strconv.ParseUint(r.PathValue("id"), 10, 32)
	if err != nil {
		fail(w, http.StatusNotFound, "player is not connected")
		return
	}
	ban, err := c.Connections.Ban(uint32(slot), *request.Address, time.Now())
	switch {
	case errors.Is(err, session.ErrNotConnected):
		fail(w, http.StatusNotFound, err.Error())
	case errors.Is(err, session.ErrAddressNotBannable):
		fail(w, http.StatusUnprocessableEntity, err.Error())
	case err != nil:
		fail(w, http.StatusInternalServerError, "session store failed: "+err.Error())
	default:
		c.OnChange()
		reply(w, http.StatusCreated, banView(ban))
	}
}

func (c *Context) setHidden(w http.ResponseWriter, r *http.Request) {
	var request struct {
		Hidden *bool `json:"hidden"`
	}
	if err := decode(r, &request); err != nil {
		invalid(w, err)
		return
	}
	if request.Hidden == nil {
		invalid(w, errInvalid("expected {\"hidden\": true|false}"))
		return
	}
	slot, err := strconv.ParseUint(r.PathValue("id"), 10, 32)
	found := false
	if err == nil {
		c.Loop.Call(func(h *hub.Hub) { found = h.SetHidden(uint32(slot), *request.Hidden) })
	}
	if !found {
		fail(w, http.StatusNotFound, "no player "+r.PathValue("id"))
		return
	}
	reply(w, http.StatusOK, map[string]bool{"hidden": *request.Hidden})
}

func (c *Context) setExcluded(w http.ResponseWriter, r *http.Request, excluded bool) {
	var request Exclusion
	if err := decode(r, &request); err != nil {
		invalid(w, err)
		return
	}
	viewer, viewerErr := strconv.ParseUint(request.ViewerID, 10, 32)
	target, targetErr := strconv.ParseUint(request.TargetID, 10, 32)
	found := false
	if viewerErr == nil && targetErr == nil {
		c.Loop.Call(func(h *hub.Hub) { found = h.SetExcluded(uint32(viewer), uint32(target), excluded) })
	}
	if !found {
		fail(w, http.StatusNotFound, "both players must be connected and different")
		return
	}
	reply(w, http.StatusOK, request)
}

func (c *Context) openPlayback(w http.ResponseWriter, r *http.Request) {
	request := struct {
		RecordingID string  `json:"recordingId"`
		Autoplay    *bool   `json:"autoplay"`
		Rate        float64 `json:"rate"`
		Loop        bool    `json:"loop"`
		StartMs     int64   `json:"startMs"`
	}{Rate: 1}
	if err := decode(r, &request); err != nil {
		invalid(w, err)
		return
	}
	if err := playback.ValidateRate(request.Rate); err != nil {
		invalid(w, errInvalid(err.Error()))
		return
	}
	if request.StartMs < 0 {
		invalid(w, errInvalid("startMs must be >= 0"))
		return
	}
	session, err := c.Playbacks.Open(request.RecordingID, playback.Options{
		Autoplay: request.Autoplay == nil || *request.Autoplay, Rate: request.Rate, Loop: request.Loop,
		StartMs: request.StartMs,
	})
	if err != nil {
		recordingFailure(w, request.RecordingID, err)
		return
	}
	reply(w, http.StatusCreated, session.State())
}

func (c *Context) withPlayback(apply func(*playback.Session, *http.Request) error) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		session, err := c.Playbacks.Get(r.PathValue("id"))
		if err != nil {
			fail(w, http.StatusNotFound, "no playback "+r.PathValue("id"))
			return
		}
		if err := apply(session, r); err != nil {
			invalid(w, err)
			return
		}
		reply(w, http.StatusOK, session.State())
	}
}

func (c *Context) createBatch(w http.ResponseWriter, r *http.Request) {
	request := struct {
		RecordingID   string  `json:"recordingId"`
		Players       int     `json:"players"`
		Transport     string  `json:"transport"`
		Seed          *int64  `json:"seed"`
		Jitter        float64 `json:"jitter"`
		RateSpread    float64 `json:"rateSpread"`
		RampUpSeconds float64 `json:"rampUpSeconds"`
	}{Jitter: 0.5, Transport: "virtual"}
	if err := decode(r, &request); err != nil {
		invalid(w, err)
		return
	}
	var problem string
	switch {
	case request.Players < 1 || request.Players > loadtest.MaximumBots:
		problem = fmt.Sprintf("players must be within 1..%d", loadtest.MaximumBots)
	case request.Transport != "virtual":
		problem = "transport must be virtual; run cmd/loadgen for bots that connect over the network"
	case request.Seed != nil && (*request.Seed < 0 || *request.Seed > math.MaxInt32):
		problem = "seed must be within 0..2147483647"
	case !finite(request.Jitter) || request.Jitter < 0 || request.Jitter > 1:
		problem = "jitter must be within 0..1"
	case !finite(request.RateSpread) || request.RateSpread < 0 || request.RateSpread > loadtest.MaximumRateSpread:
		problem = fmt.Sprintf("rateSpread must be within 0..%g", loadtest.MaximumRateSpread)
	case !finite(request.RampUpSeconds) || request.RampUpSeconds < 0 || request.RampUpSeconds > loadtest.MaximumRampUpSeconds:
		problem = fmt.Sprintf("rampUpSeconds must be within 0..%g", loadtest.MaximumRampUpSeconds)
	}
	if problem != "" {
		fail(w, http.StatusUnprocessableEntity, problem)
		return
	}
	var seed uint64
	if request.Seed != nil {
		seed = uint64(*request.Seed)
	} else {
		var random [4]byte
		rand.Read(random[:])
		seed = uint64(binary.LittleEndian.Uint32(random[:]) & math.MaxInt32)
	}
	batch, err := c.Batches.Create(request.RecordingID, loadtest.Settings{
		Players: request.Players, Seed: seed, Jitter: request.Jitter, RateSpread: request.RateSpread,
		RampUpSeconds: request.RampUpSeconds,
	})
	if err != nil {
		recordingFailure(w, request.RecordingID, err)
		return
	}
	reply(w, http.StatusCreated, batch.State())
}

func (c *Context) withBatch(apply func(*loadtest.Batch)) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		batch, err := c.Batches.Get(r.PathValue("id"))
		if err != nil {
			fail(w, http.StatusNotFound, "no load batch "+r.PathValue("id"))
			return
		}
		apply(batch)
		reply(w, http.StatusOK, batch.State())
	}
}

type invalidRequest struct{ message string }

func (e *invalidRequest) Error() string { return e.message }

func errInvalid(message string) error { return &invalidRequest{message} }

// Browsers send a text/plain or form body cross-site without a preflight;
// requiring JSON makes them ask first, and the API grants no CORS.
var errContentType = errors.New("Content-Type must be application/json")

func decode(r *http.Request, into any) error {
	if mediaType, _, err := mime.ParseMediaType(r.Header.Get("Content-Type")); err != nil || mediaType != "application/json" {
		return errContentType
	}
	decoder := json.NewDecoder(http.MaxBytesReader(nil, r.Body, 64*1024))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(into); err != nil {
		return errInvalid("invalid request body: " + err.Error())
	}
	return nil
}

func invalid(w http.ResponseWriter, err error) {
	if errors.Is(err, errContentType) {
		fail(w, http.StatusUnsupportedMediaType, err.Error())
		return
	}
	fail(w, http.StatusUnprocessableEntity, err.Error())
}

func recordingFailure(w http.ResponseWriter, id string, err error) {
	var recordingError *recording.Error
	switch {
	case errors.Is(err, recording.ErrNotFound):
		fail(w, http.StatusNotFound, "no recording "+id)
	case errors.As(err, &recordingError), errors.Is(err, playback.ErrNoCheckpoints):
		fail(w, http.StatusUnprocessableEntity, err.Error())
	default:
		fail(w, http.StatusInternalServerError, err.Error())
	}
}

func finite(v float64) bool {
	return !math.IsNaN(v) && !math.IsInf(v, 0)
}

func reply(w http.ResponseWriter, status int, body any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(body)
}

func fail(w http.ResponseWriter, status int, message string) {
	reply(w, status, map[string]string{"detail": message})
}
