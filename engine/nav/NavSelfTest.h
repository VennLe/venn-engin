#pragma once
// nav/NavSelfTest —— `Game.exe --selftest nav`（纯 CPU，不要 GPU、不要窗口）
//
// A 网格 / B A* / C 视线+平滑 / D 吸附 / E 代理推进 / F 存盘往返 / G ASCII
namespace nav {
int runNavSelfTest();
} // namespace nav
