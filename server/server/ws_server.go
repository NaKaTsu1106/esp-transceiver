package server

import (
	"fmt"
	"log"
	"net/http"

	"github.com/NaKaTsu1106/esp-transceiver/server/config"
	"github.com/NaKaTsu1106/esp-transceiver/server/monitor"
	"golang.org/x/crypto/acme/autocert"
)

func RunWSServer(cfg *config.Config) {
	mux := http.NewServeMux()
	mux.HandleFunc("/events", monitor.ServeSSE)
	mux.HandleFunc("/audio/events", monitor.ServeAudioSSE)
	mux.Handle("/", http.FileServer(http.Dir("web/static")))

	if cfg.Domain != "" {
		runHTTPS(cfg.Domain, mux)
	} else {
		addr := fmt.Sprintf(":%d", cfg.WSPort)
		log.Printf("Monitor server listening on %s (HTTP)", addr)
		if err := http.ListenAndServe(addr, mux); err != nil {
			log.Printf("Monitor server error: %v", err)
		}
	}
}

func runHTTPS(domain string, handler http.Handler) {
	m := &autocert.Manager{
		Cache:      autocert.DirCache("/var/cache/autocert"),
		Prompt:     autocert.AcceptTOS,
		HostPolicy: autocert.HostWhitelist(domain),
	}

	// port 80: ACMEチャレンジ応答 + HTTPSへリダイレクト
	go func() {
		log.Printf("HTTP server listening on :80 (ACME + redirect)")
		if err := http.ListenAndServe(":80", m.HTTPHandler(nil)); err != nil {
			log.Printf("HTTP server error: %v", err)
		}
	}()

	// port 443: HTTPS
	srv := &http.Server{
		Addr:      ":443",
		Handler:   handler,
		TLSConfig: m.TLSConfig(),
	}
	log.Printf("Monitor HTTPS server listening on :443 (domain=%s)", domain)
	if err := srv.ListenAndServeTLS("", ""); err != nil {
		log.Printf("HTTPS server error: %v", err)
	}
}
