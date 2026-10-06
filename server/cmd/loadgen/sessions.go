package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"math/rand/v2"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"time"
)

type sessionFile struct {
	Run    string            `json:"run"`
	Tokens map[string][]byte `json:"tokens"`
}

func randomRun() string {
	return fmt.Sprintf("%06x", rand.Uint32()&0xFFFFFF)
}

func displayName(kind, run string, index int) string {
	return fmt.Sprintf("%s%s_%d", kind, run, index+1)
}

// prepareSessions sets o.run and returns each bot's token. Without an admin address the bots
// take new sessions under names of a new run.
func prepareSessions(ctx context.Context, o *options, kind string) ([][]byte, error) {
	tokens := make([][]byte, o.bots)
	if o.admin == "" {
		o.run = randomRun()
		return tokens, nil
	}
	path := o.sessions
	if path == "" {
		var err error
		if path, err = defaultSessionsPath(o.url); err != nil {
			return nil, err
		}
	}
	file := sessionFile{}
	if data, err := os.ReadFile(path); err == nil {
		if err := json.Unmarshal(data, &file); err != nil {
			return nil, fmt.Errorf("%s: %w", path, err)
		}
	} else if !errors.Is(err, fs.ErrNotExist) {
		return nil, err
	}
	if file.Run == "" {
		file.Run = randomRun()
	}
	if file.Tokens == nil {
		file.Tokens = make(map[string][]byte)
	}
	o.run = file.Run

	client := &http.Client{Timeout: 10 * time.Second}
	changed := false
	var issueErr error
	for i := range tokens {
		name := displayName(kind, o.run, i)
		token, err := issueSession(ctx, client, o.admin, name, file.Tokens[name])
		if errors.Is(err, errDisplayNameTaken) {
			issueErr = fmt.Errorf("%s is taken by a session %s does not hold; delete the file for new names", name, path)
			break
		}
		if err != nil {
			issueErr = fmt.Errorf("cannot get a session for %s from %s: %w", name, o.admin, err)
			break
		}
		changed = changed || !bytes.Equal(token, file.Tokens[name])
		file.Tokens[name], tokens[i] = token, token
	}
	// Sessions issued before a failure are kept too, or their names would stay taken.
	if changed {
		if err := saveSessions(path, file); err != nil {
			return nil, errors.Join(issueErr, err)
		}
	}
	if issueErr != nil {
		return nil, issueErr
	}
	return tokens, nil
}

func defaultSessionsPath(serverURL string) (string, error) {
	u, err := url.Parse(serverURL)
	if err != nil {
		return "", err
	}
	cache, err := os.UserCacheDir()
	if err != nil {
		return "", err
	}
	host := strings.Map(func(r rune) rune {
		if 'a' <= r && r <= 'z' || 'A' <= r && r <= 'Z' || '0' <= r && r <= '9' || r == '.' || r == '-' {
			return r
		}
		return '_'
	}, u.Host)
	return filepath.Join(cache, "playerhub", "loadgen-"+host+".json"), nil
}

func saveSessions(path string, file sessionFile) error {
	data, err := json.MarshalIndent(file, "", "\t")
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return err
	}
	temporary := path + ".tmp"
	if err := os.WriteFile(temporary, data, 0o600); err != nil {
		return err
	}
	return os.Rename(temporary, path)
}

var errDisplayNameTaken = errors.New("display name taken")

func issueSession(ctx context.Context, client *http.Client, admin, displayName string, token []byte) ([]byte, error) {
	body, err := json.Marshal(struct {
		DisplayName string `json:"displayName"`
		Token       []byte `json:"token,omitempty"`
	}{displayName, token})
	if err != nil {
		return nil, err
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, strings.TrimSuffix(admin, "/")+"/api/sessions",
		bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	request.Header.Set("Content-Type", "application/json")
	response, err := client.Do(request)
	if err != nil {
		return nil, err
	}
	defer response.Body.Close()
	var reply struct {
		Token  []byte `json:"token"`
		Detail string `json:"detail"`
	}
	decodeErr := json.NewDecoder(response.Body).Decode(&reply)
	switch {
	case response.StatusCode == http.StatusConflict:
		return nil, errDisplayNameTaken
	case response.StatusCode != http.StatusOK && response.StatusCode != http.StatusCreated:
		return nil, fmt.Errorf("%s: %s", response.Status, reply.Detail)
	case decodeErr != nil:
		return nil, decodeErr
	case len(reply.Token) == 0:
		return nil, errors.New("the reply has no token")
	}
	return reply.Token, nil
}
