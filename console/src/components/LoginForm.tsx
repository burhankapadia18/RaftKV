import { useState } from 'preact/hooks';

import { storeCredential } from '../lib/auth';
import { Motif } from './Motif';

interface Props {
  /** Message from the rejected request, if any. */
  reason?: string;
  onAuthenticated: () => void;
}

export function LoginForm({ reason, onAuthenticated }: Props) {
  const [user, setUser] = useState('');
  const [password, setPassword] = useState('');

  const onSubmit = (event: Event) => {
    event.preventDefault();
    storeCredential(user, password);
    onAuthenticated();
  };

  return (
    <form class="login" onSubmit={onSubmit}>
      <span class="login-motif">
        <Motif size={40} />
      </span>
      <h2 class="login-title">Sign in</h2>
      <p class="login-note">
        This node has client ACLs enabled. Credentials are sent to this node
        only and are not stored beyond the browser session.
      </p>
      {reason !== undefined && (
        <p class="error-note" role="alert">
          {reason}
        </p>
      )}
      <label class="field">
        <span class="field-label">User</span>
        <input
          class="field-input"
          value={user}
          autocomplete="username"
          required
          onInput={(event) => setUser((event.target as HTMLInputElement).value)}
        />
      </label>
      <label class="field">
        <span class="field-label">Password</span>
        <input
          class="field-input"
          type="password"
          value={password}
          autocomplete="current-password"
          required
          onInput={(event) =>
            setPassword((event.target as HTMLInputElement).value)
          }
        />
      </label>
      <button class="button is-primary" type="submit">
        Sign in <span class="arrow" aria-hidden="true">→</span>
      </button>
    </form>
  );
}
