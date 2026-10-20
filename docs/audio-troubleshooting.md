# Streamed audio: diagnosis, fix and validation

## Missing dialogue and intermittent background music

The observed failure affected long audio streams: subtitles appeared without
speech, and background music could play a short fragment and then fall silent.
Short effects could still work. These symptoms alone are not sufficient to
identify the cause; device routing and other runtime faults can also cause
audio problems.

In the reproduced case, the game constructed requests such as:

```text
content://Sound/Resource/Stream/Dm_OP_0001.bwav
```

The old RomFS parser removed exactly one separator after the mount name. Its
relative-path validator then rejected the remaining leading slash, returning
file-not-found even though the resource was present. Prefetched audio could
play while the remaining stream buffers contained zeros.

The fix in `games/totk/runtime/romfs_path.h` skips repeated separators immediately
after the mount prefix before validating the relative resource path. Mount
names still have to match. Empty resources, `.` and `..` components,
backslashes, and empty interior components remain rejected. Resource bytes,
shader code and save formats are unchanged.

## Rebuild and verify

1. Rebuild with the normal `build_app.py` command and your own original inputs.
   The same work directory can be reused. A rebuild to the same app path replaces
   the runtime; the profile and shader cache are stored outside the app.
2. Run the game-data-free parser regression:

   ```sh
   python3 -m unittest discover -s tests -p test_romfs_paths.py -v
   ```

3. With Python 3.14 and the build requirements installed, run the existing
   container/update tests together with the parser regression:

   ```sh
   python3.14 -m unittest discover -s tests -v
   ```

4. For local integration testing, the rebuilt runtime supports `probe-fs` and
   `probe-audio`. Run from its resource directory, for example:

   ```sh
   cd "/path/to/Tears of the Kingdom.app/Contents/Resources/totk"
   ./build/totk-native rewritten/v0 probe-fs
   ./build/totk-native rewritten/v0 probe-audio
   ```

   `probe-fs` checks that normal, double-slash and triple-slash paths read the
   same opening-audio bytes and still reject traversal. `probe-audio` checks
   constructed mixer/decoder state without opening an audio device; it does not
   prove that in-game streaming or the selected speaker/headphones are audible.
   For a 1.1.2 app, use `rewritten/v112` instead. The added resource integration
   checks have been exercised locally on 1.0.0, not on 1.1.2.

5. Play the opening scene and confirm both dialogue and background music through
   the intended output device. This listening check confirmed the 1.0.0 fix on
   an Apple M5 Mac running macOS 26.5.2.

## If sound is still missing

- Check the current macOS output device and volume. Test a known system sound;
  if that also fails, inspect the device connection first.
- Check `~/Library/Logs/MacNativeGames/totk.log` for CoreAudio setup failures,
  decode errors and output-level reports. A nonzero mixed level does not by
  itself prove the selected physical output is audible.
- For local file-open diagnostics, quit the game normally and launch with:

  ```sh
  open --env TOTK_FS_LOG=1 "/path/to/Tears of the Kingdom.app"
  ```

  A logged request is not proof of a successful open. Compare it with the
  parser regression and the rebuilt runtime's `probe-fs` results.
- First-use shader compilation can interrupt playback under load. That is a
  separate issue from repeated root separators and is not fixed by this patch.

Do not upload game packages, extracted resources, keys, saves, generated runtime
tables or complete logs when sharing a report. Describe the game version, the
scene, the output device, and relevant redacted error messages instead.

## Audio effects and output buffering (2026-10-18)

The renderer runs every known effect type (1-9): besides the auxiliary send/return, 3D reverb and limiter it already had,
delay, plain reverb (its early reflections chosen by the early mode), compressor, effect-level biquads, buffer mix and
capture. It also fixes the revision decoding, the six-channel mapping, compressor statistics, PCM24 input, the circular
sink and a fast Stop -> Start queue, and keeps the mixing scratch state per renderer instance. Output starts once 20 ms of
audio is queued, also after an underrun; `TOTK_AUDIO_PREFILL_MS=<0..500>` changes that, rounded up to 5 ms render blocks
(0 / 20 / 40 ms ran without underruns in the tests behind the 20 ms default). This work came from the original port and
was merged with this repository's REV15 float biquads for 1.4.3.

After the output device stalls (a Bluetooth headset reconnecting, a CoreAudio hiccup) the mixer keeps at most the prefill
plus 30 ms queued and drops older audio, so the delay does not stay (#64); the `output level` line counts these as
`backlog trims N (M frames)`.

Checks, without game data:

- `./build/totk-native-v143 rewritten/v143 probe-audio`: 46,587 production-mixer checks, REV15 voices and splitters
  included; with `TOTK_AUDIO_STOP_RELEASE=0` or `TOTK_AUDIO_REV15_NEGATIVE=1` it must fail.
- `make test-audio-effects RYUJINX_AUDIO_REF=<folder with Ryujinx.Audio.dll> DOTNET=<dotnet>`: 24 groups, 1,872,000
  samples against an unmodified Ryujinx.Audio (largest difference 4.47e-08), then 1,885,461 checks of the production
  effect graph, routing, capture / circular buffers and output prefill / recovery / stop.

## 中文说明

这次确认的问题是：游戏将音频资源拼成了 `content://Sound/...`，旧版
RomFS 解析器只去掉一个根目录斜杠，随后又将剩余斜杠判为非法路径，导致
已有的配乐和对白资源打开失败。短音效或预读片段可能仍能播放，因此表现为
“有音效、对白缺失、背景音乐断续”。

修复仅规范化挂载根目录后的重复斜杠，并保留挂载名称、路径穿越、反斜杠和
非法内部路径的校验。使用原始游戏输入重新构建即可，存档和着色器缓存无需
删除。已有游戏资源本身不会因这项修复而被改写。

验证分三层：27 个无需游戏数据的解析器用例；`probe-fs` 对真实开场音频进行
单/双/三斜杠路径读取比对；最后实际播放开场，确认对白和配乐。音频混音自检
单独通过并不代表真实游戏流式播放正常。已实测 1.0.0 开场，不将结果外推为
所有场景、1.1.2 或其他游戏均已验证。

如果仍然无声，先确认 macOS 输出设备与系统提示音，再检查日志中的
CoreAudio/解码错误及音频电平。着色器首次编译造成的卡顿属于另一类问题。

### 音效与输出缓冲（2026-10-18）

已知的音效类型 1–9 都能执行：原有的辅助收发、3D 混响、限幅器之外，补上了延迟、普通混响（早期反射按 EarlyMode 选表）、压缩器、
效果级双二阶滤波、BufferMix 和 Capture；修正了 revision 解码、六声道映射、压缩器统计、PCM24、环形输出和快速 Stop → Start 的旧队列，
混音用的临时状态改成每个实例各一份。输出在攒够 20 ms 声音后开始（欠载之后也一样），`TOTK_AUDIO_PREFILL_MS=<0..500>` 可以改，
按 5 ms 的渲染块向上取整（定 20 ms 之前，0 / 20 / 40 ms 的测试局都没有欠载）。这些来自原移植项目，合并时保留了本仓库为 1.4.3 加的
REV15 浮点双二阶滤波。

不用游戏数据的检查：`./build/totk-native-v143 rewritten/v143 probe-audio`（生产混音器 46,587 项，含 REV15；加
`TOTK_AUDIO_STOP_RELEASE=0` 或 `TOTK_AUDIO_REV15_NEGATIVE=1` 时必须失败）；`make test-audio-effects RYUJINX_AUDIO_REF=<Ryujinx.Audio.dll 所在目录> DOTNET=<dotnet>`
（对未修改的 Ryujinx.Audio 共 24 组、1,872,000 个样本，最大误差 4.47e-08；再加效果图、路由、环形缓冲、预填充 / 欠载恢复 / 停止 1,885,461 项）。
