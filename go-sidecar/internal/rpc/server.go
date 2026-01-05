// Package rpc provides the gRPC server for the Raft sidecar.
package rpc

import (
	"context"
	"fmt"
	"log"
	"net"
	"time"

	"google.golang.org/grpc"

	"my-raft-sidecar/internal/raftnode"
	pb "my-raft-sidecar/pb"
)

// Server represents the gRPC server for Raft operations.
type Server struct {
	pb.UnimplementedRaftNodeServer
	node       *raftnode.Node
	grpcServer *grpc.Server
	listener   net.Listener
}

// NewServer creates a new gRPC server for the Raft node.
func NewServer(node *raftnode.Node) *Server {
	return &Server{
		node:       node,
		grpcServer: grpc.NewServer(),
	}
}

// Propose handles client proposals to the Raft cluster.
func (s *Server) Propose(ctx context.Context, cmd *pb.Command) (*pb.ProposeResponse, error) {
	if err := s.node.Apply(cmd.Data, 5*time.Second); err != nil {
		return &pb.ProposeResponse{
			Success: false,
			Error:   err.Error(),
		}, nil
	}
	return &pb.ProposeResponse{Success: true}, nil
}

// Start starts the gRPC server on the specified port.
func (s *Server) Start(port string) error {
	addr := ":" + port
	lis, err := net.Listen("tcp", addr)
	if err != nil {
		return fmt.Errorf("failed to listen on %s: %w", addr, err)
	}
	s.listener = lis

	pb.RegisterRaftNodeServer(s.grpcServer, s)

	log.Printf("gRPC server listening on %s", addr)
	return s.grpcServer.Serve(lis)
}

// Stop gracefully stops the gRPC server.
func (s *Server) Stop() {
	if s.grpcServer != nil {
		s.grpcServer.GracefulStop()
	}
}
