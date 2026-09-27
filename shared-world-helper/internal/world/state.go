package world

import (
	"fmt"
	"time"

	"github.com/Reece01-Dock/satisfactory-shared-world/helper/internal/model"
)

// State is the local session state for one world on this machine. It is the
// ONLY session state machine in the system: the mod renders it and reports
// game events, but never keeps a parallel copy of the decision logic.
type State string

const (
	StateIdle           State = "IDLE"
	StateChecking       State = "CHECKING"
	StateWaitingForHost State = "WAITING_FOR_HOST" // someone holds the lease but has not published join info yet
	StateJoinReady      State = "JOIN_READY"       // decision JOIN; join info available
	StateAcquiring      State = "ACQUIRING"
	StateRecovering     State = "RECOVERING" // uploading unsynced progress from a crashed session
	StateDownloading    State = "DOWNLOADING"
	StateReadyToHost    State = "READY_TO_HOST" // save verified and placed; mod should load it
	StateHosting        State = "HOSTING"
	StateUploading      State = "UPLOADING"
	StateReleasing      State = "RELEASING"
	StateLeaseLost      State = "LEASE_LOST" // another host took over; stop and exit
	StateError          State = "ERROR"
)

// transitions lists every legal state change. Anything else is a bug and is
// refused rather than silently applied.
var transitions = map[State][]State{
	StateIdle:           {StateChecking},
	StateChecking:       {StateWaitingForHost, StateJoinReady, StateAcquiring, StateError},
	StateWaitingForHost: {StateJoinReady, StateChecking, StateError, StateIdle},
	StateJoinReady:      {StateIdle, StateChecking},
	StateAcquiring:      {StateChecking, StateRecovering, StateDownloading, StateReadyToHost, StateError, StateLeaseLost},
	StateRecovering:     {StateDownloading, StateReadyToHost, StateError, StateLeaseLost},
	StateDownloading:    {StateReadyToHost, StateError, StateLeaseLost},
	StateReadyToHost:    {StateHosting, StateReleasing, StateError, StateLeaseLost},
	StateHosting:        {StateUploading, StateLeaseLost, StateError},
	StateUploading:      {StateHosting, StateReleasing, StateLeaseLost, StateError},
	StateReleasing:      {StateIdle, StateError},
	StateLeaseLost:      {StateIdle},
	StateError:          {StateIdle, StateChecking}, // Retry
}

func canTransition(from, to State) bool {
	for _, s := range transitions[from] {
		if s == to {
			return true
		}
	}
	return false
}

// Decision is the answer to "Play Shared World".
type Decision string

const (
	DecisionNone Decision = ""
	DecisionHost Decision = "HOST"
	DecisionJoin Decision = "JOIN"
)

// ErrorInfo is a user-facing error. Message is written for players; Detail
// is for logs / "View details".
type ErrorInfo struct {
	Code               string `json:"code"`
	Message            string `json:"message"`
	Detail             string `json:"detail,omitempty"`
	LocalSaveUnchanged bool   `json:"localSaveUnchanged"`
	BackupPath         string `json:"backupPath,omitempty"`
	CloudRevision      uint64 `json:"cloudRevision,omitempty"`
	LocalRevision      uint64 `json:"localRevision,omitempty"`
	Retryable          bool   `json:"retryable"`
}

// Step is one notification line shown to the player.
type Step struct {
	At      time.Time `json:"at"`
	Message string    `json:"message"`
}

// SessionView is the local session as exposed over IPC.
type SessionView struct {
	WorldID    string          `json:"worldId"`
	State      State           `json:"state"`
	Decision   Decision        `json:"decision,omitempty"`
	Message    string          `json:"message"`
	Steps      []Step          `json:"steps"`
	Error      *ErrorInfo      `json:"error,omitempty"`
	HostName   string          `json:"hostName,omitempty"`
	Join       *model.JoinInfo `json:"join,omitempty"`
	SaveName   string          `json:"saveName,omitempty"`
	SavePath   string          `json:"savePath,omitempty"`
	Generation uint64          `json:"generation,omitempty"`
	Revision   uint64          `json:"revision,omitempty"`
	UpdatedAt  time.Time       `json:"updatedAt"`
}

// RemoteStatus is the shared-world status derived from the cloud record.
type RemoteStatus string

const (
	RemoteAvailable RemoteStatus = "AVAILABLE"
	RemoteStarting  RemoteStatus = "STARTING"
	RemoteOnline    RemoteStatus = "ONLINE"
	RemoteSaving    RemoteStatus = "SAVING"
	RemoteStopping  RemoteStatus = "STOPPING"
	// RemoteRecoverable: the last host stopped heartbeating; the next
	// player to press Play recovers the world from the latest revision.
	RemoteRecoverable RemoteStatus = "RECOVERABLE"
	RemoteNoSave      RemoteStatus = "NO_SAVE"
	RemoteUnreachable RemoteStatus = "UNREACHABLE"
)

// WorldStatus is what the main-menu panel shows.
type WorldStatus struct {
	WorldID      string         `json:"worldId"`
	WorldName    string         `json:"worldName"`
	Status       RemoteStatus   `json:"status"`
	StatusText   string         `json:"statusText"`
	HostName     string         `json:"hostName,omitempty"`
	PlayerCount  int            `json:"playerCount"`
	Players      []model.Player `json:"players,omitempty"`
	Revision     uint64         `json:"revision"`
	Generation   uint64         `json:"generation"`
	LastPlayedAt *time.Time     `json:"lastPlayedAt,omitempty"`
	LastHostName string         `json:"lastHostName,omitempty"`
	LeaseExpires *time.Time     `json:"leaseExpiresAt,omitempty"`
	Error        string         `json:"error,omitempty"`
	Local        SessionView    `json:"local"`
}

func errorf(code, msg string, detail error) *ErrorInfo {
	e := &ErrorInfo{Code: code, Message: msg, LocalSaveUnchanged: true}
	if detail != nil {
		e.Detail = detail.Error()
	}
	return e
}

func describeRemote(rec *model.WorldRecord, live bool, now time.Time) (RemoteStatus, string) {
	l := rec.Lease
	switch {
	case l != nil && live:
		switch l.Phase {
		case model.PhasePreparing:
			return RemoteStarting, fmt.Sprintf("%s is starting the world", l.Holder.DisplayName)
		case model.PhaseSaving:
			return RemoteSaving, fmt.Sprintf("Online (host %s is saving)", l.Holder.DisplayName)
		case model.PhaseStopping:
			return RemoteStopping, fmt.Sprintf("%s is closing the world", l.Holder.DisplayName)
		default:
			return RemoteOnline, "Online"
		}
	case l != nil:
		return RemoteRecoverable, fmt.Sprintf("Available (%s stopped responding %s ago; press Play to recover)", l.Holder.DisplayName, now.Sub(l.ExpiresAt).Round(time.Second))
	case rec.Head == nil:
		return RemoteNoSave, "No shared save yet"
	default:
		return RemoteAvailable, "Available"
	}
}
