package session

import (
	"time"

	"playerhub/server/internal/auth"
	"playerhub/server/internal/hub"
	"playerhub/server/internal/wire/pb"
)

// PublishRoles gives the hub the store's roles, which a starting server has not sent yet.
func (s *Server) PublishRoles() error {
	s.roles.Lock()
	defer s.roles.Unlock()
	return s.publishRoles()
}

func (s *Server) publishRoles() error {
	records, err := s.store.Roles(time.Now())
	if err != nil {
		return err
	}
	roles := make([]*pb.Role, len(records))
	for i, record := range records {
		roles[i] = &pb.Role{Id: record.ID, Name: record.Name, Color: record.Color}
	}
	s.loop.Call(func(h *hub.Hub) { h.SetRoles(roles) })
	return nil
}

func (s *Server) CreateRole(name string, color uint32) (auth.Role, error) {
	s.roles.Lock()
	defer s.roles.Unlock()
	role, err := s.store.CreateRole(name, color)
	if err != nil {
		return auth.Role{}, err
	}
	s.log.Info("role created", "role", role.ID, "name", role.Name)
	return role, s.publishRoles()
}

func (s *Server) UpdateRole(role auth.Role) error {
	s.roles.Lock()
	defer s.roles.Unlock()
	if err := s.store.UpdateRole(role); err != nil {
		return err
	}
	s.log.Info("role changed", "role", role.ID, "name", role.Name)
	return s.publishRoles()
}

func (s *Server) DeleteRole(id uint32) (bool, error) {
	s.roles.Lock()
	defer s.roles.Unlock()
	deleted, err := s.store.DeleteRole(id)
	if err != nil || !deleted {
		return deleted, err
	}
	s.log.Info("role deleted", "role", id)
	return true, s.publishRoles()
}

// SetSessionRole applies to a connected player at once; role 0 takes the session's role.
func (s *Server) SetSessionRole(id uint64, role uint32, now time.Time) error {
	s.roles.Lock()
	defer s.roles.Unlock()
	if err := s.store.SetRole(id, role, now); err != nil {
		return err
	}
	s.log.Info("session role set", "session", id, "role", role)
	s.loop.Call(func(h *hub.Hub) { h.SetRole(id, role) })
	return nil
}
