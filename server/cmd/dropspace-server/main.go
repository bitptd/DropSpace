package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"

	"dropspace/server/internal/api"
	"dropspace/server/internal/store"
)

type config struct {
	Listen      string `json:"listen"`
	DataDir     string `json:"data_dir"`
	Development bool   `json:"development"`
}

func main() {
	if len(os.Args) < 2 {
		fatal(errors.New("usage: dropspace-server serve|pair [options]"))
	}
	switch os.Args[1] {
	case "serve":
		serve(os.Args[2:])
	case "pair":
		pair(os.Args[2:])
	default:
		fatal(fmt.Errorf("unknown command %q", os.Args[1]))
	}
}

func loadConfig(filename string) (config, error) {
	value := config{Listen: "127.0.0.1:8787", DataDir: "./data"}
	if filename == "" {
		return value, nil
	}
	bytes, err := os.ReadFile(filename)
	if err != nil {
		return config{}, err
	}
	decoderError := json.Unmarshal(bytes, &value)
	if decoderError != nil {
		return config{}, decoderError
	}
	if value.Listen == "" || value.DataDir == "" {
		return config{}, errors.New("listen and data_dir are required")
	}
	if !filepath.IsAbs(value.DataDir) {
		absolute, err := filepath.Abs(filepath.Join(filepath.Dir(filename), value.DataDir))
		if err != nil {
			return config{}, err
		}
		value.DataDir = absolute
	}
	return value, nil
}

func serve(arguments []string) {
	flags := flag.NewFlagSet("serve", flag.ExitOnError)
	configPath := flags.String("config", "", "path to JSON configuration")
	_ = flags.Parse(arguments)
	configuration, err := loadConfig(*configPath)
	if err != nil {
		fatal(err)
	}
	logger := slog.New(slog.NewJSONHandler(os.Stdout, nil))
	st, err := store.Open(configuration.DataDir)
	if err != nil {
		fatal(err)
	}
	defer st.Close()
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()
	if err := st.Recover(ctx, logger); err != nil {
		fatal(err)
	}
	spaceID, err := st.EnsureSpace(ctx, "DropSpace")
	if err != nil {
		fatal(err)
	}
	logger.Info("space ready", "space_id", spaceID)

	server := &http.Server{
		Addr: configuration.Listen, Handler: api.New(st, logger, configuration.Development),
		ReadHeaderTimeout: 10 * time.Second, ReadTimeout: 30 * time.Minute,
		WriteTimeout: 30 * time.Minute, IdleTimeout: 2 * time.Minute,
	}
	go func() {
		logger.Info("server listening", "address", configuration.Listen)
		if listenErr := server.ListenAndServe(); listenErr != nil && !errors.Is(listenErr, http.ErrServerClosed) {
			logger.Error("server stopped", "error", listenErr)
			cancel()
		}
	}()
	go func() {
		ticker := time.NewTicker(time.Minute)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				if gcErr := st.RunGC(ctx, logger); gcErr != nil {
					logger.Error("gc failed", "error", gcErr)
				}
			}
		}
	}()
	<-ctx.Done()
	shutdown, stop := context.WithTimeout(context.Background(), 15*time.Second)
	defer stop()
	_ = server.Shutdown(shutdown)
}

func pair(arguments []string) {
	flags := flag.NewFlagSet("pair", flag.ExitOnError)
	configPath := flags.String("config", "", "path to JSON configuration")
	minutes := flags.Int("minutes", 10, "pairing token lifetime")
	_ = flags.Parse(arguments)
	configuration, err := loadConfig(*configPath)
	if err != nil {
		fatal(err)
	}
	st, err := store.Open(configuration.DataDir)
	if err != nil {
		fatal(err)
	}
	defer st.Close()
	ctx := context.Background()
	spaceID, err := st.EnsureSpace(ctx, "DropSpace")
	if err != nil {
		fatal(err)
	}
	token, err := st.CreatePairingToken(ctx, spaceID, time.Duration(*minutes)*time.Minute)
	if err != nil {
		fatal(err)
	}
	fmt.Printf("Pairing token (shown once, expires in %d minutes):\n%s\n", *minutes, token)
}

func fatal(err error) {
	fmt.Fprintln(os.Stderr, "dropspace-server:", err)
	os.Exit(1)
}
