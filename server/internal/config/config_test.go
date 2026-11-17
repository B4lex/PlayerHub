package config

import "testing"

func TestAdminHosts(t *testing.T) {
	for _, c := range []struct {
		hosts []string
		valid bool
	}{
		{[]string{"panel.lan", "panel"}, true},
		{[]string{"panel.lan:3001"}, false},
		{[]string{"http://panel.lan"}, false},
		{[]string{"[::1]"}, false},
		{[]string{""}, false},
	} {
		config := Default()
		config.AdminHosts = c.hosts
		if err := config.Validate(); (err == nil) != c.valid {
			t.Errorf("%q: got %v, want valid %v", c.hosts, err, c.valid)
		}
	}
}

func TestAdminPasswordHash(t *testing.T) {
	for _, c := range []struct {
		hash  string
		valid bool
	}{
		{"", true},
		{"pbkdf2-sha256$600000$v8C0FWmgsM+TWCWEN7aB4g$Jn/w3WqS4BCzF3p5/bLnoPYD4A04+tcAMp6bwfRaX0g", true},
		{"correct horse battery", false},
		{"pbkdf2-sha256$600000$v8C0FWmgsM+TWCWEN7aB4g$Jn/w3WqS4BCzF3p5", false},
	} {
		config := Default()
		config.AdminPasswordHash = c.hash
		if err := config.Validate(); (err == nil) != c.valid {
			t.Errorf("%q: got %v, want valid %v", c.hash, err, c.valid)
		}
		if config.AdminPassword().Set() != (c.valid && c.hash != "") {
			t.Errorf("%q: AdminPassword set %v", c.hash, config.AdminPassword().Set())
		}
	}
}
