import { useCallback, useEffect, useState } from 'preact/hooks';

import { AuthBanner } from './components/AuthBanner';
import { LoginForm } from './components/LoginForm';
import { useHashRoute } from './hooks/useHashRoute';
import { clearCredential, loadCredential } from './lib/auth';
import {
  getClusterStatus,
  isAuthenticationFailure,
  type ClusterStatus,
} from './lib/api';
import { ClusterPage } from './pages/ClusterPage';
import { KeysPage } from './pages/KeysPage';
import { UsersPage } from './pages/UsersPage';
import './styles/cluster.css';

const ROUTES = [
  { id: 'cluster', label: 'Cluster' },
  { id: 'keys', label: 'Keys' },
  { id: 'users', label: 'Users' },
] as const;

export function App() {
  const [route, setRoute] = useHashRoute();
  const [status, setStatus] = useState<ClusterStatus | undefined>(undefined);
  const [authReason, setAuthReason] = useState<string | undefined>(undefined);
  // Set once the first status call has COMPLETED, success or failure. Distinct
  // from `status` being set: a 502 (sidecar unreachable) leaves the status
  // unknown indefinitely, and the console must still render rather than hang.
  const [bootstrapped, setBootstrapped] = useState(false);
  // Bumped to force a remount after signing in or out, so every page refetches
  // with the new credential instead of showing the previous identity's data.
  const [session, setSession] = useState(0);

  const onUnauthorized = useCallback((reason: string) => {
    clearCredential();
    setAuthReason(reason);
  }, []);

  const onStatus = useCallback((next: ClusterStatus) => {
    setStatus(next);
    setAuthReason(undefined);
  }, []);

  // The shell asks for the cluster status ITSELF, on every route.
  //
  // `auth_enabled` decides whether user management is usable and whether the
  // "unauthenticated" banner belongs on screen -- app-wide facts. Letting only
  // ClusterPage fetch them meant that opening #/users directly (a bookmark, a
  // reload, a shared link) left them at their defaults, and the Users page then
  // told an authenticated administrator that authentication was off. State the
  // whole app depends on must not be sourced from one page.
  useEffect(() => {
    let cancelled = false;
    getClusterStatus()
      .then((next) => {
        if (cancelled) return;
        setStatus(next);
        setAuthReason(undefined);
      })
      .catch((failure: unknown) => {
        if (cancelled) return;
        if (isAuthenticationFailure(failure)) {
          clearCredential();
          setAuthReason(failure.message);
        }
        // Any other failure (a 502 from an unreachable sidecar, a dropped
        // connection) is left to the pages to report in context; the shell only
        // needs to stop blocking.
      })
      .finally(() => {
        if (!cancelled) setBootstrapped(true);
      });
    return () => {
      cancelled = true;
    };
  }, [session]);

  const signOut = () => {
    clearCredential();
    setStatus(undefined);
    setBootstrapped(false);
    setAuthReason(undefined);
    setSession((n) => n + 1);
  };

  // A rejected credential always means "authenticate", whether the engine
  // called it 401 (nothing presented) or 403 (presented and refused).
  if (authReason !== undefined) {
    return (
      <div class="app-shell">
        <LoginForm
          reason={authReason}
          onAuthenticated={() => {
            setAuthReason(undefined);
            setBootstrapped(false);
            setSession((n) => n + 1);
          }}
        />
      </div>
    );
  }

  // Undefined, not false, while the answer is unknown: only the engine can say
  // authentication is off, and assuming "off" is what produced the wrong banner.
  const authEnabled = status?.auth_enabled;
  const signedIn = loadCredential() !== null;

  return (
    <div class="app-shell">
      <header class="app-header">
        <h1 class="app-title">RaftKV</h1>
        <nav class="app-nav" aria-label="Console sections">
          {ROUTES.map((entry) => (
            <a
              key={entry.id}
              href={`#/${entry.id}`}
              aria-current={route === entry.id ? 'page' : undefined}
              onClick={() => setRoute(entry.id)}
            >
              {entry.label}
            </a>
          ))}
        </nav>
        {authEnabled === true && signedIn && (
          <button class="button" type="button" onClick={signOut}>
            Sign out
          </button>
        )}
      </header>

      {authEnabled === false && <AuthBanner />}

      <main key={session}>
        {!bootstrapped ? (
          <p class="page-note">Loading…</p>
        ) : (
          <>
            {route === 'cluster' && (
              <ClusterPage onUnauthorized={onUnauthorized} onStatus={onStatus} />
            )}
            {route === 'keys' && <KeysPage onUnauthorized={onUnauthorized} />}
            {route === 'users' && (
              <UsersPage
                authEnabled={authEnabled}
                onUnauthorized={onUnauthorized}
              />
            )}
          </>
        )}
      </main>
    </div>
  );
}
