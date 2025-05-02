package main

import (
	"embed"
	"encoding/binary"
	"flag"
	"fmt"
	"html/template"
	"io"
	"log"
	"net"
	"net/http"
	"sync"
	"time"

	"github.com/pkg/errors"
)

const maxStale time.Duration = 10 * time.Minute

var listenAddr *string = flag.String("listen", ":80", "address to listen on")
var connectAddr *string = flag.String("connect", "127.0.0.1:21", "address to connect to")

//go:embed index.html.tmpl
var templatesFS embed.FS

type Svr struct {
	tmpls *template.Template

	mu      sync.Mutex
	data    NetMessage
	dataErr error
	dataAt  time.Time
}

type NetMessage struct {
	Paddry_time_left int32
	Temp_inside      int16
	Temp_outside     int16
	Temp_setpoint    int16
	Humid_inside     int16
	Humid_outside    int16
	Humid_setpoint   int16
	// 0: Pump on
	// 1: Single shutter
	// 2: Double shutter
	// 3: Fan
	Mechanism_state int8
	// RECIRC, VENT, etc.
	Operating_state int8
}

func (nm *NetMessage) Decode(buf []byte) error {
	if len(buf) < 18 {
		return errors.Wrapf(io.ErrUnexpectedEOF, "got %d bytes", len(buf))
	}

	nm.Paddry_time_left = int32(binary.LittleEndian.Uint32(buf[0:]))
	nm.Temp_inside = int16(binary.LittleEndian.Uint16(buf[4:]))
	nm.Temp_outside = int16(binary.LittleEndian.Uint16(buf[6:]))
	nm.Temp_setpoint = int16(binary.LittleEndian.Uint16(buf[8:]))
	nm.Humid_inside = int16(binary.LittleEndian.Uint16(buf[10:]))
	nm.Humid_outside = int16(binary.LittleEndian.Uint16(buf[12:]))
	nm.Humid_setpoint = int16(binary.LittleEndian.Uint16(buf[14:]))
	nm.Mechanism_state = int8(buf[16])
	nm.Operating_state = int8(buf[17])
	return nil
}

func formatTemperature(temp int16) string {
	return fmt.Sprintf("%d °C", temp)
}

func formatHumidity(humid int16) string {
	return fmt.Sprintf("%d%%", humid)
}

func formatDuration(t int32) string {
	d := time.Millisecond * time.Duration(t)
	return d.String()
}

const OpStatePadDry int8 = 4

func formatOperatingState(opstate int8) string {
	switch opstate {
	case 1:
		return "RECIRC"
	case 2:
		return "VENT"
	case 3:
		return "COOL"
	case OpStatePadDry:
		return "PADDRY"
	case 5:
		return "DEHUM"
	}
	return "(Error)"
}

type TemplateData struct {
	NM    NetMessage
	Error string

	IsPadDry bool

	Pump         bool
	ShutterVent  bool
	ShutterSwamp bool
	Fan          bool
}

func (td *TemplateData) parseMechanismState() {
	td.Pump = td.NM.Mechanism_state&1 == 1
	td.ShutterVent = td.NM.Mechanism_state&2 == 2
	td.ShutterSwamp = td.NM.Mechanism_state&4 == 4
	td.Fan = td.NM.Mechanism_state&8 == 8

	td.IsPadDry = td.NM.Operating_state == OpStatePadDry
}

func (s *Svr) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	var td TemplateData
	{
		s.mu.Lock()
		if time.Since(s.dataAt) >= maxStale {
			s.dataErr = s.runCommandLocked(0)
			s.dataAt = time.Now()
		}
		td.NM = s.data // copy struct
		td.Error = s.dataErr.Error()
		s.mu.Unlock()
	}

	td.parseMechanismState()

	w.Header().Set("Content-Type", "text/html")
	s.tmpls.ExecuteTemplate(w, "index.html.tmpl", td)
}

const (
	CommandNone int8 = 0
)

func (s *Svr) runCommandLocked(command byte) error {
	conn, err := net.Dial("tcp", *connectAddr)
	if err != nil {
		return errors.Wrap(err, "could not connect to Arduino")
	}
	defer conn.Close()

	_, err = conn.Write([]byte{command})
	if err != nil {
		return errors.Wrap(err, "while writing to Arduino")
	}
	bytes, err := io.ReadAll(conn)
	if err != nil {
		return errors.Wrap(err, "while reading from Arduino")
	}

	err = s.data.Decode(bytes)
	if err != nil {
		return err
	}
	return nil
}

func main() {
	tmpls := template.New("")
	var funcmap template.FuncMap = make(template.FuncMap)
	funcmap["formatTemperature"] = formatTemperature
	funcmap["formatHumidity"] = formatHumidity
	funcmap["formatDuration"] = formatDuration
	funcmap["formatOperatingState"] = formatOperatingState
	tmpls.Funcs(funcmap)
	template.Must(tmpls.ParseFS(templatesFS, "index.html.tmpl"))

	flag.Parse()
	s := &http.Server{
		Addr:        *listenAddr,
		ReadTimeout: 10 * time.Second,
		Handler: &Svr{
			tmpls: tmpls,
		},
	}

	log.Fatal(s.ListenAndServe())
}
