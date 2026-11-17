package adminauth

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"log/slog"
	"math"
	"mime"
	"net/http"
	"net/netip"
	"strconv"
	"strings"
	"sync"
	"time"

	"playerhub/server/internal/admission"
	"playerhub/server/internal/limit"
)

const (
	CookieName    = "playerhub_admin"
	loginLifetime = 7 * 24 * time.Hour
	maxLogins     = 64
	tokenBytes    = 32
	maxLoginBody  = 4 << 10

	attemptBurstPerAddress      = 5
	attemptsPerAddressPerSecond = 5.0 / 60
	// Every attempt runs the password hash, so all addresses together are capped too.
	attemptBurst           = 20
	attemptsPerSecond      = 2.0
	maxAttemptingAddresses = 4096
)

type Gate struct {
	password Password
	trusted  []netip.Prefix
	log      *slog.Logger
	now      func() time.Time

	mu          sync.Mutex
	logins      map[[sha256.Size]byte]*login
	attempts    map[netip.Prefix]*limit.TokenBucket
	allAttempts limit.TokenBucket
}

type login struct {
	expires time.Time
	// Ends with the login, so the requests it authorized, event streams included, end too.
	ctx    context.Context
	cancel context.CancelFunc
}

func New(password Password, trusted []netip.Prefix, log *slog.Logger) *Gate {
	return &Gate{password: password, trusted: trusted, log: log, now: time.Now,
		logins: make(map[[sha256.Size]byte]*login), attempts: make(map[netip.Prefix]*limit.TokenBucket)}
}

func (g *Gate) Wrap(next http.Handler) http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/login", g.status)
	mux.HandleFunc("POST /api/login", g.logIn)
	mux.HandleFunc("POST /api/logout", g.logOut)
	mux.Handle("/", g.require(next))
	return mux
}

// ServeMux has already redirected paths that are not clean, so "/admin/../api" cannot pass
// as panel files.
func (g *Gate) require(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !g.password.Set() || r.URL.Path == "/admin" || strings.HasPrefix(r.URL.Path, "/admin/") {
			next.ServeHTTP(w, r)
			return
		}
		current := g.current(r)
		if current == nil {
			fail(w, http.StatusUnauthorized, "log in to the admin panel first")
			return
		}
		ctx, cancel := context.WithCancel(r.Context())
		defer cancel()
		defer context.AfterFunc(current.ctx, cancel)()
		next.ServeHTTP(w, r.WithContext(ctx))
	})
}

func (g *Gate) status(w http.ResponseWriter, r *http.Request) {
	reply(w, http.StatusOK, struct {
		Required bool `json:"required"`
		SignedIn bool `json:"signedIn"`
	}{g.password.Set(), g.password.Set() && g.current(r) != nil})
}

func (g *Gate) logIn(w http.ResponseWriter, r *http.Request) {
	if !g.password.Set() {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	if media, _, _ := mime.ParseMediaType(r.Header.Get("Content-Type")); media != "application/json" {
		fail(w, http.StatusUnsupportedMediaType, "send the request body as Content-Type: application/json")
		return
	}
	var request struct {
		Password string `json:"password"`
	}
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, maxLoginBody)).Decode(&request); err != nil {
		fail(w, http.StatusBadRequest, "invalid JSON body")
		return
	}
	address := admission.ClientAddress(r, g.trusted)
	if wait, ok := g.attempt(address); !ok {
		g.log.Debug("admin login limited", "address", address)
		w.Header().Set("Retry-After", strconv.Itoa(int(math.Ceil(wait.Seconds()))))
		fail(w, http.StatusTooManyRequests, "too many login attempts; try again later")
		return
	}
	if !g.password.Matches(request.Password) {
		g.log.Warn("admin login refused", "address", address, "reason", "wrong password")
		fail(w, http.StatusUnauthorized, "wrong password")
		return
	}
	token := g.begin()
	http.SetCookie(w, &http.Cookie{Name: CookieName, Value: token, Path: "/",
		MaxAge: int(loginLifetime.Seconds()), HttpOnly: true, Secure: secure(r), SameSite: http.SameSiteStrictMode})
	g.log.Info("admin logged in", "address", address)
	w.WriteHeader(http.StatusNoContent)
}

func (g *Gate) logOut(w http.ResponseWriter, r *http.Request) {
	if cookie, err := r.Cookie(CookieName); err == nil {
		key := sha256.Sum256([]byte(cookie.Value))
		g.mu.Lock()
		if current := g.logins[key]; current != nil {
			current.cancel()
			delete(g.logins, key)
		}
		g.mu.Unlock()
	}
	http.SetCookie(w, &http.Cookie{Name: CookieName, Path: "/", MaxAge: -1, HttpOnly: true, Secure: secure(r),
		SameSite: http.SameSiteStrictMode})
	w.WriteHeader(http.StatusNoContent)
}

// Logins are keyed by a digest of the token, so looking one up takes the same time whatever
// part of a guess is right.
func (g *Gate) current(r *http.Request) *login {
	cookie, err := r.Cookie(CookieName)
	if err != nil {
		return nil
	}
	key := sha256.Sum256([]byte(cookie.Value))
	g.mu.Lock()
	defer g.mu.Unlock()
	current := g.logins[key]
	if current == nil {
		return nil
	}
	if !g.now().Before(current.expires) {
		current.cancel()
		delete(g.logins, key)
		return nil
	}
	return current
}

func (g *Gate) begin() string {
	raw := make([]byte, tokenBytes)
	rand.Read(raw)
	token := base64.RawURLEncoding.EncodeToString(raw)
	now := g.now()
	expires := now.Add(loginLifetime)
	ctx, cancel := context.WithDeadline(context.Background(), expires)

	g.mu.Lock()
	defer g.mu.Unlock()
	if len(g.logins) >= maxLogins {
		var oldest [sha256.Size]byte
		var oldestExpiry time.Time
		for key, existing := range g.logins {
			if !now.Before(existing.expires) {
				existing.cancel()
				delete(g.logins, key)
			} else if oldestExpiry.IsZero() || existing.expires.Before(oldestExpiry) {
				oldest, oldestExpiry = key, existing.expires
			}
		}
		if len(g.logins) >= maxLogins {
			g.logins[oldest].cancel()
			delete(g.logins, oldest)
		}
	}
	g.logins[sha256.Sum256([]byte(token))] = &login{expires: expires, ctx: ctx, cancel: cancel}
	return token
}

// attempt answers how long to wait when the address or the server has no attempt left.
func (g *Gate) attempt(address netip.Addr) (time.Duration, bool) {
	key := admission.AddressKey(address)
	now := g.now()
	g.mu.Lock()
	defer g.mu.Unlock()
	bucket := g.attempts[key]
	if bucket == nil {
		if len(g.attempts) >= maxAttemptingAddresses {
			for existing, b := range g.attempts {
				if b.Full(now, attemptsPerAddressPerSecond, attemptBurstPerAddress) {
					delete(g.attempts, existing)
				}
			}
		}
		if len(g.attempts) >= maxAttemptingAddresses {
			return time.Minute, false
		}
		bucket = &limit.TokenBucket{}
		g.attempts[key] = bucket
	}
	if !bucket.Allow(now, attemptsPerAddressPerSecond, attemptBurstPerAddress) {
		return time.Duration(float64(time.Second) / attemptsPerAddressPerSecond), false
	}
	if !g.allAttempts.Allow(now, attemptsPerSecond, attemptBurst) {
		return time.Second, false
	}
	return 0, true
}

// A proxy that ends TLS says so in X-Forwarded-Proto; claiming it falsely only keeps the
// cookie from being sent over plain HTTP.
func secure(r *http.Request) bool {
	return r.TLS != nil || strings.EqualFold(r.Header.Get("X-Forwarded-Proto"), "https")
}

func reply(w http.ResponseWriter, status int, body any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(body)
}

func fail(w http.ResponseWriter, status int, detail string) {
	reply(w, status, struct {
		Detail string `json:"detail"`
	}{detail})
}
