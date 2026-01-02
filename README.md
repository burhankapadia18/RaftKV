# RaftKV
RaftKV is a lightweight distributed key–value store with persistent storage, built around the Raft consensus algorithm. The storage engine is implemented in C++, while cluster coordination and replication are handled by a Go-based sidecar using HashiCorp Raft, allowing strong consistency with minimal coupling between components.
