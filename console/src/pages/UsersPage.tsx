import { useCallback, useEffect, useState } from 'preact/hooks';

import {
  ApiFailure,
  deleteUser,
  getUser,
  isAuthenticationFailure,
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
  /**
   * Whether the engine reports client auth as ON. `undefined` means the answer
   * is not known yet (the status call failed), which is NOT the same as "off" --
   * claiming "off" without being told is what made this page tell an
   * authenticated admin that authentication was disabled.
   */
  authEnabled: boolean | undefined;
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
  // A 403 on the listing means "you are not an administrator", which is a
  // different screen from "there are no users". Showing the empty table for it
  // states something false -- there may well be users, this caller just may not
  // see them -- and offering "New user" advertises an action that cannot work.
  const [forbidden, setForbidden] = useState(false);

  const handle = useCallback(
    (failure: unknown) => {
      if (isAuthenticationFailure(failure)) {
        onUnauthorized(failure.message);
        return;
      }
      if (failure instanceof ApiFailure) {
        setError(`${failure.status}: ${failure.message}`);
        return;
      }
      setError(failure instanceof Error ? failure.message : 'Request failed.');
    },
    [onUnauthorized],
  );

  const refresh = useCallback(() => {
    if (authEnabled === false) return;
    listUsers()
      .then((loaded) => {
        setNames(loaded);
        setError(undefined);
        setForbidden(false);
      })
      .catch((failure: unknown) => {
        if (failure instanceof ApiFailure && failure.status === 403) {
          setForbidden(true);
          setNames([]);
          return;
        }
        handle(failure);
      });
    // whoami needs no class, so it answers for a non-admin too -- which is what
    // lets the page say WHO you are while explaining what you may not do.
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

  if (authEnabled === false) {
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
        {!forbidden && (
          <div class="page-actions">
            <button
              class="button is-primary"
              type="button"
              onClick={() => setEditing({ mode: 'create' })}
            >
              New user
            </button>
          </div>
        )}
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

      {forbidden ? (
        <p class="banner">
          <strong>Administrator access required.</strong> Managing users needs
          the <code>admin</code> class; your account does not hold it. Your own
          identity and permissions are shown above.
        </p>
      ) : (
        <UserTable names={names} onSelect={edit} onDelete={remove} />
      )}

      {!forbidden && editing.mode === 'create' && (
        <UserForm
          onSubmit={save}
          onCancel={() => setEditing({ mode: 'none' })}
        />
      )}
      {!forbidden && editing.mode === 'edit' && (
        <UserForm
          existing={editing.record}
          onSubmit={save}
          onCancel={() => setEditing({ mode: 'none' })}
        />
      )}
    </section>
  );
}
