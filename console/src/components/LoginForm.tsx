import { useState } from 'preact/hooks';

import { storeCredential } from '../lib/auth';

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
      <h2 class="login-title">Sign in</h2>
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
        Sign in
      </button>
    </form>
  );
}
