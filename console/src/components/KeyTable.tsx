import { decodeKey } from '../lib/api';
import { Motif } from './Motif';

interface Props {
  /** Percent-encoded keys, exactly as the API returned them. */
  encodedKeys: string[];
  selected?: string;
  onSelect: (rawKey: string) => void;
}

/**
 * Keys arrive percent-encoded because a key is arbitrary bytes and a JSON
 * string is Unicode text. Decoding happens here, for display only -- the raw
 * key is what goes back to the API.
 */
export function KeyTable({ encodedKeys, selected, onSelect }: Props) {
  if (encodedKeys.length === 0) {
    return (
      <div class="empty">
        <Motif />
        <p>No keys under this prefix.</p>
      </div>
    );
  }

  return (
    <div class="table-scroll">
      <table class="data-table">
        <thead>
          <tr>
            <th scope="col">Key</th>
            <th scope="col" class="col-shrink">
              <span class="visually-hidden">Actions</span>
            </th>
          </tr>
        </thead>
        <tbody>
          {encodedKeys.map((encoded) => {
            const raw = decodeKey(encoded);
            return (
              <tr
                key={encoded}
                class={raw === selected ? 'is-selected' : undefined}
              >
                {/* Rendered as text, never innerHTML: a key is attacker-supplied
                    bytes and this is the CSP's backstop, not its replacement. */}
                <td class="numeric key-cell">{raw}</td>
                <td class="col-shrink">
                  <button
                    class="button"
                    type="button"
                    onClick={() => onSelect(raw)}
                  >
                    Open <span class="arrow" aria-hidden="true">→</span>
                  </button>
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
    </div>
  );
}
