package management

import (
	"crypto/subtle"
	"net/http"
	"strings"
)

// AuthHeader is the header carrying the cluster-admin bearer token.
const AuthHeader = "Authorization"

// bearerPrefix is the scheme prefix expected in AuthHeader.
const bearerPrefix = "Bearer "

// extractBearer pulls the token out of an Authorization header.
//
// Returns "" when the header is absent or is not a Bearer credential. The scheme
// match is case-insensitive because RFC 7235 says it is, and a client that sends
// "bearer" is not the attacker we are defending against.
func extractBearer(r *http.Request) string {
	value := r.Header.Get(AuthHeader)
	if len(value) < len(bearerPrefix) {
		return ""
	}
	if !strings.EqualFold(value[:len(bearerPrefix)], bearerPrefix) {
		return ""
	}
	return strings.TrimSpace(value[len(bearerPrefix):])
}

// authorize checks the bearer token on a mutating management request (R6.1).
//
// Returns true when it has already written a rejection, so the caller must return
// immediately.
//
// The default is CLOSED. With no token configured the mutating endpoints answer
// 403 and refuse to act, rather than staying open — this is the endpoint through
// which anyone who could reach port 6000 was previously able to add a voter to the
// cluster, and "secure only if you remembered to configure it" is how that hole
// stays open in practice. A cluster that is misconfigured cannot grow; a cluster
// that is silently unauthenticated can be taken over.
func (s *Server) authorize(w http.ResponseWriter, r *http.Request) bool {
	if s.authToken == "" {
		http.Error(w,
			"cluster-admin token is not configured on this node, so mutating "+
				"management endpoints are disabled; set -mgmt-token or "+
				"RAFTKV_MGMT_TOKEN",
			http.StatusForbidden)
		return true
	}

	presented := extractBearer(r)
	if presented == "" {
		// 401, not 403: the client did not present a credential at all, and the
		// distinction is what tells an operator "you forgot the header" apart from
		// "your token is wrong".
		w.Header().Set("WWW-Authenticate", `Bearer realm="raftkv-management"`)
		http.Error(w, "missing bearer token", http.StatusUnauthorized)
		return true
	}

	// Constant-time comparison. A plain == would leak the token prefix through
	// timing, one byte at a time, to anyone who can measure the response — and
	// this token is the whole of cluster-membership authority.
	if subtle.ConstantTimeCompare([]byte(presented), []byte(s.authToken)) != 1 {
		http.Error(w, "invalid bearer token", http.StatusForbidden)
		return true
	}

	return false
}
