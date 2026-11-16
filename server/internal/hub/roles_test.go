package hub

import (
	"fmt"
	"testing"

	"playerhub/server/internal/delivery"
	"playerhub/server/internal/wire/pb"
)

func describeRoles(m *pb.ServerMessage) string {
	if roster := m.GetRoster(); roster != nil {
		s := "roster"
		for _, e := range roster.GetJoined() {
			s += fmt.Sprintf(" +%d %s role %d", e.GetSlot(), e.GetName(), e.GetRole())
		}
		return s
	}
	if m.GetRoles() != nil {
		s := "roles"
		for _, role := range m.GetRoles().GetRoles() {
			s += fmt.Sprintf(" %d %s %06x", role.GetId(), role.GetName(), role.GetColor())
		}
		return s
	}
	return describe(m)
}

func (f *fixture) receivedRoles(slot uint32) []string {
	f.t.Helper()
	var lines []string
	for _, m := range f.messages(slot) {
		lines = append(lines, describeRoles(m))
	}
	return lines
}

func (f *fixture) connectWithRole(session uint64, displayName string, role uint32) uint32 {
	box := delivery.NewOutbox(1024)
	slot, _ := f.hub.ConnectClient(box, Identity{Session: session, DisplayName: displayName, Role: role})
	f.boxes[slot] = box
	return slot
}

func TestRolesReachEveryClientBeforeTheRoster(t *testing.T) {
	f := newFixture(t)
	a := f.connectWithRole(1, "Lydia", 0)
	expect(t, f.receivedRoles(a), "welcome 1", "roster +1 Lydia role 0")

	moderator := &pb.Role{Id: 1, Name: "Moderator", Color: 0x3366FF}
	f.hub.SetRoles([]*pb.Role{moderator})
	expect(t, f.receivedRoles(a), "roles 1 Moderator 3366ff")

	b := f.connectWithRole(2, "Serana", 1)
	expect(t, f.receivedRoles(b), "welcome 2", "roles 1 Moderator 3366ff", "roster +1 Lydia role 0 +2 Serana role 1")
	expect(t, f.receivedRoles(a), "roster +2 Serana role 1")

	f.send(b, chat(1, "hi"))
	if line := f.messages(a)[0].GetChatMessage(); line.GetSender().GetRole() != 1 {
		t.Fatalf("chat line: %v", line)
	}
}

func TestSettingARoleTellsEveryClient(t *testing.T) {
	f := newFixture(t)
	f.hub.SetRoles([]*pb.Role{{Id: 1, Name: "Moderator"}, {Id: 2, Name: "Author"}})
	a, b := f.connectWithRole(1, "Lydia", 0), f.connectWithRole(2, "Serana", 0)
	f.messages(a)
	f.messages(b)

	f.hub.SetRole(1, 2)
	expect(t, f.receivedRoles(a), "roster +1 Lydia role 2")
	expect(t, f.receivedRoles(b), "roster +1 Lydia role 2")
	f.hub.SetRole(1, 2)
	f.hub.SetRole(99, 1)
	expect(t, f.receivedRoles(b))
	f.hub.SetRole(1, 0)
	expect(t, f.receivedRoles(b), "roster +1 Lydia role 0")
}

func TestADeletedRoleLeavesItsHolders(t *testing.T) {
	f := newFixture(t)
	f.hub.SetRoles([]*pb.Role{{Id: 1, Name: "Moderator"}, {Id: 2, Name: "Author"}})
	a, b := f.connectWithRole(1, "Lydia", 1), f.connectWithRole(2, "Serana", 2)
	f.messages(a)
	f.messages(b)

	f.hub.SetRoles([]*pb.Role{{Id: 2, Name: "Author"}})
	expect(t, f.receivedRoles(a), "roles 2 Author 000000")
	c := f.connectWithRole(3, "Jordis", 0)
	expect(t, f.receivedRoles(c), "welcome 3", "roles 2 Author 000000",
		"roster +1 Lydia role 0 +2 Serana role 2 +3 Jordis role 0")

	f.hub.SetRoles(nil)
	expect(t, f.receivedRoles(a), "roster +3 Jordis role 0", "roles")
	d := f.connectWithRole(4, "Mjoll", 0)
	expect(t, f.receivedRoles(d), "welcome 4", "roster +1 Lydia role 0 +2 Serana role 0 +3 Jordis role 0 +4 Mjoll role 0")
}
