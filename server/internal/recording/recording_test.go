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

	"skyrim-mp/server/internal/wire/pb"
)

func quietLog() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

const v2Recording = `{"type":"header","format":"skyrim-mp-recording","version":2,"id":"20260921-101500-abc123","label":"Test capture","playerId":"player-1","playerName":"Lydia Stormblade","startedAt":"2026-09-21T10:15:00Z"}
{"type":"message","t":0,"event":"player:appearance","payload":{"revision":1,"name":"Lydia Stormblade","level":12,"race":{"plugin":"Skyrim.esm","formId":79686},"female":true,"weight":42.5,"height":1.0,"headParts":[{"plugin":"Skyrim.esm","formId":333173,"type":1}],"hairColor":{"r":59,"g":42,"b":30},"faceDetails":null,"bodyTint":{"r":222,"g":188,"b":170},"faceMorphs":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3.4028234663852886e38],"faceParts":[0,1,-1,2],"tints":[{"index":44,"type":6,"r":1,"g":2,"b":3,"alpha":0.5}],"defaultRace":{"race":{"plugin":"Skyrim.esm","formId":1},"female":true}}}
{"type":"message","t":0,"event":"player:equipment","payload":{"revision":1,"armor":[{"plugin":"Skyrim.esm","formId":77385,"slotMask":4}],"rightHand":{"plugin":"Skyrim.esm","formId":77495,"kind":"weapon","weaponType":1},"leftHand":{"kind":"spell"},"ammo":null,"defaultOutfit":{"armor":[],"hands":[{"plugin":"Skyrim.esm","formId":2,"kind":"torch"}],"ammo":null}}}
{"type":"message","t":0,"event":"player:enter","payload":{}}
{"type":"message","t":50,"event":"player:update","payload":{"x":1.4,"y":2.6,"z":-3.5,"heading":3.14159,"pitch":0.1,"speed":400.2,"direction":0.5,"movementState":"running","jumping":false,"weaponDrawn":true,"bowDrawn":false,"castingLeft":false,"castingRight":false,"blocking":false,"dead":false,"mounted":false,"swimming":false,"cellId":1,"worldspaceId":60,"furniture":null,"graphEpoch":1,"sequence":1}}
{"type":"message","t":100,"event":"player:update","payload":{"x":5,"y":2,"z":3,"heading":0,"speed":0,"direction":0,"movementState":"idle","jumping":false,"weaponDrawn":true,"cellId":1,"worldspaceId":60,"graphEpoch":1,"sequence":2}}
{"type":"message","t":120,"event":"animation:action","payload":{"actionSequence":1,"graphEpoch":1,"checkpointSequence":2,"family":"attack","action":"dual","power":true,"direction":"left","prerequisites":{"weaponDrawn":true}}}
{"type":"message","t":150,"event":"player:update","payload":{"x":5,"y":2,"z":3,"heading":0,"speed":0,"direction":0,"movementState":"idle","jumping":false,"weaponDrawn":false,"cellId":7,"worldspaceId":0,"furniture":{"plugin":"Skyrim.esm","formId":9},"graphEpoch":1,"sequence":3}}
{"type":"end","t":200,"reason":"stopped"}
`

func TestConvertV2(t *testing.T) {
	converted, err := ConvertV2([]byte(v2Recording))
	if err != nil {
		t.Fatal(err)
	}
	r, err := Parse(converted)
	if err != nil {
		t.Fatal(err)
	}
	if r.Header.PlayerName != "Lydia Stormblade" || r.End == nil || r.End.Reason != "stopped" || r.DurationMs() != 200 {
		t.Fatalf("header or end lost: %+v %+v", r.Header, r.End)
	}
	var kinds []string
	for _, m := range r.Messages {
		kinds = append(kinds, strings.TrimPrefix(strings.Split(m.Message.String(), ":")[0], "skyrimmp.v1."))
	}
	want := "appearance equipment enter context checkpoint checkpoint action context checkpoint"
	if got := strings.Join(kinds, " "); got != want {
		t.Fatalf("got %s\nwant %s", got, want)
	}
	first := r.Messages[4].Message.GetCheckpoint()
	if first.X != 1 || first.Y != 3 || first.Z != -4 || first.Heading != 2048 || first.Pitch != 102 ||
		first.Speed != 400 || first.Direction != 128 || first.Flags != 2|16 {
		t.Fatalf("checkpoint quantized as %v", first)
	}
	if a := r.Messages[6].Message.GetAction(); a.Action != pb.AttackAction_ATTACK_ACTION_DUAL ||
		a.Direction != pb.PowerDirection_POWER_DIRECTION_LEFT || !a.Power {
		t.Fatalf("action converted as %v", a)
	}
	if c := r.Messages[7].Message.GetContext(); c.CellId != 7 || c.Furniture.GetFormId() != 9 {
		t.Fatalf("context converted as %v", c)
	}
	if e := r.Messages[1].Message.GetEquipment(); e.LeftHand.Kind != pb.HandKind_HAND_KIND_SPELL ||
		len(e.DefaultOutfit.GetHands()) != 1 {
		t.Fatalf("equipment converted as %v", e)
	}
}

func TestParseToleratesATornLastLine(t *testing.T) {
	converted, err := ConvertV2([]byte(v2Recording))
	if err != nil {
		t.Fatal(err)
	}
	lines := strings.Split(strings.TrimSpace(string(converted)), "\n")
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
	if _, err := Parse([]byte(v2Recording)); !errors.As(err, &recordingError) || !strings.Contains(err.Error(), "convert") {
		t.Fatalf("got %v", err)
	}
}

func TestMarkers(t *testing.T) {
	converted, _ := ConvertV2([]byte(v2Recording))
	r, err := Parse(converted)
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
	converted, _ := ConvertV2([]byte(v2Recording))
	unfinished := strings.Join(strings.Split(strings.TrimSpace(string(converted)), "\n")[:4], "\n") + "\n"
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
