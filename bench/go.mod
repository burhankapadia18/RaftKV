// A standalone module, not part of go-sidecar.
//
// kvbench is a client: it speaks HTTP to a running cluster and shares no code
// with the sidecar. Folding it into go-sidecar would put a benchmark harness's
// dependencies into the binary that ships in the image, and would mean `go test
// ./...` in the sidecar tree compiled a tool that has nothing to do with it.
//
// It deliberately has no third-party dependencies at all — net/http and the
// standard library are enough — so `go build ./...` here needs no network.
module raftkv-bench

go 1.24
