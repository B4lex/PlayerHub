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
