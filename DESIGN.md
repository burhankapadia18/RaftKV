# DESIGN.md — the RaftKV console's visual language

Source of the language: a screenshot of the Passionfroot marketing site,
supplied as `design.webp` and **deliberately not checked in** — it is someone
else's copyrighted page, and this repository is public. Everything that was
taken from it is recorded below, sampled off the pixels rather than eyeballed,
so this document stands on its own without the image.

What follows is the *extraction* — what that page actually does — followed by
the translation into an operations console, which is a different job with
different obligations.

Read this before changing anything under `console/src/styles/`. Every rule below
has a reason attached; a rule whose reason no longer holds should be changed,
not quietly ignored.

---

## 1. What the reference actually is

A warm, editorial, **paper-and-ink** system. Not "clean minimal", not
glassmorphism, not a dark dashboard. Four devices carry the whole personality:

1. **Warm cream ground.** Never white. `#F7F3EB` — a paper stock, slightly
   yellow-green. White is reserved for cards, which is what makes cards read as
   *objects placed on paper*.
2. **Hard ink borders and offset shadows.** Every card, button, chip and tab has
   a `1–2px` near-black border and a **solid, un-blurred** shadow offset down and
   right. No `blur-radius`, no alpha. This is the single most recognisable trait
   of the reference and the cheapest to reproduce faithfully.
3. **A display serif at large scale, against a plain sans at small scale.** The
   hierarchy is carried almost entirely by *scale and family contrast*, not by
   weight or colour. Headlines are enormous; body copy is quiet.
4. **Saturated pastel as full-bleed bands**, with **torn-paper edges** where a
   band meets the cream. Colour arrives as an area, not as an accent dot.

### Measured palette

Sampled directly from `design.webp`:

| Role in the reference | Hex | Note |
|---|---|---|
| Paper ground | `#F7F3EB` | the default background of the whole page |
| Card | `#FFFFFF` | only ever inside an ink border |
| Ink | `#21221F` | warm near-black, not `#000` and not a grey |
| Coral | `#FE976C` | primary action + the loudest band |
| Mint | `#6ACCA2` | band |
| Butter | `#FFF387` | band |
| Sky | `#AAEBFE` | band |
| Orchid | `#EAA7E5` | band (not used by the console — see §4) |

The four non-coral pastels are used in the reference purely as sequence — one
per "how it works" step. They carry no meaning there. **In the console they
carry meaning** (§4), which is the main deliberate departure.

### Measured components

- **Button** — radius ~8px, ~2px ink border, hard shadow `3px 3px 0` ink, bold
  sans label, trailing `→`. Primary is coral-filled; secondary is white-filled.
- **Card** — radius ~12–14px, ~2px ink border, hard shadow `4–5px 4–5px 0` ink.
- **Tab chip** — radius ~6px, 1px ink border, no shadow, small sans label.
  Selected state is an **ink fill with cream text**, not a colour fill.
- **Pill** — fully rounded, hairline border, used for dense metadata rows.
- **Rules** — hairline ink dividers inside cards, never a shadow, to separate
  list rows.

---

## 2. What does *not* transfer, and why

The reference is a consumer marketing page. The console is a tool an operator
opens because something might be wrong. Three things are therefore left behind:

- **The mascot doodles.** Charming on a landing page; noise on a screen someone
  is reading during an incident. One restrained line-drawn motif is kept for
  empty states (§7) — that is the whole budget.
- **The huge centred hero.** A console's first screen must be *data*, above the
  fold, on a laptop. Display type is used at section scale, not page scale.
- **Pastel as decoration.** A band of colour that means nothing is a band an
  operator learns to ignore, which is exactly the wrong reflex to train on a
  screen where colour also reports raft state.

---

## 3. Typography

```
--font-display : ui-serif, 'New York', 'Iowan Old Style', Georgia, 'Times New Roman', serif
--font-sans    : ui-sans-serif, system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif
--font-mono    : ui-monospace, 'SF Mono', 'Cascadia Mono', Menlo, Consolas, monospace
```

**Why no webfont.** The reference's display face is a contemporary transitional
serif (Fraunces-family). Matching it exactly means shipping a `.woff2`. The
asset pipeline would take it — `GenerateConsoleAssets.cmake` already knows
`font/woff2` — but the console is **embedded in the engine binary and served to
operators who may have no route to the internet**, so a Google Fonts `<link>` is
out on principle, and a self-hosted subset costs ~30 KB of `.rodata` against a
console that is currently ~44 KB in total. `ui-serif` resolves to New York on
Apple platforms and Georgia elsewhere: both are warm, sturdy, large-x-height
transitional serifs, which is the same *genus* as the reference. That is the
right trade for this artefact. If the console ever ships a font, subset it and
say so here.

**Where each family is allowed:**

| Family | Used for | Never used for |
|---|---|---|
| display (serif) | the wordmark, page titles, section titles, the login title | numbers, keys, addresses |
| sans | all UI chrome — buttons, labels, nav, notes, table headers, prose | large figures |
| mono | keys, node ids, raft addresses, **every stat figure** | headings |

**Stat figures stay mono, not serif** — and that is a considered break from the
reference, which sets its big numbers in the serif. Georgia ships *old-style*
figures whose heights vary digit to digit, so `1907` and `3204` do not align and
cannot be compared down a column. Every figure on this page exists to be
compared against another figure. `font-variant-numeric: tabular-nums` on the
mono face is the requirement; the serif cannot meet it.

**Scale.** Contrast is wide on purpose — the display steps are ~3× the body
step, which is what does the work the reference's weight-and-size contrast does.

---

## 4. Colour, and the one rule that matters

Cream paper, white cards, ink text, coral for the primary action. Then:

**The four pastels are the raft state vocabulary, and they mean only that.**

| Colour | Meaning | Where it appears |
|---|---|---|
| Mint `#6ACCA2` | leader | node band, state chip, member-table role |
| Sky `#AAEBFE` | follower | node band, state chip, member-table role |
| Butter `#FFF387` | candidate / election in progress | node band, state chip |
| Coral `#FE976C` | primary action, **and** danger/unreachable | buttons, error bands |

Coral doing double duty is deliberate and is the reference's own logic — coral
is the colour that demands a response, whether that response is "click this" or
"look at this". They are never adjacent in the same component, so the ambiguity
does not arise in practice.

There is deliberately **no fifth colour in the stylesheet**. The reference's
orchid `#EAA7E5` is the obvious candidate if a fifth raft state ever needs one,
but a token nothing references is dead code, and a spare colour sitting in the
palette is an invitation to spend it decoratively. Adding it is a design
decision — decide the meaning first, then add the token.

A pastel must never be used because a surface looked empty.

**Contrast.** All four pastels are light, so text on them is always ink, never
white. Ink `#21221F` on butter `#FFF387` is ~15:1; on mint ~9:1; on sky ~13:1;
on coral ~8.5:1. All clear AA at body size, all clear AAA at large.

---

## 5. Both themes are real

The reference has no dark mode. The console needs one — operators work at night —
so dark mode is *designed*, not derived by inverting lightness.

Dark is **night paper**, not a blue-grey dashboard: a warm brown-black ground
(`#1A1815`) with cream ink (`#F2EDE3`). The structural devices survive the flip
by swapping which end of the scale draws the border:

- borders and hard shadows become **cream** on dark, so a card still reads as an
  ink-drawn object on paper rather than dissolving into the ground;
- the four pastels are re-chosen at dark-appropriate chroma rather than
  lightness-flipped, so mint still reads as mint;
- cards are a lifted warm brown, never `#000`.

Every colour is defined on bare `:root` first and only *re-defined* inside the
dark block. No colour may have its sole definition inside a media query.

---

## 6. Motion

Compositor-friendly properties only — `transform`, `opacity`, `box-shadow`.
Never `width`, `top`, `margin`, `font-size`.

The signature interaction is the **press**: a button's hard shadow is offset
`3px 3px`; on `:active` the button translates by exactly that offset and its
shadow goes to zero, so it appears to be physically pushed onto the paper. It
costs two lines, it is the reference's own physics, and it is the one moment of
character in an otherwise sober tool.

`prefers-reduced-motion: reduce` sets both duration tokens to `0ms`, removing
transitions rather than shortening them. The press still works — it is a state,
not an animation.

---

## 7. The one illustration

A single hand-drawn-feeling inline SVG line motif — three stacked log entries
with a tick — appears in empty states and on the sign-in card. Inline, monochrome
(`currentColor`), a few hundred bytes, no separate asset. It is the entire
"doodle" budget: it exists so an empty screen reads as *deliberately empty*
rather than broken, which is a real ambiguity on a console that also fails.

---

## 8. Deckled edges, and the CSP rule behind them

Where a full-bleed colour band meets the surface beneath it, the seam is a
deckled edge, not a straight line. Implemented as a **gradient** mask on the
band's `::after` — decorative and unreachable by assistive technology by
construction, since it is a pseudo-element.

**It must not be an SVG `data:` URI, and this is not a style preference.** The
engine serves every console asset under
`default-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'`
(`cpp-app/src/network/http_server.hpp`). A `data:` URI mask is a *fetched
resource* and that policy blocks it outright. The first version of this edge was
an SVG data URI: it rendered perfectly from the filesystem, passed every check,
and rendered not at all on a real node — silently, because a blocked mask just
means no mask. A CSS gradient is generated rather than fetched, so there is
nothing for the CSP to refuse.

The general rule: **the console's CSP is tight and stays tight.** No console
change may widen it. Anything that needs a fetched resource — a font file, an
image, an icon sprite — is either inlined as markup (see §7) or does not ship.
Verify visual changes against a running node, not only against the dev server or
a local file: those two do not enforce the header the engine sends.

If the edge ever costs more than ~400 bytes of CSS, drop it — the borders and
shadows carry the language on their own.

---

## 9. Checklist for any new console surface

- [ ] Cream ground, white card, ink border, hard offset shadow — no blurred
      shadows anywhere.
- [ ] Title in the display serif; figures in mono with tabular figures.
- [ ] Any colour used is from §4 and means what §4 says it means.
- [ ] Focus is visible and designed — an ink ring at 2px offset, not the UA default.
- [ ] Both themes checked, and neither looks like the other's leftovers.
- [ ] Wide content (tables, keys) scrolls inside its own container; the body
      never scrolls horizontally.
- [ ] No new network fetch, and nothing that needs the CSP widened (§8). The
      console is embedded in the binary and must render fully offline.
- [ ] Checked against a **running node**, not just the dev server: only the
      engine sends the Content-Security-Policy the page has to live under.
