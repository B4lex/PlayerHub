// Package config loads the server's TOML configuration. Link-dependent limits
// live here so the homelab and a remote server differ only in this file.
package config

import (
	"errors"
	"fmt"
	"os"

	"github.com/BurntSushi/toml"
)

type Config struct {
	Listen              string   `toml:"listen"`
	RecordingsDirectory string   `toml:"recordings_directory"`
	SendRateHz          float64  `toml:"send_rate_hz"`
	Limits              Limits   `toml:"limits"`
	Liveness            Liveness `toml:"liveness"`
}

type Limits struct {
	MessagesPerSecond   float64 `toml:"messages_per_second"`
	MessageBurst        float64 `toml:"message_burst"`
	ActionsPerSecond    float64 `toml:"actions_per_second"`
	ActionBurst         float64 `toml:"action_burst"`
	OrderedBacklog      int     `toml:"ordered_backlog"`
	HelloTimeoutSeconds float64 `toml:"hello_timeout_seconds"`
}

type Liveness struct {
	PingIntervalSeconds float64 `toml:"ping_interval_seconds"`
	TimeoutSeconds      float64 `toml:"timeout_seconds"`
}

func Default() Config {
	return Config{
		Listen:              "0.0.0.0:3000",
		RecordingsDirectory: "recordings",
		SendRateHz:          40,
		Limits: Limits{
			MessagesPerSecond:   120,
			MessageBurst:        240,
			ActionsPerSecond:    10,
			ActionBurst:         10,
			OrderedBacklog:      4096,
			HelloTimeoutSeconds: 5,
		},
		Liveness: Liveness{PingIntervalSeconds: 5, TimeoutSeconds: 10},
	}
}

// Load reads path over the defaults. An empty path means defaults only.
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
	return c, c.validate()
}

func (c Config) validate() error {
	switch {
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
	case c.Liveness.PingIntervalSeconds <= 0 || c.Liveness.TimeoutSeconds <= 0:
		return errors.New("liveness intervals must be positive")
	}
	return nil
}
