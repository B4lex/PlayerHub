package recording

import (
	"crypto/rand"
	"encoding/hex"
	"errors"
	"log/slog"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strings"
	"sync"
	"time"
)

const suffix = ".jsonl"

var idPattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$`)

var ErrNotFound = errors.New("recording not found")

func ValidID(id string) bool {
	return idPattern.MatchString(id)
}

type Store struct {
	directory string
	log       *slog.Logger
	mu        sync.Mutex
	cache     map[string]cachedInfo
}

type cachedInfo struct {
	modified time.Time
	size     int64
	info     Info
}

func NewStore(directory string, log *slog.Logger) *Store {
	return &Store{directory: directory, log: log, cache: make(map[string]cachedInfo)}
}

func (s *Store) NewID(now time.Time) string {
	stamp := now.UTC().Format("20060102-150405")
	for {
		var random [3]byte
		rand.Read(random[:])
		id := stamp + "-" + hex.EncodeToString(random[:])
		if _, err := os.Stat(s.path(id)); errors.Is(err, os.ErrNotExist) {
			return id
		}
	}
}

func (s *Store) path(id string) string {
	return filepath.Join(s.directory, id+suffix)
}

func (s *Store) Path(id string) (string, error) {
	if !ValidID(id) {
		return "", ErrNotFound
	}
	return s.path(id), nil
}

func (s *Store) Create(header Header) (*os.File, error) {
	path, err := s.Path(header.ID)
	if err != nil {
		return nil, err
	}
	if err := os.MkdirAll(s.directory, 0o755); err != nil {
		return nil, err
	}
	return os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
}

func (s *Store) Infos(skip map[string]bool) []Info {
	entries, err := os.ReadDir(s.directory)
	if err != nil {
		return nil
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	seen := make(map[string]bool)
	var infos []Info
	for _, entry := range entries {
		id, ok := strings.CutSuffix(entry.Name(), suffix)
		if !ok || entry.IsDir() || !ValidID(id) {
			continue
		}
		seen[id] = true
		if skip[id] {
			continue
		}
		if info, ok := s.infoLocked(id); ok {
			infos = append(infos, info)
		}
	}
	for id := range s.cache {
		if !seen[id] {
			delete(s.cache, id)
		}
	}
	SortNewestFirst(infos)
	return infos
}

func SortNewestFirst(infos []Info) {
	slices.SortFunc(infos, func(a, b Info) int {
		if c := strings.Compare(b.StartedAt, a.StartedAt); c != 0 {
			return c
		}
		return strings.Compare(b.ID, a.ID)
	})
}

func (s *Store) Info(id string) (Info, error) {
	if !ValidID(id) {
		return Info{}, ErrNotFound
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	info, ok := s.infoLocked(id)
	if !ok {
		return Info{}, ErrNotFound
	}
	return info, nil
}

func (s *Store) infoLocked(id string) (Info, bool) {
	path := s.path(id)
	stat, err := os.Stat(path)
	if err != nil {
		return Info{}, false
	}
	if cached, ok := s.cache[id]; ok && cached.modified.Equal(stat.ModTime()) && cached.size == stat.Size() {
		return cached.info, true
	}
	info, err := ScanInfo(path)
	if err != nil {
		s.log.Warn("skipping unreadable recording", "file", filepath.Base(path), "error", err)
		return Info{}, false
	}
	if info.ID != id {
		s.log.Warn("skipping recording whose header ID does not match", "file", filepath.Base(path), "id", info.ID)
		return Info{}, false
	}
	s.cache[id] = cachedInfo{stat.ModTime(), stat.Size(), info}
	return info, true
}

func (s *Store) Load(id string) (*Recording, error) {
	path, err := s.Path(id)
	if err != nil {
		return nil, err
	}
	if _, err := os.Stat(path); err != nil {
		return nil, ErrNotFound
	}
	return Read(path)
}

func (s *Store) Delete(id string) error {
	path, err := s.Path(id)
	if err != nil {
		return err
	}
	if err := os.Remove(path); err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return ErrNotFound
		}
		return err
	}
	s.mu.Lock()
	delete(s.cache, id)
	s.mu.Unlock()
	return nil
}
