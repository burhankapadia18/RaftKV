package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"time"

	"github.com/hashicorp/raft"
	raftboltdb "github.com/hashicorp/raft-boltdb"
	"google.golang.org/grpc"

	pb "my-raft-sidecar/pb"
)

var (
	nodeID      = flag.String("id", "node1", "Unique Node ID")
	raftPort    = flag.String("raft", "8088", "Raft TCP Port")
	sidecarPort = flag.String("srv", "50052", "Sidecar gRPC Port")
	appAddr     = flag.String("app", "localhost:50051", "Address of C++ App gRPC")
	mgmtPort    = flag.String("mgmt", "6000", "Management HTTP Port")
	bootstrap   = flag.Bool("bootstrap", false, "Bootstrap the cluster (Leader only)")
	dataDir     = flag.String("data", "raft-data", "Directory to store Raft logs")
	joinAddr    = flag.String("join", "", "Address of Leader's Management API to join")

	// Hostname to advertise (e.g., "node1")
	raftAdvertise = flag.String("advertise", "", "Address to advertise to other nodes")
)

type CppFSM struct {
	client pb.StateMachineClient
}

func (f *CppFSM) Apply(l *raft.Log) interface{} {
	_, err := f.client.Apply(context.Background(), &pb.Command{Data: l.Data})
	if err != nil {
		log.Printf("ERROR: Failed to apply to C++ DB: %v", err)
	}
	return nil
}

func (f *CppFSM) Snapshot() (raft.FSMSnapshot, error) { return &DummySnapshot{}, nil }
func (f *CppFSM) Restore(rc io.ReadCloser) error      { defer rc.Close(); return nil }

type DummySnapshot struct{}

func (d *DummySnapshot) Persist(sink raft.SnapshotSink) error { defer sink.Close(); return nil }
func (d *DummySnapshot) Release()                             {}

type RaftRPC struct {
	pb.UnimplementedRaftNodeServer
	raft *raft.Raft
}

func (r *RaftRPC) Propose(ctx context.Context, cmd *pb.Command) (*pb.ProposeResponse, error) {
	future := r.raft.Apply(cmd.Data, 5*time.Second)
	if err := future.Error(); err != nil {
		return &pb.ProposeResponse{Success: false, Error: err.Error()}, nil
	}
	return &pb.ProposeResponse{Success: true}, nil
}

func startManagementServer(r *raft.Raft) {
	http.HandleFunc("/join", func(w http.ResponseWriter, req *http.Request) {
		peerAddress := req.URL.Query().Get("peerAddress")
		peerID := req.URL.Query().Get("peerID")
		if peerAddress == "" || peerID == "" {
			http.Error(w, "Missing peerAddress or peerID", http.StatusBadRequest)
			return
		}
		log.Printf("Received join request for %s at %s", peerID, peerAddress)
		future := r.AddVoter(raft.ServerID(peerID), raft.ServerAddress(peerAddress), 0, 0)
		if err := future.Error(); err != nil {
			log.Printf("Failed to add voter: %v", err)
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		w.Write([]byte("Joined successfully"))
	})

	// Bind to 0.0.0.0 so Docker exposes it
	log.Printf("Management API listening on 0.0.0.0:%s", *mgmtPort)
	go http.ListenAndServe("0.0.0.0:"+*mgmtPort, nil)
}

func tryJoinCluster(leaderMgmtAddr, myID, myRaftAddr string) {
	for i := 0; i < 20; i++ {
		time.Sleep(2 * time.Second)
		url := fmt.Sprintf("http://%s/join?peerID=%s&peerAddress=%s",
			leaderMgmtAddr, myID, myRaftAddr)

		log.Printf("Attempting to join cluster via %s...", url)
		resp, err := http.Get(url)
		if err != nil {
			log.Printf("Join attempt %d failed (conn): %v", i+1, err)
			continue
		}
		defer resp.Body.Close()
		if resp.StatusCode == 200 {
			log.Println("Successfully joined the cluster!")
			return
		}
		body, _ := io.ReadAll(resp.Body)
		log.Printf("Join attempt %d failed (status %d): %s", i+1, resp.StatusCode, body)
	}
	log.Println("CRITICAL: Failed to join cluster.")
}

func main() {
	flag.Parse()
	os.MkdirAll(*dataDir, 0700)

	var conn *grpc.ClientConn
	var err error
	for i := 0; i < 15; i++ {
		conn, err = grpc.Dial(*appAddr, grpc.WithInsecure())
		if err == nil {
			break
		}
		time.Sleep(1 * time.Second)
		fmt.Println("Waiting for C++ Backend at", *appAddr)
	}
	if err != nil {
		log.Fatalf("Could not connect to C++: %v", err)
	}
	fsm := &CppFSM{client: pb.NewStateMachineClient(conn)}

	config := raft.DefaultConfig()
	config.LocalID = raft.ServerID(*nodeID)

	boltDB, err := raftboltdb.NewBoltStore(filepath.Join(*dataDir, "logs.dat"))
	if err != nil {
		log.Fatal(err)
	}

	// Bind Address: Listen on ALL interfaces (0.0.0.0) so Docker works
	bindAddr := "0.0.0.0:" + *raftPort

	// Advertise Address: What we tell others to connect to.
	// If -advertise is set (e.g. "node1"), use that. Otherwise fall back to bindAddr.
	advertiseString := bindAddr
	if *raftAdvertise != "" {
		advertiseString = *raftAdvertise + ":" + *raftPort
	}

	// Resolve the Advertise Address to a TCPAddr
	// Raft needs this to be a valid, routable address (NOT 0.0.0.0)
	advAddr, err := net.ResolveTCPAddr("tcp", advertiseString)
	if err != nil {
		log.Fatalf("Failed to resolve advertise address %s: %v", advertiseString, err)
	}

	// Create Transport
	// First arg: Bind Address (Where we listen)
	// Second arg: Advertise Address (What we tell others)
	transport, err := raft.NewTCPTransport(bindAddr, advAddr, 3, 10*time.Second, os.Stderr)
	if err != nil {
		log.Fatalf("Failed to create TCP transport: %v", err)
	}

	r, err := raft.NewRaft(config, fsm, boltDB, boltDB, raft.NewDiscardSnapshotStore(), transport)
	if err != nil {
		log.Fatal(err)
	}

	if *bootstrap {
		fmt.Println("Bootstrapping cluster...")
		r.BootstrapCluster(raft.Configuration{
			Servers: []raft.Server{{ID: config.LocalID, Address: transport.LocalAddr()}},
		})
	}

	startManagementServer(r)

	if *joinAddr != "" {
		// IMPORTANT: Send the advertised address (e.g., node2:8088), not 0.0.0.0
		go tryJoinCluster(*joinAddr, *nodeID, advertiseString)
	}

	lis, err := net.Listen("tcp", ":"+*sidecarPort)
	if err != nil {
		log.Fatal(err)
	}

	grpcServer := grpc.NewServer()
	pb.RegisterRaftNodeServer(grpcServer, &RaftRPC{raft: r})

	fmt.Printf("Go Sidecar %s running (Bind: %s, Adv: %s). Mgmt: %s\n", *nodeID, bindAddr, advertiseString, *mgmtPort)
	if err := grpcServer.Serve(lis); err != nil {
		log.Fatal(err)
	}
}
