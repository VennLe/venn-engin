#pragma once
// ============================================================
// project/ProjectDisplay —— 工程的"画面设置"读写
//
// 路线图 D3 的核心纪律：**启动器不存第二份配置**。
// 分辨率 / 全屏 / 标题的权威是 `<root>/assets/game.json`；
// 画质的权威是 `<root>/config/render.json`（`SettingScope::Project` 层）。
// 这个文件只负责**按既有格式**读写这两处，绝不引入第三份。
//
// ------------------------------------------------------------
// 为什么画质要写"全量平铺键"而不是"只写非默认"
// ------------------------------------------------------------
// `captureFromRegistry()` 的"只抓非默认"语义是给**编辑器的 Export 按钮**用的
// （用户手调了哪几个旋钮就存哪几个）。
//
// 启动器的画质档位是另一回事：它的语义是"**这一档明确打开这些特性，
// 没列到的一律关**"（见 `render/QualityPresets.h`）。如果只写打开的那几个，
// 剩下的会**回退到引擎默认** —— 而引擎默认里 shadow/ssao 可能是开着的，
// 于是"选 Low"实际关不掉它们，用户在设置里看到 Low、跑起来是 Medium。
// 所以这里写**每一个** enable 参数（true 和 false 都写）。
//
// 这是"两份数据漂移"的另一种形态（不是两份文件，是"文件只说了半个真相"），
// 同样属于必须避免的静默错误。
// ============================================================

#include "project/ProjectFile.h"

#include <string>
#include <vector>

namespace project {

// 启动器要写的画面设置。
struct DisplaySpec {
    int width = 1280;
    int height = 720;
    bool fullscreen = false;
    std::string title;        // 空 = 不改（保持 game.json 里原有的）
    int qualityPreset = -1;   // -1 = 不改 render.json
};

// 读回来的现状（"打开已有工程"右栏要显示）。
struct DisplayInfo {
    int width = 1280;
    int height = 720;
    bool fullscreen = false;
    std::string title;
    // game.json 里是否还留着**已废弃**的 `quality` 键。
    // ⚠ 留着但没消费者 = 路线图 P4 的原始 bug（"选了 Cinematic 画面不变、
    //   也没有任何提示"）。M4 起它被明确废弃，读到就在日志里说清楚。
    std::string deprecatedQuality;
    bool valid = false;
};

// 读 `<root>/assets/game.json`。文件缺 / 坏 → `valid=false` + 默认值
// （**不是错误**：一门新工程可能还没写 game.json，编辑器会用内置默认）。
IoResult readDisplay(const std::string& root, DisplayInfo& out, std::string* err);

// 写 `<root>/assets/game.json` 的 width/height/fullscreen/title。
// ⚠ **保留其它所有键**（maxFps / audio / language / scene …）—— 读进来
//   改动再写回，绝不整份重建。整份重建会把用户手调过的 maxFps 冲掉。
IoResult writeDisplay(const std::string& root, const DisplaySpec& d,
                      std::string* err);

// 写 `<root>/config/render.json`：把该预设的**每一个** enable 参数平铺成
// `"<feature>.<param>": true/false`。走既有的 `saveLayerFile`（格式/版本/
// `_comment` 与编辑器 Export 完全一致）。
IoResult writeQuality(const std::string& root, int preset, std::string* err);

// 给向导 Step 3 右下角那个"**将写入**"清单（路线图 §5.3）。
// 返回若干行人话，UI 直接逐行 Text 出来。
std::vector<std::string> describeWrites(const DisplaySpec& d);

// game.json 的绝对路径（`<root>/assets/game.json`）
std::string gameJsonPath(const std::string& root);
// render.json 的绝对路径（`<root>/config/render.json`）
std::string renderJsonPath(const std::string& root);

// ---------------------------------------------------------------- 自检
//
// 无窗口（`--selftest project` 的 J 组）。覆盖：
// 写→读往返 / 保留其它键 / 平铺键全量 / 预设名映射 / 废弃 quality 的识别。
int runDisplaySelfTest();

} // namespace project
