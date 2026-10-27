# 脚本 API 速查：演出与世界控制（C6）

> 适用：`assets/**.vks`（引擎自带脚本语言）。
> 语言本体（语法 / 变量 / 钩子 / 协程）见 `docs/script_roadmap.md`；
> 这篇只讲**"脚本能指挥引擎做什么"**。

---

## 1 · 七个内建，两组用途

`.vks` 在 C6 之前只能"摆弄自己"：改位置、改颜色、放音效、扣血。
一个能动的 demo，不是游戏。C6 补上两类能力：

| 组 | 内建 | 回答的问题 |
|---|---|---|
| **演出**（C6 第一批） | `playSequence` `showDialogue` `playAnim` | "把这段过场/对话/动作播出来" |
| **世界**（C6 第二批） | `setFlag` `getFlag` `spawn` `loadScene` | "记住/刷出/换掉这一局游戏" |

区别在于**副作用落在哪**：演出改的是"这一刻演什么"，世界改的是"这一局是什么"。

---

## 2 · 签名与语义

路径一律**相对 `assets/`**，例如 `"sequences/opening.seq.json"`、
`"models/anime_girl.fbx"`、`"scenes/ch02.json"`。
对话可以只写 id（`"ch01_open_01"`），由 `resolve*Path()` 归一。

| 调用 | 返回 | 语义 |
|---|---|---|
| `playSequence(path)` | bool | 起播一段 `.seq.json` 时间轴。**插播**：播完自动让位给场景里配好的时间轴，不是抢占 |
| `showDialogue(id)` | bool | 按 id 起一段对话。宿主 = 玩家实体（没有玩家时传无效句柄，照样能播） |
| `playAnim(clip)` / `playAnim(e, clip)` | bool | 给 self / 指定实体切 clip，0.2 s crossfade。骨架里**没有**这段就失败 |
| `setFlag(name, value)` | bool | 写一个剧情旗标。`value` 可以是数字或 `true/false`（存 1/0） |
| `getFlag(name)` | number | 读。**没设过 = 0，不报错**（"第 1 章没遇见的人，好感度就是 0"） |
| `spawn(path, [pos])` | entity | 按模型路径刷一个实体，返回**可用句柄**（能立刻 `setPos(e,...)` / `playAnim(e,...)`）。不给 `pos` = 原点 |
| `loadScene(path)` | bool | **请求**换场景。返回值 = "请求已受理"，真正的切换在**下一帧开头** |

### `loadScene` 为什么是"下一帧"

`.vks` 是在 `ScriptRuntime::tick` 里被调用的，此刻正遍历着旧世界的
`ScriptComponent`。这时候销毁世界 = 把正在用的迭代器指向已释放内存。
所以：**受理 → 记下 → 下一帧 start-of-frame 执行**。

代价必须说清楚：**紧接着的那几行脚本看不到新场景的实体**。
`loadScene(...)` 后面不要再写 `find("新场景的物体")`，那一定找不到。
需要"进新场景后做点什么"，请把那段逻辑写进**新场景里的脚本**。

### 旗标 vs 变量 vs 组件

| | 活多久 | 谁看得见 | 典型用途 |
|---|---|---|---|
| 脚本变量 `vars` | 跨帧，直到热重载重启 | 只有这个脚本实例 | 局部状态、计时器 |
| `setFlag` 表 | 整局（**换场景不清**） | 所有脚本 | 好感度、剧情分支、见过谁 |
| 组件 | 跟着实体 | 所有系统 | 位置、血量、碰撞 |

---

## 3 · 三条硬纪律

1. **失败必报错，不许静默成功。**
   七个内建全部返回 `bool`（`getFlag`/`spawn` 返回 0/无效句柄），
   失败会让**那条语句中止**并写一行 `GAME-SCRIPT runtime error`。
   静默返回 "成功" 的后果是"剧本写完了、日志全绿、画面什么都没发生"。

2. **`spawn` 出来的实体不进物理世界。**
   场景物理在启动 / 换场景时**构建一次**。刷出来的东西是纯视觉的，
   要它参与碰撞请预放在场景里，或用 `setPos` 手动驱动。
   （做到"刷出来就会掉下来"需要增量加刚体，那是另一件事。）

3. **演出是插播，不是抢占。**
   `playSequence` 用最高优先级压过场景里的 `SequencerComponent`，
   播完（`finished`）立刻把手交回去。循环时间轴走的是回绕分支，不会触发让位。

---

## 4 · 三个踩过的坑

### 坑 1：`.vks` 的 `let` 只能写在顶层

```text
# ✗ 编译失败：line 2: let is only allowed at the top level
on start {
    let a = playSequence("sequences/opening.seq.json")
}

# ✓
on start {
    playSequence("sequences/opening.seq.json")
}
```

需要跨帧记住的东西写在顶层：`let n = 0`，然后在钩子里 `n = n + 1`。

⚠ 这个坑的症状极具误导性：脚本 `not runnable`（**编译**错，不是运行错），
于是所有内建一条都没调到 → 验收脚本的断言**全红**，而日志里只有一行 WARN。

### 坑 2：`playAnim` 必须同时改两个组件

`SkinnedMeshComponent::clipName` 是采样层真正读的字段；
`AnimatorComponent::defaultClip` 只在 `!appliedDefaultClip` 时补一次。
只改后者 = 脚本返回 true、日志干净、**画面不动**。
引擎侧的实现已经写对了两处，但如果你自己加一个"切 clip"的功能，
这是最容易漏的地方 —— 写入点有 `RUNTIME-SWITCH-ANIM` 锚点，专为它留的。

### 坑 3：`playAnim` 的 clip 名要真的存在于骨架

拼错 `"Run"`（`anime_girl` 只有 `Idle|Walk`）时，失败日志会直接列出可选项：

```text
RUNTIME-SWITCH-ANIM-MISS entity=12 clip=Run reason=no-such-clip available=[Idle|Walk]
```

看到 `available=[...]` 就知道是拼错，不用去查"组件挂没挂"。

---

## 5 · 一个完整的例子

```text
# assets/scripts/ch01_intro.vks
# 进门 → 播过场 → 弹对话 → 刷两只敌人 → 进第 2 章
let waves = 0

on start {
    playSequence("sequences/opening.seq.json")
    showDialogue("ch01_open_01")
    setFlag("met_alice", true)
    spawn("models/slime.fbx", vec3(3, 0, 0))
    spawn("models/slime.fbx", vec3(-3, 0, 0))
    waves = waves + 1
}

on update {
    # 玩家走到 (10, 0, 0) 附近就切关卡（只切一次：旗标当锁）
    if (dist(player(), vec3(10, 0, 0)) < 2) {
        if (getFlag("ch02_started") == 0) {
            setFlag("ch02_started", true)
            loadScene("scenes/ch02.json")
        }
    }
}
```

新场景（`ch02.json`）里的脚本负责"进第 2 章之后干什么" ——
包括读 `getFlag("met_alice")` 决定那句台词怎么说。

---

## 6 · 验收在哪

| 层 | 用例 | 验什么 |
|---|---|---|
| 接线（无窗口） | `Game.exe --selftest script` 的 **C13~C19** | 参数原样到宿主 / 失败必报错 / 类型纪律 / 旗标写读 / spawn 句柄可用 |
| 端到端（真资产） | `tools/verify_director_api.py` | 演出三件**真的播出来了**（`SEQ-CUE` / `DLG-START` / `RUNTIME-SWITCH-ANIM`） |
| 端到端（真资产） | `tools/verify_world_api.py` | 旗标进宿主 / spawn 真装配 / 换场景真发生 / **旗标跨场景保留** |

回归登记见 `tools/run_regression.py` 的 `TRIGGERS`：
改 `ScriptEngine.*` / `SequencerSystem.*` / `runtime/RuntimeApp.cpp` /
`runtime/GameFlags.h` 时这三条都会被拉起来。

---

## 7 · 还没做（写下来是为了不反复讨论）

- **存档**：`GameFlags` 现在只在内存里。存/读盘属于"存进度"那一块。
- **`spawn` 进物理**：需要增量加刚体（`SceneBinding` 现在是整体重建）。
- **按名字找实体的旗标化条件**：`getFlag` 只有数值，没有"列表/字符串"。
- **`loadScene` 带参数**（"进第 2 章，从东门进"）：需要场景参数表。
