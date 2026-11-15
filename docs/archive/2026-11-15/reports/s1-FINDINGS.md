# Font claims verification — upstream dbd319a / AnimeEffects

Read-only investigation. HEAD `c9ec3c3` (feat/export-ui-redesign), `upstream/master` `abb4b88`.
Tools: `fc-query`/`fc-scan` (fontconfig), pure-Python `struct` sfnt parser (fontTools NOT installed),
and **Qt 6.11.1 itself** (`pkg-config Qt6Core/Gui/Widgets 6.11.1`, `QT_QPA_PLATFORM=offscreen`)
compiled against the extracted fonts — the real renderer.

Scratch: `.git/dsh-scratch/s1/` (`parse.py`, `deep.py`, `tnum.py`, `summary.py`, `probe2.cpp`..`probe5.cpp`, `cjk_deep.txt`).

## Blob identity facts
- `HEAD:assets/fonts/Geist-{Regular,Medium,SemiBold,Bold}.ttf` are **byte-identical** to
  `dbd319a^:data/fonts/…` (same blob SHAs: cff0681b, ef03d3f7, 74469484, 2aef2078).
- `dbd319a:data/fonts/NotoKR.ttf` == `upstream/master:data/fonts/NotoKO.ttf` (blob `36e634d2…`) — pure rename.
- `dbd319a:data/locale/preference_kr.txt` = `font_family = NotoKR`; renamed to `preference_ko.txt`
  (`font_family = NotoKO`) in commit `09c75b3` (A preference_ko.txt / D preference_kr.txt).
- upstream `preference_ja.txt` → `NotoJP`; `preference_zh.txt` → `NotoCN`; `preference_ko.txt` → `NotoKO`.

## 1. Name tables (all values raw, platform 3 / encoding 1 / lang 0x0409 — the only records present)

| file | bytes | 1 family | 2 subfam | 4 full | 6 PS | 16 typoFamily | 17 typoSubfam |
|---|---|---|---|---|---|---|---|
| upstream Geist.ttf | 168804 | Geist | Regular | Geist Regular | Geist-Regular | (absent) | (absent) |
| head Geist-Regular.ttf | 126072 | Geist | Regular | Geist Regular | Geist-Regular | (absent) | (absent) |
| head Geist-Medium.ttf | 127684 | Geist | Medium | Geist Medium | Geist-Medium | (absent) | (absent) |
| head Geist-SemiBold.ttf | 90420 | Geist | SemiBold | Geist SemiBold | Geist-SemiBold | (absent) | (absent) |
| head Geist-Bold.ttf | 128848 | Geist | Bold | Geist Bold | Geist-Bold | (absent) | (absent) |
| upstream NotoJP.ttf | 12912444 | **Noto Serif JP ExtraLight** | Regular | Noto Serif JP ExtraLight | NotoSerifJP-ExtraLight | **Noto Serif JP** | ExtraLight |
| upstream NotoCN.ttf | 25053412 | **Noto Serif SC ExtraLight** | Regular | Noto Serif SC ExtraLight | NotoSerifSC-ExtraLight | **Noto Serif SC** | ExtraLight |
| upstream NotoKO.ttf | 10400436 | **Noto Sans KR Thin** | Regular | Noto Sans KR Thin | NotoSansKR-Thin | **Noto Sans KR** | Thin |

`NotoJP` / `NotoKR` / `NotoKO` / `NotoCN`: **0 occurrences** in any file, ASCII and UTF-16BE
(whole-file byte scan). CJK fonts expose `NotoSerifJP`, `NotoSerifSC`, `NotoSansKR` as name ID 25 only.

## 2. Qt matching (Qt 6.11.1, real execution)

All 4 CJK-name probes on registered fonts:
```
  hasFamily(NotoJP)     = 0        hasFamily(Noto Serif JP)          = 1
  hasFamily(NotoKR)     = 0        hasFamily(Noto Serif SC)          = 1
  hasFamily(NotoKO)     = 0        hasFamily(Noto Sans KR)           = 1
  hasFamily(NotoCN)     = 0        hasFamily(Noto Serif JP ExtraLight)= 1
  hasFamily(NotoSerifJP)= 0        hasFamily(NotoSerifJP-ExtraLight) = 0
  hasFamily(NotoSansKR) = 0        hasFamily(Geist Regular)          = 0
  hasFamily(noto serif jp) = 1     (family match is case-insensitive)
  QFont("NotoJP") -> QFontInfo.family()="Noto Sans" exactMatch=0
  QSS 'font-family: NotoJP;' -> QWidget::font().family()="NotoJP" (echoes request)
                                QFontInfo(w.font()).family()="Noto Sans" exactMatch=0
```
`addApplicationFont` registers **two** families per CJK file: `applicationFontFamilies` =
`[Noto Serif JP | Noto Serif JP ExtraLight]`, `[Noto Serif SC | Noto Serif SC ExtraLight]`,
`[Noto Sans KR | Noto Sans KR Thin]` (name ID 16 and name ID 1).

## 3. Styles / weights of the CJK fonts
- NotoJP.ttf = Noto Serif JP, variable `wght` 200..900 default **200** → ExtraLight, usWeightClass 200, panose[1]=2.
- NotoCN.ttf = Noto Serif SC, variable `wght` 200..900 default **200** → ExtraLight, usWeightClass 200, panose[1]=2.
- NotoKO.ttf = Noto Sans KR, variable `wght` 100..900 default **100** → Thin, usWeightClass 100, panose[1]=**11** (sans).
- All three: exactly 1 sfnt face, `fvar`+`gvar`+`HVAR`+`STAT`+`avar`; no `lnum/onum/tnum/pnum`.
- Geist (all 5 files): panose all zeros (no serif/sans classification in-file); Geist is Vercel's sans family.
- Qt exposes only the default instance: `styles("Noto Serif JP")=[ExtraLight]`, `styles("Noto Sans KR")=[Thin]`;
  `setWeight(Normal/Bold/Black)` on "Noto Serif JP" still yields styleName "ExtraLight".

## 4. Geist variable vs statics

### 4a. Geist.ttf IS a variable font
`fvar` raw header `0001 0000 0010 0002 0001 0014 0009 000a`; axis `wght` min=100 default=400 max=900
(nameID 256 "Weight"); 9 named instances Thin100 ExtraLight200 Light300 Regular400 Medium500
SemiBold600 Bold700 ExtraBold800 Black900. Tables: `fvar gvar HVAR MVAR STAT avar` present.
All four statics: no `fvar`.

### 4b. Figure features (GSUB FeatureList, 26 tags identical in all five Geist files)
`aalt case ccmp dlig dnom frac liga locl numr ordn **pnum** sinf ss01..ss11 subs sups **tnum**`
`lnum`, `onum`, `zero`: **ABSENT in all five files.**
- VF `tnum` → lookup 31 (SingleSubst, 10 entries): gids 644-653 → 676-685.
- VF `pnum` → lookup 30 (SingleSubst, 10 entries): gids 676-685 → 644-653.
- `post` names: gid 676-685 = `zero.tf one.tf two.tf … nine.tf` (**tabular figures**); gid 644-653 = default figures.
- LangSys featureIndex arrays list pnum+tnum for DFLT/latn, but that array is a *resolution map*, not a
  default-on list: the same parser also lists `subs`/`sups` for latn in Liberation Sans and `ss01` for
  Noto Serif Thai, which are demonstrably not applied by default. Decisive evidence is glyph-level (4c).
- OS/2: fsSelection 0x00c0 (REGULAR|USE_TYPO_METRICS) R/M/SB/Bold=0x00a0 for Bold (BOLD|USE_TYPO_METRICS);
  macStyle 0x0001 only for Bold; post.isFixedPitch=0; CJK fsSelection=0x0040.

### 4c. Digit advance widths, units/em = 1000 (pure Python hmtx + Qt px, scale 0.015983 px/unit)
| file | cmap gids 0-9 | advances 0..9 | uniform? | Qt px 0..9 |
|---|---|---|---|---|
| upstream/Geist.ttf | 644-653 | 663,384,619,613,615,626,593,524,604,593 | **NO** Δ279 | 10.59,6.14,9.89,9.80,9.83,10.02,9.48,8.38,9.66,9.48 |
| head/Geist-Regular.ttf | 676-685 | 600 ×10 | **YES** Δ0 | 9.59 ×10 |
| head/Geist-Medium.ttf | 676-685 | 612 ×10 | **YES** Δ0 | 9.78 ×10 |
| head/Geist-SemiBold.ttf | **644-653** | 683,427,642,637,643,656,615,538,644,618 | **NO** Δ256 | 10.92,6.83,10.27,10.19,10.28,10.48,9.83,8.59,10.30,9.88 |
| head/Geist-Bold.ttf | 676-685 | 636 ×10 | **YES** Δ0 | 10.17 ×10 |
| VF + tnum=1 | 676-685 | 600 ×10 | YES | 9.59 ×10 |
| SemiBold + tnum=1 | 676-685 | 624 ×10 | YES | 9.97 ×10 |

Qt px values reproduce the hmtx numbers exactly (e.g. 600/1000×16px = 9.59), independently validating the parser.
Three of four statics are tabular-by-default; **Geist-SemiBold.ttf is proportional-by-default**, like the VF.
`tnum` is a working opt-in in Qt: `QFont::setFeature(QFont::Tag("tnum"),1)` turns the VF into 10×9.59 px.

### 4d. Extra regressions of the switch
- `styles("Geist")` = `[Regular]` with Geist.ttf alone vs `[Regular, Medium, SemiBold, Bold]` with the statics;
  with the VF, `setWeight(Medium/DemiBold/Bold)` all resolve to styleName "Regular".
- Statics are hinted (`cvt `/`fpgm`/`prep`, version string "…; ttfautohint (v1.8.4.16-eb64)"); SemiBold has
  `prep` only. Geist.ttf has **no** hinting instructions (`prep`+`gasp` only).
- CJK digit advances: NotoJP/NotoCN 542,473,544,542,540,544,544,533,544,548 (proportional);
  NotoKO 521×10 (tabular).
