# Toolchain and Verification

Every number in this handbook came out of a tool run, and every one of those
runs is repeatable. This page is how.

## 1. The method

Four rules, learned the hard way, in the order they matter:

**Measure, then decide.** If a thing cannot be measured, say so and hand over
the measurement rather than guessing. Most of the corrections marked in this
handbook are places where an earlier session reasoned instead of measuring, and
was plausible and wrong.

**Never identify an asset by comparing renders.** Not as a fallback, not as a
tiebreaker, not "just to rank candidates". Match by hash, or by a binding the
files state. A render match is a coincidence with a confidence score attached.

**Prove a check fires by deliberately breaking it.** A green test you have
never seen fail is not evidence. Every self-test in this project has been run
against a deliberately wrong build and watched to go red, and the report says
what the red looked like.

**When a feature lands, the thing it replaces goes.** Two live spellings of one
behaviour is the bug that keeps coming back — the second copy drifts from the
first exactly when it matters.

## 2. Verifying without a game install

Most of this can be exercised against a **loose folder** of extracted assets
mounted as if it were an archive. That makes the whole toolchain testable in a
container with no game:

```
--game <empty dir>  --loose <root>  --dict <dict dir>
```

Two things to get right, and both have cost a session:

- **Mount the loose tree with `Assets/` at its top.** Pulls often unpack with
  `ssd/ mgo/ tpp/` at the top level, and mounting them without the `Assets/`
  prefix silently breaks every hash lookup — a model's textures all read as
  unresolved, which looks exactly like a code bug and is not.
- **A loose mount is not container-scanned**, on purpose. See
  [Archives and Hashing §4](Archives-and-Hashing).

## 3. The invariants that catch a wrong result

| invariant | what it catches |
|---|---|
| `world == parent.world + local` on every bone | a mis-parsed hierarchy |
| bind pose carries translation only | a wrong matrix convention |
| decoded normal mean ≈ (132, 127, 132), alpha ≈ 126 | a normal map decoded as ordinary RGB |
| the rig resolves 53 bones on a human model | a wrong or missing FRIG |
| an export is **byte-identical across two runs** | any hidden clock, hash-order or thread-order dependency |
| a parallel pass produces the same bytes as a serial one | a merge that is out of chunk order |
| a re-chunked mip matches the **shipped** chunk count | a wrong constant that a round trip cannot see |
| the PFTXS pack's own hashes reproduce from a name | a wrong hash implementation |

The last three are the interesting ones, because each catches a class of error
that self-consistency cannot. A round trip through your own reader and writer
is green when both are wrong together.

## 4. Determinism is a feature, not a nicety

Any export this project writes must be **byte-identical across runs**. It is
the cheapest possible test for a whole family of bugs — a timestamp, an
unstable hash iteration order, a thread that finishes in a different order —
and every one of those shows up as a diff before it shows up as a wrong result.

Two real instances:

- A ZIP writer stamped `now` into each member header. Every test passed; two
  packages of an unchanged folder differed in eight bytes per member. Fixed by
  using each source file's own mtime.
- A parallel deep scan is checked by building the index with one worker and
  with eight and comparing hashes. Different hashes mean the merge is out of
  chunk order, and nothing else about the run matters.

## 5. A menu cannot be photographed

A context menu is a top-level window and never appears in a screenshot, so
every menu claim in this project is checked by **dumping the menu as text** —
its entries, their order, their separators and their enabled state.

This is not pedantry. Menu bugs found only by the log include: a doubled
separator when a block was empty; two entries that both promised a `.glb` and
neither said which was which; and an action that ran and did nothing because
its silent twin was never disabled.

The complement also holds: **look at screenshots, do not just take them.**
Bugs found only by opening the image include a value column painted off the
side of a panel, a count that was written and then immediately overwritten
before it could be seen, and — most importantly — an accessory sitting half a
metre off a character's head while the numeric check reported it as seated.

## 6. What a good measurement looks like

The histogram, not the pass/fail count. Compare:

> "The animation-binding threshold is 0.5 and it works."

with:

```
character (115 bones)   0.0-0.1: 27   0.1-0.4: 10   0.4-0.9: 0   0.9-1.0: 71
weapon    (  1 bone )   0.0-0.1: 108  — nothing at or over 0.50
```

The second tells you the band from 0.4 to 0.9 is **empty**, so any threshold in
it gives the same answer and the exact value does not matter. That is knowable
only from the distribution.

Similarly, the FTEX chunking rule came from a census — 283 files, 2,795 mips,
4,293 chunks — that established `chunkCount == 0` **never occurs in shipped
data** even though the reader supports it. Writing the form the engine has
never been asked to read is the difference between a mod and a crash, and no
amount of reading the format documentation would have said so.

## 7. Reproducing anything here

FOX Asset Browser carries a headless harness: a flag per measurement, each one
writing a TSV and logging the summary lines that answer the question. The
project ships them as double-clickable scripts (see the README) so that a
measurement nobody runs does not stay unmeasured.

The rule for adding one: **a flag that writes a file and ends the run must arm
the run.** That mistake has been made three times in this project — a flag
added without being registered as an output, so the app came up with no window
and sat there. It is in the list of scars for a reason.
