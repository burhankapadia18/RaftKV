// Credential handling for the console.
//
// The credential is a Basic pair the operator just typed, held in
// sessionStorage. Stated plainly because it matters: this is WEAKER than an
// httpOnly cookie session -- a cross-site scripting bug in this app could read
// it. The mitigations are the CSP the engine sends with index.html and the
// no-innerHTML rule this app follows throughout. A real session-token endpoint
// would need a new committed record type and a token store, which is a larger
// change than the whole console; it is the documented upgrade path, not an
// oversight.
//
// sessionStorage rather than localStorage: it dies with the tab, so a shared
// machine does not keep an admin credential around after the operator leaves.

const STORAGE_KEY = 'raftkv.console.credential';

/** The stored `user:password` base64, or null. */
export function loadCredential(): string | null {
  try {
    return sessionStorage.getItem(STORAGE_KEY);
  } catch {
    // Private-browsing modes and hardened settings THROW on access rather than
    // returning null. The console must still render (and simply ask again).
    return null;
  }
}

export function storeCredential(user: string, password: string): void {
  // btoa operates on latin1, so a non-ASCII password has to be encoded to
  // bytes first -- otherwise btoa throws and the login silently fails.
  const bytes = new TextEncoder().encode(`${user}:${password}`);
  let latin1 = '';
  for (const byte of bytes) latin1 += String.fromCharCode(byte);
  try {
    sessionStorage.setItem(STORAGE_KEY, btoa(latin1));
  } catch {
    // Nothing to do but continue unauthenticated; the next call 401s and the
    // login form reappears.
  }
}

export function clearCredential(): void {
  try {
    sessionStorage.removeItem(STORAGE_KEY);
  } catch {
    /* see loadCredential */
  }
}

export function authHeader(): Record<string, string> {
  const credential = loadCredential();
  return credential === null ? {} : { Authorization: `Basic ${credential}` };
}
