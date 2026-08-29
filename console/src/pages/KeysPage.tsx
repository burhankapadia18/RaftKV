import { useCallback, useEffect, useState } from 'preact/hooks';

import { isAuthenticationFailure, listKeys } from '../lib/api';
import { KeyEditor } from '../components/KeyEditor';
import { KeyTable } from '../components/KeyTable';

const PAGE_SIZE = 100;

/**
 * How long the prefix field sits idle before a scan is issued.
 *
 * Not cosmetic. Scanning on every keystroke would put one prefix scan per
 * character typed onto a database whose console is supposed to cost it nothing
 * while idle -- typing "app:config:" would be eleven scans to answer one
 * question.
 */
const PREFIX_DEBOUNCE_MS = 250;

interface Props {
  onUnauthorized: (reason: string) => void;
}

export function KeysPage({ onUnauthorized }: Props) {
  const [prefix, setPrefix] = useState('');
  const [committedPrefix, setCommittedPrefix] = useState('');
  const [encodedKeys, setEncodedKeys] = useState<string[]>([]);
  // A stack, so "Back" is exact rather than a re-scan from the beginning: the
  // API's cursor is forward-only. The first page's cursor is `undefined`, which
  // is why the stack starts empty rather than holding a sentinel.
  const [cursors, setCursors] = useState<(string | undefined)[]>([undefined]);
  const [nextCursor, setNextCursor] = useState<string | undefined>(undefined);
  const [selected, setSelected] = useState('');
  const [error, setError] = useState<string | undefined>(undefined);

  useEffect(() => {
    const timer = window.setTimeout(
      () => setCommittedPrefix(prefix),
      PREFIX_DEBOUNCE_MS,
    );
    return () => window.clearTimeout(timer);
  }, [prefix]);

  const fetchPage = useCallback(
    (cursor: string | undefined) => {
      listKeys({ prefix: committedPrefix, cursor, limit: PAGE_SIZE })
        .then((page) => {
          setEncodedKeys(page.keys);
          setNextCursor(page.next_cursor);
          setError(undefined);
        })
        .catch((failure: unknown) => {
          if (isAuthenticationFailure(failure)) {
            onUnauthorized(failure.message);
            return;
          }
          setEncodedKeys([]);
          setNextCursor(undefined);
          setError(
            failure instanceof Error ? failure.message : 'Request failed.',
          );
        });
    },
    [committedPrefix, onUnauthorized],
  );

  // No polling here: a key listing changes when the operator changes it, and a
  // background scan of the store is exactly the cost this console must not add.
  useEffect(() => {
    setCursors([undefined]);
    fetchPage(undefined);
  }, [fetchPage]);

  const goForward = (): void => {
    if (nextCursor === undefined) return;
    setCursors((stack) => [...stack, nextCursor]);
    fetchPage(nextCursor);
  };

  const goBack = (): void => {
    if (cursors.length < 2) return;
    // Computed outside the state updater on purpose: an updater may be invoked
    // more than once for one logical update, so issuing the request from inside
    // it can fire the same scan twice.
    const shortened = cursors.slice(0, -1);
    setCursors(shortened);
    fetchPage(shortened[shortened.length - 1]);
  };

  const reloadCurrent = (): void => fetchPage(cursors[cursors.length - 1]);

  return (
    <section class="page" aria-labelledby="keys-heading">
      <div class="page-head">
        <h2 class="page-title" id="keys-heading">
          Keys
        </h2>
      </div>

      <label class="field">
        <span class="field-label">Prefix</span>
        <input
          class="field-input"
          value={prefix}
          placeholder="all keys"
          onInput={(event) =>
            setPrefix((event.target as HTMLInputElement).value)
          }
        />
      </label>

      <p class="page-note">
        Values are fetched only when a key is opened — a page of {PAGE_SIZE}{' '}
        values could be very large.
      </p>

      {error !== undefined && (
        <p class="error-note" role="alert">
          {error}
        </p>
      )}

      {/* Suppressed on failure: "No keys under this prefix" next to a 403 is a
          claim about the store that the request never established. */}
      {error === undefined && (
        <KeyTable
          encodedKeys={encodedKeys}
          selected={selected}
          onSelect={setSelected}
        />
      )}

      <div class="page-actions is-left">
        <button
          class="button"
          type="button"
          disabled={cursors.length < 2}
          onClick={goBack}
        >
          Back
        </button>
        <button
          class="button"
          type="button"
          /* Paging stops on an ABSENT cursor, never on a short page: filtering
             can shorten a page while more keys remain. */
          disabled={nextCursor === undefined}
          onClick={goForward}
        >
          Next
        </button>
        <button class="button" type="button" onClick={reloadCurrent}>
          Refresh
        </button>
      </div>

      <KeyEditor
        keyName={selected}
        onKeyNameChange={setSelected}
        onMutated={reloadCurrent}
        onUnauthorized={onUnauthorized}
      />
    </section>
  );
}
