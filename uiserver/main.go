package main

import (
	_ "embed"
	"flag"
	"html/template"
	"log"
	"net/http"
	"sync"
	"time"
)

const maxStale time.Duration = 10 * time.Minute

var listenAddr *string = flag.String("listen", ":80", "address to listen on")

//go:embed index.html.tmpl
var indexTemplateStr string
var indexTemplate *template.Template = template.Must(template.New("index").Parse(indexTemplateStr))

type Svr struct {
	mu     sync.Mutex
	data   NetMessage
	dataAt time.Time
}

type NetMessage struct {
	Temp_inside int16
	Temp_outside int16
	Temp_setpoint int16
	Humid_inside int16
	Humid_outside int16
	Humid_setpoint int16
	// 0: Pump on
	// 1: Single shutter
	// 2: Double shutter
	// 3: Fan
	Mechanism_state int8 
	// RECIRC, VENT, etc.
	Operating_state int8  
	Paddry_time_left int32 
}

func (s *Svr) ServeHTTP(w http.ResponseWriter, r *http.Request) {
}

func main() {
	flag.Parse()

	s := &http.Server{
		Addr:        *listenAddr,
		ReadTimeout: 10 * time.Second,
		Handler:     &Svr{},
	}

	log.Fatal(s.ListenAndServe())

}
