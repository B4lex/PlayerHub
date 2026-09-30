// Package wire encodes the hot server messages by hand so each checkpoint is
// encoded once and batches are built by concatenation.
package wire

import (
	"google.golang.org/protobuf/encoding/protowire"
	"google.golang.org/protobuf/proto"

	"skyrim-mp/server/internal/wire/pb"
)

const (
	peerStatesField      protowire.Number = 3
	peerStatesEntryField protowire.Number = 1
)

// AppendEntry appends a checkpoint as one PeerStates.states element, player
// set to the sender's slot. Zero fields are omitted as proto3 does.
func AppendEntry(b []byte, slot uint32, c *pb.Checkpoint) []byte {
	var body [64]byte
	m := body[:0]
	m = appendVarint(m, 1, uint64(slot))
	m = appendVarint(m, 2, c.GetSequence())
	m = appendZigZag(m, 3, c.GetX())
	m = appendZigZag(m, 4, c.GetY())
	m = appendZigZag(m, 5, c.GetZ())
	m = appendVarint(m, 6, uint64(c.GetHeading()))
	m = appendZigZag(m, 7, c.GetPitch())
	m = appendVarint(m, 8, uint64(c.GetSpeed()))
	m = appendVarint(m, 9, uint64(c.GetDirection()))
	m = appendVarint(m, 10, uint64(c.GetFlags()))
	b = protowire.AppendTag(b, peerStatesEntryField, protowire.BytesType)
	return protowire.AppendBytes(b, m)
}

// AppendPeerStatesHeader appends the ServerMessage framing of a PeerStates
// whose entries, entriesLength bytes in total, follow it.
func AppendPeerStatesHeader(b []byte, entriesLength int) []byte {
	b = protowire.AppendTag(b, peerStatesField, protowire.BytesType)
	return protowire.AppendVarint(b, uint64(entriesLength))
}

// Marshal encodes an ordered server message. They are rare enough that the
// generated code is fine.
func Marshal(m *pb.ServerMessage) []byte {
	b, err := proto.MarshalOptions{Deterministic: true}.Marshal(m)
	if err != nil {
		// Only invalid UTF-8 fails, and every string was validated on ingest.
		panic(err)
	}
	return b
}

func appendVarint(b []byte, field protowire.Number, v uint64) []byte {
	if v == 0 {
		return b
	}
	b = protowire.AppendTag(b, field, protowire.VarintType)
	return protowire.AppendVarint(b, v)
}

func appendZigZag(b []byte, field protowire.Number, v int32) []byte {
	return appendVarint(b, field, protowire.EncodeZigZag(int64(v)))
}
