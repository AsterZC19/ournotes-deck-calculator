# Chart schema: raw `MusicScore` input and converted runtime chart

Reference for the two JSON shapes used by the deck calculator.

| | |
|---|---|
| Converter | `tools/convert_chart.py` (byte-identical body of upstream `chart_converter.py`) |
| Upstream | <https://github.com/empty-sekai/nnnotes> @ `cc43e132875206d02d8665b19c06b27255ab0037` (MIT) |
| Licence text | `tools/nnnotes-LICENSE` |
| Converter public entry point | `convert(root, mirror=False, start_note_id=0) -> dict` |
| Runtime format tag | `"nnnotes.live-score/1"` |
| Evidence basis | raw chart `/home/Minus/projects/work/sirius-asset-updater/downloads/new-pack-20260929/highlights/event-detail/charts/0109/0109_03.json`, the 12 converted charts in `charts/`, `prepare.py`, `model.py` |

The upstream pipeline is `MusicScoreLoader.Load -> SsRootDeserializer ->
SsTickConverter -> SsMusicScoreConverter -> MusicScoreNoteCreator ->
MusicScoreUtility.GetTimeMsFromBar`, and this module re-implements it function by
function; `convert()` is the single public entry point of the vendored copy.

---

## 1. Public API of `convert()`

```python
def convert(root: dict, mirror: bool = False, start_note_id: int = 0) -> dict:
    """SsMusicScoreConverter.Load -> plain dict of the runtime score."""
```

| Argument | Type | Default | Meaning |
|---|---|---|---|
| `root` | `dict` | - | Parsed raw chart JSON (or any object with the same shape). This is the uncompressed `json.loads` result; see `load_bytes()` if you hold the gzipped file bytes. |
| `mirror` | `bool` | `False` | Mirrors lanes (`lane -> 24 - lane - width`) and swaps flick `left`/`right`. Recorded in the output as `mirror`. |
| `start_note_id` | `int` | `0` | Base note id (the game passes the first free `MusicScore` id). Slide-combo ids and the long/guide line-id split are derived from it. Recorded as `startNoteId`. |

**Return value.** A plain `dict` that is JSON-serialisable with `json.dumps(...)`,
built from stdlib types plus `numpy.float32` values unwrapped via `float(...)`.
Top-level keys are listed in section 3.

**Purity.** `convert()` is pure: it reads no files, uses no globals that it
mutates, no clock, no RNG and no I/O, and calling it twice with equal arguments
returns equal results. The module helpers that do touch the outside world are
`load_bytes(data)` (gunzip + JSON decode of file bytes) and `extract()` (writes
files); `convert()` calls neither.

**Exceptions / failure modes raised by `convert()`**

The converter is as strict as the game in the same places. Callers must handle:

| Exception | Message / source | Trigger |
|---|---|---|
| `ValueError` | `ss score` | `root['score']` missing or not a dict (`parse()`), mirroring `SsMusicScoreConverter.Error("ss score")`. |
| `ValueError` | `ss type value '<v>'` | A note `type` string outside `NOTE_TYPES`. |
| `ValueError` | `ss dir value '<v>'` | A flick `dir` string other than `up/left/right/down`. |
| `ValueError` | `ss ease value '<v>'` | An `ease` string outside `linear/in/out` (single string or 2-element list). |
| `ValueError` | `ss lineIndex 20 startTick <tick>` | More than `MAX_LINES = 20` concurrent long/guide lines (`LineIndexAssigner.acquire`). |
| `ValueError` | `ConvertJudgementType: operateType <op>` | An `op` with no `NoteJudgementType` mapping (`judgement_type()`); unreachable for ops this converter emits. |
| `KeyError` | key from `OP_NAMES` / `DIRECTION_NAMES` / `LINE_EASE_NAMES` / `OFFSET_TYPE_NAMES` | Only if the module constant tables are edited inconsistently. |
| `TypeError` / `ValueError` | from `json` / `float` / `int` | Malformed value types (e.g. `pos` a non-numeric string other than `"auto"`). |

There are **no `assert` statements** in the converter; the `assert`s seen in
`prepare.py` belong to the caller. Note that `FADES.get(n["alpha"], 0)` silently
degrades an unknown `alpha` string to `none` instead of raising.

---

## 2. RAW input format

Produced by the game's `MusicScoreLoader` and shipped as
`.../charts/<musicScoreTextFileName>.json`. Plain UTF-8 JSON; if the first two
bytes are `1F 8B` it is gzip (use `load_bytes()`).

### 2.1 Top level

```json
{"meta": {"version": 100}, "score": {"events": { ... }, "notes": [ ... ]}}
```

| Path | Type | Meaning |
|---|---|---|
| `meta` | `object` | Informational only; `convert()` ignores it (`parse()` reads `score` only). Observed: `{"version": 100}`. |
| `score` | `object` | Required, must be a dict. |
| `score.events` | `object` | Optional; treated as empty (`SsEvents()`) when absent or not a dict. |
| `score.notes` | `array` | Optional; treated as `[]` when absent. Order is chart order and is significant (`src[0]` indexes it). |

For the reference chart `0109_03`, `score.events` is exactly:

```json
{"bpm": [{"t": 0, "bpm": 153.0}],
 "sig": [{"t": 0, "sig": [4, 4]}],
 "skill": [11520, 26880, 57600, 72960, 103680],
 "fever": [[26880, 41760], [72960, 86400], [103680, 118560]],
 "call": [{"t": 0, "timing": [0, 1, 0, 1]}]}
```

| Key | Type | Item shape | Meaning |
|---|---|---|---|
| `bpm` | `array` | `{"t": int, "bpm": number}` | Tempo change at tick `t`. A missing tick-0 event synthesises `120.0`. |
| `sig` | `array` | `{"t": int, "sig": [num:int, den:int]}` | Time-signature change; a missing tick-0 event synthesises `4/4`. Ticks per measure = `num * 1920 / den`. |
| `skill` | `array` | `int` (tick) | Skill-activation ticks. Entry `i` becomes `skill[i]` with `index = i`, in array order. |
| `fever` | `array` | `[start_tick:int, end_tick:int]` | Fever windows; enumerated over sorted starts. |
| `call` | `array` | `{"t": int, "timing": [int, ...]}` | Audience call; each `timing` entry equal to `1` becomes a `rhythms` fraction `(i+1)/n`. |

### 2.2 `score.notes[]`

Every note object is one of three shapes discriminated by `type`; all other keys
are optional and fall back to the `SsNote` dataclass defaults. Tap is the default
(`type` absent). Observed key set over the whole raw file:
`type, t, pos, size, crit, dir, node`. Node children use
`type, t, pos, size, ease, visible`.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `type` | `string` | `"tap"` | `tap \| flick \| trace \| long \| guide \| node` (numeric `SsNoteType` 0..5). `tap/flick/trace` are single notes, `long`/`guide` are node chains, `node` is only valid as a child. |
| `t` | `int` | `0` | Tick position. Floats are accepted and rounded half-to-even (`_to_int`). |
| `pos` | `number \| "auto"` | `0.0` | Lane index of the note's left edge, or the string `"auto"` (position interpolated between neighbouring nodes). |
| `size` | `number` | `6.0` | Lane width in lanes. The default is 6, not 0. |
| `crit` | `bool` | `false` | Critical (easy/life) note. In this pack it appears only on single notes; 2 occurrences in `0109_03`. |
| `dir` | `string` | `"up"` | `flick` direction: `up \| left \| right \| down`. Only meaningful for `flick`. |
| `ease` | `string \| [string, string]` | `"linear"` | Lane-move easing; a 2-element list sets the left/right edges separately. |
| `visible` | `bool` | `true` | `false` makes the note hidden (yields `Hidden*` ops, or a merged `Hidden`). |
| `alpha` | `string` | `"none"` | `none \| in \| out`; unknown strings degrade to `none`. |
| `node` | `array` | `null` | Required for `long`/`guide`: the ordered node chain, each child a note-shaped object. |

`type` is also legal on node children: in `0109_03` the node `type` values are
`{absent (tap): 525, "trace": 43, "flick": 8}`. Node `ease` values observed:
`"out"` x88, `"in"` x66, `["in","linear"]` x3, `["out","in"]` x2,
`["in","out"]` x1. `pos: "auto"` does not occur in this pack (0 occurrences), but
the code path exists and those nodes are marked `slideAlong`.

Note-kind histogram of the raw file (`0109_03`, 454 raw note objects):

| Kind | Count |
|---|---|
| `tap` (`type` absent) | 261 |
| `long` (chain) | 145 |
| `trace` (single) | 31 |
| `flick` (single) | 14 |
| `guide` (chain) | 3 |

#### 2.3 Real example rows

Minimal legal note - single tap, every optional key omitted:

```json
{"t": 109200, "pos": 6, "size": 6}
```

Single flick with a direction, single trace, and a critical tap:

```json
{"type": "flick", "t": 9600, "pos": 16, "size": 8, "dir": "right"}
{"type": "trace", "t": 77040, "pos": 12, "size": 12}
{"t": 53280, "pos": 12, "size": 6, "crit": true}
```

Long chain (first node visible, interior nodes hidden -> `Hidden`; `ease`
appears on interior nodes). Real row, truncated at 9 of its nodes:

```json
{"type": "long", "node": [
  {"t": 54000, "pos": 6, "size": 6},
  {"visible": false, "t": 54001, "pos": 7, "size": 5},
  {"visible": false, "t": 54119, "pos": 8, "size": 4},
  {"visible": false, "t": 54120, "pos": 6, "size": 6},
  {"visible": false, "t": 54148, "pos": 5, "size": 7},
  {"visible": false, "t": 54149, "pos": 5, "size": 6},
  {"visible": false, "t": 54208, "pos": 3, "size": 9, "ease": "out"},
  {"visible": false, "t": 54420, "pos": 2, "size": 10, "ease": "in"},
  {"t": 54720, "pos": 8, "size": 4}]}
```

Guide chain (both nodes hidden; this one became `op 101 GuideBeginNormal`
because a same-position `tap` was merged into it):

```json
{"type": "guide", "node": [
  {"visible": false, "t": 87360, "pos": 9, "size": 6},
  {"visible": false, "t": 87480, "pos": 12, "size": 0}]}
```

Chain lengths in `0109_03`: `long` nodes-per-note min 2, max 11; 140 of the 145
long chains start with a visible node, 5 start hidden.

### 2.4 Where the `op` enum comes from

The raw format never mentions `op` or `NoteOperateType`. `op` is the game's
`NoteOperateType` enum and is derived in `_build_note_infos()` from `type` +
`visible` + `pos: "auto"` + the node's position in its chain
(`ResolveLongNodeOperateType`, `ResolveGuideNodeOperateType`), then adjusted by
merging in `_info_dictionary()` and by the slide-interior expansion in
`_Creator._end_line()`:

| Raw input | Derived `op` |
|---|---|
| `tap` (single) | `1 Normal` |
| `flick` (single) | `40 Flick` |
| `trace` (single) | `60 Trace` |
| `long` node 0, visible, tap/flick/trace | `20 / 41 / 61` |
| `long` node 0, `visible: false` | `80 HiddenSlideBegin` |
| `long` interior node, `pos: "auto"` or tap | `21 SlideConnection` |
| `long` interior node, trace | `63 SlideConnectionTrace` |
| `long` interior node, `visible: false` | `122 Hidden` |
| `long` last node, visible, tap/flick/trace | `22 / 42 / 62` |
| `long` last node, `visible: false` | `82 HiddenSlideEnd` |
| `guide` node 0, merged onto a same-position tap/flick/trace | `101 / 102 / 104` |
| `guide` node 0, not merged | `100 GuideBegin` |
| `guide` interior node | `63 SlideConnectionTrace`, or `122 Hidden` when hidden/non-auto |
| `guide` last node, `trace` | `105 GuideEndTrace`, else `103 GuideEnd` |
| every interior eighth note of a slide line | `120 Combo` (or `121 ComboSkip` inside a judgement window) |

`0 None` is declared but never emitted in this pack.

---

## 3. CONVERTED runtime format

`convert()` returns exactly these 19 top-level keys.

| Key | Type | Meaning |
|---|---|---|
| `format` | `string` | Always `"nnnotes.live-score/1"`. Consumers should verify it. |
| `mirror` | `bool` | The `mirror` argument, echoed. |
| `startNoteId` | `int` | The `start_note_id` argument, echoed. |
| `laneCount` | `int` | Always `24` (`LANE_COUNT`). Valid lanes are `0..23`. |
| `judgementNoteCount` | `int` | Number of notes with `judgement == true`. Equals the Master `_fullComboCount` for all 12 local charts (section 6). |
| `counts` | `object` | `{opName: count}` histogram over **all** notes (judged and non-judged), sorted by `op`. Keys are `opName` strings; `sum(counts.values()) == len(notes)`. |
| `notes` | `array` | Every runtime note, ascending by `timeMs`; see section 3.1. |
| `lines` | `array` | `{"lineId": int, "type": "long" or "guide", "noteIds": [int], "comboIds": [int]}`, sorted by `lineId`. A line is `"guide"` when `lineId > startNoteId + 10000`. `noteIds` are the line's begin/end notes; `comboIds` its generated `Combo`/`ComboSkip` notes. |
| `bpmChanges` | `array` | `{"bpm": float, <position>}` per BPM event, in tick order. `0109_03`: one entry, `bpm 153.0` at `timeMs 0`. |
| `barChanges` | `array` | Same shape with `beatsPerBar: float` instead of `bpm` (from `sig`; `num * 4 / den`). `0109_03`: one entry, `4.0` at `timeMs 0`. |
| `tickSegments` | `object` | `{"bpm": [{"startTick", "startTimeMs", "bpm"}], "sig": [{"startTick", "startBar", "ticksPerMeasure", "numerator", "denominator"}]}` - the converter's internal tick-to-time segments, useful for reproducing `timeMs` exactly. |
| `barLineTimeMs` | `array[int]` | `timeMs` of the head of every bar `0..maxBar`; the index is the bar number. `0109_03`: 66 entries, `[0, 1568, 3137, ...]`. |
| `fever` | `array` | `{"index": int, "start": <position>, "end": <position>}` sorted by start tick, where `<position>` is the 6-key object below. |
| `skill` | `array` | `{"index": int, <position>}` per skill tick - exactly what the AP model needs. `0109_03` times: `9411, 21960, 47058, 59607, 84705`. |
| `call` | `array` | `{<position>, "rhythms": [float]}`; the `rhythms` values are the `(i+1)/n` fractions of `timing` entries equal to 1. `0109_03`: one entry at `timeMs 0` with `rhythms [0.5, 1.0]`. |
| `lastNoteTimeMs` | `int` | `max(timeMs)` over all notes, `0` when there are none. `0109_03`: `103137`. |
| `log` | `array[string]` | Conversion diagnostics; see section 6. Empty for all 12 local charts. |

`<position>` is always `{"bar": int, "rhythm": int, "rhythmicUnit": int,
"barProgress": float, "timeMs": int}` with `barProgress == rhythm /
rhythmicUnit` as float32.

### 3.1 `notes[]`

Two field sets exist: a **base note** (863 of 1051 rows in `0109_03`) which also
carries the raw-chart fields `tick`/`laneIndexFloat`/`visible`/`alpha`/`src`, and
a **`SlideComboNote`** row (188 of 1051, `op` 120/121) which carries
`lineBeginId`/`lineEndId` instead. The first 27 fields are present on every row.

| Field | Type | Meaning |
|---|---|---|
| `id` | `int` | Runtime note id, unique per chart. Base ids increment from `startNoteId`; slide combos use `lineId + k*10000 + 10000`; guide lines use `startNoteId + 10001 + n`. |
| `op` | `int` | Game `NoteOperateType` value that drives scoring. See section 3.3. |
| `opName` | `string` | `OP_NAMES[op]` - the same string used as a `counts` key. |
| `timeMs` | `int` | Judgement time in milliseconds, `floor()` of the float32 recomputation (`MusicScoreUtility.GetTimeMsFromBar`). The scoring model keys entirely off this. |
| `bar` | `int` | Bar index. |
| `rhythm` | `int` | Sub-bar tick offset within the measure. |
| `rhythmicUnit` | `int` | Ticks per measure of the enclosing segment (`1920` for 4/4, `8` for generated combo notes). |
| `barProgress` | `float` | `rhythm / rhythmicUnit` as float32. |
| `laneStart` | `int` | Left lane index (rounded). For normal notes equals `laneIndex`; combo notes use `floor()`. |
| `laneEnd` | `int` | Right lane index, inclusive. |
| `laneStartFloat` | `float` | Unrounded left edge (can be fractional, e.g. `12.5`). |
| `laneEndFloat` | `float` | Unrounded right edge; may be *less* than `laneStartFloat` on hidden/degenerate nodes. |
| `width` | `float` | `laneEndFloat - laneStartFloat + 1` as float32 (combos: `(le - ls) + 1`); can be `0.0`. |
| `critical` | `bool` | Critical (easy) note; selects the `EasyNormal` judgement and a different scoring offset. |
| `direction` | `string` | `Normal` / `Left` / `Right` - flick direction after mirroring. |
| `slideAlong` | `bool` | `true` for a chain node whose position was `"auto"` (it slides along its line and is not judged at that tick). |
| `lineIds` | `array[int]` | Runtime line ids this note belongs to (empty for plain taps/flicks/traces). |
| `lineEase` | `string` | `Linear` / `EaseIn` / `EaseOut` for the left edge. |
| `lineEaseR` | `string` | Same for the right edge. |
| `pairNoteId` | `int` | Id of the simultaneous partner note (`IsPairNoteType` pairs sharing a position), else `0`. |
| `fever` | `int` or `null` | Index into `fever[]` when `fever.start.ms <= timeMs <= fever.end.ms`, else `null`. |
| `judgement` | `bool` | **Scoring flag.** `true` iff the note is judged and therefore counts toward combo/score: `is_judgement_note(op) == op not in (0, 80, 82, 100, 103, 121, 122, 123)`. Only `judgement == true` notes are joined with `MasterLiveNoteParameter`. |
| `flick` | `bool` | `is_flick_type(op)`: `op in (40, 41, 42, 102)`. |
| `judgementType` | `string` or `null` | `JUDGEMENT_TYPE_NAMES[judgement_type(op, critical)]`; `null` only for `op 123`. For non-judged notes it is still a name (e.g. `op 82`/`op 122` -> `"Normal"`). |
| `judgementAreaOffset` | `string` | `Default` / `Slide` / `SlideBegin` / `SlideEnd` / `Flick` / `Trace` / `SlideMin` / `SlideMax` / `EasyDefault` / `EasySlideBegin`; the note's offset-table selector. |
| `tick` | `int` | Source tick in the raw chart. *Base notes only.* |
| `laneIndexFloat` | `float` | Same value as `laneStartFloat` under the `NoteInfo` name. *Base notes only.* |
| `widthFloat` | `float` | Same value as `width`. *Base notes only.* |
| `laneIndex` | `int` | Same value as `laneStart`. *Base notes only.* |
| `laneWidth` | `int` | `max(1, round(widthFloat))` - integer lane span, always `>= 1` even when `width` is `0.0`. *Base notes only.* |
| `lineIndices` | `array[int]` | Line **slot** indices `0..19` (`MAX_LINES = 20`), as opposed to the runtime `lineIds`. *Base notes only.* |
| `visible` | `bool` | Raw-chart visibility. *Base notes only.* |
| `alpha` | `string` | `none` / `in` / `out`. *Base notes only.* |
| `src` | `array[int]` | Traceability path into the raw chart: `[noteIndex]` or `[noteIndex, nodeIndex]`. `[13]` means `score.notes[13]`; `[315, 0]` means `score.notes[315].node[0]`. *Base notes only.* |
| `lineBeginId` | `int` | Id of the slide-begin note that owns this combo. *`SlideComboNote` rows only.* |
| `lineEndId` | `int` | Id of the slide-end note that owns this combo. *`SlideComboNote` rows only.* |

Base row example - `op 1`, critical, paired, judged:

```json
{"id": 366, "op": 1, "opName": "Normal", "timeMs": 43529, "bar": 27, "rhythm": 1440,
 "rhythmicUnit": 1920, "barProgress": 0.75, "laneStart": 12, "laneEnd": 17,
 "laneStartFloat": 12.0, "laneEndFloat": 17.0, "width": 6.0, "critical": true,
 "direction": "Normal", "slideAlong": false, "lineIds": [], "lineEase": "Linear",
 "lineEaseR": "Linear", "pairNoteId": 367, "fever": null, "judgement": true,
 "flick": false, "judgementType": "EasyNormal", "judgementAreaOffset": "EasyDefault",
 "tick": 53280, "laneIndexFloat": 12.0, "widthFloat": 6.0, "laneIndex": 12,
 "laneWidth": 6, "lineIndices": [], "visible": true, "alpha": "none", "src": [1]}
```

`SlideComboNote` example - no `src`/`tick`/`visible`, has the line ids:

```json
{"id": 10001, "op": 120, "opName": "Combo", "timeMs": 3333, "bar": 2, "rhythm": 1,
 "rhythmicUnit": 8, "barProgress": 0.125, "laneStart": 12, "laneEnd": 23,
 "laneStartFloat": 12.5, "laneEndFloat": 23.0, "width": 11.5, "critical": false,
 "direction": "Normal", "slideAlong": false, "lineIds": [1], "lineEase": "Linear",
 "lineEaseR": "Linear", "pairNoteId": 0, "fever": null, "judgement": true,
 "flick": false, "judgementType": "Trace", "judgementAreaOffset": "Slide",
 "lineBeginId": 1, "lineEndId": 3}
```

Non-judged, non-scoring note example - `Hidden`, from hidden interior node
`[389, 1]` of a long chain. A calculator must ignore it:

```json
{"id": 21, "op": 122, "opName": "Hidden", "timeMs": 9607, "bar": 6, "rhythm": 239,
 "rhythmicUnit": 1920, "barProgress": 0.12447916716337204, "laneStart": 6,
 "laneEnd": 11, "laneStartFloat": 6.0, "laneEndFloat": 11.0, "width": 6.0,
 "critical": false, "direction": "Normal", "slideAlong": false, "lineIds": [7],
 "lineEase": "Linear", "lineEaseR": "Linear", "pairNoteId": 0, "fever": null,
 "judgement": false, "flick": false, "judgementType": "Normal",
 "judgementAreaOffset": "Default", "tick": 11759, "laneIndexFloat": 6.0,
 "widthFloat": 6.0, "laneIndex": 6, "laneWidth": 6, "lineIndices": [0],
 "visible": false, "alpha": "none", "src": [389, 1]}
```

### 3.2 The `judgement` flag

`judgement` is computed from the op alone (`is_judgement_note`), not from the raw
`visible` flag and not from whether a `MasterLiveNoteParameter` row exists. It is
the single filter a scorer should apply - and "judged" and "scored" coincide here
because the parameter table covers exactly the judged op set (section 3.3).
Non-judged notes are still fully populated (position, lanes, line ids) so a
renderer can draw them; they must be excluded from combo/score accounting.

The exclusion set is `(0, 80, 82, 100, 103, 121, 122, 123)`: the two hidden-slide
endpoints, both plain guide endpoints, the skipped combo, hidden notes, and the
two never-emitted values.

### 3.3 `op` values, meanings, and scoring behaviour

| `op` | `opName` | `judgement` | `MasterLiveNoteParameter._scorePercent` | Origin in the raw chart |
|---|---|---|---|---|
| `0` | `None` | `true` | *(no row)* | Declared only; never emitted. |
| `1` | `Normal` | `true` | 100 | single `tap` (or `type` absent). |
| `20` | `SlideBegin` | `true` | 100 | `long` first node, visible, tap. |
| `21` | `SlideConnection` | `true` | 10 | `long` interior node, tap or `pos: "auto"`. |
| `22` | `SlideEnd` | `true` | 100 | `long` last node, visible, tap. |
| `40` | `Flick` | `true` | 100 | single `flick`. |
| `41` | `SlideBeginFlick` | `true` | 100 | `long` first node, visible, `type: flick`. |
| `42` | `SlideEndFlick` | `true` | 100 | `long` last node, visible, `type: flick`. |
| `60` | `Trace` | `true` | 10 | single `trace`. |
| `61` | `SlideBeginTrace` | `true` | 10 | `long` first node, visible, `type: trace`. |
| `62` | `SlideEndTrace` | `true` | 10 | `long` last node, visible, `type: trace`. |
| `63` | `SlideConnectionTrace` | `true` | 10 | `long`/`guide` interior node, `type: trace` or `pos: "auto"`. |
| `80` | `HiddenSlideBegin` | `false` | *(no row)* | `long` first node with `visible: false`. |
| `82` | `HiddenSlideEnd` | `false` | *(no row)* | `long` last node with `visible: false`. |
| `100` | `GuideBegin` | `false` | *(no row)* | `guide` first node not merged onto a same-position tap/flick/trace. |
| `101` | `GuideBeginNormal` | `true` | 100 | `guide` first node merged onto a same-position `tap`. |
| `102` | `GuideBeginFlick` | `true` | 100 | `guide` first node merged onto a same-position `flick`. |
| `103` | `GuideEnd` | `false` | *(no row)* | `guide` last node, not `trace`. |
| `104` | `GuideBeginTrace` | `true` | 10 | `guide` first node merged onto a same-position `trace`. |
| `105` | `GuideEndTrace` | `true` | 10 | `guide` last node, `type: trace`. |
| `120` | `Combo` | `true` | 10 | synthetic interior eighth note of a slide line. |
| `121` | `ComboSkip` | `false` | *(no row)* | synthetic slide eighth note suppressed inside a judgement window. |
| `122` | `Hidden` | `false` | *(no row)* | hidden interior chain node (raw `"visible": false` on a `node`). |
| `123` | `InvalidHidden` | `false` | *(no row)* | Declared only; never emitted. |

The parameter table has 16 rows covering exactly
`{1, 20, 21, 22, 40, 41, 42, 60, 61, 62, 63, 101, 102, 104, 105, 120}` - every
`judgement == true` op and no non-judged op. `_scorePercent` is `100` for
`1/20/22/40/41/42/101/102` and `10` for `21/60/61/62/63/104/105/120`. The model
layer then adds `MasterLiveComboScoreBonus` (`+1%` per 10-combo threshold reached,
`_comboBonusType == 0`).

---

## 4. Observed distributions - `charts/0109_03.json` (expert, song 100109)

| Quantity | Value |
|---|---|
| Notes in `notes[]` | **1051** |
| Judged notes (`judgement == true`), i.e. `judgementNoteCount` | **759** |
| Non-judged notes | 292 |
| Base-note rows / `SlideComboNote` rows | 863 / 188 |
| Bars (`len(barLineTimeMs)`) | 66 |
| `lastNoteTimeMs` | 103137 |
| `skill` entries | 5 |
| `log` entries | 0 |

`counts` verbatim:

```json
{"Normal": 258, "SlideBegin": 137, "SlideEnd": 78, "Flick": 14, "SlideEndFlick": 8,
 "Trace": 31, "SlideEndTrace": 43, "HiddenSlideBegin": 3, "HiddenSlideEnd": 5,
 "GuideBeginNormal": 3, "GuideEnd": 3, "Combo": 187, "ComboSkip": 1, "Hidden": 280}
```

`op` histogram across judged notes only (`{op: count}`), summing to 759:

```json
{"1": 258, "20": 137, "22": 78, "40": 14, "42": 8, "60": 31, "62": 43, "101": 3, "120": 187}
```

Judged totals by family: `Normal` 258; slides 215 (`20` 137 + `22` 78); `Flick`
14; `SlideEndFlick` 8; `Trace` 31; `SlideEndTrace` 43; `GuideBeginNormal` 3;
`Combo` 187. The `Combo` share (187/759 = 24.6%) is why "note count" is not a
measure of chart length for scoring.

`op` histogram across **all** notes (sums to 1051), for completeness:

```json
{"1": 258, "20": 137, "22": 78, "40": 14, "42": 8, "60": 31, "62": 43, "80": 3,
 "82": 5, "101": 3, "103": 3, "120": 187, "121": 1, "122": 280}
```

`judgement` flag by op in this chart, `{op: {flag: count}}`:

```json
{"1": {"true": 258}, "20": {"true": 137}, "22": {"true": 78}, "40": {"true": 14},
 "42": {"true": 8}, "60": {"true": 31}, "62": {"true": 43}, "80": {"false": 3},
 "82": {"false": 5}, "101": {"true": 3}, "103": {"false": 3},
 "120": {"true": 187}, "121": {"false": 1}, "122": {"false": 280}}
```

Note the near miss: `120 Combo` is judged, `121 ComboSkip` is not, even though
both are generated by the same slide-end expansion. Only one `ComboSkip` occurs
in this chart (id 20065, `timeMs` 35784).

---

## 5. Fields consumed by the scoring model (`model.py`)

`model.py` touches a converted chart only through `chart_weights()`:

| model.py line | Field consumed | Purpose |
|---|---|---|
| 94 | `runtime['notes']` | whole array |
| 95 | `n['judgement']` | filters `notes` down to judged notes, keeping file order |
| 96 | `MasterLiveNoteParameter._noteOperateType` | builds the `{op: row}` join table keyed by the same `op` values the converter emits |
| 98 | `n['timeMs']` | `np.array(..., dtype=np.int64)` - the timeline for combo counting and skill windows |
| 102 | `n['timeMs']` | `np.searchsorted(times, times, side='left')` - prior-combo count; chords share a combo |
| 103 | `n['op']` | `noteparam[n['op']]['_scorePercent'] / 100`, then multiplied by the combo bonus |
| 109 | `c['skill_times_ms']` | the five skill start times from the chart manifest in `inputs.json`, filled by `prepare.py` line 119 as `[s['timeMs'] for s in runtime['skill']]` |
| 109-113 | `times >= start` / `times < start + duration` | window `[skillTimeMs, +durationMs)` over judged `timeMs` values |

So the model's complete dependency surface on a converted chart is
`notes[].timeMs`, `notes[].op`, `notes[].judgement` and `skill[].timeMs`;
everything else (`lane*`, `lines`, `bpmChanges`, `tickSegments`, `fever`, `src`,
...) is diagnostic or rendering data and is not scored.

`prepare.py` additionally asserts per chart:
`runtime['judgementNoteCount'] == score['_fullComboCount']` (line 111),
`runtime['log'] == []` (line 112), and
`sorted(x['index'] for x in runtime['skill']) == list(range(5))` (line 113), plus
that judged notes are sorted by `timeMs` (line 115).

---

## 6. Skill count, `log` diagnostics, and Master count agreement

### 6.1 Skills are exactly 5 in every local chart

`skill[]` has one entry per `score.events.skill` tick
(`skills = [(i, make_position(t, tc)) for i, t in enumerate(ev.skill)]`), so the
count equals the raw chart's skill-event count - it is **not** a hard converter
invariant, it just happens to be 5 for every chart in this event pack. A general
validator must assert it explicitly. Verified across all 12 files in `charts/`:

| Chart | Difficulty | `len(skill)` | `index` values | `[s['timeMs'] for s in skill]` |
|---|---|---|---|---|
| `charts/0056_00.json` | easy | 5 | 0..4 | 7382, 16178, 36282, 61413, 81518 |
| `charts/0056_01.json` | normal | 5 | 0..4 | 7382, 16178, 36282, 61413, 81518 |
| `charts/0056_02.json` | hard | 5 | 0..4 | 7382, 16178, 36282, 61413, 81518 |
| `charts/0056_03.json` | expert | 5 | 0..4 | 7382, 16178, 36282, 61413, 81518 |
| `charts/0063_00.json` | easy | 5 | 0..4 | 13186, 36923, 55219, 65934, 87032 |
| `charts/0063_01.json` | normal | 5 | 0..4 | 13186, 36923, 55219, 65934, 87032 |
| `charts/0063_02.json` | hard | 5 | 0..4 | 13186, 36923, 55219, 65934, 87032 |
| `charts/0063_03.json` | expert | 5 | 0..4 | 13186, 36923, 55219, 65934, 87032 |
| `charts/0109_00.json` | easy | 5 | 0..4 | 9411, 21960, 47058, 59607, 84705 |
| `charts/0109_01.json` | normal | 5 | 0..4 | 9411, 21960, 47058, 59607, 84705 |
| `charts/0109_02.json` | hard | 5 | 0..4 | 9411, 21960, 47058, 59607, 84705 |
| `charts/0109_03.json` | expert | 5 | 0..4 | 9411, 21960, 47058, 59607, 84705 |

All 12 also satisfy: `notes[]` non-decreasing by `timeMs`; judged subsequence
non-decreasing; `sum(counts.values()) == len(notes)`;
`judgementNoteCount == count of judgement == true`.

### 6.2 `log` diagnostics

`log` is appended in three places; a non-empty log means the converter dropped or
flagged something, so a consumer needing faithful scoring input should reject the
chart.

| Trigger | Emitted text | Source |
|---|---|---|
| More than 2 judged notes on the same key (bar + float32 barProgress) and lane while building the note dictionary | `3 bar {bar} barProgress {progress} lane {lane} op {opName}` (the literal `3` is the game's error code) | `_info_dictionary()`, `chart_converter.py` line 648 |
| An `op 122 Hidden` note whose line id is registered on neither the long-line nor the guide-line array | `noteId {id} pos {Pos(...)}` | `_Creator.create()`, line 770 |
| Any `op` the creator has no branch for | `noteId {id} type {opName}` | `_Creator.create()`, line 777 |

The third kind means that note is **omitted from `notes[]`** entirely
(`create()` returns `None`), which would also break `judgementNoteCount` against
the Master count - so a non-empty `log` is a "silently missing notes" signal too.

**All 12 local charts have `log == []`**, and `judgementNoteCount` equals the
Master `_fullComboCount` for every one of them (values from `inputs.json`, which
`prepare.py` wrote only after its assertions passed):

| Chart | Difficulty | `judgementNoteCount` | Master `_fullComboCount` | Match | `log` |
|---|---|---|---|---|---|
| `charts/0056_00.json` | easy | 364 | 364 | yes | `[]` |
| `charts/0056_01.json` | normal | 488 | 488 | yes | `[]` |
| `charts/0056_02.json` | hard | 651 | 651 | yes | `[]` |
| `charts/0056_03.json` | expert | 843 | 843 | yes | `[]` |
| `charts/0063_00.json` | easy | 304 | 304 | yes | `[]` |
| `charts/0063_01.json` | normal | 492 | 492 | yes | `[]` |
| `charts/0063_02.json` | hard | 615 | 615 | yes | `[]` |
| `charts/0063_03.json` | expert | 718 | 718 | yes | `[]` |
| `charts/0109_00.json` | easy | 272 | 272 | yes | `[]` |
| `charts/0109_01.json` | normal | 350 | 350 | yes | `[]` |
| `charts/0109_02.json` | hard | 600 | 600 | yes | `[]` |
| `charts/0109_03.json` | expert | 759 | 759 | yes | `[]` |

No mismatches were found: every chart's `counts` sum equals its `len(notes)`,
every judged `op` in all 12 charts resolves to a `MasterLiveNoteParameter` row,
and the judged op domain over all 12 charts is
`{1, 20, 21, 22, 40, 42, 60, 62, 63, 101, 104, 120}` (12 of the 16 table rows;
`41`, `61`, `102`, `105` are unused by this pack).

---

## 7. Integration notes: what a general calculator must validate

When accepting a converted chart (from `convert_chart.py` or any producer
claiming `format == "nnnotes.live-score/1"`):

1. **Format tag.** `runtime['format'] == "nnnotes.live-score/1"`. Refuse other versions.
2. **Count match.** `runtime['judgementNoteCount'] == sum(1 for n in notes if n['judgement'])`
   and, when a Master row is available,
   `judgementNoteCount == MasterLiveMusicScore['_fullComboCount']`. A mismatch
   means notes were dropped (see `log`) or the wrong difficulty file was used.
3. **Diagnostics.** `runtime['log'] == []`. Any entry means dropped or flagged
   notes; treat it as fatal for scoring.
4. **Sorted times.** `notes[]` ascending by `timeMs`, and the judged subsequence
   ascending too. `model.py` line 102 uses `searchsorted(..., side='left')`, which
   silently yields wrong combo weights on an unsorted array.
5. **Exactly 5 skill times.** `len(runtime['skill']) == 5` and
   `sorted(s['index'] for s in runtime['skill']) == [0, 1, 2, 3, 4]`, each a
   distinct ascending `timeMs` within `[0, lastNoteTimeMs]`. Check this
   explicitly; the converter only mirrors the raw event count.
6. **Op coverage.** Every `n['op']` with `n['judgement'] == True` must have a row
   in the note-parameter table (`MasterLiveNoteParameter._noteOperateType`, keyed
   by `op`). Raise on an unknown op rather than defaulting its score to 0 or 100.
7. **Non-judged notes are inert.** Filter on `judgement`, not on `opName`, and do
   not let `Hidden`, `ComboSkip`, `HiddenSlide*`, `GuideEnd` or `GuideBegin`
   inflate the combo count. `Combo` (120) *is* judged and *does* count.
8. **Lane sanity.** `0 <= laneStart <= laneEnd <= laneCount - 1` for judged
   notes; `laneEndFloat` may be fractional and `width` may be `0.0` on hidden
   nodes, so prefer `laneWidth`/`laneIndex` for integer geometry.
9. **Combo semantics.** Chords at the same `timeMs` share the prior combo count
   (`FindLastIndexBefore`; `model.py` lines 99-103). Do not compute combo as a
   running index over the filtered array if you need to match the runtime.
10. **Determinism.** `convert()` is pure, so the same raw bytes must always
    produce the same runtime dict. Record the raw source `sha256` (as
    `prepare.py` does via `inputs.json` -> `charts[].source_sha256`) so a mismatch
    is diagnosable.

---

## Appendix: constants and enumerations from the converter

| Constant | Value | Meaning |
|---|---|---|
| `LANE_COUNT` | 24 | `MusicScore.LaneCount`. |
| `TICKS_PER_BEAT` | 480 | tick-to-time conversion denominator. |
| `TICKS_PER_WHOLE` | 1920 | ticks per 4/4 measure. |
| `MAX_LINES` | 20 | line slots; exceeding it raises `ss lineIndex ...`. |
| `DIFFICULTIES` | `("easy", "normal", "hard", "expert")` | maps to file suffixes `_00.._03`. |

`DIRECTION_NAMES = {0: "Normal", 1: "Left", 2: "Right"}`;
`LINE_EASE_NAMES = {0: "Linear", 1: "EaseIn", 2: "EaseOut"}` (note the swapped
meaning: chart `"in"` -> 2 and chart `"out"` -> 1 via `ConvertEase`);
`JUDGEMENT_TYPE_NAMES = {0: "None", 1: "Normal", 2: "EasyNormal", 5: "Flick",
10: "SlideBegin", 11: "SlideEnd", 12: "SlideEndFlick", 15: "SlideBeginEasy",
21: "Trace", 22: "SlideEndTrace"}`; `OFFSET_TYPE_NAMES = {0: "Default",
1: "Slide", 2: "SlideBegin", 3: "SlideEnd", 4: "Flick", 5: "Trace",
6: "SlideMin", 7: "SlideMax", 8: "EasyDefault", 9: "EasySlideBegin"}`.
