# 出厂界面语言 = 当前电脑的系统语言（2026-10-29）

> 用户原话："引擎的语言默认是设置为当前电脑系统的语言"
>
> 判据：`tools/verify_system_lang.py`（**11** 条，全绿）
> 事实源头：`engine/i18n/I18n.h` 文件头「出厂语言」一节

---

## 1 · 改之前是什么样

`i18n::g_lang` 初值写死 `English`，`RuntimeConfig::language` 默认 `"en"`。
后果是**三个入口第一次打开全是英文**：

| 入口 | 之前 | 现在（无存档 / 未配） |
|---|---|---|
| 编辑器 `Editor.exe` | 英文，要去 `Preferences ▸ Appearance ▸ Language` 点一次 | **系统语言** |
| 启动器 `Launcher.exe` | 英文 | **系统语言** |
| 游戏 `Game.exe` | 英文（`game.json` 缺字段时） | **系统语言** |

这不是"默认值保守"，是**默认值不对**：界面语言的第一猜测只能是系统语言
（UE5 / Unity / VS Code 全这么答）。

## 2 · 改成了什么样：一条链，三档

高 → 低。整条链**同一个答案**，各入口只负责自己的覆盖面：

1. **命令行 / 环境变量** —— `--language` / `MYVK_LANG`（运行时）、
   `MYVK_EDITOR_LANG`（编辑器验收钩子）
2. **落盘的用户选择** —— `config/editor_prefs.json` 的 `language`、
   `assets/game.json` 的 `language`
3. **系统界面语言** —— `i18n::systemLanguage()`，即 `language()` 的初值

`i18n::systemLanguage()` 是第 3 档的唯一实现：

- **Windows**：`PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE`
  → 中文，否则英文。
  判据是**界面语言（UI language）**，不是区域格式 —— "格式：中国 /
  显示语言：English" 的机器，用户看到的是英文界面，那就该给英文。
- **POSIX**：`LC_ALL` → `LC_MESSAGES` → `LANG` 的前缀（`zh_*` → 中文）。
- 取不到 → 英文（0）。**不抛、不 WARN**：这是"这台机器怎么答"，不是配置错误。

`g_lang` 用 **`-1` 哨兵 + 惰性求值**，不是在静态初始化期问 Win32 ——
DLL 依赖还没走完时调 API 是真出过事的；顺便保住"英文模式不建表"那条
（没人查过语言 = 一条都不碰）。

## 3 · 编辑器的那条例外：自动化跑默认英文

`editorLayoutAutomationRun()`（= `MYVK_FRAMES` 或 `MYVK_NO_WINDOW_SAVE`）
为真时，出厂语言是**英文**，不是系统语言。

理由：屏幕 / 像素判据要**确定性文本**。跑在中文 Windows 上的回归不该因为
系统语言换词（换词 = 一堆与本用例无关的像素差），而这类"环境差异"造成的
红是最难查的一类。

⚠ 只影响**脚本跑法**。用户双击 `Editor.exe` 照旧跟随系统。
⚠ `MYVK_EDITOR_LANG` 的优先级比它更高（B4 盯着这条：验收钩子没被改弱）。

## 4 · 落点 & 可观测

三行日志，每行都带 `src=`（**谁定的**）：

```
I18N-KEY     lang=zh count=2493 src=config        # 运行时（配置里写的 auto）
I18N-KEY     lang=zh count=2493 src=env:MYVK_LANG # 环境变量压过一切
I18N-KEY     lang=zh count=2493 src=system        # game.json 没有 language 键
EDITOR-LANG  lang=zh src=system                   # 交互式 + 无存档
EDITOR-LANG  lang=en src=automation               # 脚本跑法（确定性文本）
EDITOR-LANG  lang=zh src=prefs                    # 存档里选过
EDITOR-LANG  lang=zh src=env                      # MYVK_EDITOR_LANG
LAUNCHER-LANG lang=zh src=system                  # 启动器：它没有偏好存储
```

`src` 不能省：`lang=en` 在"系统是英文"和"自动化默认英文"两种情况下
**字面相同**，但排查方向完全相反。

`Reset All` 的落点也是同一个答案（`factoryLanguage()`），不是常数 0 ——
否则中文机器上"重置默认 → 英文、下次冷启动 → 中文"，两处对不上。

## 5 · 配套改动

- `RuntimeConfig::language` 默认 `""`。空串 / `"auto"` / `"system"` 三种写法
  都走"跟随系统"（`RuntimeApp::onInit` 同一条分支）。
  `GAME-CONFIG` 那行打的是**配置值**（空 → 打 `auto`），**生效值**看
  `I18N-KEY` —— 两行各管一件事。
- 工程模板（`tools/gen_templates.py`）从 `"language": "zh"` 改成 `"auto"`：
  模板是给**别人**的起点，写死中文等于让英文玩家装完就看到中文。
- `--language <en|zh>` 的 help 文本补一句"不给 = auto = 跟随系统语言"。

## 6 · 怎么判（`tools/verify_system_lang.py`）

| 组 | 查什么 |
|---|---|
| A 运行时 | A1 独立 oracle（Python 侧 `GetUserDefaultUILanguage`，不当自己是原告）；A2 `--language auto` → 系统语言；A2b 中文档下 `count>0`（表真建了）；A3 显式 `en` 覆盖系统且 `count=0`；A4 `MYVK_LANG` 压过命令行 |
| B 编辑器 | B1 自动化跑 → `en src=automation`；B2 **交互式**跑 → `系统 src=system`；B2b 退出落盘时 prefs 里**没有** `language` 键（不把"跟随系统"钉死）；B3 存档 `language=1` → `zh src=prefs`；B4 `MYVK_EDITOR_LANG` → `src=env` |
| C 启动器 | C1 `LAUNCHER-LANG lang=<系统> src=system` |
| D 静态 | D1 偏好表里 `language` 的 `def` **不是常数 0**（`Reset All` 与冷启动同一个答案） |

⚠ `--language auto` 是"缺省那档"的等价代理（空 / `auto` / `system` 走同一条
分支）。造一份"没有 `language` 键"的 `game.json` 要连 `assets/shaders` 一起
复制一份 `build/bin`，代价远大于它多证明的那一行。

## 7 · 还没做

- **语言下拉框里没有"跟随系统"这一项**：现在只有 `English` / `中文`，
  选了就是固定值，之后系统语言再怎么变都不再跟随（`Reset All` 能回去）。
  要加就得动 `Language::Count` / 落盘下标 / 一堆断言 `language==0|1` 的用例，
  单独立项更划算。
- **启动器不落盘语言**：它的下拉框只管本次进程（它本来就没有偏好存储）。
