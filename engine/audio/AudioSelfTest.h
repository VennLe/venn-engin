#pragma once
// ============================================================
// engine/audio/AudioSelfTest —— `Game.exe --selftest audio` 的全部断言
//
// 为什么自检住在 engine/ 里而不是 runtime/：
//   被断言的对象（Clip / Mixer / 3D / 总线 / 流式）全在 engine/audio，
//   而 `runtime` 只是"谁在驱动它"。把断言和被测对象放在同一个模块里，
//   将来编辑器那边要跑同一套自检（`Editor.exe --selftest audio`）时
//   不需要把断言复制到 editor/ 去（那正是"改一处要同步改 N 处"）。
//
// ⚠ 断言的**输出格式**与 runtime 的其余自检一致，回归脚本只读最后一行
//   `AUDIO-TOTAL pass=N fail=M` 与进程返回码。
// ============================================================

namespace audio {

// 返回 fail 数（0 = 全绿）。调用方把它映射成进程退出码。
int runAudioSelfTest();

} // namespace audio
