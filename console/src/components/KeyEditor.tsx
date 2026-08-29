import { useEffect, useState } from 'preact/hooks';

import {
  ApiFailure,
  deleteValue,
  getValue,
  isAuthenticationFailure,
  putValue,
} from '../lib/api';

interface Props {
  /** Raw key. Empty string means "the single-key console with nothing loaded". */
  keyName: string;
  onKeyNameChange: (next: string) => void;
  onMutated: () => void;
  onUnauthorized: (reason: string) => void;
}

type Status =
  | { kind: 'idle' }
  | { kind: 'busy' }
  | { kind: 'note'; text: string }
  | { kind: 'error'; text: string };

/**
 * Get / put / delete one key, with the consistency toggle.
 *
 * The engine's retry contract is surfaced rather than hidden: a 503 says retry
 * (and does NOT mean the write did not happen -- writes are at-least-once under
 * failure), a 502 says it will fail again. Papering over that difference is the
 * mistake the chaos harness made.
 */
export function KeyEditor({
  keyName,
  onKeyNameChange,
  onMutated,
  onUnauthorized,
}: Props) {
  const [value, setValue] = useState('');
  const [linearizable, setLinearizable] = useState(false);
  const [status, setStatus] = useState<Status>({ kind: 'idle' });

  const handle = (failure: unknown): void => {
    if (isAuthenticationFailure(failure)) {
      onUnauthorized(failure.message);
      return;
    }
    if (failure instanceof ApiFailure) {
      const suffix =
        failure.status === 503
          ? ' — retry; this does not mean the write was lost'
          : failure.status === 502
            ? ' — retrying the same request will fail the same way'
            : '';
      setStatus({
        kind: 'error',
        text: `${failure.status}: ${failure.message}${suffix}`,
      });
      return;
    }
    setStatus({
      kind: 'error',
      text: failure instanceof Error ? failure.message : 'Request failed.',
    });
  };

  const load = (): void => {
    if (keyName === '') return;
    setStatus({ kind: 'busy' });
    getValue(keyName, linearizable)
      .then((loaded) => {
        if (loaded === null) {
          setValue('');
          setStatus({ kind: 'note', text: 'Key not found.' });
          return;
        }
        setValue(loaded);
        setStatus({ kind: 'idle' });
      })
      .catch(handle);
  };

  // Reload whenever the selected key changes, so clicking a row in the table
  // shows that row's value rather than the previous one's. Deliberately keyed
  // on keyName alone: adding `load` would re-fetch on every keystroke in the
  // key field, and adding `linearizable` would re-fetch on a toggle the
  // operator has not yet acted on.
  useEffect(() => {
    if (keyName !== '') load();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [keyName]);

  const save = (): void => {
    setStatus({ kind: 'busy' });
    putValue(keyName, value)
      .then(() => {
        setStatus({ kind: 'note', text: 'Written.' });
        onMutated();
      })
      .catch(handle);
  };

  const remove = (): void => {
    setStatus({ kind: 'busy' });
    deleteValue(keyName)
      .then(() => {
        setValue('');
        // DELETE is idempotent and 200 describes the resulting state, not
        // whether anything changed -- so this deliberately does not claim the
        // key existed.
        setStatus({ kind: 'note', text: 'Deleted (or already absent).' });
        onMutated();
      })
      .catch(handle);
  };

  const busy = status.kind === 'busy';

  return (
    <section class="editor-card" aria-labelledby="key-editor-heading">
      <h3 class="section-title" id="key-editor-heading">
        Key console
      </h3>

      <label class="field">
        <span class="field-label">Key</span>
        <input
          class="field-input"
          value={keyName}
          placeholder="exact key"
          onInput={(event) =>
            onKeyNameChange((event.target as HTMLInputElement).value)
          }
        />
      </label>

      <label class="field">
        <span class="field-label">Value</span>
        <textarea
          class="field-textarea"
          value={value}
          onInput={(event) =>
            setValue((event.target as HTMLTextAreaElement).value)
          }
        />
      </label>

      <div class="page-actions is-left">
        <button
          class="button"
          type="button"
          aria-pressed={linearizable}
          onClick={() => setLinearizable((was) => !was)}
        >
          {linearizable ? 'Linearizable read' : 'Local read'}
        </button>
        <button
          class="button"
          type="button"
          disabled={busy || keyName === ''}
          onClick={load}
        >
          Load
        </button>
        <button
          class="button is-primary"
          type="button"
          disabled={busy || keyName === ''}
          onClick={save}
        >
          Write <span class="arrow" aria-hidden="true">→</span>
        </button>
        <button
          class="button is-danger"
          type="button"
          disabled={busy || keyName === ''}
          onClick={remove}
        >
          Delete
        </button>
      </div>

      <p class="page-note">
        A local read is served from this node and may be stale. A linearizable
        read forwards to the leader and costs roughly 7× as much.
      </p>

      {status.kind === 'note' && (
        <p class="banner" role="status">
          {status.text}
        </p>
      )}
      {status.kind === 'error' && (
        <p class="error-note" role="alert">
          {status.text}
        </p>
      )}
    </section>
  );
}
