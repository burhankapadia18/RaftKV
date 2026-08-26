import { useCallback, useEffect, useState } from 'preact/hooks';

import {
  ApiFailure,
  deleteUser,
  getUser,
  listUsers,
  putUser,
  whoami,
  type Identity,
  type UserRecord,
  type UserUpsert,
} from '../lib/api';
import { UserForm } from '../components/UserForm';
import { UserTable } from '../components/UserTable';

interface Props {
  authEnabled: boolean;
  onUnauthorized: (reason: string) => void;
}

type Editing =
  | { mode: 'none' }
  | { mode: 'create' }
  | { mode: 'edit'; record: UserRecord };

export function UsersPage({ authEnabled, onUnauthorized }: Props) {
  const [names, setNames] = useState<string[]>([]);
  const [identity, setIdentity] = useState<Identity | undefined>(undefined);
  const [editing, setEditing] = useState<Editing>({ mode: 'none' });
  const [error, setError] = useState<string | undefined>(undefined);
  const [note, setNote] = useState<string | undefined>(undefined);

  const handle = useCallback(
    (failure: unknown) => {
      if (failure instanceof ApiFailure) {
        if (failure.status === 401) {
          onUnauthorized(failure.message);
          return;
        }
        setError(`${failure.status}: ${failure.message}`);
        return;
      }
      setError(failure instanceof Error ? failure.message : 'Request failed.');
    },
    [onUnauthorized],
  );

  const refresh = useCallback(() => {
    if (!authEnabled) return;
    listUsers()
      .then((loaded) => {
        setNames(loaded);
        setError(undefined);
      })
      .catch(handle);
    whoami().then(setIdentity).catch(handle);
  }, [authEnabled, handle]);

  useEffect(refresh, [refresh]);

  const save = (name: string, body: UserUpsert): void => {
    putUser(name, body)
      .then(() => {
        setEditing({ mode: 'none' });
        // A user record is a raft entry, so it is committed here but may take a
        // replication delay to be usable on another node.
        setNote(`Saved ${name}. Replicating to the other nodes.`);
        refresh();
      })
      .catch(handle);
  };

  const remove = (name: string): void => {
    // A native confirm(): this is destructive and irreversible, and a custom
    // modal would be more code for strictly less trust.
    if (!window.confirm(`Delete user "${name}"?`)) return;
    deleteUser(name)
      .then(() => {
        setNote(`Deleted ${name}.`);
        refresh();
      })
      .catch(handle);
  };

  const edit = (name: string): void => {
    getUser(name)
      .then((record) => setEditing({ mode: 'edit', record }))
      .catch(handle);
  };

  if (!authEnabled) {
    return (
      <section class="page" aria-labelledby="users-heading">
        <h2 class="page-title" id="users-heading">
          Users
        </h2>
        <p class="banner">
          <strong>Authentication is off.</strong> User management is closed
          without <code>RAFTKV_ADMIN_PASSWORD</code>, because with no admin
          password there is no way to authenticate an administrator. Start the
          cluster with <code>docker-compose.auth.yml</code>.
        </p>
      </section>
    );
  }

  return (
    <section class="page" aria-labelledby="users-heading">
      <div class="page-head">
        <h2 class="page-title" id="users-heading">
          Users
        </h2>
        <div class="page-actions">
          <button
            class="button is-primary"
            type="button"
            onClick={() => setEditing({ mode: 'create' })}
          >
            New user
          </button>
        </div>
      </div>

      {identity !== undefined && (
        <p class="page-note">
          Signed in as <strong>{identity.name}</strong> — classes{' '}
          {identity.classes.join(', ') || 'none'}; patterns{' '}
          {identity.patterns.join(', ') || 'none'}.
        </p>
      )}

      {note !== undefined && (
        <p class="banner" role="status">
          {note}
        </p>
      )}
      {error !== undefined && (
        <p class="error-note" role="alert">
          {error}
        </p>
      )}

      <UserTable names={names} onSelect={edit} onDelete={remove} />

      {editing.mode === 'create' && (
        <UserForm
          onSubmit={save}
          onCancel={() => setEditing({ mode: 'none' })}
        />
      )}
      {editing.mode === 'edit' && (
        <UserForm
          existing={editing.record}
          onSubmit={save}
          onCancel={() => setEditing({ mode: 'none' })}
        />
      )}
    </section>
  );
}
