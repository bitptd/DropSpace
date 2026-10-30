package store

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// Recover reconciles the filesystem/SQLite boundary conservatively. A ready row
// without bytes is never served. An object renamed before a failed ready
// transaction is reclaimed so the same upload intent can be retried from byte 0.
func (s *Store) Recover(ctx context.Context, logger *slog.Logger) error {
	rows, err := s.DB.QueryContext(ctx, "SELECT id,space_id,state,object_key FROM files WHERE state IN ('uploading','ready')")
	if err != nil {
		return err
	}
	type row struct{ id, spaceID, state, key string }
	var records []row
	for rows.Next() {
		var value row
		if err := rows.Scan(&value.id, &value.spaceID, &value.state, &value.key); err != nil {
			rows.Close()
			return err
		}
		records = append(records, value)
	}
	if err := rows.Close(); err != nil {
		return err
	}
	known := make(map[string]struct{}, len(records))
	for _, value := range records {
		known[value.key] = struct{}{}
		path := s.ObjectPath(value.key)
		_, statErr := os.Stat(path)
		if value.state == "uploading" && statErr == nil {
			if removeErr := os.Remove(path); removeErr != nil {
				return removeErr
			}
			logger.Warn("reclaimed object published before failed database transaction", "file_id", value.id)
		}
		if value.state == "ready" && errors.Is(statErr, os.ErrNotExist) {
			if err := s.markDamaged(ctx, value.spaceID, value.id); err != nil {
				return err
			}
			logger.Error("ready object missing; marked damaged", "file_id", value.id)
		}
	}
	entries, err := os.ReadDir(filepath.Join(s.DataDir, "objects"))
	if err != nil {
		return err
	}
	for _, entry := range entries {
		if entry.IsDir() || !strings.HasSuffix(entry.Name(), ".bin") {
			continue
		}
		key := strings.TrimSuffix(entry.Name(), ".bin")
		if _, exists := known[key]; !exists {
			if err := os.Remove(filepath.Join(s.DataDir, "objects", entry.Name())); err != nil {
				return err
			}
			logger.Warn("removed orphan object", "object_key", key)
		}
	}
	return nil
}

func (s *Store) markDamaged(ctx context.Context, spaceID, fileID string) error {
	tx, err := s.DB.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	now := time.Now().UTC().Format(time.RFC3339Nano)
	result, err := tx.ExecContext(ctx, "UPDATE files SET state='damaged',deleted_at=? WHERE id=? AND state='ready'", now, fileID)
	if err != nil {
		return err
	}
	count, _ := result.RowsAffected()
	if count == 0 {
		return tx.Commit()
	}
	payload, _ := json.Marshal(map[string]string{"id": fileID, "state": "damaged"})
	event, err := tx.ExecContext(ctx, `INSERT INTO events(space_id,type,file_id,payload_json,created_at)
    VALUES(?,'file.deleted',?,?,?)`, spaceID, fileID, string(payload), now)
	if err != nil {
		return err
	}
	seq, _ := event.LastInsertId()
	if _, err = tx.ExecContext(ctx, "UPDATE event_streams SET last_seq=? WHERE space_id=?", seq, spaceID); err != nil {
		return err
	}
	return tx.Commit()
}

func (s *Store) RunGC(ctx context.Context, logger *slog.Logger) error {
	now := time.Now().UTC().Format(time.RFC3339Nano)
	rows, err := s.DB.QueryContext(ctx, "SELECT id,space_id,object_key FROM files WHERE state='ready' AND expires_at<=?", now)
	if err != nil {
		return err
	}
	type expired struct{ id, spaceID, key string }
	var due []expired
	for rows.Next() {
		var value expired
		if err := rows.Scan(&value.id, &value.spaceID, &value.key); err != nil {
			rows.Close()
			return err
		}
		due = append(due, value)
	}
	if err := rows.Close(); err != nil {
		return err
	}
	for _, value := range due {
		tx, beginErr := s.DB.BeginTx(ctx, nil)
		if beginErr != nil {
			return beginErr
		}
		result, updateErr := tx.ExecContext(ctx, "UPDATE files SET state='expired',deleted_at=? WHERE id=? AND state='ready'", now, value.id)
		if updateErr == nil {
			count, _ := result.RowsAffected()
			if count == 1 {
				payload, _ := json.Marshal(map[string]string{"id": value.id, "state": "expired"})
				var event sql.Result
				event, updateErr = tx.ExecContext(ctx, `INSERT INTO events(space_id,type,file_id,payload_json,created_at)
            VALUES(?,'file.expired',?,?,?)`, value.spaceID, value.id, string(payload), now)
				if updateErr == nil {
					seq, _ := event.LastInsertId()
					_, updateErr = tx.ExecContext(ctx, "UPDATE event_streams SET last_seq=? WHERE space_id=?", seq, value.spaceID)
				}
			}
		}
		if updateErr == nil {
			updateErr = tx.Commit()
		} else {
			_ = tx.Rollback()
		}
		if updateErr != nil {
			return updateErr
		}
		if removeErr := os.Remove(s.ObjectPath(value.key)); removeErr != nil && !errors.Is(removeErr, os.ErrNotExist) {
			logger.Error("expired object cleanup failed", "file_id", value.id, "error", removeErr)
		}
	}
	// Deleted/damaged objects were already made unavailable transactionally.
	deletedRows, err := s.DB.QueryContext(ctx, "SELECT id,object_key FROM files WHERE state IN ('deleted','damaged')")
	if err != nil {
		return err
	}
	for deletedRows.Next() {
		var id, key string
		if err := deletedRows.Scan(&id, &key); err != nil {
			deletedRows.Close()
			return err
		}
		if removeErr := os.Remove(s.ObjectPath(key)); removeErr != nil && !errors.Is(removeErr, os.ErrNotExist) {
			logger.Error("tombstone object cleanup failed", "file_id", id, "error", removeErr)
		}
	}
	return deletedRows.Close()
}
