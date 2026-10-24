package recording

import (
	"errors"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"playerhub/server/internal/wire/pb"
)

func quietLog() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

const testRecording = `{"type":"header","format":"playerhub-recording","version":5,"id":"20260921-101500-abc123","label":"Test capture","playerId":"player-1","playerName":"Lydia Stormblade","startedAt":"2026-09-21T10:15:00Z"}
{"type":"message","t":0,"message":{"appearance":{"revision":"1","name":"Lydia Stormblade","level":12,"race":{"plugin":"Skyrim.esm","formId":79686},"female":true,"weight":42.5,"height":1,"headParts":[{"form":{"plugin":"Skyrim.esm","formId":333173},"type":1}],"hairColor":{"r":59,"g":42,"b":30},"bodyTint":{"r":222,"g":188,"b":170},"faceMorphs":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3.4028235e+38],"faceParts":[0,1,-1,2],"tints":[{"index":44,"type":6,"color":{"r":1,"g":2,"b":3},"alpha":0.5}],"defaultRace":{"race":{"plugin":"Skyrim.esm","formId":1},"female":true}}}}
{"type":"message","t":0,"message":{"equipment":{"revision":"1","armor":[{"form":{"plugin":"Skyrim.esm","formId":77385},"slotMask":4}],"rightHand":{"kind":"HAND_KIND_WEAPON","form":{"plugin":"Skyrim.esm","formId":77495},"weaponType":1},"leftHand":{"kind":"HAND_KIND_SPELL"},"defaultOutfit":{"hands":[{"kind":"HAND_KIND_TORCH","form":{"plugin":"Skyrim.esm","formId":2}}]}}}}
{"type":"message","t":0,"message":{"enter":{}}}
{"type":"message","t":50,"message":{"context":{"worldspaceId":60,"graphEpoch":"1"}}}
{"type":"message","t":50,"message":{"checkpoint":{"sequence":"1","x":6,"y":10,"z":-14,"heading":2048,"pitch":102,"speed":400,"direction":128,"flags":18}}}
{"type":"message","t":100,"message":{"checkpoint":{"sequence":"2","x":20,"y":8,"z":12,"flags":16}}}
{"type":"message","t":120,"message":{"action":{"actionSequence":"1","graphEpoch":"1","checkpointSequence":"2","attack":{"action":"ATTACK_ACTION_DUAL","direction":"POWER_DIRECTION_LEFT"}}}}
{"type":"message","t":150,"message":{"context":{"interiorCellId":7,"graphEpoch":"1","furniture":{"plugin":"Skyrim.esm","formId":9}}}}
{"type":"message","t":150,"message":{"checkpoint":{"sequence":"3","x":20,"y":8,"z":12}}}
{"type":"end","t":200,"reason":"stopped"}
`

func TestParse(t *testing.T) {
	r, err := Parse([]byte(testRecording))
	if err != nil {
		t.Fatal(err)
	}
	if r.Header.PlayerName != "Lydia Stormblade" || r.End == nil || r.End.Reason != "stopped" || r.DurationMs() != 200 {
		t.Fatalf("header or end lost: %+v %+v", r.Header, r.End)
	}
	var kinds []string
	for _, m := range r.Messages {
		kinds = append(kinds, strings.TrimPrefix(strings.Split(m.Message.String(), ":")[0], "playerhub.v1."))
	}
	want := "appearance equipment enter context checkpoint checkpoint action context checkpoint"
	if got := strings.Join(kinds, " "); got != want {
		t.Fatalf("got %s\nwant %s", got, want)
	}
	if a := r.Messages[6].Message.GetAction().GetAttack(); a.GetAction() != pb.AttackAction_ATTACK_ACTION_DUAL ||
		a.GetDirection() != pb.PowerDirection_POWER_DIRECTION_LEFT {
		t.Fatalf("action parsed as %v", a)
	}
	if c := r.Messages[7].Message.GetContext(); c.InteriorCellId != 7 || c.Furniture.GetFormId() != 9 {
		t.Fatalf("context parsed as %v", c)
	}
}

func TestParseToleratesATornLastLine(t *testing.T) {
	lines := strings.Split(strings.TrimSpace(testRecording), "\n")
	torn := strings.Join(lines[:len(lines)-1], "\n") + "\n" + lines[len(lines)-2][:20]
	r, err := Parse([]byte(torn))
	if err != nil {
		t.Fatal(err)
	}
	if r.End != nil || len(r.Messages) != len(lines)-2 {
		t.Fatalf("got %d messages, end %v", len(r.Messages), r.End)
	}
	broken := strings.Join(append([]string{lines[0], "{"}, lines[1:]...), "\n")
	if _, err := Parse([]byte(broken)); err == nil {
		t.Fatal("a broken middle line was accepted")
	}
}

func TestParseRejectsOtherVersions(t *testing.T) {
	var recordingError *Error
	older := strings.Replace(testRecording, `"version":5`, `"version":4`, 1)
	if _, err := Parse([]byte(older)); !errors.As(err, &recordingError) || !strings.Contains(err.Error(), "version 4") {
		t.Fatalf("got %v", err)
	}
}

func TestMarkers(t *testing.T) {
	r, err := Parse([]byte(testRecording))
	if err != nil {
		t.Fatal(err)
	}
	var labels []string
	for _, m := range Markers(r) {
		labels = append(labels, m.Kind+": "+m.Label)
	}
	want := "attack: Power dual attack (left)|furniture: Started using furniture|area: Entered interior 0x00000007|weapon: Weapon sheathed"
	if got := strings.Join(labels, "|"); got != want {
		t.Fatalf("got %s", got)
	}
}

func TestMarkersBeastAndUnknownActions(t *testing.T) {
	action := func(a *pb.Action) Message {
		return Message{10, &pb.ClientMessage{Message: &pb.ClientMessage_Action{Action: a}}}
	}
	r := &Recording{Messages: []Message{
		action(&pb.Action{Kind: &pb.Action_Beast{Beast: pb.BeastAction_BEAST_ACTION_WEREWOLF_ATTACK_RIGHT_POWER}}),
		action(&pb.Action{Kind: &pb.Action_Attack{Attack: &pb.Attack{Action: pb.AttackAction_ATTACK_ACTION_UNSPECIFIED}}}),
		action(&pb.Action{Kind: &pb.Action_Attack{Attack: &pb.Attack{Action: pb.AttackAction(99)}}}),
		action(&pb.Action{}),
	}}
	markers := Markers(r)
	if len(markers) != 1 || markers[0].Label != "Werewolf attack right power" {
		t.Fatalf("got %+v", markers)
	}
}

func TestCaptureLifecycle(t *testing.T) {
	directory := t.TempDir()
	store := NewStore(directory, quietLog())
	captures := NewCaptures(store, quietLog(), func() {})
	now := time.Date(2026, 9, 30, 12, 0, 0, 0, time.UTC)
	initial := []*pb.ClientMessage{
		{Message: &pb.ClientMessage_Enter{Enter: &pb.Enter{}}},
		{Message: &pb.ClientMessage_Context{Context: &pb.Context{WorldspaceId: 60, GraphEpoch: 1}}},
		{Message: &pb.ClientMessage_Checkpoint{Checkpoint: &pb.Checkpoint{Sequence: 4}}},
	}
	info, err := captures.Start(3, "Lydia", "", initial, now)
	if err != nil {
		t.Fatal(err)
	}
	if info.Status != StatusRecording || info.MessageCount != 3 || !strings.HasPrefix(info.Label, "Lydia ") {
		t.Fatalf("got %+v", info)
	}
	if _, err := captures.Start(3, "Lydia", "", nil, now); !errors.Is(err, ErrAlreadyCapturing) {
		t.Fatalf("second capture: %v", err)
	}
	captures.Record(3, &pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: &pb.Checkpoint{Sequence: 5}}})
	captures.Record(4, &pb.ClientMessage{Message: &pb.ClientMessage_Exit{Exit: &pb.Exit{}}})
	if listed := store.Infos(map[string]bool{info.ID: true}); len(listed) != 0 {
		t.Fatalf("an active capture was listed: %v", listed)
	}
	final, err := captures.StopRecording(info.ID, "stopped")
	if err != nil {
		t.Fatal(err)
	}
	if final.Status != StatusComplete || final.MessageCount != 4 || *final.EndReason != "stopped" {
		t.Fatalf("got %+v", final)
	}
	r, err := store.Load(info.ID)
	if err != nil {
		t.Fatal(err)
	}
	if len(r.Messages) != 4 || r.Messages[3].Message.GetCheckpoint().GetSequence() != 5 {
		t.Fatalf("got %d messages", len(r.Messages))
	}
	if err := store.Delete(info.ID); err != nil {
		t.Fatal(err)
	}
	if _, err := store.Load(info.ID); !errors.Is(err, ErrNotFound) {
		t.Fatalf("deleted recording loaded: %v", err)
	}
}

func TestInterruptedCaptureIsListed(t *testing.T) {
	directory := t.TempDir()
	store := NewStore(directory, quietLog())
	unfinished := strings.Join(strings.Split(strings.TrimSpace(testRecording), "\n")[:4], "\n") + "\n"
	path := filepath.Join(directory, "20260921-101500-abc123.jsonl")
	if err := os.WriteFile(path, []byte(unfinished), 0o644); err != nil {
		t.Fatal(err)
	}
	os.WriteFile(filepath.Join(directory, "bad name.jsonl"), []byte("x"), 0o644)
	infos := store.Infos(nil)
	if len(infos) != 1 || infos[0].Status != StatusInterrupted || infos[0].MessageCount != 3 {
		t.Fatalf("got %+v", infos)
	}
	if _, err := store.Path("../escape"); !errors.Is(err, ErrNotFound) {
		t.Fatal("an unsafe ID was accepted")
	}
}
