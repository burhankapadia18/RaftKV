import type { ClusterStatus } from '../lib/api';
import { formatBytes, formatInt } from '../lib/format';
import { StatPair } from './StatPair';

interface Props {
  /** The node's base URL as the browser reaches it, for display. */
  label: string;
  status?: ClusterStatus;
  error?: string;
}

function stateClass(state: string): string {
  const lowered = state.toLowerCase();
  if (lowered === 'leader') return 'is-leader';
  if (lowered === 'candidate') return 'is-candidate';
  return 'is-follower';
}

export function NodeCard({ label, status, error }: Props) {
  if (status === undefined) {
    return (
      <article class="node-card is-unreachable">
        <header class="node-card-head">
          <h3 class="node-card-title">{label}</h3>
          <span class="state-chip">unreachable</span>
        </header>
        <div class="node-card-body">
          <p class="error-note">{error ?? 'No response.'}</p>
        </div>
      </article>
    );
  }

  // Replication lag, not a raw index: the number an operator acts on is how far
  // this node's applied state trails the log it has been given.
  const lag = Math.max(0, status.last_log_index - status.applied_index);

  return (
    <article class={`node-card ${stateClass(status.state)}`}>
      {/* The state colour fills the head of the card rather than edging it: on
          a screen read during an incident, "which node is the leader" has to be
          answerable before any figure is. */}
      <header class="node-card-head">
        <h3 class="node-card-title">{status.node_id || label}</h3>
        <span class="state-chip">{status.state.toLowerCase()}</span>
        <span class="node-card-leader">
          Leader{' '}
          <span class="numeric">
            {status.leader_id === '' ? 'none (election)' : status.leader_id}
          </span>
        </span>
      </header>

      <div class="node-card-body">
        {status.error !== undefined && status.error !== '' && (
          <p class="error-note" role="alert">
            Partial: {status.error}
          </p>
        )}

        <div class="stat-grid">
          <StatPair label="Term" value={formatInt(status.term)} />
          <StatPair
            label="Applied"
            value={formatInt(status.applied_index)}
            note={`commit ${formatInt(status.commit_index)}`}
          />
          <StatPair
            label="Lag"
            value={formatInt(lag)}
            note="log − applied"
            tone={lag > 0 ? 'danger' : 'normal'}
          />
          <StatPair label="Keys" value={formatInt(status.key_count)} />
          <StatPair label="WAL" value={formatBytes(status.wal_bytes)} />
          <StatPair
            label="Log"
            value={`${formatInt(status.first_log_index)}–${formatInt(status.last_log_index)}`}
            note={`snapshot ${formatInt(status.last_snapshot_index)}`}
            kind="range"
          />
        </div>
      </div>
    </article>
  );
}
