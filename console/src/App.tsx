import { useCallback, useState } from 'preact/hooks';

import { AuthBanner } from './components/AuthBanner';
import { LoginForm } from './components/LoginForm';
import { useHashRoute } from './hooks/useHashRoute';
import { clearCredential, loadCredential } from './lib/auth';
import type { ClusterStatus } from './lib/api';
import { ClusterPage } from './pages/ClusterPage';
import { KeysPage } from './pages/KeysPage';
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

  const signOut = () => {
    clearCredential();
    setStatus(undefined);
    setAuthReason(undefined);
    setSession((n) => n + 1);
  };

  // A 401 always means "authenticate", whether or not a credential was stored.
  if (authReason !== undefined) {
    return (
      <div class="app-shell">
        <LoginForm
          reason={authReason}
          onAuthenticated={() => {
            setAuthReason(undefined);
            setSession((n) => n + 1);
          }}
        />
      </div>
    );
  }

  const authEnabled = status?.auth_enabled === true;
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
        {authEnabled && signedIn && (
          <button class="button" type="button" onClick={signOut}>
            Sign out
          </button>
        )}
      </header>

      {status !== undefined && !status.auth_enabled && <AuthBanner />}

      <main key={session}>
        {route === 'cluster' && (
          <ClusterPage onUnauthorized={onUnauthorized} onStatus={onStatus} />
        )}
        {route === 'keys' && <KeysPage onUnauthorized={onUnauthorized} />}
        {route === 'users' && (
          <p class="page-note">User management arrives in the next task.</p>
        )}
      </main>
    </div>
  );
}
