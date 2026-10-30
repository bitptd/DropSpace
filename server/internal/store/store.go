package store

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"time"

	_ "modernc.org/sqlite"
)

const schema = `
PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;
CREATE TABLE IF NOT EXISTS schema_migrations(version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS spaces(
  id TEXT PRIMARY KEY, name TEXT NOT NULL, quota_bytes INTEGER NOT NULL,
  max_file_bytes INTEGER NOT NULL, max_items INTEGER NOT NULL, retention_seconds INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS devices(
  id TEXT PRIMARY KEY, space_id TEXT NOT NULL REFERENCES spaces(id), display_name TEXT NOT NULL,
  platform TEXT NOT NULL CHECK(platform IN ('windows','macos')),
  token_hash BLOB NOT NULL UNIQUE, created_at TEXT NOT NULL, last_seen_at TEXT NOT NULL, revoked_at TEXT
);
CREATE TABLE IF NOT EXISTS pairing_tokens(
  token_hash BLOB PRIMARY KEY, space_id TEXT NOT NULL REFERENCES spaces(id), expires_at TEXT NOT NULL,
  attempts_left INTEGER NOT NULL, consumed_at TEXT
);
CREATE TABLE IF NOT EXISTS files(
  id TEXT PRIMARY KEY, space_id TEXT NOT NULL REFERENCES spaces(id), upload_device_id TEXT NOT NULL REFERENCES devices(id),
  kind TEXT NOT NULL CHECK(kind IN ('file','folder')), display_name TEXT NOT NULL,
  object_format TEXT NOT NULL CHECK(object_format IN ('raw','zip')), object_size_bytes INTEGER NOT NULL,
  object_sha256 TEXT NOT NULL, expanded_size_bytes INTEGER NOT NULL, entry_count INTEGER NOT NULL,
  tree_sha256 TEXT, state TEXT NOT NULL CHECK(state IN ('uploading','ready','deleted','expired','damaged')),
  created_at TEXT NOT NULL, ready_at TEXT, expires_at TEXT, deleted_at TEXT,
  object_key TEXT NOT NULL UNIQUE, client_upload_id TEXT NOT NULL,
  UNIQUE(upload_device_id, client_upload_id)
);
CREATE TABLE IF NOT EXISTS uploads(
  file_id TEXT PRIMARY KEY REFERENCES files(id) ON DELETE CASCADE, reserved_bytes INTEGER NOT NULL,
  bytes_received INTEGER NOT NULL DEFAULT 0, started_at TEXT NOT NULL, last_activity_at TEXT NOT NULL,
  temp_key TEXT, attempt_id TEXT
);
CREATE TABLE IF NOT EXISTS folder_manifests(
  file_id TEXT PRIMARY KEY REFERENCES files(id) ON DELETE CASCADE,
  manifest_json TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS events(
  seq INTEGER PRIMARY KEY AUTOINCREMENT, space_id TEXT NOT NULL REFERENCES spaces(id),
  type TEXT NOT NULL, file_id TEXT NOT NULL, payload_json TEXT NOT NULL, created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS event_streams(
  space_id TEXT PRIMARY KEY REFERENCES spaces(id), last_seq INTEGER NOT NULL DEFAULT 0,
  minimum_valid_after INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_files_space_state ON files(space_id,state);
CREATE INDEX IF NOT EXISTS idx_files_expiry ON files(state,expires_at);
CREATE INDEX IF NOT EXISTS idx_events_space_seq ON events(space_id,seq);
`

type Store struct {
	DB      *sql.DB
	DataDir string
}

type Identity struct {
	DeviceID string
	SpaceID  string
	Platform string
}

type Limits struct {
	QuotaBytes       int64
	MaxFileBytes     int64
	MaxItems         int64
	RetentionSeconds int64
}

func Open(dataDir string) (*Store, error) {
	if dataDir == "" {
		return nil, errors.New("data directory is required")
	}
	for _, dir := range []string{dataDir, filepath.Join(dataDir, "objects"), filepath.Join(dataDir, "uploads")} {
		if err := os.MkdirAll(dir, 0700); err != nil {
			return nil, fmt.Errorf("create data directory: %w", err)
		}
	}
	db, err := sql.Open("sqlite", filepath.Join(dataDir, "dropspace.sqlite")+"?_pragma=busy_timeout(5000)")
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(1)
	if _, err = db.Exec(schema); err != nil {
		db.Close()
		return nil, fmt.Errorf("migrate database: %w", err)
	}
	return &Store{DB: db, DataDir: dataDir}, nil
}

func (s *Store) Close() error { return s.DB.Close() }

func RandomHex(bytes int) (string, error) {
	value := make([]byte, bytes)
	if _, err := rand.Read(value); err != nil {
		return "", err
	}
	return hex.EncodeToString(value), nil
}

func TokenHash(token string) [32]byte { return sha256.Sum256([]byte(token)) }

func (s *Store) EnsureSpace(ctx context.Context, name string) (string, error) {
	var id string
	err := s.DB.QueryRowContext(ctx, "SELECT id FROM spaces ORDER BY id LIMIT 1").Scan(&id)
	if err == nil {
		return id, nil
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return "", err
	}
	id, err = RandomHex(16)
	if err != nil {
		return "", err
	}
	_, err = s.DB.ExecContext(ctx, `INSERT INTO spaces
      (id,name,quota_bytes,max_file_bytes,max_items,retention_seconds) VALUES(?,?,?,?,?,?)`,
		id, name, int64(10*1024*1024*1024), int64(1024*1024*1024), 10_000, int64(24*60*60))
	if err == nil {
		_, err = s.DB.ExecContext(ctx, "INSERT INTO event_streams(space_id) VALUES(?)", id)
	}
	return id, err
}

func (s *Store) CreatePairingToken(ctx context.Context, spaceID string, lifetime time.Duration) (string, error) {
	token, err := RandomHex(32)
	if err != nil {
		return "", err
	}
	hash := TokenHash(token)
	_, err = s.DB.ExecContext(ctx,
		"INSERT INTO pairing_tokens(token_hash,space_id,expires_at,attempts_left) VALUES(?,?,?,?)",
		hash[:], spaceID, time.Now().UTC().Add(lifetime).Format(time.RFC3339Nano), 5)
	if err != nil {
		return "", err
	}
	return token, nil
}

func (s *Store) Authenticate(ctx context.Context, token string) (Identity, error) {
	if len(token) < 32 {
		return Identity{}, sql.ErrNoRows
	}
	hash := TokenHash(token)
	var identity Identity
	err := s.DB.QueryRowContext(ctx, `SELECT id,space_id,platform FROM devices
      WHERE token_hash=? AND revoked_at IS NULL`, hash[:]).Scan(
		&identity.DeviceID, &identity.SpaceID, &identity.Platform)
	if err == nil {
		_, _ = s.DB.ExecContext(ctx, "UPDATE devices SET last_seen_at=? WHERE id=?",
			time.Now().UTC().Format(time.RFC3339Nano), identity.DeviceID)
	}
	return identity, err
}

func (s *Store) Limits(ctx context.Context, spaceID string) (Limits, error) {
	var limits Limits
	err := s.DB.QueryRowContext(ctx, `SELECT quota_bytes,max_file_bytes,max_items,retention_seconds
      FROM spaces WHERE id=?`, spaceID).Scan(&limits.QuotaBytes, &limits.MaxFileBytes,
		&limits.MaxItems, &limits.RetentionSeconds)
	return limits, err
}

func (s *Store) ObjectPath(objectKey string) string {
	return filepath.Join(s.DataDir, "objects", objectKey+".bin")
}

func (s *Store) UploadPath(tempKey string) string {
	return filepath.Join(s.DataDir, "uploads", tempKey+".part")
}
