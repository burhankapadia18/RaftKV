package fsm

import (
	"bytes"
	"context"
	"errors"
	"io"
	"log"
	"os"
	"sync"
	"testing"

	"github.com/hashicorp/raft"
	"google.golang.org/grpc"

	pb "my-raft-sidecar/pb"
)

// TestMain silences the package's log output: CppFSM.Apply logs on every failure
// path and the error-path tests would otherwise spam the test log.
func TestMain(m *testing.M) {
	log.SetOutput(io.Discard)
	code := m.Run()
	log.SetOutput(os.Stderr)
	os.Exit(code)
}

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

// fakeStateMachineClient records every command it receives and returns a canned
// (response, error) pair. It stands in for the gRPC client to the C++ engine.
type fakeStateMachineClient struct {
	mu    sync.Mutex
	calls []*pb.Command
	ctxs  []context.Context

	resp *pb.ApplyResponse
	err  error
}

var _ StateMachineClient = (*fakeStateMachineClient)(nil)

func (f *fakeStateMachineClient) Apply(ctx context.Context, cmd *pb.Command) (*pb.ApplyResponse, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls = append(f.calls, cmd)
	f.ctxs = append(f.ctxs, ctx)
	return f.resp, f.err
}

func (f *fakeStateMachineClient) callCount() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return len(f.calls)
}

func (f *fakeStateMachineClient) lastCommand() *pb.Command {
	f.mu.Lock()
	defer f.mu.Unlock()
	if len(f.calls) == 0 {
		return nil
	}
	return f.calls[len(f.calls)-1]
}

func (f *fakeStateMachineClient) lastContext() context.Context {
	f.mu.Lock()
	defer f.mu.Unlock()
	if len(f.ctxs) == 0 {
		return nil
	}
	return f.ctxs[len(f.ctxs)-1]
}

// fakeGRPCStateMachineClient implements the generated pb.StateMachineClient so
// NewStateMachineClient's wrapping behaviour can be exercised without gRPC.
type fakeGRPCStateMachineClient struct {
	mu      sync.Mutex
	gotCmd  *pb.Command
	gotOpts int

	resp *pb.ApplyResponse
	err  error
}

var _ pb.StateMachineClient = (*fakeGRPCStateMachineClient)(nil)

func (f *fakeGRPCStateMachineClient) Apply(ctx context.Context, in *pb.Command, opts ...grpc.CallOption) (*pb.ApplyResponse, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.gotCmd = in
	f.gotOpts = len(opts)
	return f.resp, f.err
}

func (f *fakeGRPCStateMachineClient) command() *pb.Command {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.gotCmd
}

func (f *fakeGRPCStateMachineClient) optCount() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.gotOpts
}

// fakeSnapshotSink implements raft.SnapshotSink and records what happened to it.
type fakeSnapshotSink struct {
	mu       sync.Mutex
	written  []byte
	writes   int
	closes   int
	cancels  int
	closeErr error
}

var _ raft.SnapshotSink = (*fakeSnapshotSink)(nil)

func (s *fakeSnapshotSink) Write(p []byte) (int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.writes++
	s.written = append(s.written, p...)
	return len(p), nil
}

func (s *fakeSnapshotSink) Close() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.closes++
	return s.closeErr
}

func (s *fakeSnapshotSink) ID() string { return "fake-snapshot-id" }

func (s *fakeSnapshotSink) Cancel() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.cancels++
	return nil
}

func (s *fakeSnapshotSink) stats() (writes, closes, cancels, bytesWritten int) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.writes, s.closes, s.cancels, len(s.written)
}

// recordingReadCloser counts Read and Close calls so Restore's handling of the
// snapshot stream can be observed.
type recordingReadCloser struct {
	reader   *bytes.Reader
	reads    int
	closes   int
	closeErr error
}

var _ io.ReadCloser = (*recordingReadCloser)(nil)

func (r *recordingReadCloser) Read(p []byte) (int, error) {
	r.reads++
	return r.reader.Read(p)
}

func (r *recordingReadCloser) Close() error {
	r.closes++
	return r.closeErr
}

// ---------------------------------------------------------------------------
// CppFSM.Apply
// ---------------------------------------------------------------------------

func TestCppFSMApply(t *testing.T) {
	transportErr := errors.New("rpc error: code = Unavailable desc = connection refused")

	tests := []struct {
		name    string
		resp    *pb.ApplyResponse
		err     error
		wantErr error // nil => Apply must return an untyped nil interface{}
	}{
		{
			name:    "success is reported as nil",
			resp:    &pb.ApplyResponse{Success: true},
			err:     nil,
			wantErr: nil,
		},
		{
			name:    "transport error is returned verbatim",
			resp:    nil,
			err:     transportErr,
			wantErr: transportErr,
		},
		{
			// !!! PINNED BUG — DO NOT "FIX" IN PHASE 0 !!!
			// The C++ state machine explicitly reported that it could NOT apply the
			// entry (ApplyResponse.Success == false), yet CppFSM.Apply discards the
			// response entirely (`_, err := f.client.Apply(...)`) and reports success
			// to Raft. That is a silent state divergence: Raft believes the entry is
			// applied on this node when it is not.
			// Phase 1 makes Apply inspect resp.GetSuccess() and return an error here;
			// this assertion flips from `nil` to a non-nil error at that point.
			name:    "PIN: apply-reported failure is silently swallowed",
			resp:    &pb.ApplyResponse{Success: false},
			err:     nil,
			wantErr: nil,
		},
		{
			// Defensive: a nil response with a nil error is also treated as success
			// today because the response is never dereferenced.
			name:    "PIN: nil response with nil error is treated as success",
			resp:    nil,
			err:     nil,
			wantErr: nil,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			client := &fakeStateMachineClient{resp: tt.resp, err: tt.err}
			f := NewCppFSM(client)

			got := f.Apply(&raft.Log{
				Index: 1,
				Term:  1,
				Type:  raft.LogCommand,
				Data:  []byte("payload"),
			})

			if tt.wantErr == nil {
				if got != nil {
					t.Fatalf("Apply() = %#v (%T), want nil", got, got)
				}
			} else {
				gotErr, ok := got.(error)
				if !ok {
					t.Fatalf("Apply() = %#v (%T), want an error", got, got)
				}
				if !errors.Is(gotErr, tt.wantErr) {
					t.Fatalf("Apply() error = %v, want %v", gotErr, tt.wantErr)
				}
			}

			if n := client.callCount(); n != 1 {
				t.Fatalf("client.Apply called %d times, want exactly 1", n)
			}
		})
	}
}

func TestCppFSMApplyForwardsDataOpaquely(t *testing.T) {
	// The MsgPack payload is never parsed on the Go side: it rides opaquely in
	// Command.data from the HTTP body all the way to the C++ state machine.
	//
	// These bytes are the MsgPack map {op: "SET", key: "k=\nz", value: bin(0xff 0x00)}:
	//   0x83                                     fixmap(3)
	//   0xa2 "op"     0xa3 "SET"                 str -> str
	//   0xa3 "key"    0xa4 "k=\n z"              str -> str containing '=' and '\n'
	//   0xa5 "value"  0xc4 0x02 0xff 0x00        str -> 2-byte bin, not valid UTF-8
	// Nothing here is line-safe or UTF-8 clean, so any accidental parsing or
	// normalisation on the Go side would show up as a mismatch below.
	payload := []byte{
		0x83,
		0xa2, 'o', 'p', 0xa3, 'S', 'E', 'T',
		0xa3, 'k', 'e', 'y', 0xa4, 'k', '=', '\n', 'z',
		0xa5, 'v', 'a', 'l', 'u', 'e', 0xc4, 0x02, 0xff, 0x00,
	}
	original := append([]byte(nil), payload...)

	client := &fakeStateMachineClient{resp: &pb.ApplyResponse{Success: true}}
	f := NewCppFSM(client)

	if got := f.Apply(&raft.Log{Index: 42, Term: 3, Type: raft.LogCommand, Data: payload}); got != nil {
		t.Fatalf("Apply() = %#v, want nil", got)
	}

	cmd := client.lastCommand()
	if cmd == nil {
		t.Fatal("client.Apply was never called")
	}
	if !bytes.Equal(cmd.GetData(), original) {
		t.Errorf("Command.Data = % x, want % x", cmd.GetData(), original)
	}
	// NOTE: a "payload was not mutated" check comparing payload against original
	// would be vacuous here. CppFSM.Apply does `&pb.Command{Data: l.Data}` with
	// no copy, so cmd.GetData() and payload are the SAME backing array — the
	// check above already covers both, and the two can only ever move together.
	//
	// Pin the property that is actually true instead: forwarding is zero-copy.
	// If Apply ever starts copying or re-encoding, this fires and whoever made
	// that change has to think about it.
	if data := cmd.GetData(); len(data) != len(payload) || &data[0] != &payload[0] {
		t.Error("Command.Data must alias raft.Log.Data (zero-copy opaque forwarding)")
	}
	// The typed proto fields exist but are unused in transit (see .claude/rules/protobuf.md).
	if cmd.GetOp() != "" || cmd.GetKey() != "" || cmd.GetValue() != "" {
		t.Errorf("op/key/value must stay empty, got op=%q key=%q value=%q",
			cmd.GetOp(), cmd.GetKey(), cmd.GetValue())
	}
	if client.lastContext() == nil {
		t.Error("Apply must pass a non-nil context to the state machine client")
	}
}

func TestCppFSMApplyForwardsEmptyAndNilPayloads(t *testing.T) {
	tests := []struct {
		name string
		data []byte
	}{
		{name: "nil data", data: nil},
		{name: "empty data", data: []byte{}},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			client := &fakeStateMachineClient{resp: &pb.ApplyResponse{Success: true}}
			f := NewCppFSM(client)

			if got := f.Apply(&raft.Log{Index: 1, Term: 1, Type: raft.LogCommand, Data: tt.data}); got != nil {
				t.Fatalf("Apply() = %#v, want nil", got)
			}

			cmd := client.lastCommand()
			if cmd == nil {
				t.Fatal("client.Apply was never called")
			}
			// No validation happens on the Go side; empty payloads are forwarded as-is
			// and only rejected (by throwing) inside the C++ StateMachine::Apply.
			if len(cmd.GetData()) != 0 {
				t.Errorf("Command.Data = % x, want empty", cmd.GetData())
			}
		})
	}
}

// ---------------------------------------------------------------------------
// Snapshot / Restore
// ---------------------------------------------------------------------------

func TestCppFSMSnapshotReturnsDummySnapshot(t *testing.T) {
	f := NewCppFSM(&fakeStateMachineClient{resp: &pb.ApplyResponse{Success: true}})

	snap, err := f.Snapshot()
	if err != nil {
		t.Fatalf("Snapshot() error = %v, want nil", err)
	}
	if snap == nil {
		t.Fatal("Snapshot() returned a nil raft.FSMSnapshot")
	}
	// PIN: snapshots are deliberately not implemented (CLAUDE.md "Known Limitations").
	// Phase 3 replaces DummySnapshot with a real C++ state export.
	if _, ok := snap.(*DummySnapshot); !ok {
		t.Fatalf("Snapshot() = %T, want *DummySnapshot", snap)
	}
}

func TestDummySnapshotPersistWritesNothingAndClosesSink(t *testing.T) {
	sink := &fakeSnapshotSink{}

	if err := (&DummySnapshot{}).Persist(sink); err != nil {
		t.Fatalf("Persist() error = %v, want nil", err)
	}

	writes, closes, cancels, n := sink.stats()
	// PIN: no state is ever written to the sink — Phase 3 changes this.
	if writes != 0 || n != 0 {
		t.Errorf("Persist wrote %d bytes in %d calls, want 0/0", n, writes)
	}
	if closes != 1 {
		t.Errorf("sink.Close called %d times, want exactly 1", closes)
	}
	if cancels != 0 {
		t.Errorf("sink.Cancel called %d times, want 0", cancels)
	}
}

func TestDummySnapshotPersistDiscardsSinkCloseError(t *testing.T) {
	// PIN: Persist closes the sink via `defer sink.Close()`, so a Close failure is
	// swallowed and Persist still reports success.
	sink := &fakeSnapshotSink{closeErr: errors.New("sink close failed")}

	if err := (&DummySnapshot{}).Persist(sink); err != nil {
		t.Fatalf("Persist() error = %v, want nil (Close's error is currently discarded)", err)
	}
	if _, closes, _, _ := sink.stats(); closes != 1 {
		t.Errorf("sink.Close called %d times, want exactly 1", closes)
	}
}

func TestDummySnapshotReleaseDoesNotPanic(t *testing.T) {
	defer func() {
		if r := recover(); r != nil {
			t.Fatalf("Release() panicked: %v", r)
		}
	}()

	d := &DummySnapshot{}
	d.Release()
	d.Release() // Release must stay idempotent / side-effect free.
}

func TestCppFSMRestoreClosesReaderAndDiscardsPayload(t *testing.T) {
	rc := &recordingReadCloser{reader: bytes.NewReader([]byte("snapshot-state"))}
	f := NewCppFSM(&fakeStateMachineClient{resp: &pb.ApplyResponse{Success: true}})

	if err := f.Restore(rc); err != nil {
		t.Fatalf("Restore() error = %v, want nil", err)
	}
	if rc.closes != 1 {
		t.Errorf("ReadCloser.Close called %d times, want exactly 1", rc.closes)
	}
	// PIN: Restore is a no-op — the snapshot bytes are never read, let alone pushed
	// into the C++ store. Phase 3 implements this and this assertion becomes reads > 0.
	if rc.reads != 0 {
		t.Errorf("ReadCloser.Read called %d times, want 0 (Restore is a no-op today)", rc.reads)
	}
}

func TestCppFSMRestoreDiscardsCloseError(t *testing.T) {
	// PIN: `defer rc.Close()` drops the error, so Restore reports success regardless.
	rc := &recordingReadCloser{
		reader:   bytes.NewReader(nil),
		closeErr: errors.New("close failed"),
	}
	f := NewCppFSM(&fakeStateMachineClient{})

	if err := f.Restore(rc); err != nil {
		t.Fatalf("Restore() error = %v, want nil (Close's error is currently discarded)", err)
	}
	if rc.closes != 1 {
		t.Errorf("ReadCloser.Close called %d times, want exactly 1", rc.closes)
	}
}

// ---------------------------------------------------------------------------
// NewStateMachineClient
// ---------------------------------------------------------------------------

func TestNewStateMachineClientWrapsGRPCClient(t *testing.T) {
	inner := &fakeGRPCStateMachineClient{resp: &pb.ApplyResponse{Success: true}}

	wrapped := NewStateMachineClient(inner)

	adapter, ok := wrapped.(*grpcStateMachineClient)
	if !ok {
		t.Fatalf("NewStateMachineClient() = %T, want *grpcStateMachineClient", wrapped)
	}
	if adapter.client != inner {
		t.Fatal("wrapper does not hold the supplied pb.StateMachineClient")
	}

	cmd := &pb.Command{Data: []byte("payload")}
	resp, err := wrapped.Apply(context.Background(), cmd)
	if err != nil {
		t.Fatalf("Apply() error = %v, want nil", err)
	}
	if resp == nil || !resp.GetSuccess() {
		t.Fatalf("Apply() response = %v, want Success=true", resp)
	}
	if inner.command() != cmd {
		t.Error("the wrapper did not forward the exact *pb.Command it was given")
	}
	// The adapter adds no grpc.CallOptions of its own.
	if n := inner.optCount(); n != 0 {
		t.Errorf("wrapper passed %d grpc.CallOptions, want 0", n)
	}
}

func TestNewStateMachineClientPropagatesError(t *testing.T) {
	wantErr := errors.New("rpc failed")
	inner := &fakeGRPCStateMachineClient{err: wantErr}

	resp, err := NewStateMachineClient(inner).Apply(context.Background(), &pb.Command{})

	if !errors.Is(err, wantErr) {
		t.Fatalf("Apply() error = %v, want %v", err, wantErr)
	}
	if resp != nil {
		t.Errorf("Apply() response = %v, want nil", resp)
	}
}
