/* 最小协程探针 —— 只用 Lua 公开 API，不碰引擎任何代码。
 * 用途：判定 `lua_resume` 的访问冲突是「库的编译方式」还是「后端代码」。
 * 编译：见同目录 run_probe.cmd
 */
#include <stdio.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

int main(void)
{
    printf("[probe] version = %s\n", LUA_RELEASE);

    lua_State* L = luaL_newstate();
    if (!L) { printf("[probe] luaL_newstate FAILED\n"); return 2; }
    luaL_openlibs(L);

    /* --- 对照 1：主线程 pcall（已知能跑） --- */
    if (luaL_loadstring(L, "local a = ...; return a + 1") != LUA_OK) {
        printf("[probe] load(main) FAILED: %s\n", lua_tostring(L, -1));
        return 3;
    }
    lua_pushinteger(L, 41);
    int rc = lua_pcall(L, 1, 1, 0);
    printf("[probe] pcall      rc=%d got=%lld\n", rc, (long long)lua_tointeger(L, -1));
    lua_pop(L, 1);

    /* --- 被测：协程 resume --- */
    lua_State* co = lua_newthread(L);
    printf("[probe] newthread  co=%p status=%d\n", (void*)co, lua_status(co));

    if (luaL_loadstring(co, "local a = ...; return a + 1") != LUA_OK) {
        printf("[probe] load(co) FAILED: %s\n", lua_tostring(co, -1));
        return 4;
    }
    printf("[probe] loaded    co=%p top=%d\n", (void*)co, lua_gettop(co));
    lua_pushinteger(co, 41);
    printf("[probe] about to resume\n");
    fflush(stdout);

    int nres = -1;
    rc = lua_resume(co, L, 1, &nres);
    printf("[probe] resume     rc=%d nres=%d got=%lld\n",
           rc, nres, (long long)lua_tointeger(co, -1));

    /* --- 第二段：yield / resume 往返 --- */
    lua_State* co2 = lua_newthread(L);
    luaL_loadstring(co2, "coroutine.yield(1) coroutine.yield(2) return 3");
    rc = lua_resume(co2, L, 0, &nres);
    printf("[probe] yield#1    rc=%d(status=%d) got=%lld\n", rc, lua_status(co2),
           (long long)lua_tointeger(co2, -1));
    lua_pop(co2, nres);
    rc = lua_resume(co2, L, 0, &nres);
    printf("[probe] yield#2    rc=%d got=%lld\n", rc, (long long)lua_tointeger(co2, -1));

    lua_close(L);
    printf("[probe] done OK\n");
    return 0;
}
