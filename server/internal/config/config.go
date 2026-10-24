package config

import (
	"errors"
	"fmt"
	"net/netip"
	"os"
	"strings"

	"github.com/BurntSushi/toml"
)

type Config struct {
	Listen              string   `toml:"listen"`
	AdminListen         string   `toml:"admin_listen"`
	AdminHosts          []string `toml:"admin_hosts"`
	TrustedProxies      []string `toml:"trusted_proxies"`
	RecordingsDirectory string   `toml:"recordings_directory"`
	AdminUIDirectory    string   `toml:"admin_ui_directory"`
	SendRateHz          float64  `toml:"send_rate_hz"`
	Limits              Limits   `toml:"limits"`
	Liveness            Liveness `toml:"liveness"`
	Interest            Interest `toml:"interest"`
	Budget              Budget   `toml:"budget"`
	Chat                Chat     `toml:"chat"`
	Trade               Trade    `toml:"trade"`
	Sessions            Sessions `toml:"sessions"`
}

type Sessions struct {
	Database     string  `toml:"database"`
	LifetimeDays float64 `toml:"lifetime_days"`
}

type Trade struct {
	RequestRadius float64 `toml:"request_radius"`
	KeepRadius    float64 `toml:"keep_radius"`
	InviteSeconds float64 `toml:"invite_seconds"`
}

type Chat struct {
	NearbyRadius float64 `toml:"nearby_radius"`
}

type Budget struct {
	Enabled                    bool    `toml:"enabled"`
	UplinkBytesPerSecond       float64 `toml:"uplink_bytes_per_second"`
	PerRecipientBytesPerSecond float64 `toml:"per_recipient_bytes_per_second"`
	EntersPerSecond            float64 `toml:"enters_per_second"`
}

type Interest struct {
	EnterRadius       float64 `toml:"enter_radius"`
	LeaveRadius       float64 `toml:"leave_radius"`
	MinDwellSeconds   float64 `toml:"min_dwell_seconds"`
	UpdateHz          float64 `toml:"update_hz"`
	MaxVisible        int     `toml:"max_visible"`
	CapHysteresis     float64 `toml:"cap_hysteresis"`
	NearDistance      float64 `toml:"near_distance"`
	FarRateHz         float64 `toml:"far_rate_hz"`
	MinimumViewRadius float64 `toml:"minimum_view_radius"`
}

type Limits struct {
	MessagesPerSecond             float64 `toml:"messages_per_second"`
	MessageBurst                  float64 `toml:"message_burst"`
	ActionsPerSecond              float64 `toml:"actions_per_second"`
	ActionBurst                   float64 `toml:"action_burst"`
	OrderedBacklog                int     `toml:"ordered_backlog"`
	HelloTimeoutSeconds           float64 `toml:"hello_timeout_seconds"`
	MaxPlayers                    int     `toml:"max_players"`
	MaxPlayersPerAddress          int     `toml:"max_players_per_address"`
	MaxPendingHellos              int     `toml:"max_pending_hellos"`
	MaxPendingHellosPerAddress    int     `toml:"max_pending_hellos_per_address"`
	NewSessionsPerAddressPerMonth float64 `toml:"new_sessions_per_address_per_month"`
}

type Liveness struct {
	PingIntervalSeconds float64 `toml:"ping_interval_seconds"`
	TimeoutSeconds      float64 `toml:"timeout_seconds"`
}

func Default() Config {
	return Config{
		Listen:              "0.0.0.0:3000",
		AdminListen:         "127.0.0.1:3001",
		RecordingsDirectory: "recordings",
		AdminUIDirectory:    "../admin-ui/dist",
		SendRateHz:          20,
		Limits: Limits{
			MessagesPerSecond:             120,
			MessageBurst:                  240,
			ActionsPerSecond:              10,
			ActionBurst:                   10,
			OrderedBacklog:                4096,
			HelloTimeoutSeconds:           5,
			MaxPlayers:                    100,
			MaxPlayersPerAddress:          3,
			MaxPendingHellos:              64,
			MaxPendingHellosPerAddress:    8,
			NewSessionsPerAddressPerMonth: 10,
		},
		Liveness: Liveness{PingIntervalSeconds: 5, TimeoutSeconds: 10},
		Interest: Interest{EnterRadius: 8192, LeaveRadius: 9830, MinDwellSeconds: 1, UpdateHz: 5, MaxVisible: 64,
			CapHysteresis: 1024, NearDistance: 4096, FarRateHz: 10, MinimumViewRadius: 1024},
		Budget:   Budget{UplinkBytesPerSecond: 6_250_000, EntersPerSecond: 20},
		Chat:     Chat{NearbyRadius: 8192},
		Trade:    Trade{RequestRadius: 400, KeepRadius: 1000, InviteSeconds: 30},
		Sessions: Sessions{Database: "sessions.db", LifetimeDays: 30},
	}
}

func Load(path string) (Config, error) {
	c := Default()
	if path != "" {
		data, err := os.ReadFile(path)
		if err != nil {
			return c, err
		}
		meta, err := toml.Decode(string(data), &c)
		if err != nil {
			return c, err
		}
		if undecoded := meta.Undecoded(); len(undecoded) > 0 {
			return c, fmt.Errorf("unknown configuration keys: %v", undecoded)
		}
	}
	return c, c.Validate()
}

func (c Config) Validate() error {
	switch {
	case c.AdminListen != "" && c.AdminListen == c.Listen:
		return errors.New("admin_listen must differ from listen")
	case c.SendRateHz < 1 || c.SendRateHz > 240:
		return errors.New("send_rate_hz must be within 1..240")
	case c.Limits.MessagesPerSecond <= 0 || c.Limits.MessageBurst < 1:
		return errors.New("limits.messages_per_second and message_burst must be positive")
	case c.Limits.ActionsPerSecond <= 0 || c.Limits.ActionBurst < 1:
		return errors.New("limits.actions_per_second and action_burst must be positive")
	case c.Limits.OrderedBacklog < 64:
		return errors.New("limits.ordered_backlog must be at least 64")
	case c.Limits.HelloTimeoutSeconds <= 0:
		return errors.New("limits.hello_timeout_seconds must be positive")
	case c.Limits.MaxPlayers < 0 || c.Limits.MaxPlayersPerAddress < 0 || c.Limits.MaxPendingHellosPerAddress < 0 ||
		c.Limits.NewSessionsPerAddressPerMonth < 0:
		return errors.New("limits.max_players, max_players_per_address, max_pending_hellos_per_address, and new_sessions_per_address_per_month must be >= 0")
	case c.Limits.MaxPendingHellos < 1:
		return errors.New("limits.max_pending_hellos must be at least 1")
	case c.Liveness.PingIntervalSeconds <= 0 || c.Liveness.TimeoutSeconds <= 0:
		return errors.New("liveness intervals must be positive")
	case c.Interest.EnterRadius <= 0 || c.Interest.LeaveRadius < c.Interest.EnterRadius:
		return errors.New("interest.enter_radius must be positive and at most interest.leave_radius")
	case c.Interest.MinDwellSeconds < 0 || c.Interest.UpdateHz < 0.5 || c.Interest.UpdateHz > 60:
		return errors.New("interest.min_dwell_seconds must be >= 0 and interest.update_hz within 0.5..60")
	case c.Interest.MaxVisible < 0 || c.Interest.CapHysteresis < 0 || c.Interest.NearDistance < 0 ||
		c.Interest.FarRateHz < 0 || c.Interest.MinimumViewRadius < 0:
		return errors.New("interest.max_visible, cap_hysteresis, near_distance, far_rate_hz, and minimum_view_radius must be >= 0")
	case c.Budget.Enabled && c.Budget.UplinkBytesPerSecond < 10_000:
		return errors.New("budget.uplink_bytes_per_second must be at least 10000 when the budget is enabled")
	case c.Budget.PerRecipientBytesPerSecond < 0 || c.Budget.EntersPerSecond < 0:
		return errors.New("budget.per_recipient_bytes_per_second and enters_per_second must be >= 0")
	case c.Chat.NearbyRadius <= 0:
		return errors.New("chat.nearby_radius must be positive")
	case c.Trade.RequestRadius <= 0 || c.Trade.KeepRadius < c.Trade.RequestRadius:
		return errors.New("trade.request_radius must be positive and at most trade.keep_radius")
	case c.Trade.InviteSeconds <= 0:
		return errors.New("trade.invite_seconds must be positive")
	case c.Sessions.LifetimeDays <= 0:
		return errors.New("sessions.lifetime_days must be positive")
	}
	for _, host := range c.AdminHosts {
		if host == "" || strings.ContainsAny(host, ":/[] \t") {
			return fmt.Errorf("admin_hosts: %q is not a host name without a port", host)
		}
	}
	_, err := c.TrustedProxyPrefixes()
	return err
}

func (c Config) TrustedProxyPrefixes() ([]netip.Prefix, error) {
	prefixes := make([]netip.Prefix, 0, len(c.TrustedProxies))
	for _, proxy := range c.TrustedProxies {
		if address, err := netip.ParseAddr(proxy); err == nil {
			address = address.Unmap()
			prefixes = append(prefixes, netip.PrefixFrom(address, address.BitLen()))
			continue
		}
		prefix, err := netip.ParsePrefix(proxy)
		if err != nil {
			return nil, fmt.Errorf("trusted_proxies: %q is not an address or a CIDR prefix", proxy)
		}
		prefixes = append(prefixes, prefix.Masked())
	}
	return prefixes, nil
}
