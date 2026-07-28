package fsm

import (
	"bytes"
	"context"
	"errors"
	"io"
	"log"
	"os"
	"strings"
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

// fakeStateMachineClient records every call it receives and returns canned
// results. It stands in for the gRPC client to the C++ engine.
type fakeStateMachineClient struct {
	mu    sync.Mutex
	calls []*pb.Command
	ctxs  []context.Context

	resp *pb.ApplyResponse
	err  error

	// snapshotStream is handed back by GetSnapshot; leaving it nil while
	// snapshotErr is also nil exercises the defensive nil-stream branch.
	snapshotStream pb.StateMachine_GetSnapshotClient
	snapshotErr    error
	snapshotCalls  int

	restoreStream pb.StateMachine_RestoreSnapshotClient
	restoreErr    error
	restoreCalls  int
}

var _ StateMachineClient = (*fakeStateMachineClient)(nil)

func (f *fakeStateMachineClient) Apply(ctx context.Context, cmd *pb.Command) (*pb.ApplyResponse, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls = append(f.calls, cmd)
	f.ctxs = append(f.ctxs, ctx)
	return f.resp, f.err
}

func (f *fakeStateMachineClient) GetSnapshot(ctx context.Context, in *pb.SnapshotRequest) (pb.StateMachine_GetSnapshotClient, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.snapshotCalls++
	if f.snapshotErr != nil {
		return nil, f.snapshotErr
	}
	return f.snapshotStream, nil
}

func (f *fakeStateMachineClient) RestoreSnapshot(ctx context.Context) (pb.StateMachine_RestoreSnapshotClient, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.restoreCalls++
	if f.restoreErr != nil {
		return nil, f.restoreErr
	}
	return f.restoreStream, nil
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

func (f *fakeStateMachineClient) snapshotCallCount() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.snapshotCalls
}

func (f *fakeStateMachineClient) setSnapshotStream(s pb.StateMachine_GetSnapshotClient) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.snapshotStream = s
}

func (f *fakeStateMachineClient) restoreCallCount() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.restoreCalls
}

// fakeGetSnapshotStream implements pb.StateMachine_GetSnapshotClient, which is
// an alias for grpc.ServerStreamingClient[pb.SnapshotChunk].
//
// grpc.ClientStream contributes six more methods (Header, Trailer, CloseSend,
// Context, SendMsg, RecvMsg) that this code never calls. Embedding the
// interface satisfies the compiler without implementing them; because the
// embedded value stays nil, anything that did start calling one would panic
// immediately. That is the behaviour we want from a test double — loud, rather
// than quietly returning a zero value.
type fakeGetSnapshotStream struct {
	grpc.ClientStream

	mu     sync.Mutex
	chunks [][]byte
	// err, when set, is returned by the Recv at index errAt instead of a
	// chunk. errAt is ignored when err is nil.
	err   error
	errAt int

	recvs int
}

var _ pb.StateMachine_GetSnapshotClient = (*fakeGetSnapshotStream)(nil)

func (s *fakeGetSnapshotStream) Recv() (*pb.SnapshotChunk, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	i := s.recvs
	s.recvs++
	if s.err != nil && i == s.errAt {
		return nil, s.err
	}
	if i >= len(s.chunks) {
		return nil, io.EOF
	}
	return &pb.SnapshotChunk{Data: s.chunks[i]}, nil
}

func (s *fakeGetSnapshotStream) recvCount() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.recvs
}

// newGetSnapshotStream builds a stream that runs cleanly to io.EOF.
func newGetSnapshotStream(chunks ...[]byte) *fakeGetSnapshotStream {
	return &fakeGetSnapshotStream{chunks: chunks, errAt: -1}
}

// fakeRestoreStream implements pb.StateMachine_RestoreSnapshotClient, an alias
// for grpc.ClientStreamingClient[pb.SnapshotChunk, pb.RestoreResponse]. It
// embeds grpc.ClientStream for the same reason fakeGetSnapshotStream does.
type fakeRestoreStream struct {
	grpc.ClientStream

	mu   sync.Mutex
	sent [][]byte

	// sendErr, when set, fails the Send at index sendErrAt.
	sendErr   error
	sendErrAt int

	resp          *pb.RestoreResponse
	recvErr       error
	closeAndRecvs int
}

var _ pb.StateMachine_RestoreSnapshotClient = (*fakeRestoreStream)(nil)

func (s *fakeRestoreStream) Send(in *pb.SnapshotChunk) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.sendErr != nil && len(s.sent) == s.sendErrAt {
		return s.sendErr
	}
	// Deliberately retains the caller's slice instead of copying it. Real gRPC
	// marshals synchronously, so retaining is stricter than reality — and that
	// is the point: if streamSnapshot ever starts recycling one buffer across
	// chunks (which grpc-go explicitly forbids), every recorded chunk would
	// show the last chunk's bytes and the multi-chunk test fails loudly.
	s.sent = append(s.sent, in.GetData())
	return nil
}

func (s *fakeRestoreStream) CloseAndRecv() (*pb.RestoreResponse, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.closeAndRecvs++
	if s.recvErr != nil {
		return nil, s.recvErr
	}
	return s.resp, nil
}

func (s *fakeRestoreStream) chunks() [][]byte {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.sent
}

func (s *fakeRestoreStream) closeAndRecvCount() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.closeAndRecvs
}

// newRestoreStream builds a stream whose sends all succeed and which reports
// the given response from CloseAndRecv.
func newRestoreStream(resp *pb.RestoreResponse) *fakeRestoreStream {
	return &fakeRestoreStream{sendErrAt: -1, resp: resp}
}

// fakeGRPCStateMachineClient implements the generated pb.StateMachineClient so
// NewStateMachineClient's wrapping behaviour can be exercised without gRPC.
type fakeGRPCStateMachineClient struct {
	mu      sync.Mutex
	gotCmd  *pb.Command
	gotOpts int

	resp *pb.ApplyResponse
	err  error

	gotSnapshotReq *pb.SnapshotRequest
	snapshotStream pb.StateMachine_GetSnapshotClient
	restoreStream  pb.StateMachine_RestoreSnapshotClient

	gotGetReq *pb.GetRequest
	getResp   *pb.GetResponse
}

var _ pb.StateMachineClient = (*fakeGRPCStateMachineClient)(nil)

// Get satisfies pb.StateMachineClient. Phase 4 added it for linearizable
// reads; the FSM does not use it (reads never go through raft.FSM), so this
// exists to keep the fake implementing the full generated interface.
func (f *fakeGRPCStateMachineClient) Get(ctx context.Context, in *pb.GetRequest, opts ...grpc.CallOption) (*pb.GetResponse, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.gotGetReq = in
	if f.getResp != nil {
		return f.getResp, nil
	}
	return &pb.GetResponse{}, nil
}

func (f *fakeGRPCStateMachineClient) Apply(ctx context.Context, in *pb.Command, opts ...grpc.CallOption) (*pb.ApplyResponse, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.gotCmd = in
	f.gotOpts = len(opts)
	return f.resp, f.err
}

func (f *fakeGRPCStateMachineClient) GetSnapshot(ctx context.Context, in *pb.SnapshotRequest, opts ...grpc.CallOption) (pb.StateMachine_GetSnapshotClient, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.gotSnapshotReq = in
	f.gotOpts = len(opts)
	return f.snapshotStream, nil
}

func (f *fakeGRPCStateMachineClient) RestoreSnapshot(ctx context.Context, opts ...grpc.CallOption) (pb.StateMachine_RestoreSnapshotClient, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.gotOpts = len(opts)
	return f.restoreStream, nil
}

func (f *fakeGRPCStateMachineClient) command() *pb.Command {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.gotCmd
}

func (f *fakeGRPCStateMachineClient) snapshotRequest() *pb.SnapshotRequest {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.gotSnapshotReq
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
	writeErr error
	closeErr error
}

var _ raft.SnapshotSink = (*fakeSnapshotSink)(nil)

func (s *fakeSnapshotSink) Write(p []byte) (int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.writes++
	if s.writeErr != nil {
		return 0, s.writeErr
	}
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

func (s *fakeSnapshotSink) writtenBytes() []byte {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.written
}

// recordingReadCloser counts Read and Close calls so Restore's handling of the
// snapshot stream can be observed, and can fail a read on demand.
type recordingReadCloser struct {
	reader   *bytes.Reader
	reads    int
	closes   int
	closeErr error

	// readErr, when set, is returned once readErrAfter successful reads have
	// happened, modelling a snapshot file that goes bad part-way through.
	readErr      error
	readErrAfter int
}

var _ io.ReadCloser = (*recordingReadCloser)(nil)

func (r *recordingReadCloser) Read(p []byte) (int, error) {
	r.reads++
	if r.readErr != nil && r.reads > r.readErrAfter {
		return 0, r.readErr
	}
	return r.reader.Read(p)
}

func (r *recordingReadCloser) Close() error {
	r.closes++
	return r.closeErr
}

// ---------------------------------------------------------------------------
// CppFSM.Apply
// ---------------------------------------------------------------------------

// TestCppFSMApply covers every outcome of a single Raft log entry.
//
// Phase 1 (R1.4) fixed what Phase 0 pinned here: Apply used to throw the
// response away (`_, err := f.client.Apply(...)`) and report success to Raft
// whenever the gRPC call itself succeeded. A C++ state machine that explicitly
// answered "I could not apply this" was therefore recorded as applied — silent
// state divergence, with Raft believing this replica holds an entry it does
// not. The two cases below that used to assert `nil` now assert a non-nil
// *ApplyError carrying the state machine's own reason.
func TestCppFSMApply(t *testing.T) {
	const (
		logIndex = uint64(7)
		logTerm  = uint64(3)
	)
	transportErr := errors.New("rpc error: code = Unavailable desc = connection refused")

	tests := []struct {
		name string
		resp *pb.ApplyResponse
		err  error

		wantErr bool
		// wantErrIs, when non-nil, must be reachable via errors.Is so the
		// original transport failure stays inspectable through the wrapper.
		wantErrIs error
		// wantContains are substrings the error message must carry. The
		// index/term are asserted separately off the typed *ApplyError.
		wantContains []string
	}{
		{
			name:    "success is reported as nil",
			resp:    &pb.ApplyResponse{Success: true},
			err:     nil,
			wantErr: false,
		},
		{
			name:      "transport error is wrapped and returned",
			resp:      nil,
			err:       transportErr,
			wantErr:   true,
			wantErrIs: transportErr,
			wantContains: []string{
				"index=7",
				"term=3",
				"connection refused",
			},
		},
		{
			// Was "PIN: apply-reported failure is silently swallowed".
			// R1.4 flipped it: the state machine's own reason now reaches Raft.
			name: "apply-reported failure is returned with the state machine's reason",
			resp: &pb.ApplyResponse{
				Success: false,
				Error:   "invalid command: empty key for op SET",
			},
			err:     nil,
			wantErr: true,
			wantContains: []string{
				"index=7",
				"term=3",
				"invalid command: empty key for op SET",
			},
		},
		{
			// A false success with no error text is still a failure; the entry
			// is unapplied either way, so Apply supplies its own reason rather
			// than reporting an empty one.
			name:    "apply-reported failure without a reason still fails",
			resp:    &pb.ApplyResponse{Success: false},
			err:     nil,
			wantErr: true,
			wantContains: []string{
				"index=7",
				"term=3",
				"state machine reported failure without a reason",
			},
		},
		{
			// Was "PIN: nil response with nil error is treated as success".
			// Defensive: the response used to be discarded, so a nil one could
			// never be noticed. It is now an explicit failure.
			name:    "nil response with nil error is a failure",
			resp:    nil,
			err:     nil,
			wantErr: true,
			wantContains: []string{
				"index=7",
				"term=3",
				"nil response",
			},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			client := &fakeStateMachineClient{resp: tt.resp, err: tt.err}
			f := NewCppFSM(client)

			got := f.Apply(&raft.Log{
				Index: logIndex,
				Term:  logTerm,
				Type:  raft.LogCommand,
				Data:  []byte("payload"),
			})

			if !tt.wantErr {
				if got != nil {
					t.Fatalf("Apply() = %#v (%T), want nil", got, got)
				}
				if n := client.callCount(); n != 1 {
					t.Fatalf("client.Apply called %d times, want exactly 1", n)
				}
				return
			}

			gotErr, ok := got.(error)
			if !ok {
				t.Fatalf("Apply() = %#v (%T), want an error", got, got)
			}
			if tt.wantErrIs != nil && !errors.Is(gotErr, tt.wantErrIs) {
				t.Errorf("Apply() error = %v, want errors.Is(..., %v)", gotErr, tt.wantErrIs)
			}
			for _, want := range tt.wantContains {
				if !strings.Contains(gotErr.Error(), want) {
					t.Errorf("Apply() error = %q, want it to contain %q", gotErr.Error(), want)
				}
			}

			// The typed error is the seam rpc.Server.Propose inspects, and the
			// index/term are what tell an operator which entry diverged.
			var applyErr *ApplyError
			if !errors.As(gotErr, &applyErr) {
				t.Fatalf("Apply() error = %#v (%T), want an *ApplyError", gotErr, gotErr)
			}
			if applyErr.Index != logIndex || applyErr.Term != logTerm {
				t.Errorf("ApplyError index/term = %d/%d, want %d/%d",
					applyErr.Index, applyErr.Term, logIndex, logTerm)
			}
			if applyErr.Reason == "" {
				t.Error("ApplyError.Reason is empty; the failure must always name a cause")
			}

			if n := client.callCount(); n != 1 {
				t.Fatalf("client.Apply called %d times, want exactly 1", n)
			}
		})
	}
}

// TestApplyErrorMessage pins the text an operator reads in the sidecar log and
// that rpc.Server.Propose forwards verbatim into ProposeResponse.error.
func TestApplyErrorMessage(t *testing.T) {
	cause := errors.New("connection refused")

	tests := []struct {
		name      string
		err       *ApplyError
		want      string
		wantCause error // nil => Unwrap must return nil
	}{
		{
			name:      "state machine rejection has no underlying cause",
			err:       &ApplyError{Index: 12, Term: 4, Reason: "unknown op FOO"},
			want:      "fsm: failed to apply raft log entry index=12 term=4: unknown op FOO",
			wantCause: nil,
		},
		{
			name: "transport failure keeps its cause reachable",
			err: &ApplyError{
				Index:  12,
				Term:   4,
				Reason: cause.Error(),
				Err:    cause,
			},
			want:      "fsm: failed to apply raft log entry index=12 term=4: connection refused",
			wantCause: cause,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := tt.err.Error(); got != tt.want {
				t.Errorf("Error() = %q, want %q", got, tt.want)
			}
			if got := errors.Unwrap(tt.err); got != tt.wantCause {
				t.Errorf("errors.Unwrap() = %v, want %v", got, tt.wantCause)
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
// CppFSM.Snapshot / cppSnapshot.Persist
//
// Phase 0 pinned DummySnapshot here: Snapshot() returned a placeholder,
// Persist() wrote nothing and swallowed the sink's Close error, and Release()
// merely had to not panic. Phase 3 (R3.4) replaces all of it with a real
// export of the C++ state, and each of those pins is replaced below by its
// real-behaviour equivalent rather than being dropped.
// ---------------------------------------------------------------------------

// TestCppFSMSnapshotPersistsTheWholeStream is the replacement for
// TestCppFSMSnapshotReturnsDummySnapshot and
// TestDummySnapshotPersistWritesNothingAndClosesSink: the snapshot now carries
// the state machine's bytes, and Persist writes exactly their concatenation.
func TestCppFSMSnapshotPersistsTheWholeStream(t *testing.T) {
	tests := []struct {
		name   string
		chunks [][]byte
		want   []byte
	}{
		{
			// An empty export is still a valid snapshot as far as Go is
			// concerned; the format lives entirely on the C++ side.
			name:   "stream with no chunks",
			chunks: nil,
			want:   nil,
		},
		{
			name:   "single chunk",
			chunks: [][]byte{[]byte("KVB1\x00\x00\x00\x00")},
			want:   []byte("KVB1\x00\x00\x00\x00"),
		},
		{
			name:   "chunks are concatenated in arrival order",
			chunks: [][]byte{[]byte("KVB1"), []byte("-middle-"), []byte("-tail")},
			want:   []byte("KVB1-middle--tail"),
		},
		{
			// A zero-length chunk mid-stream must not be mistaken for the end
			// of the stream; only io.EOF terminates it.
			name:   "empty chunk mid-stream is not end of stream",
			chunks: [][]byte{[]byte("head"), {}, []byte("tail")},
			want:   []byte("headtail"),
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			client := &fakeStateMachineClient{snapshotStream: newGetSnapshotStream(tt.chunks...)}
			f := NewCppFSM(client)

			snap, err := f.Snapshot()
			if err != nil {
				t.Fatalf("Snapshot() error = %v, want nil", err)
			}
			if snap == nil {
				t.Fatal("Snapshot() returned a nil raft.FSMSnapshot")
			}
			if _, ok := snap.(*cppSnapshot); !ok {
				t.Fatalf("Snapshot() = %T, want *cppSnapshot (DummySnapshot is gone)", snap)
			}

			sink := &fakeSnapshotSink{}
			if err := snap.Persist(sink); err != nil {
				t.Fatalf("Persist() error = %v, want nil", err)
			}

			if got := sink.writtenBytes(); !bytes.Equal(got, tt.want) {
				t.Errorf("Persist wrote %q, want %q", got, tt.want)
			}
			writes, closes, cancels, _ := sink.stats()
			if writes != 1 {
				t.Errorf("sink.Write called %d times, want exactly 1", writes)
			}
			if closes != 1 {
				t.Errorf("sink.Close called %d times, want exactly 1", closes)
			}
			if cancels != 0 {
				t.Errorf("sink.Cancel called %d times, want 0 on the success path", cancels)
			}

			snap.Release() // Raft always releases; it must stay harmless.
		})
	}
}

// TestCppFSMSnapshotCapturesStateEagerly pins Seam 4, which is the whole
// correctness argument for this phase.
//
// Raft calls Snapshot() on the FSM goroutine with no Apply in flight, then may
// call Persist() much later while further entries are being applied. The
// snapshot is labelled with the index applied at Snapshot() time, so the bytes
// must be read at Snapshot() time too. A lazy implementation that talked to C++
// inside Persist would file later state under an earlier index — and because
// SET/DELETE are idempotent, the cluster would still re-converge on replay and
// hide the bug. This test makes the timing observable instead.
func TestCppFSMSnapshotCapturesStateEagerly(t *testing.T) {
	atSnapshotTime := newGetSnapshotStream([]byte("state-at-snapshot-time"))
	client := &fakeStateMachineClient{snapshotStream: atSnapshotTime}
	f := NewCppFSM(client)

	snap, err := f.Snapshot()
	if err != nil {
		t.Fatalf("Snapshot() error = %v, want nil", err)
	}
	if got := client.snapshotCallCount(); got != 1 {
		t.Fatalf("GetSnapshot called %d times during Snapshot(), want exactly 1", got)
	}
	if atSnapshotTime.recvCount() == 0 {
		t.Error("Snapshot() never read the stream; the state must be drained eagerly")
	}

	// The backend now holds completely different state. Nothing Persist does
	// may pick it up.
	client.setSnapshotStream(newGetSnapshotStream([]byte("state-at-persist-time")))

	sink := &fakeSnapshotSink{}
	if err := snap.Persist(sink); err != nil {
		t.Fatalf("Persist() error = %v, want nil", err)
	}

	if got := string(sink.writtenBytes()); got != "state-at-snapshot-time" {
		t.Errorf("Persist wrote %q; the snapshot must hold the state captured by "+
			"Snapshot(), not whatever the state machine holds at Persist() time", got)
	}
	if got := client.snapshotCallCount(); got != 1 {
		t.Errorf("GetSnapshot called %d times in total; Persist must not open a "+
			"second export (it runs concurrently with later Applies)", got)
	}
}

func TestCppFSMSnapshotFailsWithoutASnapshot(t *testing.T) {
	openErr := errors.New("rpc error: code = Unavailable desc = connection refused")
	midErr := errors.New("rpc error: code = Internal desc = store read failed")

	tests := []struct {
		name         string
		client       *fakeStateMachineClient
		wantContains []string
	}{
		{
			name:         "the export cannot be opened",
			client:       &fakeStateMachineClient{snapshotErr: openErr},
			wantContains: []string{"GetSnapshot", "connection refused"},
		},
		{
			// Defensive, mirroring Apply's nil-response guard: a nil stream
			// with a nil error would otherwise panic on the first Recv.
			name:         "a nil stream is not treated as an empty snapshot",
			client:       &fakeStateMachineClient{},
			wantContains: []string{"nil snapshot stream"},
		},
		{
			name: "the stream fails before any chunk arrives",
			client: &fakeStateMachineClient{snapshotStream: &fakeGetSnapshotStream{
				chunks: [][]byte{[]byte("KVB1")},
				err:    midErr,
				errAt:  0,
			}},
			wantContains: []string{"store read failed", "after 0 byte(s)"},
		},
		{
			// The dangerous one: some data arrived, so a truncated snapshot is
			// available and must NOT be handed to raft as a complete one.
			name: "the stream fails mid-way and the partial state is discarded",
			client: &fakeStateMachineClient{snapshotStream: &fakeGetSnapshotStream{
				chunks: [][]byte{[]byte("KVB1"), []byte("partial")},
				err:    midErr,
				errAt:  1,
			}},
			wantContains: []string{"store read failed", "after 4 byte(s)"},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			f := NewCppFSM(tt.client)

			snap, err := f.Snapshot()

			if err == nil {
				t.Fatal("Snapshot() error = nil, want an error; raft would otherwise " +
					"persist an incomplete snapshot")
			}
			if snap != nil {
				t.Errorf("Snapshot() = %#v, want a nil raft.FSMSnapshot alongside the error", snap)
			}
			for _, want := range tt.wantContains {
				if !strings.Contains(err.Error(), want) {
					t.Errorf("Snapshot() error = %q, want it to contain %q", err.Error(), want)
				}
			}
		})
	}
}

// TestCppSnapshotPersistCancelsSinkOnFailure replaces
// TestDummySnapshotPersistDiscardsSinkCloseError, which pinned the old
// behaviour of swallowing Close's error and reporting success.
//
// A sink that is Close()d is finalised and offered back to raft as a usable
// snapshot. A half-written one must therefore be cancelled, never closed, and
// the failure must reach the caller.
func TestCppSnapshotPersistCancelsSinkOnFailure(t *testing.T) {
	writeErr := errors.New("no space left on device")
	closeErr := errors.New("fsync failed")

	tests := []struct {
		name       string
		sink       *fakeSnapshotSink
		wantCloses int
		wantCancel int
		wantErrIs  error
	}{
		{
			name:       "a failed write is cancelled and never closed",
			sink:       &fakeSnapshotSink{writeErr: writeErr},
			wantCloses: 0,
			wantCancel: 1,
			wantErrIs:  writeErr,
		},
		{
			// Close failed, so the snapshot was never finalised. Cancel is a
			// no-op on raft's FileSnapshotSink but is issued anyway so any
			// other sink implementation discards the partial write.
			name:       "a failed close is reported and the sink is cancelled",
			sink:       &fakeSnapshotSink{closeErr: closeErr},
			wantCloses: 1,
			wantCancel: 1,
			wantErrIs:  closeErr,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			snap := &cppSnapshot{state: []byte("KVB1-some-state")}

			err := snap.Persist(tt.sink)

			if err == nil {
				t.Fatal("Persist() error = nil, want an error")
			}
			if !errors.Is(err, tt.wantErrIs) {
				t.Errorf("Persist() error = %v, want errors.Is(..., %v)", err, tt.wantErrIs)
			}
			if !strings.Contains(err.Error(), tt.sink.ID()) {
				t.Errorf("Persist() error = %q, want it to name the sink %q",
					err.Error(), tt.sink.ID())
			}
			_, closes, cancels, _ := tt.sink.stats()
			if closes != tt.wantCloses {
				t.Errorf("sink.Close called %d times, want %d", closes, tt.wantCloses)
			}
			if cancels != tt.wantCancel {
				t.Errorf("sink.Cancel called %d times, want %d", cancels, tt.wantCancel)
			}
		})
	}
}

// TestCppSnapshotReleaseIsANoOp replaces TestDummySnapshotReleaseDoesNotPanic.
// Raft calls Release after Persist on both the success and failure paths, so it
// must stay repeatable and must not disturb the captured state.
func TestCppSnapshotReleaseIsANoOp(t *testing.T) {
	snap := &cppSnapshot{state: []byte("KVB1-state")}

	snap.Release()
	snap.Release()

	sink := &fakeSnapshotSink{}
	if err := snap.Persist(sink); err != nil {
		t.Fatalf("Persist() after Release() error = %v, want nil", err)
	}
	if got := string(sink.writtenBytes()); got != "KVB1-state" {
		t.Errorf("Persist wrote %q after Release(), want the captured state unchanged", got)
	}
}

// ---------------------------------------------------------------------------
// CppFSM.Restore
// ---------------------------------------------------------------------------

// TestCppFSMRestoreStreamsReaderInChunks replaces
// TestCppFSMRestoreClosesReaderAndDiscardsPayload, which pinned Restore as a
// no-op that never even read the snapshot.
func TestCppFSMRestoreStreamsReaderInChunks(t *testing.T) {
	// Two full 64 KiB chunks plus a short tail, so both the boundary and the
	// remainder are exercised.
	payload := make([]byte, 2*snapshotChunkSize+18928)
	for i := range payload {
		// Position-dependent with no repeat across the payload: identical
		// bytes at the same offset in two chunks would let a reordering or a
		// duplicated chunk slip past the comparison below.
		payload[i] = byte(i) ^ byte(i>>8) ^ byte(i>>16)
	}

	rc := &recordingReadCloser{reader: bytes.NewReader(payload)}
	stream := newRestoreStream(&pb.RestoreResponse{Success: true})
	client := &fakeStateMachineClient{restoreStream: stream}
	f := NewCppFSM(client)

	if err := f.Restore(rc); err != nil {
		t.Fatalf("Restore() error = %v, want nil", err)
	}

	if n := client.restoreCallCount(); n != 1 {
		t.Errorf("RestoreSnapshot opened %d times, want exactly 1", n)
	}
	chunks := stream.chunks()
	wantSizes := []int{snapshotChunkSize, snapshotChunkSize, 18928}
	if len(chunks) != len(wantSizes) {
		t.Fatalf("Restore sent %d chunks, want %d", len(chunks), len(wantSizes))
	}
	offset := 0
	for i, want := range wantSizes {
		if len(chunks[i]) != want {
			t.Errorf("chunk %d is %d bytes, want %d", i, len(chunks[i]), want)
		}
		if end := offset + len(chunks[i]); end <= len(payload) {
			if !bytes.Equal(chunks[i], payload[offset:end]) {
				t.Errorf("chunk %d does not match payload[%d:%d]", i, offset, end)
			}
		}
		offset += len(chunks[i])
	}
	if got := bytes.Join(chunks, nil); !bytes.Equal(got, payload) {
		t.Errorf("the concatenated chunks (%d bytes) do not reproduce the snapshot (%d bytes)",
			len(got), len(payload))
	}
	if rc.reads == 0 {
		t.Error("Restore never read the snapshot")
	}
	if rc.closes != 1 {
		t.Errorf("ReadCloser.Close called %d times, want exactly 1", rc.closes)
	}
	if n := stream.closeAndRecvCount(); n != 1 {
		t.Errorf("CloseAndRecv called %d times, want exactly 1", n)
	}
}

// TestCppFSMRestoreFailsHard is R3.5: a node that could not install the
// snapshot must not go on serving. Every row asserts a NON-nil error — a nil
// return is the dangerous outcome, because raft would then believe this node
// holds state it never received.
func TestCppFSMRestoreFailsHard(t *testing.T) {
	openErr := errors.New("rpc error: code = Unavailable desc = connection refused")
	sendErr := errors.New("rpc error: code = ResourceExhausted desc = message too large")
	recvErr := errors.New("rpc error: code = Internal desc = stream aborted")
	readErr := errors.New("input/output error")

	tests := []struct {
		name         string
		payload      []byte
		stream       *fakeRestoreStream
		openErr      error
		readErr      error
		wantContains []string
	}{
		{
			name:         "the restore stream cannot be opened",
			payload:      []byte("KVB1-state"),
			openErr:      openErr,
			wantContains: []string{"RestoreSnapshot", "connection refused"},
		},
		{
			// Defensive, mirroring the Snapshot guard: a nil stream with a nil
			// error would panic on the first Send.
			name:         "a nil stream is rejected instead of panicking",
			payload:      []byte("KVB1-state"),
			wantContains: []string{"nil restore stream"},
		},
		{
			name:    "the state machine reports failure with a reason",
			payload: []byte("KVB1-state"),
			stream: newRestoreStream(&pb.RestoreResponse{
				Success: false,
				Error:   "bad snapshot magic",
			}),
			wantContains: []string{"rejected the snapshot", "bad snapshot magic"},
		},
		{
			name:         "the state machine reports failure without a reason",
			payload:      []byte("KVB1-state"),
			stream:       newRestoreStream(&pb.RestoreResponse{Success: false}),
			wantContains: []string{"rejected the snapshot", "reported failure without a reason"},
		},
		{
			name:         "CloseAndRecv fails",
			payload:      []byte("KVB1-state"),
			stream:       &fakeRestoreStream{sendErrAt: -1, recvErr: recvErr},
			wantContains: []string{"completing RestoreSnapshot", "stream aborted"},
		},
		{
			name:         "a nil response is not treated as success",
			payload:      []byte("KVB1-state"),
			stream:       newRestoreStream(nil),
			wantContains: []string{"nil RestoreSnapshot response"},
		},
		{
			name:         "a chunk cannot be sent",
			payload:      []byte("KVB1-state"),
			stream:       &fakeRestoreStream{sendErrAt: 0, sendErr: sendErr},
			wantContains: []string{"sending snapshot chunk", "message too large"},
		},
		{
			// gRPC reports io.EOF from Send when the server has already ended
			// the stream. If the server then claims success it cannot have seen
			// the whole snapshot, so success must not be believed.
			name:    "the server ends the stream early and still claims success",
			payload: []byte("KVB1-state"),
			stream: &fakeRestoreStream{
				sendErrAt: 0,
				sendErr:   io.EOF,
				resp:      &pb.RestoreResponse{Success: true},
			},
			wantContains: []string{"truncated restore", "0 byte(s)"},
		},
		{
			name:         "the snapshot cannot be read",
			payload:      []byte("KVB1-state"),
			stream:       newRestoreStream(&pb.RestoreResponse{Success: true}),
			readErr:      readErr,
			wantContains: []string{"reading the snapshot to restore", "input/output error"},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			rc := &recordingReadCloser{
				reader:  bytes.NewReader(tt.payload),
				readErr: tt.readErr,
			}
			client := &fakeStateMachineClient{restoreErr: tt.openErr}
			if tt.stream != nil {
				client.restoreStream = tt.stream
			}
			f := NewCppFSM(client)

			err := f.Restore(rc)

			if err == nil {
				t.Fatal("Restore() error = nil, want an error; a node that could not " +
					"restore must not claim state it does not have")
			}
			for _, want := range tt.wantContains {
				if !strings.Contains(err.Error(), want) {
					t.Errorf("Restore() error = %q, want it to contain %q", err.Error(), want)
				}
			}
			// Raft hands the reader over on every path, including the failing
			// ones; leaking it would leak the snapshot file handle.
			if rc.closes != 1 {
				t.Errorf("ReadCloser.Close called %d times, want exactly 1", rc.closes)
			}
		})
	}
}

// TestCppFSMRestoreDoesNotCommitATruncatedStream covers the reason the restore
// RPC runs on a cancellable context.
//
// When the local snapshot file goes bad part-way through, calling CloseAndRecv
// would tell the C++ side "that was the whole snapshot" and let it commit the
// prefix it received. Abandoning the RPC instead leaves the node's existing
// state alone.
func TestCppFSMRestoreDoesNotCommitATruncatedStream(t *testing.T) {
	payload := make([]byte, 2*snapshotChunkSize)
	rc := &recordingReadCloser{
		reader:       bytes.NewReader(payload),
		readErr:      errors.New("input/output error"),
		readErrAfter: 1, // the first chunk lands, the second read fails
	}
	stream := newRestoreStream(&pb.RestoreResponse{Success: true})
	f := NewCppFSM(&fakeStateMachineClient{restoreStream: stream})

	err := f.Restore(rc)

	if err == nil {
		t.Fatal("Restore() error = nil, want an error")
	}
	if n := len(stream.chunks()); n != 1 {
		t.Errorf("Restore sent %d chunks, want 1 before the read failed", n)
	}
	if n := stream.closeAndRecvCount(); n != 0 {
		t.Errorf("CloseAndRecv called %d times, want 0: closing the send side would "+
			"present the truncated prefix to C++ as a complete snapshot", n)
	}
	if !strings.Contains(err.Error(), "offset 65536") {
		t.Errorf("Restore() error = %q, want it to report how far it got", err.Error())
	}
	if rc.closes != 1 {
		t.Errorf("ReadCloser.Close called %d times, want exactly 1", rc.closes)
	}
}

// TestCppFSMRestoreDiscardsReaderCloseError keeps the Phase 0 pin: the reader's
// Close error is deliberately dropped. By the time it fires the bytes have been
// read and accepted by C++, so failing a good restore over a file handle would
// take a healthy node out of service for nothing.
func TestCppFSMRestoreDiscardsReaderCloseError(t *testing.T) {
	rc := &recordingReadCloser{
		reader:   bytes.NewReader([]byte("KVB1-state")),
		closeErr: errors.New("close failed"),
	}
	f := NewCppFSM(&fakeStateMachineClient{
		restoreStream: newRestoreStream(&pb.RestoreResponse{Success: true}),
	})

	if err := f.Restore(rc); err != nil {
		t.Fatalf("Restore() error = %v, want nil (Close's error is deliberately discarded)", err)
	}
	if rc.closes != 1 {
		t.Errorf("ReadCloser.Close called %d times, want exactly 1", rc.closes)
	}
}

func TestCppFSMRestoreOfAnEmptySnapshotSendsNoChunks(t *testing.T) {
	// Raft never produces a zero-length snapshot — Persist always writes at
	// least the "KVB1" header — so this only happens with a corrupt snapshot
	// file. Go stays format-agnostic and forwards nothing; rejecting it is the
	// C++ side's job (it validates the magic), which is why success here is
	// reported faithfully rather than second-guessed.
	rc := &recordingReadCloser{reader: bytes.NewReader(nil)}
	stream := newRestoreStream(&pb.RestoreResponse{Success: true})
	f := NewCppFSM(&fakeStateMachineClient{restoreStream: stream})

	if err := f.Restore(rc); err != nil {
		t.Fatalf("Restore() error = %v, want nil", err)
	}
	if n := len(stream.chunks()); n != 0 {
		t.Errorf("Restore sent %d chunks for an empty snapshot, want 0", n)
	}
	if n := stream.closeAndRecvCount(); n != 1 {
		t.Errorf("CloseAndRecv called %d times, want exactly 1", n)
	}
	if rc.closes != 1 {
		t.Errorf("ReadCloser.Close called %d times, want exactly 1", rc.closes)
	}
}

// ---------------------------------------------------------------------------
// NewStateMachineClient
// ---------------------------------------------------------------------------

func TestNewStateMachineClientWrapsGRPCClient(t *testing.T) {
	snapshotStream := newGetSnapshotStream([]byte("KVB1"))
	restoreStream := newRestoreStream(&pb.RestoreResponse{Success: true})
	inner := &fakeGRPCStateMachineClient{
		resp:           &pb.ApplyResponse{Success: true},
		snapshotStream: snapshotStream,
		restoreStream:  restoreStream,
	}

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

	req := &pb.SnapshotRequest{}
	gotSnapshot, err := wrapped.GetSnapshot(context.Background(), req)
	if err != nil {
		t.Fatalf("GetSnapshot() error = %v, want nil", err)
	}
	if gotSnapshot != snapshotStream {
		t.Error("the wrapper did not return the generated client's snapshot stream")
	}
	if inner.snapshotRequest() != req {
		t.Error("the wrapper did not forward the exact *pb.SnapshotRequest it was given")
	}
	if n := inner.optCount(); n != 0 {
		t.Errorf("wrapper passed %d grpc.CallOptions to GetSnapshot, want 0", n)
	}

	gotRestore, err := wrapped.RestoreSnapshot(context.Background())
	if err != nil {
		t.Fatalf("RestoreSnapshot() error = %v, want nil", err)
	}
	if gotRestore != restoreStream {
		t.Error("the wrapper did not return the generated client's restore stream")
	}
	if n := inner.optCount(); n != 0 {
		t.Errorf("wrapper passed %d grpc.CallOptions to RestoreSnapshot, want 0", n)
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
