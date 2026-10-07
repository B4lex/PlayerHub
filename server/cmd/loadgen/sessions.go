package main

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"math/rand/v2"
	"net/http"
	"time"

	"playerhub/server/internal/adminclient"
)

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
		if path, err = adminclient.DefaultSessionsPath("loadgen", o.url); err != nil {
			return nil, err
		}
	}
	file, err := adminclient.LoadSessions(path)
	if err != nil {
		return nil, err
	}
	if file.Run == "" {
		file.Run = randomRun()
	}
	o.run = file.Run

	client := &http.Client{Timeout: 10 * time.Second}
	changed := false
	var issueErr error
	for i := range tokens {
		name := displayName(kind, o.run, i)
		token, err := adminclient.IssueSession(ctx, client, o.admin, name, file.Tokens[name])
		if errors.Is(err, adminclient.ErrDisplayNameTaken) {
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
		if err := adminclient.SaveSessions(path, file); err != nil {
			return nil, errors.Join(issueErr, err)
		}
	}
	if issueErr != nil {
		return nil, issueErr
	}
	return tokens, nil
}
