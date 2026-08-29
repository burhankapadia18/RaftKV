/**
 * Shown whenever the engine reports authentication is OFF.
 *
 * Not decoration. The default compose profile has no client authentication, so
 * anyone who can reach this port can read and write every key. The console does
 * not create that exposure -- it is served from the port that already has it --
 * but it is the first thing that makes it visible, so it says so.
 */
export function AuthBanner() {
  return (
    <p class="banner" role="status">
      <strong>Unauthenticated.</strong> This node has no client authentication:
      anyone who can reach it can read and write every key. Enable ACLs with{' '}
      <code>docker-compose.auth.yml</code> and{' '}
      <code>RAFTKV_ADMIN_PASSWORD</code>.
    </p>
  );
}
