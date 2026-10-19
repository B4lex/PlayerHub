package main

import (
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"playerhub/server/internal/recording"
)

func main() {
	dryRun := flag.Bool("dry-run", false, "check every file without writing")
	flag.Usage = func() {
		fmt.Fprintln(os.Stderr, "usage: convert-recordings [-dry-run] [file.jsonl | directory]...")
		fmt.Fprintln(os.Stderr, "Converts every version 2 or 3 recording named, or in the directories named (default: recordings).")
		flag.PrintDefaults()
	}
	flag.Parse()
	targets := flag.Args()
	if len(targets) == 0 {
		targets = []string{"recordings"}
	}
	var files []string
	for _, target := range targets {
		if info, err := os.Stat(target); err == nil && info.IsDir() {
			matches, _ := filepath.Glob(filepath.Join(target, "*.jsonl"))
			files = append(files, matches...)
		} else {
			files = append(files, target)
		}
	}
	converted, skipped, failed := 0, 0, 0
	for _, file := range files {
		switch err := convert(file, *dryRun); {
		case err == errNotConvertible:
			skipped++
		case err != nil:
			failed++
			fmt.Fprintf(os.Stderr, "%s: %v\n", file, err)
		default:
			converted++
			fmt.Printf("%s: converted\n", file)
		}
	}
	fmt.Printf("%d converted, %d already current or not recordings, %d failed\n", converted, skipped, failed)
	if failed > 0 {
		os.Exit(1)
	}
}

var errNotConvertible = fmt.Errorf("not a version 2 or 3 recording")

var converters = map[int]func([]byte) ([]byte, error){2: recording.ConvertV2, 3: recording.ConvertV3}

func convert(file string, dryRun bool) error {
	data, err := os.ReadFile(file)
	if err != nil {
		return err
	}
	first, _, _ := bytes.Cut(data, []byte("\n"))
	var header struct {
		Format  string `json:"format"`
		Version int    `json:"version"`
	}
	if json.Unmarshal(first, &header) != nil || header.Format != recording.FormatName {
		return errNotConvertible
	}
	converter, ok := converters[header.Version]
	if !ok {
		return errNotConvertible
	}
	converted, err := converter(data)
	if err != nil || dryRun {
		return err
	}
	backup := fmt.Sprintf("%s.v%d", strings.TrimSuffix(file, ".jsonl"), header.Version)
	if _, err := os.Stat(backup); err == nil {
		return fmt.Errorf("backup %s already exists", backup)
	}
	if err := os.WriteFile(backup, data, 0o644); err != nil {
		return err
	}
	temporary := file + ".tmp"
	if err := os.WriteFile(temporary, converted, 0o644); err != nil {
		return err
	}
	return os.Rename(temporary, file)
}
