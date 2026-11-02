# 配表与本地化编辑（C1 / C2）

> 2026-11-02。对应审计文档 `panel_gap_review_2026-11-02.md` 的 C1 / C2 条。
> 判据：`tools/verify_data_tables.py`（A / B / C / D 四组）。
> 引擎：`engine/core/Csv.{h,cpp}`（格式本体）+ `engine/project/DataTable.{h,cpp}`
> （配表）+ `engine/i18n/LocaleFile.{h,cpp}`（本地化覆盖层）；
> 编辑器：`editor/TablePanels.{h,cpp}`（两张面板）。

## 1. 它补的是什么洞

两件事的共同点是"**数据在代码里**"，改一次就要重编译一次：

```
grep -rli "datatable\|\.csv" editor/ engine/project/   → 0（2026-11-02 之前）
```

- 角色属性 / 技能 / 掉落 / 文本表只能写成脚本常量或 C++ 数组；
- 译文只有 `engine/i18n/ZhTable.cpp` 一张**编译进去**的静态表，
  策划改一句翻译 = 改 C++ + 重编译 + 重新分发。

## 2. 为什么是 CSV

不是 JSON。三条理由，都来自"谁在用这个文件"：

1. 策划要能用 Excel / WPS 直接打开；
2. 交给不懂代码的人不会因为少一个逗号整份失败；
3. diff 里一行一行看得见，"这一格从 12 改成 15"是可读的。

代价是引号/换行的边角必须自己处理对 —— 所以 `core/Csv` 是**唯一**的
解析器，配表与本地化共用。分开写一定是一边对一个错，而错的那一半会把
一句带逗号的台词从中间劈开，界面上只表现为"多了一列"，**不会报错**。

## 3. 配表（C1）

### 3.1 文件形态

`assets/data/<表名>.csv`，表名 = 文件名去掉扩展名。

```csv
id,#note,name,hp,atk,speed,elite
slime,"starter, 一级就出现",Slime,12,3,1.2,0
bat,,Bat,8,2,2.4,0
boss,,Big Boss,900,40,0.9,1
```

- **第一列约定是 id**，`rowById()` 按它找行；
- `#` 前缀的列是**注释列**：保留、显示、不参与取值；
- **没有类型行**。类型由访问函数决定（`num` / `str` / `flag`）。

为什么不要类型行：多一行就多一处要同步的地方，而 CSV 里那一行策划经常忘改
（改了之后"类型说 int、值是 abc"到底是信谁？）。访问函数是**调用点**决定的，
那里本来就知道要什么类型；写错了返回默认值 + 一条 `TABLE-PARSE` 警告，
不会让整个场景加载失败。

### 3.2 代码里怎么读

```cpp
auto* t = project::TableStore::get().find("enemies");
const int r = t->rowById("slime");
const double hp = t->num(r, "hp");      // 按列名，不是按下标
const bool elite = t->flag(r, "elite");
```

**按列名取，不按下标**：下标会随策划插入一列全体错位，而错位后的值
全都"看起来是个合法数字"——这是最难查的一类静默错误。

`TableStore` 是进程级单例，`loadAll("assets/data")` 后 `find("<表名>")`。
目录不存在 = 成功 + 0 张表（新工程还没配表是正常状态）。

### 3.3 面板

`Authoring ▸ Data tables`：选表 / 改格子 / Save / Reload。

命令（与按钮走同一条路）：

```
table.list                    # TABLE-LIST name= rows= cols=
table.select <name>
table.cell <row> <col>
table.set <row> <col> <value> # 值可带空格
table.save
table.reload
```

## 4. 本地化编辑（C2）

### 4.1 覆盖层，不是第二份词典

`assets/i18n/zh.csv`：

```csv
en,zh
Save,保存（自定义）
```

**它只写"与内置值不同"的行**。这条纪律是整件事的关键：

| 做法 | 后果 |
|---|---|
| 把 2900 多条内置译文全写进 CSV | 静态表以后再改，永远被旧 CSV 掩盖 |
| **只写覆盖行**（现在的做法） | 静态表仍是权威，CSV 只记"我们改过什么" |

与 `project::saveGameFile` 的"合并而非重写"是同一条纪律的两面：那边防
**丢掉别人的键**，这边防**固化出厂值**。

### 4.2 撤销必须"清空重放"

`i18n::addTranslation` 是"后写覆盖"，但撤销一条覆盖没法用"再写一次旧值"
表达 —— 旧值就是内置值，写进去等于没撤销。所以 `apply()` 是
`clearAddedTranslations()` + 重放全部覆盖行。

⚠ 代价：其它模块如果也调过 `addTranslation` 会一起被清掉。目前全仓只有
这里调它（`grep addTranslation` 可证）。将来真有人用，要改成"按 key 撤销"。

### 4.3 面板

`Authoring ▸ Localization`：筛选 / 只看缺译文 / 只看改过的 / 直接改译文 /
Save / Reload。缺译文的行标黄。

命令：

```
i18n.count                    # I18N-UI count= missing= dirty=
i18n.find <text>              # I18N-FIND idx=<i> exact=<0|1>
i18n.row <index>              # I18N-EN / I18N-ROW 两行
i18n.set <index> <zh>
i18n.save
i18n.reload
```

`i18n.find` 是为自动化准备的：键里常有空格与标点，写死下标会随静态表
增删而漂，**先找再改**才是稳的。

## 5. 语义细节（踩出来的）

### 5.1 ⚠⚠ 新资产目录必须进 CMake 复制清单

`assets/data` 与 `assets/i18n` 是**编辑器会写回**的目录，与 `scenes` /
`scripts` 同一档：`copy_directory` + `ALL`，三个 exe 都 `add_dependencies`。

第一次跑 C 组时 `TABLE-LIST-TOTAL count=0` —— 解析器没问题，是构建根本没
把 `assets/data/` 拷到 `build/bin/`。症状与 `assets/expressions` 那条一模一样：
面板里"什么都没有"，日志报的是那个**不存在**的路径，看起来像解析器坏了。

### 5.2 BOM 必须吃掉

Excel 导出的 UTF-8 CSV 一定带 BOM。不吃的话第一个列名是
`"\xEF\xBB\xBFid"`，`column("id")` 永远返回 -1 —— 而表现只是"这一列读不到"。

### 5.3 空行跳过、短行补齐

- 末尾空行（Excel 常留）不当成"一行空数据"；
- 短行补空串：不补的话 `cell(r, c)` 越界。手写表短一行是常事。

### 5.4 空单元格 ≠ 缺省值

`str(r, c, def)` 只在**越界**时返回 `def`；单元格是空串就返回空串。
表里留空是策划表达"没有"的唯一手段，用默认值盖掉它就等于让人没法表达。

### 5.5 数值解析整串必须吃掉

`12abc` 用 `atof` 会静默得到 12（同 `project::setField` / `ui2::parseUiAction`
的那两处判断）。`core::parseNumber` 要求 `*end == '\0'`。

### 5.6 ⚠ 自动化传参不能用中文

Windows 的**环境变量是 ANSI 代码页**，`MYVK_EDITOR_CONSOLE_CMD` 里的中文
到进程里就已经不是 UTF-8 了 —— 写进文件后按 UTF-8 读会抛异常。

这是**自动化钩子的限制**，不是本地化功能的限制：面板里手输的中文走 ImGui
的 UTF-8，完全没有问题。所以判据里用纯 ASCII 的测试值，别拿这条当被测对象
（那会让"中文能不能存"看起来像坏了）。

## 6. 判据表

`tools/verify_data_tables.py`：

| 组 | 验什么 | 关键断言 |
|---|---|---|
| A | `--selftest project` 的 TABLE 组 | 18 条全过；`G1d comma-in-quotes` / `G1f newline-in-quotes` / `G3 bom-stripped` / `G4 unterminated-quote-detected` / `G5b roundtrip-identical` |
| B | `--selftest locale` 的 L 组 | 18 条全过；`L2c tr-applied` / `L3c only-overrides-written` / `L4c tr-back-to-builtin` |
| C | 配表面板（两次启动） | 锚点 6 个；两张表都扫到；`table.set` → `table.save` → **磁盘 `enemies.csv` 里那一格变、其它行原样** |
| D | 本地化面板（三次启动） | 锚点 8 个；`i18n.find` 找到精确键；改 → 存 → **`zh.csv` 只有表头 + 1 行**；重开 `dirty=1` |

**几条不能省的**：

- **C 组的"其它行原样保留"**：整表重写会丢注释列与引号，而丢东西**不报错**。
- **D 组的"只写覆盖行"**：把内置译文全写进去，静态表以后再改就永远被旧 CSV 掩盖。
- **D 组第三次启动**：`dirty=1` 只能来自**文件**（新进程内存里是 0）——
  这是"读的是磁盘而不是内存"的唯一判据。

## 7. 已知边界

- **配表没有类型校验**（见 §3.1）。写错一格 = 默认值 + 一条警告，不阻断。
- **没有引用完整性检查**（表 A 的 `skill_id` 指向表 B 不存在的行）。要做的话
  归 G29-a 的 `AssetScan` 那条路，不是这里。
- **只在编辑器里能改**。运行时（`Game.exe`）读表但不写。
- **`assets/i18n/` 目前只有中文**。加一种语言 = 加一个 `<code>.csv` +
  `i18n::languageCount()` 那一处枚举；面板本身与语言无关。
- **`.dlg.json` 的台词仍不走 i18n**（审计 C2 原文提到的另一半）。台词是
  **资产**不是界面文字，它该走"每个语言一份 dlg 资产"或"对话资产里带
  多语言字段"——那是对话系统的活，与界面词典不是一件事。
