import { useCallback, useState } from 'preact/hooks';

import {
  getClusterStatus,
  isAuthenticationFailure,
  type ClusterStatus,
} from '../lib/api';
import { useVisiblePoll } from '../hooks/useVisiblePoll';
import { NodeCard } from '../components/NodeCard';

const POLL_INTERVAL_MS = 3000;

interface Props {
  onUnauthorized: (reason: string) => void;
  onStatus: (status: ClusterStatus) => void;
}

/**
 * The overview.
 *
 * Polls only THIS node, which is deliberate: /cluster/status is answered
 * locally by whichever node served the page, and it carries the whole committed
 * peer list. Fanning out to sibling nodes would need their browser-reachable
 * URLs, which a page served from one of them cannot know (published host ports
 * are a compose detail, and behind the secure profile's proxy there is one
 * address for all three). So the peer table below is the cluster's own view of
 * its membership, and the card is this node's view of itself -- which is the
 * honest thing to show.
 */
export function ClusterPage({ onUnauthorized, onStatus }: Props) {
  const [status, setStatus] = useState<ClusterStatus | undefined>(undefined);
  const [error, setError] = useState<string | undefined>(undefined);
  const [paused, setPaused] = useState(false);

  const refresh = useCallback(() => {
    getClusterStatus()
      .then((next) => {
        setStatus(next);
        setError(undefined);
        onStatus(next);
      })
      .catch((failure: unknown) => {
        if (isAuthenticationFailure(failure)) {
          onUnauthorized(failure.message);
          return;
        }
        setError(failure instanceof Error ? failure.message : 'Request failed.');
      });
  }, [onUnauthorized, onStatus]);

  useVisiblePoll(refresh, POLL_INTERVAL_MS, !paused);

  return (
    <section class="page" aria-labelledby="cluster-heading">
      <div class="page-head">
        <h2 class="page-title" id="cluster-heading">
          Cluster
        </h2>
        <div class="page-actions">
          <button class="button" type="button" onClick={refresh}>
            Refresh
          </button>
          <button
            class="button"
            type="button"
            aria-pressed={paused}
            onClick={() => setPaused((was) => !was)}
          >
            {paused ? 'Resume polling' : 'Pause polling'}
          </button>
        </div>
      </div>

      <p class="page-note">
        Polls every {POLL_INTERVAL_MS / 1000}s while this tab is visible, and
        not at all while it is hidden.
      </p>

      <div class="node-grid">
        <NodeCard label="this node" status={status} error={error} />
      </div>

      <h3 class="section-title">Members</h3>
      {status === undefined ? (
        <p class="page-note">No configuration yet.</p>
      ) : (
        <div class="table-scroll">
          <table class="data-table">
            <thead>
              <tr>
                <th scope="col">ID</th>
                <th scope="col">Raft address</th>
                <th scope="col">Suffrage</th>
                <th scope="col">Role</th>
              </tr>
            </thead>
            <tbody>
              {status.peers.map((peer) => (
                <tr key={peer.id}>
                  <td class="numeric">{peer.id}</td>
                  <td class="numeric">{peer.address}</td>
                  <td>{peer.suffrage}</td>
                  <td>
                    {peer.id === status.leader_id ? (
                      <span class="state-chip on-paper is-leader">leader</span>
                    ) : (
                      <span class="state-chip on-paper is-follower">
                        follower
                      </span>
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </section>
  );
}
