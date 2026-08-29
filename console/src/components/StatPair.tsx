interface Props {
  label: string;
  value: string;
  /** Rendered small and muted beneath the figure. */
  note?: string;
  tone?: 'normal' | 'danger';
  /**
   * `range` for a value that describes an interval rather than being a single
   * quantity. Set smaller, because at figure scale it would be the loudest
   * thing on the card while being the least urgent.
   */
  kind?: 'figure' | 'range';
}

/** A label above a figure. Scale contrast is what makes the figure readable. */
export function StatPair({
  label,
  value,
  note,
  tone = 'normal',
  kind = 'figure',
}: Props) {
  const modifiers = [
    tone === 'danger' ? 'is-danger' : '',
    kind === 'range' ? 'is-range' : '',
  ]
    .filter((cls) => cls !== '')
    .join(' ');

  return (
    <div class="stat-pair">
      <span class="stat-label">{label}</span>
      <span class={`stat-value numeric ${modifiers}`.trimEnd()}>{value}</span>
      {note !== undefined && <span class="stat-note">{note}</span>}
    </div>
  );
}
