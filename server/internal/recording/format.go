// Package recording stores captured players as append-only JSON Lines files:
//
//	{"type":"header","format":"skyrim-mp-recording","version":3,"id":...}
//	{"type":"message","t":0,"message":{"enter":{}}}
//	{"type":"end","t":75266,"reason":"stopped"}
//
// A message line holds a validated ClientMessage in protojson. A file without
// an end line is an interrupted capture, and a torn last line is ignored.
// Playback numbers sequences and revisions itself, so nothing depends on the
// capture's numbering.
package recording

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"os"

	"google.golang.org/protobuf/encoding/protojson"

	"skyrim-mp/server/internal/validate"
	"skyrim-mp/server/internal/wire/pb"
)

const (
	FormatName    = "skyrim-mp-recording"
	FormatVersion = 3
)

// Error is a recording file that cannot be read or is not valid.
type Error struct{ message string }

func (e *Error) Error() string { return e.message }

func errorf(format string, args ...any) error {
	return &Error{fmt.Sprintf(format, args...)}
}

type Header struct {
	ID         string `json:"id"`
	Label      string `json:"label"`
	PlayerID   string `json:"playerId"`
	PlayerName string `json:"playerName"`
	// ISO-8601 UTC.
	StartedAt string `json:"startedAt"`
}

type Message struct {
	TMs     int64
	Message *pb.ClientMessage
}

type End struct {
	TMs    int64
	Reason string
}

type Recording struct {
	Header   Header
	Messages []Message
	End      *End
}

func (r *Recording) DurationMs() int64 {
	if r.End != nil {
		return r.End.TMs
	}
	if len(r.Messages) > 0 {
		return r.Messages[len(r.Messages)-1].TMs
	}
	return 0
}

// Info is listing metadata, without the messages.
type Info struct {
	ID           string  `json:"id"`
	Label        string  `json:"label"`
	PlayerID     string  `json:"playerId"`
	PlayerName   string  `json:"playerName"`
	StartedAt    string  `json:"startedAt"`
	DurationMs   int64   `json:"durationMs"`
	MessageCount int     `json:"messageCount"`
	SizeBytes    int64   `json:"sizeBytes"`
	Status       string  `json:"status"`
	EndReason    *string `json:"endReason"`
}

const (
	StatusRecording   = "recording"
	StatusComplete    = "complete"
	StatusInterrupted = "interrupted"
)

type headerLine struct {
	Type    string `json:"type"`
	Format  string `json:"format"`
	Version int    `json:"version"`
	Header
}

type bodyLine struct {
	Type    string          `json:"type"`
	T       *int64          `json:"t"`
	Message json.RawMessage `json:"message,omitempty"`
	Reason  *string         `json:"reason,omitempty"`
}

var protojsonOptions = protojson.MarshalOptions{}

func EncodeHeader(h Header) []byte {
	line, _ := json.Marshal(headerLine{Type: "header", Format: FormatName, Version: FormatVersion, Header: h})
	return line
}

func EncodeMessage(m Message) ([]byte, error) {
	payload, err := protojsonOptions.Marshal(m.Message)
	if err != nil {
		return nil, err
	}
	// protojson output is not guaranteed stable across releases; compact it so
	// every message is one line.
	var compact bytes.Buffer
	if err := json.Compact(&compact, payload); err != nil {
		return nil, err
	}
	t := m.TMs
	return json.Marshal(bodyLine{Type: "message", T: &t, Message: compact.Bytes()})
}

func EncodeEnd(e End) []byte {
	t, reason := e.TMs, e.Reason
	line, _ := json.Marshal(bodyLine{Type: "end", T: &t, Reason: &reason})
	return line
}

func parseHeader(line []byte) (Header, error) {
	var h headerLine
	if err := json.Unmarshal(line, &h); err != nil {
		return Header{}, errorf("invalid header: %v", err)
	}
	if h.Type != "header" || h.Format != FormatName {
		return Header{}, errorf("not a %s file", FormatName)
	}
	if h.Version != FormatVersion {
		return Header{}, errorf("recording format version %d; version %d is needed (convert-recordings converts version 2)",
			h.Version, FormatVersion)
	}
	return h.Header, nil
}

func splitLines(data []byte) [][]byte {
	var lines [][]byte
	for _, line := range bytes.Split(data, []byte("\n")) {
		if len(bytes.TrimSpace(line)) > 0 {
			lines = append(lines, line)
		}
	}
	return lines
}

// Read loads and validates a recording, tolerating a torn last line.
func Read(path string) (*Recording, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, errorf("cannot read %s: %v", path, err)
	}
	return Parse(data)
}

func Parse(data []byte) (*Recording, error) {
	lines := splitLines(data)
	if len(lines) == 0 {
		return nil, errorf("empty recording")
	}
	header, err := parseHeader(lines[0])
	if err != nil {
		return nil, err
	}
	r := &Recording{Header: header}
	var previous int64
	for i, line := range lines[1:] {
		number := i + 2
		var body bodyLine
		if err := json.Unmarshal(line, &body); err != nil {
			if number == len(lines) {
				// Interrupted mid-write; everything before it is intact.
				break
			}
			return nil, errorf("line %d: invalid JSON: %v", number, err)
		}
		if r.End != nil {
			return nil, errorf("line %d: content after the end marker", number)
		}
		if body.T == nil || *body.T < 0 {
			return nil, errorf("line %d: missing or invalid timestamp", number)
		}
		if *body.T < previous {
			return nil, errorf("line %d: timestamp goes backwards", number)
		}
		previous = *body.T
		switch body.Type {
		case "end":
			if body.Reason == nil {
				return nil, errorf("line %d: invalid end reason", number)
			}
			r.End = &End{*body.T, *body.Reason}
		case "message":
			message := &pb.ClientMessage{}
			if err := protojson.Unmarshal(body.Message, message); err != nil {
				return nil, errorf("line %d: invalid message: %v", number, err)
			}
			if err := validateRecorded(message); err != nil {
				return nil, errorf("line %d: %v", number, err)
			}
			r.Messages = append(r.Messages, Message{*body.T, message})
		default:
			return nil, errorf("line %d: unknown line type %q", number, body.Type)
		}
	}
	return r, nil
}

func validateRecorded(m *pb.ClientMessage) error {
	switch m.Message.(type) {
	case *pb.ClientMessage_Enter, *pb.ClientMessage_Exit, *pb.ClientMessage_Context, *pb.ClientMessage_Checkpoint,
		*pb.ClientMessage_Action, *pb.ClientMessage_Appearance, *pb.ClientMessage_Equipment:
		return validate.Message(m)
	}
	return errors.New("message kind is never recorded")
}

// ScanInfo summarizes a recording from its header and last lines without
// validating messages; Read validates when one is opened.
func ScanInfo(path string) (Info, error) {
	stat, err := os.Stat(path)
	if err != nil {
		return Info{}, errorf("cannot read %s: %v", path, err)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return Info{}, errorf("cannot read %s: %v", path, err)
	}
	lines := splitLines(data)
	if len(lines) == 0 {
		return Info{}, errorf("empty recording")
	}
	header, err := parseHeader(lines[0])
	if err != nil {
		return Info{}, err
	}
	body := lines[1:]
	var last bodyLine
	parseLast := func() bool {
		return len(body) > 0 && json.Unmarshal(body[len(body)-1], &last) == nil
	}
	ok := parseLast()
	if !ok && len(body) > 0 {
		// Torn final line.
		body = body[:len(body)-1]
		ok = parseLast()
	}
	info := Info{
		ID: header.ID, Label: header.Label, PlayerID: header.PlayerID, PlayerName: header.PlayerName,
		StartedAt: header.StartedAt, SizeBytes: stat.Size(), Status: StatusInterrupted,
	}
	if ok && last.T != nil && *last.T >= 0 {
		info.DurationMs = *last.T
	}
	if ok && last.Type == "end" {
		reason := "unknown"
		if last.Reason != nil {
			reason = *last.Reason
		}
		info.Status, info.EndReason = StatusComplete, &reason
		body = body[:len(body)-1]
	}
	info.MessageCount = len(body)
	return info, nil
}
