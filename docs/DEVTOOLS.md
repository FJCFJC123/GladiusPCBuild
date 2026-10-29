# GladiusPort Developer Tools

`devtools.dll` is an optional add-on for modders and developers. It gives the running game:

- an **in-game console** that reaches the game's own command and variable system;
- a separate **tools window** with live variables, logs, performance graphs, a **Dog script debugger** and a
  **quest flow editor**;
- readable **crash reports** (debug builds only).

Nothing here changes how the game plays unless you use it. It's off by default, and it never runs next to an
online match.

---

## Contents

- [Installing and switching it on](#installing-and-switching-it-on)
- [Safety: online play and release builds](#safety-online-play-and-release-builds)
- [Keys](#keys)
- [The in-game console](#the-in-game-console)
- [The tools window](#the-tools-window)
  - [Console](#console-tab), [Variables](#variables-tab), [Commands](#commands-tab), [Log](#log-tab)
  - [Graphs](#graphs-tab)
  - [Scripts: the Dog script debugger](#scripts-tab-the-dog-script-debugger)
  - [Quests: game variables and quest flow](#quests-tab-game-variables-and-quest-flow)
- [Console commands](#console-commands)
- [Crash reports and dumps](#crash-reports-and-dumps)
- [Settings (`gladiusport.ini`)](#settings-gladiusportini)
- [Recipes](#recipes)
- [Troubleshooting](#troubleshooting)

---

## Installing and switching it on

**With the builder:** tick **Developer tools** in GladiusPort Builder. It installs:

| File | What it is |
|---|---|
| `devtools.dll` | the tools themselves |
| `devtools\dog\` | the Dog script decompiler (Python), for the script debugger's source view |

The builder also looks for Python 3.8 or newer. If it finds one, it writes `[devtools] Python=` so the script
debugger can show readable source. Without Python everything still works, but scripts show as disassembly.

**By hand:** put `devtools.dll` next to `gladius.exe` and set:

```ini
[devtools]
Enabled=1
```

Unticking the option in the builder removes the files again. The decompiled-source cache in
`devtools\scripts\` is kept.

---

## Safety: online play and release builds

- **Online lockout.** As soon as you **host or join an online lobby**, the developer tools shut down completely:
  - every hook they placed in the game is taken out;
  - the tools window closes and the console stops working.

  They stay off until you restart the game. Memory readers and editors have no place next to an online match. You
  can't switch them back on mid-match. Restart without hosting or joining to use them again.
- **No dumps in release builds.** Public (release) builds never write memory dumps, crash reports or Windows Error
  Reporting dumps. A crash is written to `port.log` (code, address, registers and call addresses) and the game
  exits. In release builds the `crashreport` and `crashtest` commands are refused. Everything else in these tools
  works the same.
- **Debug builds** (for development) keep full crash dumps. See [Crash reports and dumps](#crash-reports-and-dumps).

---

## Keys

| Key | Where | What it does |
|---|---|---|
| `` ` `` (backtick) | game window | open / close the in-game console |
| `` Ctrl+` `` | game window | show / hide the tools window |
| `F5` | game or tools window | script debugger: continue |
| `F10` | game or tools window | script debugger: step over |
| `F11` / `Shift+F11` | game or tools window | script debugger: step / step out |

In the in-game console:

| Key | What it does |
|---|---|
| `Enter` | run the line |
| `Tab` | complete a command or variable name |
| `Up` / `Down` | command history |
| `Page Up` / `Page Down` | scroll the output |
| `Ctrl+L` | clear |
| `Esc` | close |

While the console is open, the keyboard types into it and doesn't move player 1.

Closing the tools window only hides it; `` Ctrl+` `` brings it back.

---

## The in-game console

Gladius shipped with its whole developer console engine: hundreds of commands and tunable variables. Retail threw
away every line of output and never drew the console. GladiusPort's console reconnects that engine:

- **Variables** are read and set by name: `camcloseness` shows the value, `camcloseness = 3` sets it.
- **Commands** run with their arguments: `wmWarp Pirgos`. `command ?` shows its usage.
- **Output** that retail discarded is shown: command replies, script prints, warnings.
- **Rebuilt commands.** Several of the game's list commands only printed to a debug screen retail removed. They're
  rebuilt to print into the console: `mem`, `heap`, `gvar`, `wmWarp`, `fx`, `panlist`, `paxlist`,
  `EventHandlers` and more.
- **Unregistered commands restored.** Commands whose code exists in retail but that were never registered are back:
  `unit`, `KeroOrder`, `StoryEvent`, `ChangeState`, `TweakUnit`, `calc`, `echo`, `alias` and `delete`.

Several lines can be run at once separated by `;`. `help` lists everything; `help text` and `find text` search.

---

## The tools window

The tools window has these tabs:

| Tab | What it's for |
|---|---|
| Console | the full console, with completion and history |
| Variables | every game variable, live and editable |
| Commands | every command, with a run box and usage |
| Log | `port.log` and game output, live |
| Graphs | frame times, renderer profiler, memory |
| Scripts | the Dog script debugger |
| Quests | game variables and quest flow |

### Console tab

The same console as in-game, with a larger output view, completion and history.

### Variables tab

Every console variable the game registered: name, type, range and the live value. Filter by name or description, and tick
**on/off switches only** to see just the toggles. Type a new value and press Enter; tick boxes flip switches.

### Commands tab

Every registered command with its description. Type arguments and press **Run**. **?** shows the command's usage
text.

### Log tab

Live views of `port.log` and **Game output** (prints from the game and its scripts, which retail discarded), with
**follow** and **Clear**. Script runtime errors always land in the Game output, with the script and offset.

### Graphs tab

- **Frame time:** wall time, CPU (game + draw submission) and time blocked in Present, plotted live. **Pause** freezes
  the plot.
- **Renderer breakdown:** tick **Renderer profiler** to time the renderer's CPU zones and GPU passes (shadows,
  lighting, bloom, ...) per frame. Every series can be shown or hidden from the legend, and **plot** pins one.
- **Memory:** live usage of the game's memory arenas and small-block heaps, with high-water marks and failed
  allocations.

### Scripts tab: the Dog script debugger

Gladius runs its game logic in **Dog**, LucasArts' scripting language: quests, conversations, town and world map
behaviour, battle events, AI moods. Retail compiled the Dog debugger out. This tab rebuilds one from the outside.

**Left column:**

- **Running scripts:** every script instance with its file, current function and state. Click one to see its code
  and variables.
- **Loaded script files:** every file in memory; click to view.
- **Breakpoints:** the list of breakpoints, with remove buttons.
- **Reset scripts** and **Event handlers**: see below.

**Code view:**

- **Source** (needs Python): the file is decompiled on first view and cached in `devtools\scripts\`. Lines map to
  bytecode, so stepping and breakpoints work per source line.
- **Disassembly:** the raw instructions. It always works, even without Python.
- A fading green **trace** shows which lines ran recently, with hit counts. **Follow** keeps the current line in
  view, and **Reset counts** clears them.

**Debugging:**

- Tick **Debugger on** (or run `scriptbreak`), then click left of a line to set a breakpoint.
- When a script stops, the game pauses with audio held. Then:

| Button | Key | What it does |
|---|---|---|
| Continue | F5 | run on |
| Step | F11 | run to the next line |
| Step over | F10 | step without going into calls |
| Step out | Shift+F11 | run until the current function returns |
| Step instruction | | run one bytecode instruction |
| Break | | stop at the next script instruction |

- **Stop on script errors** and **Stop on DebugBreak()** stop automatically.
- **Inspector** while stopped:
  - **Call stack**;
  - **Locals and arguments**;
  - **File globals** (this instance);
  - **Shared variables** (every instance of the file);
  - **Operand stack**.

  Integer and float values can be edited in place while the script is stopped.

**Sandbox: editing scripts live.**

**Sandbox: edit this script** copies the decompiled source to `devtools\sandbox\<name>.scp` and opens it in your
editor (`[devtools] Editor=`, default Notepad). Each time you save, the script is compiled and swapped into the
running game:

- **between frames:** no script is mid-instruction when the swap happens;
- **values carried over:** variables move across by name;
- **restarted at the top level:** running instances restart the function they were in.

**Compile + apply** does it by hand. **Revert** goes back to the original. Compile errors are listed with their
line numbers. Tick **reset** to start the script fresh instead of carrying values over. The swap lasts until the
game restarts; game files are never touched.

> The sandbox needs a Dog compiler. GladiusPort's own compiler is in development. Until it ships, the sandbox
> reports that no compiler is installed. Everything else in the debugger works without one.

**Reset scripts:** tick one or more files and press **Reset selected**. Each ticked file then:

- gets its variables back to their defaults;
- has its running instances stopped;
- runs its start-up function again (**Run OnInit after**, on by default).

**Run OnKill first** runs the shutdown function before the reset. It's off by default, because many OnKill
functions move the game on to another state. Functions that would change the game state are never run by a reset.

The panel suggests linked files ("X calls it", "calls X") and warns before you reset, for example:

- OnInit registers event handlers again without an OnKill to remove the old ones;
- OnInit summons characters again (the old ones stay in the world);
- OnInit hands out waypoint paths again;
- handlers that **count** (e.g. towns left to beat) would count twice.

**Event handlers:** every event handler the game currently has, with its owning script and what it changes, e.g.
`ImperiaTownsToBeat -1`.

- Registering a handler twice makes its function run twice per event. Duplicates show in red with a count.
- **Remove duplicates** (or **remove extra** on one row) keeps one entry of each. It doesn't work while the game is
  stopped at a breakpoint.
- The filters show only the selected script or only duplicates.

### Quests tab: game variables and quest flow

Quest progress in Gladius lives in **game variables**, set by scripts with `CreateIntVar` / `SetIntVar` and saved
with your game. Examples: `qstPriamStatus`, `ImperiaTownsToBeat`, `PriamDay`.

On first open the tab reads **every script** (from `gladius.bec`, loose `data\script\bin` files and the copies in
memory). From those it builds a map of where each variable is created, set, counted and checked.

- **Variables list:** every game variable with its live value. Filters: text, **in scripts**, **not created yet**
  (variables a script uses that the game doesn't have yet) and **changed** (this session). Recently changed rows
  light up.
- **Flow points:** pick a variable to see every value scripts set it to or check it against, with where. Example for
  `qstPriamStatus`:
  - `1`: set by cnvpriam5;
  - `2`: set by cnvpriambegin, checked by wmqstpriam ("== 2");
  - `4`: set by qstpriambattle;
  - and so on.

  **Set** jumps the quest straight to that point. Each place is a link into the Scripts tab when that file is
  loaded.
- **Edit:** type a value, or use **-** / **+**. A variable that doesn't exist yet is created.
- **Counters:** variables that scripts add to or subtract from are marked. If the counting handler is registered
  more than once right now, the tab says so (it would count twice per event).
- **Changes this session:** every change with its time, old and new value, and **undo**. The newest are at the top.
  Loading a save shows as one line.
- **Snapshots:** **Take snapshot** before a quest step, play it, then **Restore** to play it again. Snapshots are
  saved in `devtools\snapshots\` and survive restarts. **zero newer ones** puts variables created or changed since
  the snapshot back to 0.

> **Restoring variables only rewinds variables.** Whatever a quest step already did stays done: journal entries,
> quest items, recruits, gold, unlocked towns and encounters. Saving after a partial rewind keeps that mix. For a
> true rewind, keep a copy of your save before testing.

---

## Console commands

The console adds these to the game's own commands. Commands marked *offline* refuse to run in an online match
(where the tools are off anyway).

| Command | What it does |
|---|---|
| `help [text]` | commands and variables, optionally containing text; `command ?` shows a command's usage |
| `find text` | search names and descriptions |
| `cmds [text]` / `vars [text]` / `aliases` | list the game's commands / variables (with values) / aliases |
| `cls` | clear the console |
| `scriptbreak` | switch the script debugger on and stop at the next script instruction |
| `scriptswap <file> <path>` | replace a loaded script with new code (`.scp` compiled, or `.scb`) - sandbox, until restart *(offline)* |
| `scriptreset <file>... [-noinit] [-kill]` | reset scripts together: fresh variables, running ones stopped, OnInit again (not with `-noinit`), OnKill first with `-kill` *(offline)* |
| `scriptdedup` | remove duplicate script event handlers *(offline)* |
| `gamevar [text]` | list the game variables containing text |
| `gamevar <name> <value>` | set a game variable (created if missing) *(offline)* |
| `gameflow <name>` | where scripts create, set, count and check a game variable |
| `crashreport` | write a crash-style state report now (`crash\manual_report.txt`). **Debug builds only** |
| `crashtest` | crash the game on purpose, to test reports and dumps. **Debug builds only** |

---

## Crash reports and dumps

**Debug builds only.** When the game crashes, aborts or hangs, `crash\` next to `gladius.exe` gets:

- `<time>_crash.txt`, a readable report:
  - the exception and registers;
  - the call stack (named game functions where known);
  - the game state, and the Dog scripts running with their positions;
  - memory totals;
  - the last console, game output and `port.log` lines;
- `<time>_crash.dmp`, a memory dump for a debugger. Its size depends on `[debug] CrashDump=`:

| `CrashDump` | Dump |
|---|---|
| `0` | none (the `.txt` report is still written) |
| `1` (default) | stacks, module data, referenced memory and the game heap (~100-300 MB) |
| `2` | full memory (1.5-2 GB) |

Only the newest few dumps are kept.

**Release builds** write neither. The crash is logged to `port.log` and the game exits (see
[Safety](#safety-online-play-and-release-builds)).

---

## Settings (`gladiusport.ini`)

```ini
[devtools]
Enabled=1            ; load devtools.dll (0 = off)
Window=1             ; open the tools window at start (else Ctrl+`)
LogConsole=1         ; also write console lines into port.log
ScriptDebugger=0     ; script debugger on from the start
Python=python        ; Python 3.8+ for the script decompiler (the builder fills it in)
Editor=notepad.exe   ; editor for sandbox scripts
Startup=             ; console commands to run once at start, separated by ;  (testing)
StartupFrame=600     ; ... this many frames after the game starts

[debug]
CrashDump=1          ; debug builds: 0 / 1 / 2, see above
```

---

## Recipes

**Watch a quest move.** Open **Quests**, filter for the quest (e.g. `qstPriam`) and tick **changed**. Play: every
step shows up live with what changed.

**Jump to a quest step and play it.** In **Quests**, pick the status variable, then **Set** the flow point you want
(e.g. `qstPriamStatus` → `2`). Go where the checking script expects you; the flow point list says which script
checks it.

**Replay a step.** **Take snapshot** before the step, play it, **Restore**, then play it again. For a clean rewind,
copy your save too.

**Find why a script misbehaves.** **Scripts** → tick **Stop on script errors**, play until it stops, and read the
call stack and locals. Script errors are always listed in **Log** → **Game output**, even with the debugger off.

**Restart a script after editing it.** Use the sandbox (with a compiler), or `scriptreset <file>` to run it again
from scratch. Check **Event handlers** for duplicates afterwards and **Remove duplicates**.

**Profile a slow scene.** **Graphs** → tick **Renderer profiler** and read which zone or GPU pass grows.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| Nothing happens on `` ` `` | `[devtools] Enabled=1`? Check `port.log` for `devtools: ready`. "is for another version" means `devtools.dll` doesn't match `gladiusport.dll`: install both from the same build |
| The tools went away mid-session | an online lobby was hosted or joined (`port.log`: `devtools: off - online lobby`). Restart the game |
| Scripts show only disassembly | no Python: install Python 3.8+ and run the builder's Developer tools option again (or set `[devtools] Python=`) |
| Decompile failed | read `devtools\scripts\decompile.log`; delete the cached `.scp` / `.map` in `devtools\scripts\` to retry |
| Sandbox says no compiler | GladiusPort's Dog compiler is still in development |
| `crashreport` refused | release build: no reports by design |
