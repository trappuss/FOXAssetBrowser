# Animation and Rigging

## 1. MTAR and GANI

`.mtar` (code 3296) is a container of embedded **GANI** clips (8074). A
character's animation arrives as a set of archives, each holding many clips.

The single most important constant: **playback is 59.94 fps**, not 60 —
`PlaybackRate = 1 / (60000/1001)`. Every duration and speed derived at 60 will
drift by 0.1 %, which is small enough to look like rounding and large enough to
misalign a long clip.

There are two GANI generations:

- **GANI1** — Ground Zeroes, and TPP facial animation.
- **GANI2** — newer, with improved framing and track sectioning, and a
  **Layout Track in the negative frame range**.

Clips carry more than bone curves: motion events, motion points and shader
parameters travel with them.

The precise account of the track format is Joey35233's **FoxKit-3**,
`FoxKit/Assets/Fox/Anim/Playback/TrackData.cs` on the `anim-dev` branch — the
`TrackData`/`TrackMiniData` headers, the `SegmentType` enum (`Quat, Float,
Vector2, Vector3, Vector4, QuatDiff, VectorDiff`), per-track
`ComponentBitSize`, the unaligned bit reader, the quaternion encoding and the
half-precision decode. Read it before writing a GANI parser.

## 2. Rig binding: which clips fit which skeleton

An animation set binds to a skeleton by matching bone names. The question "does
this set fit this model" therefore has a numeric answer — the fraction of the
set's tracks that resolve to bones the model carries — and the threshold you
pick for it looks arbitrary until you measure the distribution.

Measured over 108 real motion archives against four skeletons:

```
character (115 bones)   0.0-0.1: 27   0.1-0.4: 10   0.4-0.9: 0   0.9-1.0: 71
character (123 bones)   identical
character (126 bones)   identical
weapon    (  1 bone )   0.0-0.1: 108   — nothing at or over 0.50
```

**The band from 0.4 to 0.9 is completely empty.** Any threshold in it gives the
same answer, so 0.50 is mid-gap rather than a knife edge — which is the useful
thing to know, and it is only knowable by looking at the histogram rather than
at a pass/fail count.

The same scan agrees with a scope query exactly: the character reports 71
archives / 358 clips, the one-bone weapon reports 0 and says why.

**[open]** This was measured over 108 archives. A full install has ~508.
Re-measure before changing the threshold.

## 3. Rig solve, in order

1. Decode the GANI clip.
2. Resolve its tracks to bone indices.
3. Solve the FRIG bone drives.
4. Solve the FRIG IK jobs.
5. Evaluate the FRDV help-bone operators.
6. Skin.

Steps 4 and 5 are the ones that fail silently. A pose with no IK solve looks
almost right; a pose with no help bones looks almost right and has collapsed
elbows.

## 4. Posing an assembly

A composed character is several models on one skeleton, and only one of them
carries the full bone list. The rules that make the rest follow are in
[Models and Skeletons §8](Models-and-Skeletons); the short version is that a
fragment **borrows** the host's matrices by name when every one of its bones
resolves, and is **carried** rigidly when it is anchored on a socket.

Two rules that cost real time to find:

- **Keep the borrow only when nothing is left unresolved.** A majority vote
  ("most bones matched, so borrow") leaves the unmatched bones on identity,
  which is a pose nobody authored.
- **A socket-anchored part is carried regardless of how many bones the clip
  drives.** Gating the carry on `driven == 0` works right up until the clip
  happens to mention a bone name the part also uses, and then the part flies.

## 5. Exporting animation

Two shapes, and they are opposites:

- **Posed** — vertices baked at the frame on screen, no skeleton. What is
  visible is what is written.
- **Animated** — parts go in rigged, with no baked pose, and the motion arrives
  as curves on the joints, one glTF animation per clip.

Everything else about the walk — which parts are in the scene, what is hidden,
where attachments sit — is identical between the two, which is why it is one
walk with a flag rather than two exporters that can disagree about what a scene
contains.
