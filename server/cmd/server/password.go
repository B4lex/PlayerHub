package main

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"strings"

	"golang.org/x/term"

	"playerhub/server/internal/adminauth"
)

// printPasswordHash reads the password twice without echo from a terminal, or once from a
// line of standard input otherwise, so it can be piped in.
func printPasswordHash() error {
	var password string
	if fd := int(os.Stdin.Fd()); term.IsTerminal(fd) {
		first, err := prompt(fd, "Admin password: ")
		if err != nil {
			return err
		}
		again, err := prompt(fd, "Again: ")
		if err != nil {
			return err
		}
		if first != again {
			return errors.New("the passwords differ")
		}
		password = first
	} else {
		line, err := bufio.NewReader(os.Stdin).ReadString('\n')
		if err != nil && line == "" {
			return fmt.Errorf("cannot read the password from standard input: %w", err)
		}
		password = strings.TrimRight(line, "\r\n")
	}
	hash, err := adminauth.HashPassword(password)
	if err != nil {
		return err
	}
	fmt.Printf("admin_password_hash = %q\n", hash)
	return nil
}

func prompt(fd int, label string) (string, error) {
	fmt.Fprint(os.Stderr, label)
	data, err := term.ReadPassword(fd)
	fmt.Fprintln(os.Stderr)
	return string(data), err
}
