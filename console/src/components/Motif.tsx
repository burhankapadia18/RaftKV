interface Props {
  /** Rendered width and height in px. The drawing is square. */
  size?: number;
}

/**
 * The console's entire illustration budget: three stacked log entries with the
 * last one committed.
 *
 * Inline, monochrome (`currentColor`) and a few hundred bytes, because the
 * console ships inside the engine binary and every asset is another byte array
 * in `.rodata`. Drawn with a round-capped hand-weight stroke rather than filled
 * shapes so it reads as a sketch, which is the one place the reference's
 * hand-drawn character survives into an operations tool.
 *
 * Decorative in every use so far -- always paired with real text -- hence
 * `aria-hidden`.
 */
export function Motif({ size = 44 }: Props) {
  return (
    <svg
      class="motif"
      width={size}
      height={size}
      viewBox="0 0 48 48"
      fill="none"
      stroke="currentColor"
      stroke-width="2"
      stroke-linecap="round"
      stroke-linejoin="round"
      aria-hidden="true"
    >
      <rect x="4" y="6" width="30" height="9" rx="3" />
      <rect x="4" y="19.5" width="30" height="9" rx="3" />
      <rect x="4" y="33" width="30" height="9" rx="3" />
      <path d="M9 10.5h7M9 24h11M9 37.5h5" />
      <path d="M38 33.5l3.5 4L47 30" />
    </svg>
  );
}
