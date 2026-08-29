import { useState } from 'preact/hooks';

import type { UserRecord, UserUpsert } from '../lib/api';

const CLASSES = ['read', 'write', 'admin'] as const;

/** The engine's `UserUpsertRequest::validation_error` rejects anything shorter. */
const MIN_PASSWORD_BYTES = 8;

interface Props {
  /** The record being edited, or undefined when creating. */
  existing?: UserRecord;
  onSubmit: (name: string, body: UserUpsert) => void;
  onCancel: () => void;
}

export function UserForm({ existing, onSubmit, onCancel }: Props) {
  const [name, setName] = useState(existing?.name ?? '');
  const [password, setPassword] = useState('');
  const [classes, setClasses] = useState<string[]>(
    existing?.classes ?? ['read'],
  );
  const [patterns, setPatterns] = useState(
    (existing?.patterns ?? ['*']).join('\n'),
  );
  const [enabled, setEnabled] = useState(existing?.enabled ?? true);

  const toggleClass = (cls: string): void => {
    setClasses((held) =>
      held.includes(cls) ? held.filter((c) => c !== cls) : [...held, cls],
    );
  };

  const submit = (event: Event): void => {
    event.preventDefault();
    onSubmit(name, {
      password,
      classes,
      // An empty pattern list DENIES every key; blank lines must not silently
      // become one, and trailing whitespace in a glob is a pattern that never
      // matches.
      patterns: patterns
        .split('\n')
        .map((line) => line.trim())
        .filter((line) => line !== ''),
      enabled,
    });
  };

  return (
    <form class="editor-card user-form" onSubmit={submit}>
      <h3 class="section-title">
        {existing === undefined ? 'Create user' : `Edit ${existing.name}`}
      </h3>

      <label class="field">
        <span class="field-label">Name</span>
        <input
          class="field-input"
          value={name}
          required
          readOnly={existing !== undefined}
          pattern="[A-Za-z0-9_.\-]+"
          onInput={(event) => setName((event.target as HTMLInputElement).value)}
        />
        <span class="field-hint">Letters, digits, '_', '.' and '-' only.</span>
      </label>

      <label class="field">
        <span class="field-label">Password</span>
        <input
          class="field-input"
          type="password"
          value={password}
          autocomplete="new-password"
          required
          minLength={MIN_PASSWORD_BYTES}
          onInput={(event) =>
            setPassword((event.target as HTMLInputElement).value)
          }
        />
        {/* Required even on an edit, and that is the API, not a UI choice: an
            upsert mints a fresh salt and hash every time, so there is no
            "leave the password alone" request to send. */}
        <span class="field-hint">
          At least {MIN_PASSWORD_BYTES} characters.
          {existing !== undefined &&
            ' Saving replaces the existing password — there is no partial update.'}
        </span>
      </label>

      <fieldset class="field">
        <legend class="field-label">Command classes</legend>
        <div class="chip-row">
          {CLASSES.map((cls) => (
            <button
              key={cls}
              class="button"
              type="button"
              aria-pressed={classes.includes(cls)}
              onClick={() => toggleClass(cls)}
            >
              {cls}
            </button>
          ))}
        </div>
      </fieldset>

      <label class="field">
        <span class="field-label">Key patterns, one per line</span>
        <textarea
          class="field-textarea"
          value={patterns}
          onInput={(event) =>
            setPatterns((event.target as HTMLTextAreaElement).value)
          }
        />
        <span class="field-hint">
          Globs with '*' and '?' and no escapes, so a key containing '*' cannot
          be named exactly. An empty list denies every key.
        </span>
      </label>

      <label class="field checkbox-field">
        <input
          type="checkbox"
          checked={enabled}
          onChange={(event) =>
            setEnabled((event.target as HTMLInputElement).checked)
          }
        />
        <span>Enabled</span>
      </label>

      <div class="page-actions is-left">
        <button class="button is-primary" type="submit">
          {existing === undefined ? 'Create' : 'Save'}
        </button>
        <button class="button" type="button" onClick={onCancel}>
          Cancel
        </button>
      </div>
    </form>
  );
}
