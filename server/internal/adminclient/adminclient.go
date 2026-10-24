package adminclient

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
)

var ErrDisplayNameTaken = errors.New("display name taken")

func IssueSession(ctx context.Context, client *http.Client, admin, displayName string, token []byte) ([]byte, error) {
	body, err := json.Marshal(struct {
		DisplayName string `json:"displayName"`
		Token       []byte `json:"token,omitempty"`
	}{displayName, token})
	if err != nil {
		return nil, err
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, endpoint(admin, "/api/sessions"), bytes.NewReader(body))
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
		return nil, ErrDisplayNameTaken
	case response.StatusCode != http.StatusOK && response.StatusCode != http.StatusCreated:
		return nil, fmt.Errorf("%s: %s", response.Status, reply.Detail)
	case decodeErr != nil:
		return nil, decodeErr
	case len(reply.Token) == 0:
		return nil, errors.New("the reply has no token")
	}
	return reply.Token, nil
}

type FormRef struct {
	Plugin string `json:"plugin"`
	FormID uint32 `json:"formId"`
}

type Player struct {
	ID           string   `json:"id"`
	Name         string   `json:"name"`
	Virtual      bool     `json:"virtual"`
	InWorld      bool     `json:"inWorld"`
	InteriorCell *FormRef `json:"interiorCell"`
	Worldspace   *FormRef `json:"worldspace"`
	Position     *struct {
		X float64 `json:"x"`
		Y float64 `json:"y"`
		Z float64 `json:"z"`
	} `json:"position"`
}

func Players(ctx context.Context, client *http.Client, admin string) ([]Player, error) {
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint(admin, "/api/state"), nil)
	if err != nil {
		return nil, err
	}
	response, err := client.Do(request)
	if err != nil {
		return nil, err
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return nil, errors.New(response.Status)
	}
	var state struct {
		Players []Player `json:"players"`
	}
	if err := json.NewDecoder(response.Body).Decode(&state); err != nil {
		return nil, err
	}
	return state.Players, nil
}

func endpoint(admin, path string) string {
	return strings.TrimSuffix(admin, "/") + path
}

type SessionFile struct {
	Run    string            `json:"run,omitempty"`
	Tokens map[string][]byte `json:"tokens"`
}

func DefaultSessionsPath(tool, serverURL string) (string, error) {
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
	return filepath.Join(cache, "playerhub", tool+"-"+host+".json"), nil
}

func LoadSessions(path string) (SessionFile, error) {
	file := SessionFile{Tokens: make(map[string][]byte)}
	data, err := os.ReadFile(path)
	if errors.Is(err, fs.ErrNotExist) {
		return file, nil
	}
	if err != nil {
		return file, err
	}
	if err := json.Unmarshal(data, &file); err != nil {
		return file, fmt.Errorf("%s: %w", path, err)
	}
	if file.Tokens == nil {
		file.Tokens = make(map[string][]byte)
	}
	return file, nil
}

func SaveSessions(path string, file SessionFile) error {
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
