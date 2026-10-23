# Play 态的鼠标两态：点视口接管 / Esc 交还（P8）

> 2026-10-23 · 承接 `docs/player_controls.md`（P6）与 `docs/editor_play_terrain.md`（P7）
> 代码：`editor/EditorContext.h/.cpp`、`editor/EditorApp.h/.cpp`、
> `editor/ViewportPanel.cpp`、`engine/core/Input.h/.cpp`
> 验收：`tools/verify_play_mouse_capture.py`（13 条）

---

## 0 · 一句话

进 Play **不等于**能操作：光标先留着可见、角色不响应任何键鼠；**用左键在 3D
视口里点一下**才真正接管（光标隐藏 + 锁定 + 视角跟手）；**按 Esc 交还**
（光标回来，游戏继续跑但不响应输入）。

这一套在运行时 `Game.exe` 里早就有了（P5 做的），**编辑器 Play 一直缺** ——
这就是这轮补的东西。

---

## 1 · 两态，不是一个开关

```
             ┌──────────── 未接管 ────────────┐
             │ 光标：可见、可自由移动          │
             │ 键鼠：WASD / 跳跃 / 视角 全冻结 │
             │ UI  ：编辑器面板照常可点        │
             └───┬─────────────────────▲──────┘
   左键点视口    │                     │   Esc
  (PLAY-CLICK)   ▼                     │
             ┌──────────── 操作中 ─────┴──────┐
             │ 光标：GLFW_CURSOR_DISABLED      │
             │       （隐藏 + 锁定 + 相对位移）│
             │ 键鼠：全归游戏                  │
             │ ImGui 鼠标：关掉（NoMouse）     │
             └─────────────────────────────────┘
```

`captured = false` 时**不是**"什么都不做"：重力与贴地照跑，动画照常呼吸。
只冻输入不冻物理，是因为"在空中按 Esc"必须还能落下来 —— 否则人会悬在天上。

---

## 2 · 状态放在哪，谁改谁同步

三个文件各管一段，接口只有三个 bool：

| 位置 | 职责 | 为什么是它 |
|---|---|---|
| `EditorContext` | 存 `m_playMouseCaptured` + `m_playCaptureRequested` + `m_playCursorDirty` | 它是 Play 状态的唯一真相；`play()/pause()/stop()` 正好能顺手复位 |
| `ViewportPanel` | **发现**"左键真的点在视口第一格里" → `requestPlayMouseCapture()` | 只有面板知道鼠标是不是在自己这块矩形里 |
| `EditorApp::updatePlayMouse` | **执行**：改 GLFW 光标模式 + ImGui 鼠标开关 + 打锚点 | 只有它拿得到 `Window` |

`dirty` 标记不是省事，是必须：每帧无条件 `setCursorVisible(false)` 会让 GLFW
反复 warp 光标，还会把 `discardNextMouseDelta` 用光（镜头每次切态都猛转一下）。

### Esc 必须"一帧只做一件事"

```cpp
if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    if (m_ctx->playMouseCaptured())      m_ctx->setPlayMouseCaptured(false);
    else if (m_ctx->gameFullscreen())    m_ctx->gameFullscreenRef() = false;
}
```

写成两个独立 `if` 的话，全屏 + 操作中按一下 Esc 会**同时**交还鼠标和退全屏，
玩家以为自己按了两次。

---

## 3 · 为什么必须关掉 ImGui 的鼠标（`ImGuiConfigFlags_NoMouse`）

`GLFW_CURSOR_DISABLED` 把光标锁在**窗口中心**。编辑器窗口中心通常正落在
Hierarchy / Inspector 上 —— ImGui 照旧认为"鼠标在这儿"。

于是玩家在游戏里按左键（射击 / 点地移动）会去点那些面板的按钮，而且**看不见
光标**，根本不知道点了什么。关掉 ImGui 的鼠标输入，才是真的"鼠标归游戏"。

副作用要记住两条：

- `io.MousePos` 变成 `(-FLT_MAX, -FLT_MAX)` → 面板里的 `IsItemHovered()` 恒
  false。所以**接管那一帧之后不要再指望 hover**，一切按捕获态分支走。
- `io.MouseDelta` 被清空 → 视角必须改用 `Input::mouseDeltaRaw()`（新加的，
  不受 `setUiCapturing` 门控）。原 `mouseDelta()` 会被
  `WaitCaptureMouse` 掐掉，正好在接管那一刻开始返 0。

---

## 4 · 途中抓到的两个真 bug

### 4.1 Play 期间内容区不一定是 3D 视口（最隐蔽的一个）

`drawPanels()` 里：

```cpp
const int hosted = viewportTabDrawnPanel();     // 内容区当前画哪一页
if (p.layoutSlot == kSlotViewport && hosted >= 0) continue;   // 视口整块不画
```

只要用户把内容区切到 Scripts / Physics 之类的标签页，`hosted >= 0` ——
**按 Run 之后视口一个像素都不画**，`ViewportPanel::draw()` 根本不执行。

表现是"游戏跑起来了但看不见画面，也点不到视口"，而一切日志都正常
（`PlayerController` 在跑、`PLAYER-STATE` 照打）。这轮验收一开始 5 条红，
全是它。

修法：

```cpp
const int hosted = m_ctx->isEditing() ? viewportTabDrawnPanel() : -1;
```

视口是 Play 的唯一出口，必须钉住。

### 4.2 未接管时"鼠标划过视口镜头就跟着转"

旧代码 `if (mouseInViewport) { yaw += io.MouseDelta.x ... }` —— 在编辑态是合理
的（鼠标确实在视口里），但在 Play 态就等于"光标路过就转镜头"，玩家会觉得
游戏坏了。现在视角只在 `active`（已接管）时更新。

---

## 5 · 验收基建也踩了一个坑：注入的位移在 DISABLED 下是失真的

`tools/verify_player.py` 原来的注入方式：`PostMessage(WM_MOUSEMOVE, 逻辑坐标)`
+ `SetCursorPos(真实坐标)`。前者骗 ImGui，后者让真实光标别跑出窗口。

**捕获态下这套是错的**：GLFW 在 `CURSOR_DISABLED` 时算 delta 用的是
`x - lastCursorPos`，而它每帧都会把光标 warp 回中心并刷新 `lastCursorPos`。
逻辑坐标与真实光标一旦不同步，得到的 delta 会**反号且量级不对** ——
实测"鼠标下移"变成了"抬头"。

修法：捕获态改用系统级相对移动 `mouse_event(MOUSEEVENTF_MOVE, dx, dy)`，
它产生的 delta 与真实鼠标完全一致。`verify_player.py` 里用一个模块级
`_captured` 开关切换两条路。

> 教训：**验收脚本的"输入注入"必须与被测的真实输入路径同源**，否则测的是
> 注入器而不是产品。这条只有在两态切换之后才暴露出来。

---

## 6 · 验收

`tools/verify_play_mouse_capture.py`（13 条，约 30 秒）

| 判据 | 断言 |
|---|---|
| M0/M1a | 编辑器起得来、能建第三人称角色 |
| M1b | 进 Play → `EDIT-MOUSE captured=0`（不自动接管） |
| M2 | 未接管时 **GetCursorInfo 报 CURSOR_SHOWING** |
| M3 | 未接管时按住 W 0.8 s → 角色**不动**（< 0.05 m） |
| M4 | 左键点视口 → `PLAY-CLICK` + `EDIT-MOUSE captured=1` |
| M5 | 接管后 **GetCursorInfo 不再报 CURSOR_SHOWING**（光标真的没了） |
| M6 | 接管后按住 W → 走 > 0.4 m |
| M7 | 接管后鼠标位移 → yaw 变化 > 0.05 rad |
| M8/M9 | Esc → `captured=0` 且光标重新可见 |
| M10 | 再点一次又能接管（两态可反复，不是一次性） |
| M11 | 全程 0 条 VK_CHECK、进程活着 |

**M2/M5/M9 为什么用 `GetCursorInfo` 而不是日志**：`captured=1` 只证明"我们
想隐藏光标"；真正把它藏起来的是 `glfwSetInputMode`。两步之间完全可以断开
（例如窗口失焦时 GLFW 会自己恢复光标），系统层的显示位才是不可辩驳的证据。

**M3 + M6 是一对**：只测 M6 的话，"进 Play 即接管"也能全绿，需求其实没实现。

---

## 7 · 顺带影响到的旧用例

两态一上线，所有"进 Play 后注入键鼠再断言"的用例都得先补一次"点视口接管"，
否则会莫名其妙红：

| 脚本 | 补了什么 |
|---|---|
| `verify_player.py` | `capture_ready()`（点视口 → 等 `cap=1`），并把"未接管时 WASD 不响应"也变成一条断言 |
| `verify_char_locomotion.py` | C5b：进 Play 后点视口 → `EDIT-MOUSE captured=1` |
| `verify_char_preview.py` | P7a：同上（否则 P7 的 Space 跳跃不响应） |

`verify_player_jump` / `verify_player_ground` / `verify_player_feel` 跑的是
`Game.exe`，运行时侧两态早就有了，不受影响。

---

## 8 · 还没做

- 暂停时按 Esc 不弹编辑器菜单（Play 态 Esc 现在只做"交还 / 退全屏"，
  暂停走工具栏按钮）。
- 手柄接管（`GLFW_GAMEPAD`）没有对应的光标处理 —— 手柄本来就不需要光标。
- 多视口分屏时只有**第一格**收这个点击（`inPane0`），与既有点选一致。
