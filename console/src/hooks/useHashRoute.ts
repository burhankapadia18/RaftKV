import { useEffect, useState } from 'preact/hooks';

/** Read the route out of the current location hash. */
function readRoute(): string {
  return window.location.hash.replace(/^#\/?/, '') || 'cluster';
}

/**
 * The current hash route, e.g. "cluster".
 *
 * Hash routing rather than the History API so the engine needs no catch-all
 * rewrite: `/console/` is the only HTML path that exists, and an unknown path
 * under `/console/` stays a real 404.
 */
export function useHashRoute(): [string, (route: string) => void] {
  const [route, setRoute] = useState(readRoute);

  useEffect(() => {
    const onHashChange = () => setRoute(readRoute());
    window.addEventListener('hashchange', onHashChange);
    return () => window.removeEventListener('hashchange', onHashChange);
  }, []);

  return [
    route,
    (next: string) => {
      window.location.hash = `#/${next}`;
    },
  ];
}
