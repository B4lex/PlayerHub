package recording

import (
	"bufio"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"skyrim-mp/server/internal/wire/pb"
)

const (
	MaximumLabelLength = 100
	captureQueueLines  = 4096
)

var ErrAlreadyCapturing = errors.New("player is already being recorded")

type Captures struct {
	store    *Store
	log      *slog.Logger
	onChange func()

	mu       sync.Mutex
	byPlayer map[uint32]*capture
}

type capture struct {
	header   Header
	started  time.Time
	lines    chan []byte
	done     chan struct{}
	messages int
	bytes    int64
}

func NewCaptures(store *Store, log *slog.Logger, onChange func()) *Captures {
	return &Captures{store: store, log: log, onChange: onChange, byPlayer: make(map[uint32]*capture)}
}

func (c *Captures) Start(slot uint32, playerName, label string, initial []*pb.ClientMessage, now time.Time) (Info, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if existing, ok := c.byPlayer[slot]; ok {
		return Info{}, fmt.Errorf("%w as %s", ErrAlreadyCapturing, existing.header.ID)
	}
	label = strings.TrimSpace(label)
	if len(label) > MaximumLabelLength {
		label = label[:MaximumLabelLength]
	}
	if label == "" {
		name := playerName
		if name == "" {
			name = "Player " + strconv.FormatUint(uint64(slot), 10)
		}
		label = name + " " + now.Local().Format("2006-01-02 15:04")
	}
	header := Header{
		ID: c.store.NewID(now), Label: label, PlayerID: strconv.FormatUint(uint64(slot), 10),
		PlayerName: playerName, StartedAt: now.UTC().Format(time.RFC3339),
	}
	file, err := c.store.Create(header)
	if err != nil {
		return Info{}, err
	}
	capture := &capture{header: header, started: time.Now(), lines: make(chan []byte, captureQueueLines), done: make(chan struct{})}
	first := append(EncodeHeader(header), '\n')
	capture.bytes = int64(len(first))
	lines := [][]byte{first}
	for _, message := range initial {
		line, err := EncodeMessage(Message{0, message})
		if err != nil {
			file.Close()
			return Info{}, err
		}
		lines = append(lines, append(line, '\n'))
		capture.messages++
		capture.bytes += int64(len(line) + 1)
	}
	go c.write(capture, file, lines)
	c.byPlayer[slot] = capture
	c.log.Info("capture started", "slot", slot, "recording", header.ID)
	c.onChange()
	return c.infoLocked(capture, time.Now()), nil
}

func (c *Captures) Record(slot uint32, message *pb.ClientMessage) {
	c.mu.Lock()
	defer c.mu.Unlock()
	capture, ok := c.byPlayer[slot]
	if !ok {
		return
	}
	now := time.Now()
	line, err := EncodeMessage(Message{now.Sub(capture.started).Milliseconds(), message})
	if err != nil {
		c.log.Error("cannot encode captured message", "recording", capture.header.ID, "error", err)
		return
	}
	select {
	case capture.lines <- append(line, '\n'):
		capture.messages++
		capture.bytes += int64(len(line) + 1)
		c.onChange()
	default:
		c.log.Error("capture cannot keep up with the disk, stopping", "recording", capture.header.ID)
		c.finishLocked(slot, "disk too slow", now)
	}
}

func (c *Captures) Stop(slot uint32, reason string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if _, ok := c.byPlayer[slot]; ok {
		c.finishLocked(slot, reason, time.Now())
	}
}

func (c *Captures) StopRecording(id, reason string) (Info, error) {
	c.mu.Lock()
	var finished *capture
	for slot, capture := range c.byPlayer {
		if capture.header.ID == id {
			finished = capture
			c.finishLocked(slot, reason, time.Now())
			break
		}
	}
	c.mu.Unlock()
	if finished == nil {
		return Info{}, ErrNotFound
	}
	<-finished.done
	return c.store.Info(id)
}

func (c *Captures) StopAll(reason string) {
	c.mu.Lock()
	var pending []*capture
	for slot, capture := range c.byPlayer {
		pending = append(pending, capture)
		c.finishLocked(slot, reason, time.Now())
	}
	c.mu.Unlock()
	for _, capture := range pending {
		<-capture.done
	}
}

func (c *Captures) RecordingFor(slot uint32) string {
	c.mu.Lock()
	defer c.mu.Unlock()
	if capture, ok := c.byPlayer[slot]; ok {
		return capture.header.ID
	}
	return ""
}

func (c *Captures) IsActive(id string) bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	for _, capture := range c.byPlayer {
		if capture.header.ID == id {
			return true
		}
	}
	return false
}

func (c *Captures) ActiveInfos() []Info {
	c.mu.Lock()
	defer c.mu.Unlock()
	now := time.Now()
	infos := make([]Info, 0, len(c.byPlayer))
	for _, capture := range c.byPlayer {
		infos = append(infos, c.infoLocked(capture, now))
	}
	return infos
}

func (c *Captures) finishLocked(slot uint32, reason string, now time.Time) {
	capture := c.byPlayer[slot]
	delete(c.byPlayer, slot)
	end := EncodeEnd(End{now.Sub(capture.started).Milliseconds(), reason})
	// The queue may be full when the disk fell behind; the end line is then dropped and the
	// recording reads as interrupted.
	select {
	case capture.lines <- append(end, '\n'):
	default:
	}
	close(capture.lines)
	c.log.Info("capture stopped", "slot", slot, "recording", capture.header.ID, "reason", reason,
		"messages", capture.messages)
	c.onChange()
}

func (c *Captures) write(capture *capture, file *os.File, initial [][]byte) {
	defer close(capture.done)
	writer := bufio.NewWriter(file)
	failed := false
	put := func(line []byte) {
		if failed {
			return
		}
		if _, err := writer.Write(line); err != nil {
			failed = true
			c.log.Error("capture failed", "recording", capture.header.ID, "error", err)
		}
	}
	for _, line := range initial {
		put(line)
	}
	for {
		// Flush whenever the queue runs dry, so a crash loses little.
		if len(capture.lines) == 0 && !failed {
			if err := writer.Flush(); err != nil {
				failed = true
				c.log.Error("capture failed", "recording", capture.header.ID, "error", err)
			}
		}
		line, ok := <-capture.lines
		if !ok {
			break
		}
		put(line)
	}
	if !failed {
		writer.Flush()
	}
	file.Close()
}

func (c *Captures) infoLocked(capture *capture, now time.Time) Info {
	h := capture.header
	return Info{
		ID: h.ID, Label: h.Label, PlayerID: h.PlayerID, PlayerName: h.PlayerName, StartedAt: h.StartedAt,
		DurationMs: now.Sub(capture.started).Milliseconds(), MessageCount: capture.messages,
		SizeBytes: capture.bytes, Status: StatusRecording,
	}
}
