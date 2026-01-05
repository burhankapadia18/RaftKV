// Package config provides configuration management for the Raft sidecar.
package config

import (
	"flag"
	"fmt"
)

// Config holds all configuration values for the sidecar application.
type Config struct {
	NodeID        string
	RaftPort      string
	SidecarPort   string
	AppAddr       string
	MgmtPort      string
	Bootstrap     bool
	DataDir       string
	JoinAddr      string
	RaftAdvertise string
}

// flags holds the command-line flag pointers
var flags struct {
	nodeID        *string
	raftPort      *string
	sidecarPort   *string
	appAddr       *string
	mgmtPort      *string
	bootstrap     *bool
	dataDir       *string
	joinAddr      *string
	raftAdvertise *string
}

func init() {
	flags.nodeID = flag.String("id", "node1", "Unique Node ID")
	flags.raftPort = flag.String("raft", "8088", "Raft TCP Port")
	flags.sidecarPort = flag.String("srv", "50052", "Sidecar gRPC Port")
	flags.appAddr = flag.String("app", "localhost:50051", "Address of C++ App gRPC")
	flags.mgmtPort = flag.String("mgmt", "6000", "Management HTTP Port")
	flags.bootstrap = flag.Bool("bootstrap", false, "Bootstrap the cluster (Leader only)")
	flags.dataDir = flag.String("data", "raft-data", "Directory to store Raft logs")
	flags.joinAddr = flag.String("join", "", "Address of Leader's Management API to join")
	flags.raftAdvertise = flag.String("advertise", "", "Address to advertise to other nodes")
}

// Parse parses command-line flags and returns a Config.
func Parse() *Config {
	flag.Parse()
	return &Config{
		NodeID:        *flags.nodeID,
		RaftPort:      *flags.raftPort,
		SidecarPort:   *flags.sidecarPort,
		AppAddr:       *flags.appAddr,
		MgmtPort:      *flags.mgmtPort,
		Bootstrap:     *flags.bootstrap,
		DataDir:       *flags.dataDir,
		JoinAddr:      *flags.joinAddr,
		RaftAdvertise: *flags.raftAdvertise,
	}
}

// BindAddr returns the address to bind the Raft transport to.
func (c *Config) BindAddr() string {
	return "0.0.0.0:" + c.RaftPort
}

// AdvertiseAddr returns the address to advertise to other nodes.
func (c *Config) AdvertiseAddr() string {
	if c.RaftAdvertise != "" {
		return c.RaftAdvertise + ":" + c.RaftPort
	}
	return c.BindAddr()
}

// String returns a human-readable representation of the config.
func (c *Config) String() string {
	return fmt.Sprintf(
		"Config{NodeID: %s, RaftPort: %s, SidecarPort: %s, AppAddr: %s, MgmtPort: %s, Bootstrap: %v, DataDir: %s}",
		c.NodeID, c.RaftPort, c.SidecarPort, c.AppAddr, c.MgmtPort, c.Bootstrap, c.DataDir,
	)
}
