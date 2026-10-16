package admin

import (
	"net"
	"net/http"
	"net/netip"
	"strings"
)

func Guard(next http.Handler, hosts []string) http.Handler {
	names := map[string]bool{"localhost": true}
	for _, host := range hosts {
		names[strings.ToLower(host)] = true
	}
	crossOrigin := http.NewCrossOriginProtection()
	crossOrigin.SetDenyHandler(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		fail(w, http.StatusForbidden, "cross-origin request refused")
	}))
	protected := crossOrigin.Handler(next)
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !allowedHost(r.Host, names) {
			fail(w, http.StatusForbidden, "unknown Host; add it to admin_hosts")
			return
		}
		// Browsers set Sec-Fetch-Site on every request, navigations and
		// no-cors GETs included, and pages cannot override it.
		if site := r.Header.Get("Sec-Fetch-Site"); site == "cross-site" || site == "same-site" {
			fail(w, http.StatusForbidden, "request from another site refused")
			return
		}
		protected.ServeHTTP(w, r)
	})
}

// A page can reach the admin address under its own domain name by DNS
// rebinding, but not under an IP literal: the browser treats that as a
// different origin from the attacker's page.
func allowedHost(host string, names map[string]bool) bool {
	if name, _, err := net.SplitHostPort(host); err == nil {
		host = name
	}
	host = strings.TrimSuffix(strings.TrimPrefix(host, "["), "]")
	if host == "" {
		return false
	}
	if _, err := netip.ParseAddr(host); err == nil {
		return true
	}
	return names[strings.ToLower(host)]
}
