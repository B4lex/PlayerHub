package adminauth

import (
	"context"
	"io"
	"log/slog"
	"net/http"
	"net/http/cookiejar"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync"
	"testing"
	"time"
)

const testPassword = "correct horse battery"

func TestPasswordHash(t *testing.T) {
	encoded, err := HashPassword(testPassword)
	if err != nil {
		t.Fatal(err)
	}
	password, err := ParsePassword(encoded)
	if err != nil {
		t.Fatal(err)
	}
	if !password.Matches(testPassword) || password.Matches(testPassword+"!") {
		t.Fatal("the hash does not tell the password apart")
	}
	if again, _ := HashPassword(testPassword); again == encoded {
		t.Fatal("two hashes of one password share a salt")
	}
	if _, err := HashPassword("short"); err == nil {
		t.Fatal("a short password was hashed")
	}
	for _, bad := range []string{"", "secret", "pbkdf2-sha256$600000$c2FsdA$a2V5", "pbkdf2-sha256$1000$" +
		strings.SplitN(encoded, "$", 3)[2], "bcrypt$" + strings.SplitN(encoded, "$", 2)[1]} {
		if _, err := ParsePassword(bad); err == nil {
			t.Errorf("ParsePassword(%q) accepted it", bad)
		}
	}
}

type panel struct {
	t      *testing.T
	gate   *Gate
	server *httptest.Server
	client *http.Client
	clock  time.Time
	mu     sync.Mutex
}

func startPanel(t *testing.T, withPassword bool) *panel {
	t.Helper()
	var password Password
	if withPassword {
		encoded, err := HashPassword(testPassword)
		if err != nil {
			t.Fatal(err)
		}
		password, _ = ParsePassword(encoded)
	}
	p := &panel{t: t, clock: time.Now()}
	p.gate = New(password, nil, slog.New(slog.NewTextHandler(io.Discard, nil)))
	p.gate.now = func() time.Time {
		p.mu.Lock()
		defer p.mu.Unlock()
		return p.clock
	}
	inner := http.NewServeMux()
	inner.HandleFunc("GET /api/state", func(w http.ResponseWriter, _ *http.Request) { w.Write([]byte("{}")) })
	inner.HandleFunc("GET /admin/", func(w http.ResponseWriter, _ *http.Request) { w.Write([]byte("panel")) })
	inner.HandleFunc("GET /api/events", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		w.(http.Flusher).Flush()
		<-r.Context().Done()
	})
	p.server = httptest.NewServer(p.gate.Wrap(inner))
	t.Cleanup(p.server.Close)
	jar, _ := cookiejar.New(nil)
	p.client = &http.Client{Jar: jar}
	return p
}

func (p *panel) advance(d time.Duration) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.clock = p.clock.Add(d)
}

func (p *panel) do(method, path, body string) *http.Response {
	p.t.Helper()
	request, _ := http.NewRequest(method, p.server.URL+path, strings.NewReader(body))
	if body != "" {
		request.Header.Set("Content-Type", "application/json")
	}
	response, err := p.client.Do(request)
	if err != nil {
		p.t.Fatal(err)
	}
	response.Body.Close()
	return response
}

func (p *panel) expect(method, path, body string, want int) *http.Response {
	p.t.Helper()
	response := p.do(method, path, body)
	if response.StatusCode != want {
		p.t.Fatalf("%s %s: %d, want %d", method, path, response.StatusCode, want)
	}
	return response
}

func TestWithoutPasswordEverythingIsOpen(t *testing.T) {
	p := startPanel(t, false)
	p.expect("GET", "/api/state", "", http.StatusOK)
	p.expect("POST", "/api/login", `{"password":"anything"}`, http.StatusNoContent)
	if len(p.client.Jar.Cookies(mustURL(p.server.URL))) != 0 {
		t.Fatal("a login without a password set a cookie")
	}
}

func TestLoginGuardsTheAPI(t *testing.T) {
	p := startPanel(t, true)
	p.expect("GET", "/api/state", "", http.StatusUnauthorized)
	p.expect("GET", "/debug/pprof/", "", http.StatusUnauthorized)
	p.expect("GET", "/admin/", "", http.StatusOK)
	p.expect("POST", "/api/login", `{"password":"wrong password!"}`, http.StatusUnauthorized)
	p.expect("GET", "/api/state", "", http.StatusUnauthorized)

	request, _ := http.NewRequest("POST", p.server.URL+"/api/login", strings.NewReader(`{"password":"`+testPassword+`"}`))
	if response, _ := p.client.Do(request); response.StatusCode != http.StatusUnsupportedMediaType {
		t.Fatalf("a login without a JSON content type: %d", response.StatusCode)
	}

	response := p.expect("POST", "/api/login", `{"password":"`+testPassword+`"}`, http.StatusNoContent)
	cookie := response.Cookies()[0]
	if cookie.Name != CookieName || !cookie.HttpOnly || cookie.SameSite != http.SameSiteStrictMode || cookie.Secure {
		t.Fatalf("cookie %+v", cookie)
	}
	p.expect("GET", "/api/state", "", http.StatusOK)

	p.expect("POST", "/api/logout", "", http.StatusNoContent)
	p.expect("GET", "/api/state", "", http.StatusUnauthorized)

	// A cookie the server no longer knows does not pass either.
	p.client.Jar.SetCookies(mustURL(p.server.URL), []*http.Cookie{cookie})
	p.expect("GET", "/api/state", "", http.StatusUnauthorized)
}

func TestLoginExpires(t *testing.T) {
	p := startPanel(t, true)
	p.expect("POST", "/api/login", `{"password":"`+testPassword+`"}`, http.StatusNoContent)
	p.advance(loginLifetime - time.Minute)
	p.expect("GET", "/api/state", "", http.StatusOK)
	p.advance(time.Minute)
	p.expect("GET", "/api/state", "", http.StatusUnauthorized)
}

func TestLogoutEndsEventStreams(t *testing.T) {
	p := startPanel(t, true)
	p.expect("POST", "/api/login", `{"password":"`+testPassword+`"}`, http.StatusNoContent)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	request, _ := http.NewRequestWithContext(ctx, "GET", p.server.URL+"/api/events", nil)
	stream, err := p.client.Do(request)
	if err != nil || stream.StatusCode != http.StatusOK {
		t.Fatalf("stream: %v %v", err, stream)
	}
	ended := make(chan error, 1)
	go func() {
		_, err := io.Copy(io.Discard, stream.Body)
		ended <- err
	}()
	p.expect("POST", "/api/logout", "", http.StatusNoContent)
	select {
	case err := <-ended:
		if ctx.Err() != nil {
			t.Fatalf("the stream ended only with the test's deadline: %v", err)
		}
	case <-ctx.Done():
		t.Fatal("the stream outlived the logout")
	}
}

func TestLoginAttemptsAreLimited(t *testing.T) {
	p := startPanel(t, true)
	for range attemptBurstPerAddress {
		p.expect("POST", "/api/login", `{"password":"wrong password!"}`, http.StatusUnauthorized)
	}
	response := p.expect("POST", "/api/login", `{"password":"`+testPassword+`"}`, http.StatusTooManyRequests)
	if response.Header.Get("Retry-After") == "" {
		t.Fatal("no Retry-After")
	}
	p.advance(time.Duration(float64(time.Second) / attemptsPerAddressPerSecond))
	p.expect("POST", "/api/login", `{"password":"`+testPassword+`"}`, http.StatusNoContent)
}

func TestSecureCookieBehindTLSProxy(t *testing.T) {
	p := startPanel(t, true)
	request, _ := http.NewRequest("POST", p.server.URL+"/api/login", strings.NewReader(`{"password":"`+testPassword+`"}`))
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("X-Forwarded-Proto", "https")
	response, err := http.DefaultClient.Do(request)
	if err != nil {
		t.Fatal(err)
	}
	if cookies := response.Cookies(); len(cookies) != 1 || !cookies[0].Secure {
		t.Fatalf("cookies %+v", cookies)
	}
}

func TestOldestLoginMakesRoom(t *testing.T) {
	p := startPanel(t, true)
	first := p.gate.begin()
	for range maxLogins {
		p.advance(time.Second)
		p.gate.begin()
	}
	if len(p.gate.logins) != maxLogins {
		t.Fatalf("%d logins held", len(p.gate.logins))
	}
	request := httptest.NewRequest("GET", "/api/state", nil)
	request.AddCookie(&http.Cookie{Name: CookieName, Value: first})
	if p.gate.current(request) != nil {
		t.Fatal("the oldest login survived")
	}
}

func mustURL(raw string) *url.URL {
	u, err := url.Parse(raw)
	if err != nil {
		panic(err)
	}
	return u
}
