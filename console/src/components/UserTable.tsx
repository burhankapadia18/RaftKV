import { Motif } from './Motif';

interface Props {
  names: string[];
  onSelect: (name: string) => void;
  onDelete: (name: string) => void;
}

export function UserTable({ names, onSelect, onDelete }: Props) {
  if (names.length === 0) {
    return (
      <div class="empty">
        <Motif />
        <p>
          No users yet. The bootstrap administrator comes from{' '}
          <code>RAFTKV_ADMIN_PASSWORD</code> and is not a record, so it is not
          listed here and cannot be edited.
        </p>
      </div>
    );
  }

  return (
    <div class="table-scroll">
      <table class="data-table">
        <thead>
          <tr>
            <th scope="col">User</th>
            <th scope="col" class="col-shrink">
              <span class="visually-hidden">Actions</span>
            </th>
          </tr>
        </thead>
        <tbody>
          {names.map((name) => (
            <tr key={name}>
              <td class="numeric">{name}</td>
              <td class="col-shrink">
                <div class="row-actions">
                  <button
                    class="button"
                    type="button"
                    onClick={() => onSelect(name)}
                  >
                    Edit
                  </button>
                  <button
                    class="button is-danger"
                    type="button"
                    onClick={() => onDelete(name)}
                  >
                    Delete
                  </button>
                </div>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
