import { authHeader } from './auth';
import { encodeMsgPack } from './msgpack';

export interface Peer {
  id: string;
  address: string;
  suffrage: string;
}

export interface ClusterStatus {
  node_id: string;
  state: string;
  term: number;
  leader_id: string;
  leader_addr: string;
  peers: Peer[];
  first_log_index: number;
  last_log_index: number;
  applied_index: number;
  commit_index: number;
  last_snapshot_index: number;
  key_count: number;
  wal_bytes: number;
  auth_enabled: boolean;
  /** A partial failure inside an otherwise usable response. */
  error?: string;
}

export interface KeyPage {
  /** Percent-encoded, because a key is arbitrary bytes. Decode to display. */
  keys: string[];
  next_cursor?: string;
}

/**
 * A stored user, as `GET /auth/users/{name}` returns it.
 *
 * There is no password field, and there is no salt or hash either: the engine
 * deliberately never serializes them, because an endpoint that returned them
 * would turn one compromised admin credential into an offline attack on every
 * user's password.
 */
export interface UserRecord {
  name: string;
  enabled: boolean;
  classes: string[];
  patterns: string[];
}

/**
 * The body of `PUT /auth/users/{name}`.
 *
 * `password` is REQUIRED and must be at least 8 bytes -- the engine's
 * `UserUpsertRequest::validation_error` enforces it, because the hash is not
 * memory-hard. There is no partial update: an upsert always mints a fresh salt
 * and hash, so editing a user means setting its password again.
 */
export interface UserUpsert {
  password: string;
  classes: string[];
  patterns: string[];
  enabled: boolean;
}

export interface Identity {
  name: string;
  classes: string[];
  patterns: string[];
}

/**
 * A non-2xx answer, carrying the status so callers can act on the engine's
 * documented retry contract: 503 means retry, 502 means it will fail again,
 * 401 means the credential is missing, 403 means it was refused.
 */
export class ApiFailure extends Error {
  readonly status: number;

  constructor(status: number, message: string) {
    super(message);
    this.name = 'ApiFailure';
    this.status = status;
  }
}

/**
 * The engine's message for a credential that was presented and rejected.
 *
 * Mirrors `ERROR_INVALID_CREDENTIALS` in tests/e2e/contracts.py.
 */
const INVALID_CREDENTIALS = 'invalid credentials';

/**
 * True when `error` means "authenticate again", as opposed to "you are who you
 * say you are, and you may not do that".
 *
 * The engine splits these deliberately and NOT along the 401/403 line:
 *
 *   - 401 -- no usable credential was presented (with a Basic challenge).
 *   - 403 "invalid credentials" -- something WAS presented and rejected: an
 *     unknown user, a disabled user, or a wrong password, kept
 *     indistinguishable on purpose so the endpoint is not a user-enumeration
 *     oracle.
 *   - 403 anything else -- a valid identity that lacks the class or the key
 *     pattern. Signing such a user out would be wrong: their credential is
 *     fine, they just cannot do this.
 *
 * Treating only 401 as re-authenticate strands anyone who mistypes a password:
 * the login form is dismissed, every later poll answers 403, and nothing ever
 * invites them to try again.
 */
export function isAuthenticationFailure(error: unknown): error is ApiFailure {
  if (!(error instanceof ApiFailure)) return false;
  if (error.status === 401) return true;
  return error.status === 403 && error.message === INVALID_CREDENTIALS;
}

/** The engine's error envelope is always `{"error": "..."}`. */
async function failureFrom(response: Response): Promise<ApiFailure> {
  const text = await response.text();
  try {
    const parsed = JSON.parse(text) as { error?: string };
    if (typeof parsed.error === 'string') {
      return new ApiFailure(response.status, parsed.error);
    }
  } catch {
    // A body that is not JSON is still worth surfacing verbatim.
  }
  return new ApiFailure(response.status, text || `HTTP ${response.status}`);
}

async function request(path: string, init: RequestInit = {}): Promise<Response> {
  const response = await fetch(path, {
    ...init,
    headers: { ...authHeader(), ...(init.headers ?? {}) },
    // Same-origin only. The console is served by the node it talks to, so
    // there is no cross-origin case to allow.
    credentials: 'omit',
    cache: 'no-store',
  });
  if (!response.ok) {
    throw await failureFrom(response);
  }
  return response;
}

async function requestJson<T>(path: string, init?: RequestInit): Promise<T> {
  const response = await request(path, init);
  return (await response.json()) as T;
}

export function getClusterStatus(): Promise<ClusterStatus> {
  return requestJson<ClusterStatus>('/cluster/status');
}

export function listKeys(opts: {
  prefix: string;
  cursor?: string;
  limit?: number;
}): Promise<KeyPage> {
  // Built by hand rather than with URLSearchParams because the cursor arrives
  // ALREADY percent-encoded and must go back out unchanged; URLSearchParams
  // would encode its '%' again and address a different position.
  const parts = [`limit=${opts.limit ?? 100}`];
  if (opts.prefix !== '') {
    parts.push(`prefix=${encodeURIComponent(opts.prefix)}`);
  }
  if (opts.cursor !== undefined && opts.cursor !== '') {
    parts.push(`cursor=${opts.cursor}`);
  }
  return requestJson<KeyPage>(`/kv?${parts.join('&')}`);
}

/**
 * @param key Raw (decoded) key.
 * @returns The value, or null when the key is absent (404 is not an error here).
 */
export async function getValue(
  key: string,
  linearizable: boolean,
): Promise<string | null> {
  const suffix = linearizable ? '?consistency=linearizable' : '';
  try {
    const response = await request(`/kv/${encodeURIComponent(key)}${suffix}`);
    return await response.text();
  } catch (error) {
    if (error instanceof ApiFailure && error.status === 404) {
      return null;
    }
    throw error;
  }
}

export async function putValue(key: string, value: string): Promise<void> {
  await request(`/kv/${encodeURIComponent(key)}`, {
    method: 'PUT',
    body: value,
  });
}

export async function deleteValue(key: string): Promise<void> {
  await request(`/kv/${encodeURIComponent(key)}`, { method: 'DELETE' });
}

export async function listUsers(): Promise<string[]> {
  const body = await requestJson<{ users: string[] }>('/auth/users');
  return body.users;
}

export function getUser(name: string): Promise<UserRecord> {
  return requestJson<UserRecord>(`/auth/users/${encodeURIComponent(name)}`);
}

/**
 * Create or replace a user.
 *
 * The body is MSGPACK, not JSON: `handle_user_put` answers 415 to anything
 * whose Content-Type is not application/msgpack, and decodes the body as a
 * msgpack map of {password, enabled, classes, patterns}. That is the same wire
 * convention the KV write path uses.
 */
export async function putUser(name: string, body: UserUpsert): Promise<void> {
  const encoded = encodeMsgPack({
    password: body.password,
    enabled: body.enabled,
    classes: body.classes,
    patterns: body.patterns,
  });
  await request(`/auth/users/${encodeURIComponent(name)}`, {
    method: 'PUT',
    headers: { 'Content-Type': 'application/msgpack' },
    body: encoded,
  });
}

export async function deleteUser(name: string): Promise<void> {
  await request(`/auth/users/${encodeURIComponent(name)}`, {
    method: 'DELETE',
  });
}

export function whoami(): Promise<Identity> {
  return requestJson<Identity>('/auth/whoami');
}

/**
 * Decode a percent-encoded key from `listKeys` for display.
 *
 * Keys are arbitrary bytes, so a key can hold a sequence that is not valid
 * UTF-8 and decodeURIComponent then throws. The escaped form is shown instead
 * of losing the row.
 */
export function decodeKey(encoded: string): string {
  try {
    return decodeURIComponent(encoded);
  } catch {
    return encoded;
  }
}
