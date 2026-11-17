package adminauth

import (
	"crypto/pbkdf2"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"errors"
	"fmt"
	"strconv"
	"strings"
)

const (
	scheme = "pbkdf2-sha256"
	// OWASP's 2023 recommendation for PBKDF2-HMAC-SHA256.
	hashIterations = 600_000
	minIterations  = 100_000
	maxIterations  = 10_000_000
	saltBytes      = 16
	keyBytes       = 32

	MinPasswordLength = 12
)

type Password struct {
	iterations int
	salt       []byte
	key        []byte
}

func HashPassword(password string) (string, error) {
	if len(password) < MinPasswordLength {
		return "", fmt.Errorf("the password must be at least %d characters", MinPasswordLength)
	}
	salt := make([]byte, saltBytes)
	rand.Read(salt)
	key, err := pbkdf2.Key(sha256.New, password, salt, hashIterations, keyBytes)
	if err != nil {
		return "", err
	}
	encoding := base64.RawStdEncoding
	return fmt.Sprintf("%s$%d$%s$%s", scheme, hashIterations, encoding.EncodeToString(salt),
		encoding.EncodeToString(key)), nil
}

var errFormat = errors.New("not a hash from --hash-password (" + scheme + "$iterations$salt$key)")

func ParsePassword(encoded string) (Password, error) {
	parts := strings.Split(encoded, "$")
	if len(parts) != 4 || parts[0] != scheme {
		return Password{}, errFormat
	}
	iterations, err := strconv.Atoi(parts[1])
	if err != nil {
		return Password{}, errFormat
	}
	if iterations < minIterations || iterations > maxIterations {
		return Password{}, fmt.Errorf("iterations must be within %d..%d", minIterations, maxIterations)
	}
	encoding := base64.RawStdEncoding
	salt, err := encoding.DecodeString(parts[2])
	if err != nil || len(salt) < saltBytes {
		return Password{}, errFormat
	}
	key, err := encoding.DecodeString(parts[3])
	if err != nil || len(key) != keyBytes {
		return Password{}, errFormat
	}
	return Password{iterations: iterations, salt: salt, key: key}, nil
}

func (p Password) Set() bool {
	return p.key != nil
}

func (p Password) Matches(password string) bool {
	key, err := pbkdf2.Key(sha256.New, password, p.salt, p.iterations, keyBytes)
	return err == nil && subtle.ConstantTimeCompare(key, p.key) == 1
}
