import { useEffect, useRef } from 'preact/hooks';

/**
 * Run `fn` immediately, then every `intervalMs` -- but ONLY while the tab is
 * visible.
 *
 * The visibility gate is the whole point. The database is the primary tenant of
 * this container, and a console tab left open in a background window for a week
 * must not keep asking it questions. A hidden tab polls nothing and resumes on
 * the next `visibilitychange`.
 */
export function useVisiblePoll(
  fn: () => void,
  intervalMs: number,
  enabled: boolean,
): void {
  // Held in a ref so a re-created closure does not restart the timer, which
  // would make the effective interval depend on render frequency.
  const latest = useRef(fn);
  latest.current = fn;

  useEffect(() => {
    if (!enabled) return;

    let timer: number | undefined;

    const stop = () => {
      if (timer !== undefined) {
        clearInterval(timer);
        timer = undefined;
      }
    };

    const start = () => {
      if (timer !== undefined) return;
      latest.current();
      timer = window.setInterval(() => latest.current(), intervalMs);
    };

    const onVisibilityChange = () => {
      if (document.visibilityState === 'visible') start();
      else stop();
    };

    onVisibilityChange();
    document.addEventListener('visibilitychange', onVisibilityChange);
    return () => {
      document.removeEventListener('visibilitychange', onVisibilityChange);
      stop();
    };
  }, [intervalMs, enabled]);
}
