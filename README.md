# Listening Wrapped for Rockbox

A Spotify Wrapped–style year in review, built from Rockbox's own playback log.

## Install on the iPod

1. Check your Rockbox version on the iPod: System → Rockbox Info.
2. Download the matching file from the
   [latest release](../../releases/latest). The file name says which Rockbox
   version and which iPod it's for:

   | Your iPod | Rockbox 4.0 | Development build (Nov 2026 or later) |
   |---|---|---|
   | iPod Video (5G / 5.5G) | `wrapped-4.0-ipodvideo.rock` | `wrapped-dev-ipodvideo.rock` |
   | iPod Classic (6G / 6.5G / 7G) | `wrapped-4.0-ipod6g.rock` | `wrapped-dev-ipod6g.rock` |
   | iPod Nano 2nd gen | `wrapped-4.0-ipodnano2g.rock` | `wrapped-dev-ipodnano2g.rock` |
   | iPod Nano 1st gen | `wrapped-4.0-ipodnano1g.rock` | `wrapped-dev-ipodnano1g.rock` |

   A plugin only loads on the Rockbox version it was built for. With the
   wrong one, the iPod says "Incompatible version".
3. Rename it to `wrapped.rock` and copy it to `/.rockbox/rocks/apps/` on the iPod.
4. On the iPod: Plugins → Applications → wrapped.

For any other Rockbox version, build it yourself from the patch (see
[Building from source](#building-from-source)).

### Headline fonts (optional)

Wrapped shows its big numbers in `27-Adobe-Helvetica-Bold` (Video/Classic) or
`21-Adobe-Helvetica-Bold` (Nano) if one is in `/.rockbox/fonts/`. Both come
with the Rockbox font pack. If you installed Rockbox with Rockbox Utility and
ticked "Fonts", you already have them. Otherwise download the font pack from
[rockbox.org/download](https://www.rockbox.org/download/) and extract it to the
root of the iPod. Without these fonts, Wrapped uses your normal UI font.

## Usage notes

- First run offers to switch on Settings → Playback Settings → Logging if it is off.
- Each run copies new entries from `playback.log` (and rotated `playback_NNNN.log`
  files) into `/.rockbox/wrapped_history.log`. That file is the permanent history,
  so it survives the LastFm Scrobbler deleting the playback log. If you use the
  scrobbler, open Wrapped first: its "Delete playback log" option is **on by
  default** and also removes the rotated `playback_NNNN.log` files, so plays
  Wrapped hasn't saved yet would be lost. (Plugins can't run in the background,
  so Wrapped can't do this on its own.)
- While copying, it reads each new track's tags once and saves them on the same
  line, after the path, separated by tabs:
  `timestamp:elapsed_ms:length_ms:/path<TAB>artist<TAB>album artist<TAB>album<TAB>title<TAB>genre`.
  Each tag is capped at 120 bytes, and tabs or newlines inside tags become spaces.
  Lines from older versions have no tags and still work.
- Menu: this year, last year, all time, and **Save summary**, which writes
  `/.rockbox/wrapped_<year>.txt`.
- On the slides: Select or scroll forward = next, Left or scroll back = previous,
  Menu = back to the menu.

## How it tracks plays

- A play is 30 seconds or more. Shorter starts on longer tracks count as skips.
- Stopping mid-track and resuming later logs two entries; they are merged into one play.
- Minutes are real elapsed listening time.
- Audiobooks and podcasts aren't counted. That covers anything with a genre
  containing "audiobook", "audio book", "spoken" or "podcast"; anything inside a
  folder named `Audiobooks`, `Audiobook`, `Audio Books`, `Podcasts` or `Podcast`;
  and any `.m4b` file. They stay in the history file; they're just skipped.
- Artist, album and title come from the tags saved in the history. If a song's
  tags change, its newest saved tags win. Lines without tags are looked up from
  the database (if loaded in RAM) or the file itself, falling back to
  `/Artist/Album/NN Title.ext` folder names.
- Songs with several artists separated by `;` (like `Lana Del Rey; Bleachers`)
  count in full for each artist. A name like "AC/DC" or "Simon & Garfunkel" is
  never split.
- Dates come from the iPod's clock. If the clock resets (an iPod's clock can go
  back to an old date when the battery runs flat), plays are still saved, but
  they count under the date the iPod thought it was.

## Desktop version

Drop `wrapped_history.log` (or `playback.log`) onto the Rockbox Wrapped web page
for the same stats with charts and a daily heatmap. It uses the tags saved in
`wrapped_history.log`. A raw `playback.log` has only file paths, so plays from
it use folder names.

## Building from source

You only need this for a Rockbox version without a ready-made download.

The repo has the plugin in two forms:

- `wrapped.c` is the plugin's source code, for reading.
- `rockbox-wrapped.patch` adds the plugin to a copy of the Rockbox source in
  one step. It adds `wrapped.c` to `apps/plugins/` and makes two one-line
  edits: in `SOURCES` it builds the plugin (only for players with a clock),
  and in `CATEGORIES` it lists the plugin under Applications. The patch was
  made against Rockbox commit `acb44be` and also applies cleanly to the 4.0
  release (`v4.0-final`).

Rockbox builds on Linux (or WSL on Windows) and needs its ARM cross-compiler.
To install the cross-compiler, run `tools/rockboxdev.sh` once from the Rockbox
folder and pick the ARM toolchain. See the
[Rockbox build guide](https://www.rockbox.org/wiki/HowToCompile) for details.

```
git clone https://github.com/Rockbox/rockbox.git && cd rockbox
git checkout v4.0-final        # the Rockbox version your player runs
git apply /path/to/rockbox-wrapped.patch
mkdir build && cd build
../tools/configure --target=ipodvideo --type=n   # or ipod6g, ipodnano2g, ipodnano1g
make -j8                       # result: apps/plugins/wrapped.rock
```

To rebuild only the plugin after changing `wrapped.c`, run
`make "$PWD/apps/plugins/wrapped.rock"` in the build folder.

## License

Copyright (C) 2026 Michael Meixner

Licensed under the GNU General Public License, version 2 or (at your option)
any later version, the same terms as Rockbox itself. See [LICENSE](LICENSE).
