interface Props {
  label: string;
  value: string;
  /** Rendered small and muted beneath the figure. */
  note?: string;
  tone?: 'normal' | 'danger';
}

/** A label above a figure. Scale contrast is what makes the figure readable. */
export function StatPair({ label, value, note, tone = 'normal' }: Props) {
  return (
    <div class="stat-pair">
      <span class="stat-label">{label}</span>
      <span class={`stat-value numeric${tone === 'danger' ? ' is-danger' : ''}`}>
        {value}
      </span>
      {note !== undefined && <span class="stat-note">{note}</span>}
    </div>
  );
}
