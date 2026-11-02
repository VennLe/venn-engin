#pragma once
// ============================================================
// engine/script/ScriptEngine —— 极简脚本语言 + 引擎属性绑定 + 热重载
//
// ⚠ **住在这里而不是 editor/ 里**（2026-10-17，G02 搬家）：
//   分层的铁律是 `runtime → engine ← editor`，而**运行时必须能用脚本**
//   —— 脚本引擎只要还留在 editor/ 里，Game.exe 就永远用不上 .vks。
//   搬家时只改了 namespace（editor:: → vks::），接口与语义一字未动：
//   ScriptHost 仍然是"脚本能碰到什么"的唯一接缝。
//   （命名空间叫 `vks` 而不是 `script`：.vks 就是这个语言的名字，
//    也不会和任何目录级的通用名撞车。engine/ 下其余模块用的是
//    core / render / scene / ecs / assets / physics 这一级名字。）
//
// ------------------------------------------------------------
// 一、为什么自己写一个脚本语言，而不是嵌 Lua / Wren
// ------------------------------------------------------------
//   · 本项目是离线构建的教学向引擎，third_party 里没有脚本 VM，
//     引入 = 新增一份需要长期维护的第三方源码；
//   · 编辑器需要的能力其实很窄：**每帧对一个实体做属性动画**。
//     为此背一整套通用语言的语义与 GC 不划算；
//   · 自己写之后，"脚本能碰什么"完全由 ScriptHost 决定 ——
//     语言里根本没有文件/网络/系统调用，安全性是结构上的。
//
//   整套东西只有两个文件，语言特性一目了然，改起来毫无负担。
//   如果将来要换成 Lua：只需要按同一个接缝（ScriptHost +
//   "每帧求值一次"的语义）重新实现本类即可，编辑器其余代码不用动。
//
// ------------------------------------------------------------
// 二、语言（文件扩展名 .vks）
// ------------------------------------------------------------
//   # 注释到行尾
//   变量 = 表达式
//   函数调用(...)                    # 表达式语句
//   if (条件) 语句 [else 语句]
//   while (条件) { 语句... }
//   语句之间用换行或 ';' 分隔
//
//   运算符：+ - * / %   < <= > >= == !=   && || !
//   内置变量：time / dt / frame / index
//   内置函数（数学）：sin cos tan asin acos atan atan2 abs floor ceil
//                     round sqrt pow min max clamp lerp
//   内置函数（引擎）：move moveBy rotate rotateBy scale scaleBy
//                     setColor setIntensity setRange setDirection
//                     setVisible setEmissive
//                     setHealth damage popup      # HUD（U8）
//                     playSequence showDialogue playAnim   # 演出（C6）
//                     spawn setFlag getFlag loadScene      # 世界（C6 第二批）
//                     openUI                              # 界面（C6 第七件）
//                     navGoto navArrived navFailed navStop  # 导航（C8-2）
//                     print
//
//   数值一律是 double；角度（rotate）单位是**度**。
//
// ------------------------------------------------------------
// 三、热重载
// ------------------------------------------------------------
//   每个脚本记录了源文件的 last_write_time。每帧（或定时）扫一遍，
//   时间戳变了就重新读盘 + 重新编译：成功则原子替换，失败则**保留旧的
//   可用版本**并把错误显示在面板上 —— 编辑到一半的语法错误不该让
//   场景里的物体直接僵住。
// ============================================================

#include "ecs/Entity.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scene {
class Scene;
}

namespace vks {

// 编译期 / 运行期错误
struct ScriptError {
    int line = 0;
    std::string message;
    bool ok() const { return message.empty(); }
    std::string toString() const {
        if (message.empty()) return {};
        return "line " + std::to_string(line) + ": " + message;
    }
};

// 脚本与引擎之间的**唯一**接缝：脚本能碰到的世界就这么多。
// 想扩展脚本能力，只需要在这里加字段 + 在求值器里加一个内置函数。
// ---------------------------------------------------------------- 世界动作
//
// 宿主能替脚本做的"世界之外"的事。**全部是窄动作**，理由与 HUD/音频那三个
// 完全一样：脚本一旦能拿到 `physics::SceneBinding` 就能改重力、清世界；
// 而这些能力里没有一件是"玩法"需要的。
//
// ⚠ 为什么物理要绕这么一道（C2 的直接后果）：物理世界**不住在 Scene 上**
//   （`physics::SceneBinding` 由 app 层持有）。`setPos` 如果只写 Transform，
//   物理步进时刚体会把位置"改回去"——表现出来就是"脚本设了位置，物体
//   弹了一下又回原处"，而日志里毫无痕迹。所以必须提供一条"立刻写进刚体"
//   的动作，由 app 层注入。
//
// ⚠ 没有安装的函数 = 该能力在本宿主里不存在。`vel/setVel/impulse` 缺失时
//   **报错**（静默返回 0 会让"忘了加 RigidBody"藏起来）；`grounded` 缺失
//   时返回 false（"不在空中"是合理的默认，且地形缺失时游戏仍应能跑）。
// ---------------------------------------------------------------- 关节（J5-4）
//
// 脚本能不能配关节，决定了"玩法里能不能临时拴住 / 放开一个东西"：
// 被抓的箱子吊上钩子、被打断的吊桥、上了锁就要转钥匙的门 —— 这些都不是
// 场景加载时能定死的，必须由脚本在运行时改。
//
// ⚠ 为什么每个动作都返回 **bool** 而不是 void：
//   传错 kind 字符串、目标没有刚体、实体身上根本没有 `JointComponent`
//   （要先在 Inspector 里 Add Joint）—— 这些都是**配置错误**，必须让那条
//   语句报错。静默成功会变成"我脚本写了关节，怎么没效果"，而根因在另一个
//   工具里（Inspector / 场景文件），从脚本这一侧完全看不见。
//
// ⚠ 为什么没有"创建关节"：关节是**两个已有刚体之间**的关系，而脚本没有
//   能力凭空给一个实体加刚体（那是场景编辑的活）。所以 `set()` 的前提是
//   那个实体身上已经有组件 —— 没有就报错并说清楚。
//
// ⚠ self 一律是**显式参数**（与 S2 的 `setPos(e, ...)` 一致，而不是老的
//   `move(x,y,z)` 那种隐式 self）：脚本最常见的用法是"把别人的门锁上"，
//   隐式 self 会把这类用法逼成 `self()` + 变量中转。
struct ScriptJointActions {
    // `joint(self, other, "hinge")`。other 无效（none）也算失败。
    std::function<bool(ecs::Entity, ecs::Entity, const std::string&)> set;
    // `jointAnchors(self, ax, ay, az, bx, by, bz)` —— 都是各自的**局部**坐标。
    std::function<bool(ecs::Entity, glm::vec3, glm::vec3)> anchors;
    // `jointAxis(self, x, y, z)` —— self 的局部坐标。
    std::function<bool(ecs::Entity, glm::vec3)> axis;
    // `jointLimit(self, on, lo, hi)` —— 铰链是弧度、滑动是米。
    std::function<bool(ecs::Entity, bool, float, float)> limit;
    // `jointMotor(self, speed, force)` —— force <= 0 等于关掉（推不动）。
    std::function<bool(ecs::Entity, float, float)> motor;
    std::function<bool(ecs::Entity, bool)> enable;
    // 返回 false = 那个实体身上本来就没有关节（不是"删失败"）。
    std::function<bool(ecs::Entity)> remove;
    // 返回的是静态字符串（"hinge" / "ball" / ...），nullptr = 没有关节。
    // 不返回 std::string：调用方只读，不拥有。
    std::function<const char*(ecs::Entity)> kindOf;
    // 另一端。没指定 / 找不到时返回**无效句柄**（不是报错）——
    // 脚本里 `if exists(jointTarget(e))` 就能判，不必额外查错。
    std::function<ecs::Entity(ecs::Entity)> target;
};

struct ScriptWorldActions {
    // 谁是玩家。engine 不知道（那是 app 的概念），所以由宿主告诉它。
    std::function<ecs::Entity()> player;

    // 刚体位姿 / 速度。返回 false = 这个实体没有刚体。
    std::function<bool(ecs::Entity, glm::vec3*)> bodyPos;
    std::function<void(ecs::Entity, glm::vec3)> setBodyPos;
    std::function<bool(ecs::Entity, glm::vec3*)> bodyVel;
    std::function<void(ecs::Entity, glm::vec3)> setBodyVel;
    std::function<void(ecs::Entity, glm::vec3)> impulse;  // 质量已含在刚体里
    std::function<bool(ecs::Entity)> grounded;

    // 输入（S3）。动作名走 InputMap，不是键码。
    std::function<bool(const std::string&)> pressed, held, released;
    std::function<float(const std::string&)> axis;

    // ---- 演出（C6 导演 API）----
    //
    // 这三件是"剧本/过场脚本"能不能写的前提：没有它们，`.vks` 只能摆位置、
    // 改颜色、放音效 —— 一台能动的演示，不是游戏。
    //
    // ⚠ 放在 `act` 里而不是 `ScriptHost` 的直属字段：它们是**宿主动作**
    //   （要有 SequencerSystem / DialogueSystem / AnimatorSystem 才成立），
    //   而 `ScriptHost` 的直属 std::function（HUD / 音频）是不依赖任何
    //   engine 子系统的窄动作。分层上这两类不是一个东西。
    //
    // 返回值是 `bool`，不是 `void`：资产不存在 / 实体没挂 Animator 时必须
    // 让脚本那条语句**报错**。静默返回成功会让"我改完剧本怎么没反应"变成
    // 玄学 —— 与 `setVel` 缺失时 `noBody()` 报错同一条纪律。
    //
    // 路径一律**相对 assets/**（`"sequences/opening.seq.json"`、
    // `"ch01_open_01"` 两种写法由各自的 `resolve*Path()` 归一）。
    std::function<bool(const std::string&)> playSequence;
    std::function<bool(const std::string&)> showDialogue;
    // `playAnim(e, "Run")` / `playAnim("Run")`（后者 = 隐式 self）。
    // 目标必须有 `AnimatorComponent` + 骨架里真有这段，否则返回 false。
    std::function<bool(ecs::Entity, const std::string&)> playAnim;

    // ---- 世界控制（C6 第二批）----
    //
    // 演出三件让脚本能"指挥一段"，这四件让脚本能"改一局"：
    // 刷东西 / 记旗标 / 换关卡 / 读旗标。少了它们，`.vks` 写不出
    // "打赢了就进下一关"这种最基础的流程。
    //
    // `spawn(path, pos)` → 新实体。无效句柄 = 失败（模型加载不了）。
    //   ⚠ 返回的是**真句柄**，所以脚本能紧接着 `setPos(e, ...)` /
    //     `playAnim(e, ...)` —— 刷出来的东西当场就能摆弄。
    //   ⚠ 刷出来的实体**不进物理世界**（场景物理在启动/换场景时构建一次）。
    //     要它参与碰撞请在场景里预放；这条限制写在 spawn 的实现旁边。
    //
    // `loadScene(path)` → "请求已受理"，**不是**"已经切完了"。
    //   真正的切换在下一帧开头做：脚本执行时正遍历着旧世界的实体，
    //   这时候销毁世界就是拿迭代器开玩笑。所以语义是"排队 + 下一帧生效"，
    //   返回值只回答"路径存在、请求已记下" —— 于是路径拼错**当场**报错，
    //   而不是等到下一帧黑屏。
    std::function<bool(const std::string&)> loadScene;
    std::function<ecs::Entity(const std::string&, glm::vec3)> spawn;

    // ---- 特效（P1-4，2026-11-02）----
    //
    // 补的是"脚本想出一发火花，只能 `spawn` 一个实体再销毁"这条歪路 ——
    // 那会反复增删实体，踩 `render_resource_lifetime` 铁律（CB 录制中
    // 引用过的对象不能销毁 → submit -4），而且没有池。
    //
    // `playEffect(path)`           在 **self 的世界坐标**出特效
    // `playEffect(path, x, y, z)`  在指定位置出（爆炸点、法阵中心）
    // 返回实例 id（>0 = 真的播了；0 = 资产缺失或池满）。**必须回传**：
    // 脚本可能要 `stopEffect(id)`（打断一个持续型特效）。
    //
    // `shake(amount)`              trauma 0..1，位置取 self（远处自动更轻）
    // `hitstop(seconds)`           顿帧（宿主用真实时间递减）
    // `setTimeScale(scale)`        全局倍速（子弹时间）；<0 一律拒收
    //
    // ⚠ `playSound` **不在这里** —— 它早就有了（`ScriptHost::playSound`，
    //   不依赖任何 engine 子系统）。当初以为缺是因为只 grep 了 `act.` 前缀。
    std::function<uint32_t(ecs::Entity, const std::string&)> playEffect;
    std::function<uint32_t(const std::string&, glm::vec3)> playEffectAt;
    std::function<bool(uint32_t)> stopEffect;
    std::function<void(float, ecs::Entity)> shake;
    std::function<void(float)> hitstop;
    std::function<void(float)> setTimeScale;

    // ---- 玩法状态（P2-6，2026-11-03）----
    //
    // 四个动词，全部作用于 **self**。它们把"回合制逻辑"从脚本里能碰到：
    //   `getAttr("attack")`      → 当前值（含状态加成）
    //   `addStatus("poison")`    → 加一层之后的层数（-1 = 失败）
    //   `damage(10, "fire")`     → **真正扣掉的血**（过量伤害不虚高）
    //   `tickRound()`            → 回合末结算的 dot 条数
    //
    // ⚠ `gameplayHasAttr` 单独一个函数：`getAttr` 返回 0 有两种含义
    //   （属性不存在 / 值就是 0），不区分的话 `getAttr("atk")` 拼错会
    //   静默返回 0 —— 战斗里表现为"攻击力永远是 0"，而日志一行错都没有。
    //
    // ⚠ `damage` 返回的是**扣掉的血**而不是 `amount`：脚本要用它判死亡
    //   （`if (damage(1000) >= hp)`），拿 amount 判会在过量伤害时判错。
    //
    // ⚠⚠ `damage` **和 U8 那个 HUD 扣血是同一个动词**（`damage(n)` /
    //   `damage(n, type)`，arity 1..2）。曾经在 kBuiltins 里登记了两条同名
    //   条目，查表先命中老的那条 `{1,1}` → `damage(100,"physical")` 在
    //   **编译期**报 "expects 1 argument(s), got 2"，整份脚本 not runnable，
    //   而错误行长得完全不像 arity 的问题。同名动词**只能有一条登记**。
    //
    //   两条路径的判定靠返回值：宿主**没有挂 GameplayComponent** 时必须返回
    //   `-1`（`0` 是合法的"打不动"，不能当哨兵）→ 引擎回退到老的 HUD 扣血。
    //   于是老脚本（打木桶）行为不变，新脚本自动吃护甲/易伤/护盾。
    std::function<float(ecs::Entity, const std::string&)> getAttr;
    std::function<bool(ecs::Entity, const std::string&)> gameplayHasAttr;
    std::function<int(ecs::Entity, const std::string&)> addStatus;
    // 返回**真正扣掉的血**；`-1` = 目标没有玩法组件（引擎会回退）。
    std::function<float(ecs::Entity, float, const std::string&)> damage;
    std::function<int()> tickRound;

    // ---- 卡牌（P3-4，2026-11-04）----
    //
    //   `castCard(idx)` → 打出手牌第 idx 张并**结算**，返回实际造成的伤害
    //   `drawCards(n)`  → 抽 n 张，返回实际抽到几张
    //
    // 目标由宿主解析（"第一个别人"，见 `GameplayRuntime::firstOtherGameplayEntity`），
    // 因为脚本层没有"选中"这个概念 —— 让脚本传实体参数会逼作者手写运行时
    // 序号，换一次加载就失效。
    //
    // ⚠ 没有牌组 / 下标越界返回 **0** 而不是报错：卡牌脚本通常在 `on update`
    //   里按按键调用，"手牌空了还按了一下"是完全正常的使用方式，抛异常会让
    //   整份脚本停摆。诊断靠宿主自己的 `GP-CARD-CAST ... ok=0` 日志行。
    std::function<float(ecs::Entity, int)> castCard;
    std::function<int(ecs::Entity, int)> drawCards;

    // ---- 导航（C8-2，2026-11-05）----
    //
    // 补的是"剧情要 NPC 走到某处"这条最基础的演出需求。在它们之前，
    // NPC 的目标只能靠在场景文件里摆一个 `goal` —— 一局只能去一个地方，
    // 而且改路线要重开场景。
    //
    //   `navGoto(x, y, z)` → 1 = 受理（网格在、实体有代理组件）；0 = 没受理
    //   `navArrived()`     → 1 = 到点（`NavAgentComponent::reached`）
    //   `navFailed()`      → 1 = 走不过去（`NavAgentComponent::failed`）
    //   `navStop()`        → 1 = 停下（有没有代理组件都算成功：脚本写
    //                        "停"的时候，本来就没在走也是对的）
    //
    // ⚠ `navGoto` 返回的是"受理"而不是"走到了"：求路发生在**下一帧**的
    //   `NavSystem::tick` 里（`repathInterval` 那条防抖链），当场返回
    //   "到了"是不可能的。所以它只回答"这次请求被记下了吗" ——
    //   真正的进展靠 `navArrived()` / `navFailed()` 轮询。
    //
    // ⚠⚠ `navArrived()` **必须**和 `navFailed()` 成对使用。目标在墙外 /
    //   网格外时 `arrived` 永远是 0，只等 `navArrived()` 的
    //   `while (!navArrived()) wait(0.1)` 会**永远转**（而脚本配额不报错，
    //   只是那一帧什么都不做）。
    //
    // ⚠ 全部作用于 **self**（与 `getAttr` / `castCard` 一致）：脚本没有
    //   "选中"这个概念，让作者手写运行时序号换一次加载就失效。
    //   代价是**"一个导演实体指挥一堆 NPC"现在做不到** —— 那需要
    //   `navGoto(e, x, y, z)` 那种显式实体重载，而 `kBuiltins[]` 的
    //   arity 只能表达"3 个还是 4 个参数"，表达不了"第 0 个参数换类型"，
    //   所以宁可先不做，也不做一个"传错了不报错"的重载。
    //   当前的解法：脚本挂在 NPC 自己身上（巡逻、跟随都是这么写的）。
    std::function<bool(ecs::Entity, glm::vec3)> navGoto;
    std::function<bool(ecs::Entity)> navArrived;
    std::function<bool(ecs::Entity)> navFailed;
    std::function<bool(ecs::Entity)> navStop;

    // ---- 配表（P3-1，G5 的 `svc.table.get` 条款）----
    //
    // 四个动词，`key` 一律写成 `表.行.列`（`"cards.strike.cost"`）：
    //   `tableNum(key)`   → 数字（列不存在 / 值不是数 → 0）
    //   `tableStr(key)`   → 字符串（同上 → ""）
    //   `tableHas(key)`   → 1 = 这一格存在
    //   `tableRows("表")` → 行数（表不存在 → **-1**）
    //
    // ⚠⚠ `tableHas` **必须**单独一个：`tableNum` 返回 0 有三种含义
    //   （表没加载 / 行不存在 / 值真的是 0），而"表没加载"恰好是最常见的
    //   那一种 —— 不区分的话症状是"配表里的数值全是 0"，而日志一行错都没有。
    //   与 `getAttr` / `gameplayHasAttr` 是同一条纪律。
    //
    // ⚠ 切分规则：**第一个点**切表名、**最后一个点**切列名，中间全是行 id。
    //   行 id 里带点是常态（`"goblin.chief"`），表名/列名带点才是异常。
    //   宿主各自实现切分（编辑器与运行时共用 `project::TableStore`）。
    std::function<double(const std::string&)> tableNum;
    std::function<std::string(const std::string&)> tableStr;
    std::function<bool(const std::string&)> tableHas;
    std::function<int(const std::string&)> tableRows;

    // `openUI(path)` → 掀一块界面（C6 第七件，2026-10-31）。
    //   路径相对 assets/（`"ui/docs/inventory.ui.json"`）。**空串 = 关掉**
    //   当前那块。
    // ⚠ 语义是"切显示哪份文档"，不是"造控件"：控件在 `.ui.json` 里。
    //   脚本管的是**演到哪一页**（导演的职责），逐控件改属性是 `setColor`
    //   那一族的事。
    // ⚠ 返回 false 时宿主**必须保住当前那块**（不许先清空再尝试加载）——
    //   "路径写错"的表现该是"界面没变"，不是"屏幕忽然空了而且报错"。
    std::function<bool(const std::string&)> openUI;

    // 剧情旗标（见 `runtime/GameFlags.h` 的头注释）。
    // 存在宿主那边，engine 只认这两次调用 —— 于是"旗标怎么存、进不进
    // 存档、要不要脏标记"全都不是引擎的事。
    // ⚠ `getFlag` 未设过返回 **0**，不报错（"第 1 章没遇见的人，
    //   好感度就是 0"是正确语义，不是笔误）。
    std::function<bool(const std::string&, double)> setFlag;
    std::function<double(const std::string&)> getFlag;

    // 关节（J5-4）。全部未安装时，脚本调用会**报错**而不是静默无操作
    // —— 与 `setVel` 缺失时 `noBody()` 报错同一条纪律。
    ScriptJointActions joints;
};

struct ScriptHost {
    scene::Scene* scene = nullptr;
    ecs::Entity entity{};
    // S2：玩家 / 物理 / 输入。整块一次拷贝（ScriptRuntime 在 tick 里赋值）。
    ScriptWorldActions act;

    // ---- HUD 绑定（U8）----
    //
    // ⚠ 为什么是 std::function 而不是直接指一个 `runtime::HudModel*`：
    //   分层铁律 `runtime → engine ← editor` —— engine 不认识 runtime。
    //   这里开三个**窄动作**（设血 / 扣血 / 弹数字），宿主启动时接到自己的
    //   模型上。脚本因此也**不可能**碰到 HUD 的内部结构（位置、寿命、
    //   对象池），那是渲染细节，不是玩法。
    std::function<void(float)> setHealth;   // 绝对值
    std::function<void(float)> damage;      // 扣血（= setHealth(cur-v)）
    // 在 (x,y,z) 弹一条伤害数字
    std::function<void(float, float, float, float)> popup;

    // ---- 音频（G14-8）----
    //
    // 与上面三个同一套路：**窄动作**，不是把 `audio::AudioSystem` 塞进来。
    // 理由不只是分层 —— 脚本一旦能拿到门面就能 `setMasterGain(0)` 把玩家
    // 永久静音、或者 `shutdown()` 把设备关掉；那是"脚本能改引擎状态"，
    // 而这门语言的设计前提是"脚本只能动自己的实体 + 几个窄动作"。
    //
    // ⚠ `playSound` 的路径是**相对资产根**（"audio/click.wav"）。绝对路径
    //   也接受（AssetPath 认），但脚本里不该出现盘符。
    std::function<void(const std::string&)> playSound;
    std::function<void()> stopAllSounds;                    // 停掉全部一次性音效
    std::function<void(const std::string&, float)> setBusVolume;  // "music" 0..1
};

// ---------------------------------------------------------------- 值
//
// ⚠ 这个结构 S0 从 .cpp 的匿名命名空间**搬到了这里**。理由不是"想公开"：
//   `ScriptInstance::vars`（跨帧状态）要持有它，而实例表住在 `ScriptRuntime`
//   —— 另一个编译单元。留在匿名命名空间里，等于"值类型只在
//   ScriptEngine.cpp 内部存在"，实例化在语言层面就无从谈起（P1 的根）。
//
// S1 会往 `K` 里加 Vec3 / Entity / List。**加的时候必须同步 `asNum()` /
// `truthy()` / `toStr()` 三个 switch**：它们的 `default:` 分支现在会**静默**
// 做转换（字符串 → strtod、非零 → true），新类型落进 default 就是最难查的
// 那类 bug。S1 会把 default 改成报错。
struct Value {
    // S1 加了三种：vec3 / entity / list。
    //
    // ⚠ 为什么 `entity` 不塞进 double：`ecs::Entity` 是 {id, generation}，
    //   generation 就是**防悬空**的那一位。塞进 double 会丢它 —— 于是"物体
    //   被删了、句柄被复用"时脚本会**静默改到别人身上**（最难查的一类 bug）。
    //
    // ⚠ 为什么 `list` 只有这几种用法（count / at / foreach）：它是为
    //   `near()` 服务的，不是通用容器。push / 切片 / 排序一律回 C++ 加窄函数
    //   —— 每多一个集合操作，"脚本里跑出一个 O(n²)"的机会就多一分。
    enum class K { Num, Bool, Str, Vec3, Entity, List };

    K k = K::Num;
    double num = 0.0;
    bool b = false;
    std::string s;
    glm::vec3 v{0.0f};
    ecs::Entity e{};
    std::vector<Value> list;

    static Value number(double v) {
        Value x;
        x.k = K::Num;
        x.num = v;
        return x;
    }
    static Value boolean(bool v) {
        Value x;
        x.k = K::Bool;
        x.b = v;
        return x;
    }
    static Value str(std::string v) {
        Value x;
        x.k = K::Str;
        x.s = std::move(v);
        return x;
    }
    static Value makeVec3(glm::vec3 vec) {
        Value x;
        x.k = K::Vec3;
        x.v = vec;
        return x;
    }
    // `name` 是可选的**显示名缓存**（`toStr()` 要给出 `Enemy#12` 而不是
    // `entity#12`）。引擎的 Entity 本身不存名字，而 Value 拿不到 Scene。
    static Value makeEntity(ecs::Entity ent, std::string name = {}) {
        Value x;
        x.k = K::Entity;
        x.e = ent;
        x.s = std::move(name);
        return x;
    }
    static Value makeList(std::vector<Value> items) {
        Value x;
        x.k = K::List;
        x.list = std::move(items);
        return x;
    }

    // ⚠ S1 之后**不要直接调它**：vec3 / entity / list 落到 default 会静默
    //   变成 0，那正是最难查的那类 bug（"位置怎么变成 0 了"）。求值器里
    //   统一走 `Evaluator::numOf()`，它带行号报类型错误。
    double asNum() const;

    bool truthy() const;
    std::string toStr() const;
    const char* typeName() const;
};

// ---------------------------------------------------------------- 断点（C8）
//
// 断点的最小定义：**(脚本路径, 行号)**。没有"条件断点 / 命中计数 / 日志点"
// —— 那些都要在求值器里多加一层判断，而它们的九成用途是
// `if (hp < 10) print(...)`，脚本自己就能写。断点解决的是脚本**写不出来**
// 的那件事：在引擎把控制权拿走之前停住，看清 `vars` 里是什么。
//
// ⚠ 为什么按**路径**而不是按 `ScriptProgram*`：热重载会重建 program
//   （`ScriptEngine::clear()` 后地址可能复用），而"第 12 行"这件事在
//   作者眼里从来是相对文件的。按路径存的断点能活过热重载 —— 这也正是
//   调脚本时最需要的（改一行、存盘、断点还在原处）。
class BreakpointSet {
public:
    void add(const std::string& path, int line) {
        if (path.empty() || line <= 0) return;
        m_bp[path].insert(line);
    }
    void remove(const std::string& path, int line) {
        auto it = m_bp.find(path);
        if (it == m_bp.end()) return;
        it->second.erase(line);
        if (it->second.empty()) m_bp.erase(it);
    }
    bool has(const std::string& path, int line) const {
        auto it = m_bp.find(path);
        if (it == m_bp.end()) return false;
        return it->second.find(line) != it->second.end();
    }
    // 某个脚本上的全部断点（升序）
    const std::set<int>* linesOf(const std::string& path) const {
        auto it = m_bp.find(path);
        return it == m_bp.end() ? nullptr : &it->second;
    }
    void clear() { m_bp.clear(); }
    std::size_t size() const {
        std::size_t n = 0;
        for (const auto& kv : m_bp) n += kv.second.size();
        return n;
    }
    // 扁平视图（面板 / 控制台共用）。按路径字典序、行号升序 ——
    // 自动化断言才不会随机红。
    std::vector<std::pair<std::string, int>> list() const {
        std::vector<std::pair<std::string, int>> out;
        for (const auto& kv : m_bp)
            for (int l : kv.second) out.emplace_back(kv.first, l);
        return out;
    }
    bool empty() const { return m_bp.empty(); }

private:
    std::map<std::string, std::set<int>> m_bp;
};

// 一次求值需要从外面拿到的全部东西（S0）。
//
// ⚠ 这个结构存在的唯一理由是 **P1**：`vars` 与 `prints` 从"求值器私有"
//   变成"调用方持有"。求值器不再自己 new 一张表，于是变量能活过一帧。
//   `prints` 同理 —— 它以前挂在 `ScriptProgram` 上，而 Program 是**按路径
//   共享**的：两个实体跑同一脚本时，第二个实体的 `run()` 会清掉第一个的
//   输出（两份宿主循环各自"跑完立刻读"，把这个 bug 盖住了）。
struct ScriptRunContext {
    std::unordered_map<std::string, Value>* vars = nullptr;  // 必填（实例状态）
    std::vector<std::string>* prints = nullptr;              // 可空（丢弃输出）
    double time = 0.0;
    double dt = 0.0;
    uint64_t frame = 0;      // 渲染帧号（老脚本的 `frame`，语义不变）
    uint64_t simFrame = 0;   // 固定步号（修 P7）
    uint64_t steps = 0;      // out：本次消耗
    // out（S4）：这一帧**真的执行了**吗。挂起未唤醒时为 false ——
    // ScriptRuntime 用它统计 skipped，宿主据此判断"这个实例本帧是空转"。
    bool executed = false;

    // ---- 断点（C8，2026-11-02）----
    //
    // `breakpoints` 可空（= 没有断点，走原来那条路）。**指针而不是值**：
    // 一张断点表要跨"很多程序 × 很多实例"共享，按值传会让热重载之后
    // 每个实例各持一份副本，改一个断点要遍历全部实例去同步。
    const BreakpointSet* breakpoints = nullptr;
    // out：本步是不是**因为断点/单步停下**的（`executed` 为 false）。
    // ⚠ 与 `Suspend`（wait）分开报：wait 是"脚本自己要等"，
    //   paused 是"调试器把它摁住了" —— ScriptRuntime 对两者的处理不同
    //   （暂停**不能**置 `started`，否则 `on start` 的后半段会被吞掉，
    //   见 ScriptRuntime.cpp 的注释）。
    bool paused = false;
    int pauseLine = 0;       // out：停在哪一行（0 = 没停）
};

// ---------------------------------------------------------------- AST（内部）

struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

// ---------------------------------------------------------------- 协程（S4）
//
// `wait(1.0)` 要"停在原地、下次从这一句后面继续"，而递归求值器做不到这件事
// （递归的"位置"在 CPU 栈上）。于是执行模型换成**显式帧栈**：栈里每一帧
// 记录"正在跑哪个语句列表、跑到第几条"，挂起 = 直接返回，栈留着不动。
//
// ⚠ 定义在 .cpp 里、这里只前向声明，和 Expr/Stmt 同一个理由：它持有
//   `const Stmt*`，而 Stmt 的布局不对外。实例通过 `shared_ptr` 持有它 ——
//   shared_ptr 的删除器在**构造时**就绑定了完整类型，所以头文件里只需要
//   一个不完整类型。
class ScriptCoroutine;

// C8：协程的"暂停位"操作。
//
// ⚠ 为什么是自由函数而不是 `ScriptCoroutine` 的成员：它的布局只在
//   `ScriptEngine.cpp` 里完整（持有 `const Stmt*`），头文件只见到一个
//   前向声明。`ScriptRuntime` 要放行暂停的实例，但它同样看不见布局 ——
//   于是把这两个动作开成自由函数，实现在 `ScriptEngine.cpp` 里。
//   （与 `ScriptEngine::makeCoroutine()` 同一条理由。）
//
// 返回值 `false` = 它本来就没暂停（调用方不必区分，做统计用）。
bool resumeCoroutine(ScriptCoroutine& coro);
bool stepCoroutine(ScriptCoroutine& coro);

// ---------------------------------------------------------------- 钩子（S3）

// `on <hook> [(param)] { ... }` —— 脚本"响应"的入口。
//
// 为什么是**块**而不是函数定义（S-D4）：
//   · 函数要处理"谁调用、参数从哪来、返回值给谁"，而这里只有"引擎在某
//     一刻跑这段"；
//   · 块里的变量仍然落在实例的 vars 里（跨帧存活），不需要闭包、不需要
//     调用栈。加了函数定义就等于把整个语言推向"真语言"，而这条路线的
//     目标是"能写玩法"，不是"能写库"。
enum class HookKind : uint8_t {
    Start,    // 实例第一次执行前一次
    Update,   // 每帧
    Collide,  // 碰上了（非触发体）
    Trigger,  // 进了触发体积
    Exit      // 离开了
};

const char* hookName(HookKind k);
bool hookFromName(const std::string& name, HookKind* out);

// 一个钩子块。⚠ 构造与析构都必须在 .cpp 里定义（`Stmt` 只在实现文件里完整）。
struct HookBlock {
    HookBlock();
    ~HookBlock();

    HookKind kind = HookKind::Update;
    // ★ 与 Stmt::id 共用一个计数器（S4）：协程栈里"挂在这个钩子体里"靠它
    //   在新旧 AST 之间翻译。0 = 未分配（不该出现）。
    uint32_t id = 0;
    // `on collide(other)` 的参数名。空 = 这个钩子不收参数。
    std::string param;
    int line = 1;
    std::vector<StmtPtr> body;
};

// 一帧要喂给某个实例的东西（S3）。相位顺序**写死在 ScriptProgram::runFrame**：
//   ① runStart → ② contacts（按数组顺序）→ ③ 顶层语句 → ④ on update
struct ScriptFrameInput {
    bool runStart = false;  // 实例新建后的第一帧
    struct Contact {
        HookKind kind = HookKind::Collide;  // Collide / Trigger / Exit
        Value other;                        // 对方实体
    };
    std::vector<Contact> contacts;
};

// ---------------------------------------------------------------- 脚本程序

// 一个已编译的脚本。不可拷贝（持有 AST）。
class ScriptProgram {
public:
    // ⚠ 构造与析构都**必须**在 .cpp 里定义：`m_ast` 是 `unique_ptr<Stmt>`，
    //    而 `Stmt` 只在实现文件里完整。默认构造若在头文件里 inline 生成，
    //    编译器为了异常展开要能析构已构造的成员 → 报
    //    "can't delete an incomplete type"（实测 ScriptSelfTest.cpp 第 143 行）。
    ScriptProgram();
    ~ScriptProgram();
    ScriptProgram(const ScriptProgram&) = delete;
    ScriptProgram& operator=(const ScriptProgram&) = delete;

    // 编译源码；失败时 valid()==false，err 里是原因
    bool compile(const std::string& source, const std::string& debugName,
                 ScriptError& err);

    bool valid() const { return m_valid; }
    const std::string& source() const { return m_source; }
    const std::string& debugName() const { return m_debugName; }

    // 求值一次（S0 之前的老接口）。`vars` 每次新建 → **脚本失忆**。
    // ⚠ 保留它只是为了 `--selftest script` 与"单次求值"这类一次性场景；
    //   任何"每帧跑"的地方都必须走下面那个带 `ScriptRunContext` 的重载，
    //   否则就是在 P1 的坑上再盖一层。
    bool run(const ScriptHost& host, double time, double dt, uint64_t frame,
             ScriptError& err);

    // 求值一次（S0）：变量与输出由调用方持有 → 跨帧存活。
    bool run(const ScriptHost& host, ScriptRunContext& ctx, ScriptError& err);

    // 一帧的完整执行（S3/S4）：相位顺序在这里，宿主只管把事件排好。
    // `coro` 是该实例**私有**的执行状态（帧栈 + 等待条件）。多个实例可以
    // 共享同一个 ScriptProgram（编译产物），但绝不能共享协程。
    //
    // C8：`ctx.breakpoints` 非空时，执行到断点行会**停在那条语句之前** ——
    // 直接返回 true（不是错误），并把 `ctx.paused` 置真、`ctx.pauseLine`
    // 填上停住的行号。下一帧若还是暂停态则什么都不做（宿主先 `resume`）。
    bool runFrame(const ScriptHost& host, ScriptRunContext& ctx,
                  ScriptCoroutine& coro, const ScriptFrameInput& in,
                  ScriptError& err);

    // 本脚本里**有语句落在**哪些行（升序去重）。
    //
    // 面板拿它给断点的可选项打对勾 —— 在空行/注释行上点一个断点永远不会
    // 命中，而"点了没反应"是最难自证的那种失败。控制台 `script.bp` 也用
    // 它做行号校验（写错行号当场报错，不是静默接受）。
    const std::vector<int>& executableLines() const { return m_lines; }

    // ★ 热重载对账（S4 / S-D7 第 3 条）：把 `coro` 里指向**旧 AST** 的帧
    // 翻译到本程序上。全部帧都按 `Stmt::id` 对上 → true（从原位置继续）；
    // 任何一帧对不上（增删了语句）→ false，调用方应整体重启实例。
    bool reattach(ScriptCoroutine& coro) const;

    // 编译期收集到的钩子（按声明顺序）。面板 / 自检读它。
    const std::vector<std::unique_ptr<HookBlock>>& hooks() const {
        return m_hooks;
    }

    // 脚本里 print(...) 的输出（**只有老 run() 会写这里**；新接口写到
    // ctx.prints 指向的 vector。保留它是为了不破坏自检的写法）
    const std::vector<std::string>& prints() const { return m_prints; }
    // 求值器内部用的输出出口（新接口不经过这里）
    void addPrint(std::string s);

private:
    // 语句 / 钩子体按 `id` 反查（S4 对账用）。编译成功后建好，只读。
    const Stmt* stmtById(uint32_t id) const;
    // 某个"语句列表"在新 AST 里的位置。ownerId==0 → 顶层。
    const std::vector<StmtPtr>* listFor(uint32_t ownerId, uint8_t branch) const;
    void indexAst();

    std::vector<StmtPtr> m_ast;
    std::unordered_map<uint32_t, const Stmt*> m_byId;
    std::unordered_map<uint32_t, const HookBlock*> m_hookById;
    // ★ S3：**声明区** = 文件开头连续的 `let`。它们在 `on start` 之前先落地。
    //
    // 为什么需要这个分区：相位顺序是 ① start → ③ 顶层 → ④ update（见
    // runFrame），而 `let hp = 100` 属于顶层 —— 照字面执行，`on start` 里
    // 读 `hp` 就是"未定义变量"。而"文件开头一串 let 声明初值"恰恰是最常见
    // 的写法（§5.5 的 enemy_patrol 就是这样）。所以把开头连续的 let 提到
    // start 之前：**只是让声明先存在，顺序仍与源码一致**。
    // ⚠ 只提**开头连续**的那一段：中间夹了别的语句之后的 `let` 留在原位，
    //   这样"先赋值后 let"这种少见写法也不会因分区而改语义。
    std::size_t m_declCount = 0;
    std::vector<std::unique_ptr<HookBlock>> m_hooks;
    // C8：可打断点的行（`executableLines()` 的存储）。编译成功后由
    // `indexAst()` 顺手收集 —— 它已经在遍历全部语句，再走一遍没意义。
    std::vector<int> m_lines;
    std::string m_source;
    std::string m_debugName;
    bool m_valid = false;
    std::vector<std::string> m_prints;
};

// ---------------------------------------------------------------- 脚本资产

// 磁盘上的一个脚本 + 热重载状态
class ScriptAsset {
public:
    ScriptAsset() = default;

    // 由 ScriptEngine 设置"场景里记的那个路径"
    void setRequestedPath(std::string p) { m_requestedPath = std::move(p); }

    const std::string& requestedPath() const { return m_requestedPath; }
    const std::string& resolvedPath() const { return m_resolvedPath; }

    // 读盘 + 编译。返回是否成功（失败时保留上一次可用的程序）
    bool load();

    // 检查文件时间戳，变了就重载。返回 true 表示发生了重载。
    bool poll();

    ScriptProgram* program() { return m_program ? m_program.get() : nullptr; }

    // ★ 编译产物的**单调版本号**（S0）。
    //
    // ⚠ 为什么不是"比较 `program()` 指针"：`ScriptEngine::clear()` 会把整个
    //   `ScriptAsset` 连同 `ScriptProgram` 一起释放，重编译时 allocator
    //   **完全可能把新对象放在同一个地址**。指针相等 → 误判"没变" →
    //   熔断的实例永远不会被解除，而日志上一切正常。这类 bug 只在高频
    //   热重载时偶发，最难查。
    //   0 = 从未成功编译过（文件不存在 / 语法错）。
    uint64_t version() const { return m_version; }

    // 由 ScriptEngine 在创建时注入"版本号发号器"（资产自己不持引擎指针）
    void attachVersionSeq(uint64_t* seq) { m_versionSeq = seq; }

    const ScriptError& lastError() const { return m_lastError; }
    // 累计重载次数（UI 上显示，用来确认热重载真的生效了）
    uint64_t reloadCount() const { return m_reloadCount; }
    bool fileExists() const { return m_fileExists; }

private:
    std::string m_requestedPath;  // 场景里记的那个相对路径
    std::string m_resolvedPath;   // AssetPath 解析后的真实路径
    std::unique_ptr<ScriptProgram> m_program;
    ScriptError m_lastError;
    uint64_t m_reloadCount = 0;
    uint64_t m_version = 0;
    uint64_t* m_versionSeq = nullptr;  // 指向 ScriptEngine::m_versionSeq
    int64_t m_lastWriteTime = 0;
    bool m_fileExists = false;
    bool m_loadedOnce = false;
};

// ---------------------------------------------------------------- 引擎

class ScriptEngine {
public:
    // 按（相对资产根的）路径取脚本，首次访问时加载。
    // 返回的指针在下一次 pollHotReload 之后依然有效（对象不搬）。
    ScriptAsset* get(const std::string& relativePath);

    // 扫描全部已加载脚本的时间戳，变了就重编译。返回本次重载的脚本数。
    int pollHotReload();

    // 立即重载某个脚本（面板上的"Reload"按钮）
    bool reloadNow(const std::string& relativePath);

    // ★ S4：造一个协程（帧栈）给实例用。定义在 .cpp —— ScriptCoroutine
    // 在那里才完整，而 shared_ptr 的删除器在**构造时**就绑定了完整类型，
    // 所以头文件里只需要前向声明。
    std::shared_ptr<ScriptCoroutine> makeCoroutine();

    // 强制重新读盘（丢弃缓存）
    void clear();

    // 最近一次热重载的日志（滚动保留最后若干条）
    const std::vector<std::string>& log() const { return m_log; }
    void logLine(const std::string& s);

    size_t scriptCount() const { return m_assets.size(); }
    // 已加载脚本的路径列表（UI 用）
    std::vector<std::string> loadedPaths() const;

private:
    std::map<std::string, std::unique_ptr<ScriptAsset>> m_assets;
    std::vector<std::string> m_log;
    // 版本号发号器：**跨资产全局单调** —— 所以 `clear()` 之后新编译出来
    // 的资产也不会撞上旧号（见 `ScriptAsset::version()` 的注释）。
    uint64_t m_versionSeq = 0;
};

} // namespace vks
