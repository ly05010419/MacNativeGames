# Memory scan, watchpoints and breakpoints (Mac)

A diagnostic for finding which game code reads or writes a value, without a debugger attached. It lives in `games/totk/runtime/mem_scan.h`, does nothing until it receives a command, and was used to find the frame-rate bugs behind #285 (the 1.2.1 map cursor, Ultrahand vertical aim and aim gyro moved a fixed step per frame).

Commands go through the runtime command channel, one per line appended to `input.txt` next to `controls.txt` (`~/Library/Application Support/MacNativeGames/TOTK`); results are appended to `memscan.txt` in the same folder.

| Command | What it does |
|---|---|
| `mscan new f32` / `mscan new i32` | Snapshot the game's touched memory (heap and alias region) |
| `mscan changed`, `unchanged`, `inc`, `dec` | Keep the candidates that did that since the last step |
| `mscan show [n]` | List candidates with their values |
| `mscan watch <hex address> <seconds> [rw]` | Hardware watchpoint in the guest threads: every writing (or reading) instruction, with count, callers and registers |
| `mscan bp <hex main offset> <seconds>` | Hardware breakpoint: how often one instruction runs, with registers |
| `mscan code <hex main offset> <words>` | Read words (code, or any address as `address - main`) |
| `mscan poke <hex address> <value>` | Write a value |
| `mscan base` | Module bases, heap and alias region |

Input to drive a test comes through the same channel: `key <Mac keycode> <ms>`, `down <keycode>`, `up <keycode>` (the arrow keys 123–126 are the right stick, so `down 126` holds it fully up), and `wshot <path>` for a screenshot. A command blocks the channel while it runs, so hold a key with `down` before a `watch` or `bp` and release it with `up` after.

## A workflow that works

1. Find the input: hold an arrow key (`down 126`), `mscan new f32` before and `changed` / `unchanged` steps while holding and after releasing; the stick value is exactly 1.0.
2. Follow its readers: `down 126`, `mscan watch <address> 0.6 rw`, `up 126` lists every instruction that reads it, with its callers.
3. When the function that uses the value is found, read the fields of its object after controlled inputs (`mscan code <address - main> <words>`) to see which field follows the input.
4. Confirm the path with `mscan bp` on the candidate instructions while the input is held.

## Things to know

- Watch the heap, not guest stacks: stack slots are reused by unrelated code, and the command refuses them.
- Much of the heap is per-frame scratch memory: values there seem to follow the input but move every frame. Many identical candidates are a sign of it.
- Camera data is copied several times per frame; a watch that lands in `memcpy` or in a function that copies field by field has not reached the source yet. The final camera controller keeps positions as doubles, so an `f32` scan misses them.
- Heap addresses can change between launches and saves; offsets from main do not.
