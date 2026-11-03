// ---------------------------------------------------------------- 参数取样
//
// 每个绑定都从这里取参数，不再各写一遍 `luaL_checknumber(L, 1)`：
// 参数个数由 `ApiTable` 在注册时校验过，这里只管取值。
inline float aNum(lua_State* L, int i) { return float(luaL_checknumber(L, i)); }
inline int aInt(lua_State* L, int i) { return int(luaL_checkinteger(L, i)); }
inline bool aBool(lua_State* L, int i) {
    luaL_checkany(L, i);
    return lua_toboolean(L, i) != 0;
}
inline const char* aStr(lua_State* L, int i) { return luaL_checkstring(L, i); }
inline glm::vec3 aVec(lua_State* L, int i) { return checkVec3(L, i); }

scene::Scene* sceneOf(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h || !h->scene) luaL_error(L, "no scene bound (self-test / edit mode?)");
    return h->scene;
}

ecs::Entity selfOf(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) luaL_error(L, "no host");
    return h->entity;
}

// 宿主动作缺失 = 这一局没有该能力 → **报错**。
// 静默返回成功会让"我写了怎么没反应"变成玄学（与 `.vks` 的 noBody() 同一条纪律）。
template <typename F>
void needAction(lua_State* L, const F& f, const char* what) {
    if (!f) luaL_error(L, "%s: not available (host did not install this action)", what);
}

// 挂载实体身上的点光 / 聚光（两者取其一）
template <typename Comp>
Comp* compOf(scene::Scene* sc, ecs::Entity e) {
    return sc ? sc->world().get<Comp>(e) : nullptr;
}

// 递归销毁（子节点跟着走）。`World::destroy` 只删一个实体。
void destroyRecursive(scene::Scene& sc, ecs::Entity e) {
    if (!sc.world().alive(e)) return;
    std::vector<ecs::Entity> kids;
    sc.world().each<ecs::HierarchyComponent>(
        [&](ecs::Entity c, ecs::HierarchyComponent& hc) {
            if (hc.parent == e) kids.push_back(c);
        });
    for (ecs::Entity k : kids) destroyRecursive(sc, k);
    if (sc.world().alive(e)) sc.world().destroy(e);
}

// ---------------------------------------------------------------- engine
int l_engine_self(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "self(): no host");
    lua_pushinteger(L, lua_Integer(packEntity(h->entity)));
    return 1;
}

int l_engine_valid(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h || !h->scene) { lua_pushboolean(L, 0); return 1; }
    const ecs::Entity e = unpackEntity(uint64_t(aInt(L, 1)));
    lua_pushboolean(L, h->scene->world().alive(e) ? 1 : 0);
    return 1;
}

int l_engine_name(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "engine.name");
    const std::string* n = sc->world().name(e);
    lua_pushstring(L, n ? n->c_str() : "");
    return 1;
}

int l_engine_vec3(lua_State* L) {
    pushVec3(L, glm::vec3(aNum(L, 1), aNum(L, 2), aNum(L, 3)));
    return 1;
}

int l_engine_dist(lua_State* L) {
    const glm::vec3 a = aVec(L, 1), b = aVec(L, 2);
    lua_pushnumber(L, double(glm::length(a - b)));
    return 1;
}

int l_engine_clamp(lua_State* L) {
    const double v = luaL_checknumber(L, 1);
    const double lo = luaL_checknumber(L, 2);
    const double hi = luaL_checknumber(L, 3);
    lua_pushnumber(L, v < lo ? lo : (v > hi ? hi : v));
    return 1;
}

int l_engine_lerp(lua_State* L) {
    const double a = luaL_checknumber(L, 1);
    const double b = luaL_checknumber(L, 2);
    const double t = luaL_checknumber(L, 3);
    lua_pushnumber(L, a + (b - a) * t);
    return 1;
}

int l_engine_log(lua_State* L) {
    std::string s;
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        size_t len = 0;
        const char* p = luaL_tolstring(L, i, &len);
        if (i > 1) s += ' ';
        if (p) s.append(p, len);
        lua_pop(L, 1);  // luaL_tolstring 压的那一份
    }
    VK_LOG_INFO("LUA-PRINT %s", s.c_str());
    return 0;
}

int l_engine_warn(lua_State* L) {
    VK_LOG_WARN("LUA-WARN %s", aStr(L, 1));
    return 0;
}

// `engine.wait` / `waitFrames` / `waitUntil` 都是"把意图寄存到 backend，
// 然后 yield"。实例 resume 时按 `takePending*` 决定还等不等。
int l_engine_wait(lua_State* L) {
    LuaBackend* be = backendOf(L);
    const double sec = luaL_checknumber(L, 1);
    if (sec < 0.0) return luaL_error(L, "wait(seconds): must be >= 0");
    be->setPendingWait(sec);
    return lua_yield(L, 0);
}

int l_engine_waitFrames(lua_State* L) {
    LuaBackend* be = backendOf(L);
    const int n = aInt(L, 1);
    if (n < 0) return luaL_error(L, "waitFrames(n): must be >= 0");
    be->setPendingWait(-double(n));  // 负数 = 按帧计（见 LuaBackend.h 的语义）
    return lua_yield(L, 0);
}

int l_engine_waitUntil(lua_State* L) {
    LuaBackend* be = backendOf(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    be->setPendingUntil(luaL_ref(L, LUA_REGISTRYINDEX));
    return lua_yield(L, 0);
}

// ---------------------------------------------------------------- world
int l_world_pos(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "world.pos");
    pushVec3(L, glm::vec3(sc->worldMatrix(e)[3]));
    return 1;
}

int l_world_setPos(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "world.setPos");
    const glm::vec3 p = aVec(L, 2);
    if (glm::any(glm::isnan(p))) return luaL_error(L, "setPos: NaN position");
    setWorldPosImpl(hostOf(L), e, p);
    (void)sc;
    return 0;
}

int l_world_rot(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "world.rot");
    const auto* t = compOf<ecs::TransformComponent>(sc, e);
    pushVec3(L, t ? t->rotation : glm::vec3(0.0f));
    return 1;
}

int l_world_setRot(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "world.setRot");
    auto* t = sc->world().get<ecs::TransformComponent>(e);
    if (!t) return luaL_error(L, "world.setRot: entity #%u has no transform", unsigned(e.id));
    t->rotation = aVec(L, 2);  // **弧度**
    return 0;
}

int l_world_rotY(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "world.rotY");
    const auto* t = compOf<ecs::TransformComponent>(sc, e);
    lua_pushnumber(L, t ? double(t->rotation.z) : 0.0);
    return 1;
}

int l_world_scale(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "world.scale");
    const auto* t = compOf<ecs::TransformComponent>(sc, e);
    pushVec3(L, t ? t->scale : glm::vec3(1.0f));
    return 1;
}

int l_world_setScale(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "world.setScale");
    auto* t = sc->world().get<ecs::TransformComponent>(e);
    if (!t) return luaL_error(L, "world.setScale: entity #%u has no transform", unsigned(e.id));
    t->scale = aVec(L, 2);
    return 0;
}

int l_world_moveBy(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "world.moveBy");
    auto* t = sc->world().get<ecs::TransformComponent>(e);
    if (!t) return luaL_error(L, "world.moveBy: entity #%u has no transform", unsigned(e.id));
    t->position += aVec(L, 2);
    return 0;
}

int l_world_near(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const glm::vec3 c = aVec(L, 1);
    const float r = aNum(L, 2);
    const int limit = (lua_gettop(L) >= 3) ? aInt(L, 3) : 0;

    struct Hit { float d; ecs::Entity e; };
    std::vector<Hit> hits;
    sc->world().each<ecs::TransformComponent>(
        [&](ecs::Entity e, ecs::TransformComponent&) {
            const glm::vec3 p(sc->worldMatrix(e)[3]);
            const glm::vec3 diff = p - c;
            const float d2 = glm::dot(diff, diff);
            if (d2 <= r * r) hits.push_back(Hit{std::sqrt(d2), e});
        });
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.d != b.d) return a.d < b.d;
        return a.e.id < b.e.id;  // 同距离按 id 定序 → 确定性
    });
    if (limit > 0 && int(hits.size()) > limit) hits.resize(size_t(limit));

    lua_createtable(L, int(hits.size()), 0);
    for (size_t i = 0; i < hits.size(); ++i) {
        lua_pushinteger(L, lua_Integer(packEntity(hits[i].e)));
        lua_rawseti(L, -2, int(i) + 1);  // 1 基下标
    }
    return 1;
}

int l_world_find(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = sc->find(aStr(L, 1));
    lua_pushinteger(L, lua_Integer(packEntity(e)));
    return 1;
}

int l_world_count(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    int n = 0;
    sc->world().each<ecs::TransformComponent>([&](ecs::Entity, ecs::TransformComponent&) { ++n; });
    lua_pushinteger(L, n);
    return 1;
}

int l_world_spawn(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "world.spawn: no host");
    needAction(L, h->act.spawn, "world.spawn");
    const char* path = aStr(L, 1);
    glm::vec3 p(0.0f);
    if (lua_gettop(L) >= 2) p = aVec(L, 2);
    const ecs::Entity e = h->act.spawn(path, p);
    lua_pushinteger(L, lua_Integer(packEntity(e)));
    return 1;
}

int l_world_despawn(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "world.despawn");
    const ecs::Entity self = selfOf(L);
    if (e == self && !sc->world().alive(e)) return 0;
    destroyRecursive(*sc, e);
    return 0;
}

int l_world_loadScene(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "world.loadScene: no host");
    needAction(L, h->act.loadScene, "world.loadScene");
    lua_pushboolean(L, h->act.loadScene(aStr(L, 1)) ? 1 : 0);
    return 1;
}

// ---------------------------------------------------------------- actor
int l_actor_visible(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "actor.visible");
    const auto* v = compOf<ecs::VisibilityComponent>(sc, e);
    lua_pushboolean(L, (v ? v->visible : true) ? 1 : 0);
    return 1;
}

int l_actor_setVisible(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    // 两种签名：`setVisible(e, false)` / `setVisible(false)`（对 self）
    if (lua_gettop(L) == 1 && lua_type(L, 1) == LUA_TBOOLEAN) {
        ecs::World& w = sc->world();
        const ecs::Entity self = selfOf(L);
        auto* v = w.get<ecs::VisibilityComponent>(self);
        if (!v) v = &w.add<ecs::VisibilityComponent>(self);
        v->visible = aBool(L, 1);
        return 0;
    }
    const ecs::Entity e = needEntity(L, 1, "actor.setVisible");
    ecs::World& w = sc->world();
    auto* v = w.get<ecs::VisibilityComponent>(e);
    if (!v) v = &w.add<ecs::VisibilityComponent>(e);
    v->visible = (lua_gettop(L) >= 2) ? aBool(L, 2) : true;
    return 0;
}

int l_actor_setShadow(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "actor.setShadow");
    ecs::World& w = sc->world();
    auto* v = w.get<ecs::VisibilityComponent>(e);
    if (!v) v = &w.add<ecs::VisibilityComponent>(e);
    v->castShadow = aBool(L, 2);
    return 0;
}

int l_actor_vel(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "actor.vel: no host");
    const ecs::Entity e = needEntity(L, 1, "actor.vel");
    needAction(L, h->act.bodyVel, "actor.vel");
    glm::vec3 v(0.0f);
    if (!h->act.bodyVel(e, &v))
        return luaL_error(L, "actor.vel: entity #%u has no rigid body", unsigned(e.id));
    pushVec3(L, v);
    return 1;
}

int l_actor_setVel(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "actor.setVel: no host");
    const ecs::Entity e = needEntity(L, 1, "actor.setVel");
    needAction(L, h->act.setBodyVel, "actor.setVel");
    h->act.setBodyVel(e, aVec(L, 2));
    return 0;
}

int l_actor_impulse(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "actor.impulse: no host");
    const ecs::Entity e = needEntity(L, 1, "actor.impulse");
    needAction(L, h->act.impulse, "actor.impulse");
    h->act.impulse(e, aVec(L, 2));
    return 0;
}

int l_actor_grounded(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "actor.grounded: no host");
    const ecs::Entity e = needEntity(L, 1, "actor.grounded");
    lua_pushboolean(L, (h->act.grounded && h->act.grounded(e)) ? 1 : 0);
    return 1;
}

int l_actor_setEmissive(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "actor.setEmissive");
    auto* mc = sc->world().get<ecs::MaterialComponent>(e);
    if (!mc || !mc->material)
        return luaL_error(L, "actor.setEmissive: entity #%u has no material", unsigned(e.id));
    mc->material->emissive = glm::vec3(aNum(L, 2), aNum(L, 3), aNum(L, 4));
    return 0;
}

// ---------------------------------------------------------------- input
int l_input_pressed(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* a = aStr(L, 1);
    lua_pushboolean(L, (h && h->act.pressed && h->act.pressed(a)) ? 1 : 0);
    return 1;
}
int l_input_held(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* a = aStr(L, 1);
    lua_pushboolean(L, (h && h->act.held && h->act.held(a)) ? 1 : 0);
    return 1;
}
int l_input_released(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* a = aStr(L, 1);
    lua_pushboolean(L, (h && h->act.released && h->act.released(a)) ? 1 : 0);
    return 1;
}
int l_input_axis(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* a = aStr(L, 1);
    lua_pushnumber(L, (h && h->act.axis) ? double(h->act.axis(a)) : 0.0);
    return 1;
}

// ---------------------------------------------------------------- anim
int l_anim_play(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "anim.play: no host");
    const ecs::Entity e = needEntity(L, 1, "anim.play");
    needAction(L, h->act.playAnim, "anim.play");
    lua_pushboolean(L, h->act.playAnim(e, aStr(L, 2)) ? 1 : 0);
    return 1;
}

// ---------------------------------------------------------------- nav
int l_nav_goto(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "nav.goto: no host");
    const ecs::Entity e = needEntity(L, 1, "nav.goto");
    needAction(L, h->act.navGoto, "nav.goto");
    lua_pushboolean(L, h->act.navGoto(e, glm::vec3(aNum(L, 2), aNum(L, 3), aNum(L, 4))) ? 1 : 0);
    return 1;
}
int l_nav_arrived(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const ecs::Entity e = needEntity(L, 1, "nav.arrived");
    lua_pushboolean(L, (h && h->act.navArrived && h->act.navArrived(e)) ? 1 : 0);
    return 1;
}
int l_nav_failed(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const ecs::Entity e = needEntity(L, 1, "nav.failed");
    lua_pushboolean(L, (h && h->act.navFailed && h->act.navFailed(e)) ? 1 : 0);
    return 1;
}
int l_nav_stop(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const ecs::Entity e = needEntity(L, 1, "nav.stop");
    lua_pushboolean(L, (h && h->act.navStop && h->act.navStop(e)) ? 1 : 0);
    return 1;
}

// ---------------------------------------------------------------- fx
int l_fx_play(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "fx.play: no host");
    const char* path = aStr(L, 1);
    uint32_t id = 0;
    if (lua_gettop(L) >= 4) {
        needAction(L, h->act.playEffectAt, "fx.play(path, x, y, z)");
        id = h->act.playEffectAt(path, glm::vec3(aNum(L, 2), aNum(L, 3), aNum(L, 4)));
    } else {
        needAction(L, h->act.playEffect, "fx.play(path)");
        id = h->act.playEffect(h->entity, path);
    }
    lua_pushinteger(L, lua_Integer(id));
    return 1;
}
int l_fx_stop(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "fx.stop: no host");
    needAction(L, h->act.stopEffect, "fx.stop");
    lua_pushboolean(L, h->act.stopEffect(uint32_t(aInt(L, 1))) ? 1 : 0);
    return 1;
}
int l_fx_shake(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "fx.shake: no host");
    needAction(L, h->act.shake, "fx.shake");
    h->act.shake(aNum(L, 1), h->entity);
    return 0;
}
int l_fx_hitstop(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "fx.hitstop: no host");
    needAction(L, h->act.hitstop, "fx.hitstop");
    h->act.hitstop(aNum(L, 1));
    return 0;
}
int l_fx_timeScale(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "fx.timeScale: no host");
    needAction(L, h->act.setTimeScale, "fx.timeScale");
    const float v = aNum(L, 1);
    if (!(v >= 0.0f)) return luaL_error(L, "fx.timeScale: must be >= 0");
    h->act.setTimeScale(v);
    return 0;
}

// ---------------------------------------------------------------- audio
int l_audio_play(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* p = aStr(L, 1);
    if (h && h->playSound) h->playSound(p);
    return 0;
}
int l_audio_stopAll(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (h && h->stopAllSounds) h->stopAllSounds();
    return 0;
}
int l_audio_bus(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* bus = aStr(L, 1);
    const float v = aNum(L, 2);
    if (h && h->setBusVolume) h->setBusVolume(bus, v);
    return 0;
}

// ---------------------------------------------------------------- light
int l_light_color(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "light.color");
    const glm::vec3 c(aNum(L, 2), aNum(L, 3), aNum(L, 4));
    bool any = false;
    if (auto* pl = sc->world().get<ecs::PointLightComponent>(e)) { pl->color = c; any = true; }
    if (auto* sl = sc->world().get<ecs::SpotLightComponent>(e)) { sl->color = c; any = true; }
    if (!any) return luaL_error(L, "light.color: entity #%u has no light", unsigned(e.id));
    return 0;
}
int l_light_intensity(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "light.intensity");
    const float v = aNum(L, 2);
    bool any = false;
    if (auto* pl = sc->world().get<ecs::PointLightComponent>(e)) { pl->intensity = v; any = true; }
    if (auto* sl = sc->world().get<ecs::SpotLightComponent>(e)) { sl->intensity = v; any = true; }
    if (!any) return luaL_error(L, "light.intensity: entity #%u has no light", unsigned(e.id));
    return 0;
}
int l_light_range(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "light.range");
    const float v = aNum(L, 2);
    bool any = false;
    if (auto* pl = sc->world().get<ecs::PointLightComponent>(e)) { pl->range = v; any = true; }
    if (auto* sl = sc->world().get<ecs::SpotLightComponent>(e)) { sl->range = v; any = true; }
    if (!any) return luaL_error(L, "light.range: entity #%u has no light", unsigned(e.id));
    return 0;
}
int l_light_direction(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "light.direction");
    auto* sl = sc->world().get<ecs::SpotLightComponent>(e);
    if (!sl) return 0;  // 点光没有方向：空操作（见 ApiTable 的 note）
    const glm::vec3 d(aNum(L, 2), aNum(L, 3), aNum(L, 4));
    sl->direction = glm::length(d) > 1e-6f ? glm::normalize(d) : glm::vec3(0, -1, 0);
    return 0;
}

// ---------------------------------------------------------------- ui
int l_ui_health(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const float v = aNum(L, 1);
    if (h && h->setHealth) h->setHealth(v);
    return 0;
}

int l_ui_damage(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return 0;
    const float v = aNum(L, 1);
    // 带伤害类型 → 先走玩法属性（护甲/易伤/护盾）；宿主没挂玩法组件时
    // `act.damage` 返回 -1，回退到 HUD 扣血。老脚本行为不变。
    if (lua_gettop(L) >= 2) {
        const char* type = aStr(L, 2);
        if (h->act.damage) {
            const float real = h->act.damage(h->entity, v, type);
            if (real >= 0.0f) { lua_pushnumber(L, double(real)); return 1; }
        }
    }
    if (h->damage) h->damage(v);
    return 0;
}

int l_ui_popup(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h || !h->popup) return 0;
    glm::vec3 p(0.0f);
    if (h->scene && h->scene->world().alive(h->entity))
        p = glm::vec3(h->scene->worldMatrix(h->entity)[3]);
    h->popup(p.x, p.y, p.z, aNum(L, 1));
    return 0;
}

int l_ui_dialogue(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "ui.dialogue: no host");
    needAction(L, h->act.showDialogue, "ui.dialogue");
    lua_pushboolean(L, h->act.showDialogue(aStr(L, 1)) ? 1 : 0);
    return 1;
}

int l_ui_sequence(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "ui.sequence: no host");
    needAction(L, h->act.playSequence, "ui.sequence");
    lua_pushboolean(L, h->act.playSequence(aStr(L, 1)) ? 1 : 0);
    return 1;
}

int l_ui_open(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "ui.open: no host");
    needAction(L, h->act.openUI, "ui.open");
    lua_pushboolean(L, h->act.openUI(aStr(L, 1)) ? 1 : 0);
    return 1;
}

// ---------------------------------------------------------------- game
int l_game_attr(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "game.attr");
    const char* key = aStr(L, 2);
    auto* sv = sc->world().get<ecs::ScriptVarsComponent>(e);
    if (sv) {
        auto it = sv->values.find(key);
        if (it != sv->values.end()) { lua_pushnumber(L, it->second); return 1; }
    }
    // 给了默认值 → **写回**。不写回的话 `if attr(e,"hp",100) < 20` 每次
    // 都重新取默认值，"血慢慢掉"永远不生效（`.vks` 的同一条修复）。
    if (lua_gettop(L) >= 3) {
        const double d = luaL_checknumber(L, 3);
        if (!sv) sv = &sc->world().add<ecs::ScriptVarsComponent>(e);
        sv->values[key] = d;
        lua_pushnumber(L, d);
        return 1;
    }
    return luaL_error(L, "game.attr '%s' not set on this entity "
                         "(pass a default: game.attr(e, \"%s\", 0))", key, key);
}

int l_game_setAttr(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "game.setAttr");
    const char* key = aStr(L, 2);
    const double v = luaL_checknumber(L, 3);
    auto& sv = sc->world().add<ecs::ScriptVarsComponent>(e);
    sv.values[key] = v;
    return 0;
}

int l_game_stat(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.stat: no host");
    const ecs::Entity e = needEntity(L, 1, "game.stat");
    if (!h->act.getAttr) return luaL_error(L, "game.stat: gameplay not installed");
    lua_pushnumber(L, double(h->act.getAttr(e, aStr(L, 2))));
    return 1;
}

int l_game_hasStat(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const ecs::Entity e = needEntity(L, 1, "game.hasStat");
    lua_pushboolean(L, (h && h->act.gameplayHasAttr &&
                        h->act.gameplayHasAttr(e, aStr(L, 2))) ? 1 : 0);
    return 1;
}

int l_game_worldAttr(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const char* key = aStr(L, 1);
    auto& m = sc->scriptVars();
    auto it = m.find(key);
    if (it != m.end()) { lua_pushnumber(L, it->second); return 1; }
    if (lua_gettop(L) >= 2) {
        const double d = luaL_checknumber(L, 2);
        m[key] = d;
        lua_pushnumber(L, d);
        return 1;
    }
    return luaL_error(L, "game.worldAttr '%s' not set "
                         "(pass a default: game.worldAttr(\"%s\", 0))", key, key);
}

int l_game_setWorldAttr(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    sc->scriptVars()[aStr(L, 1)] = luaL_checknumber(L, 2);
    return 0;
}

int l_game_status(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.status: no host");
    const ecs::Entity e = needEntity(L, 1, "game.status");
    if (!h->act.addStatus) return luaL_error(L, "game.status: gameplay not installed");
    lua_pushinteger(L, h->act.addStatus(e, aStr(L, 2)));
    return 1;
}

int l_game_round(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.round: no host");
    if (!h->act.tickRound) return luaL_error(L, "game.round: gameplay not installed");
    lua_pushinteger(L, h->act.tickRound());
    return 1;
}

int l_game_flag(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.flag: no host");
    const char* name = aStr(L, 1);
    if (lua_gettop(L) >= 2) {
        needAction(L, h->act.setFlag, "game.flag(name, value)");
        h->act.setFlag(name, luaL_checknumber(L, 2));
        return 0;
    }
    // 未设过 → 0，不报错（"第 1 章没遇见的人好感度就是 0"是正确语义）
    lua_pushnumber(L, h->act.getFlag ? h->act.getFlag(name) : 0.0);
    return 1;
}

int l_game_castCard(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.castCard: no host");
    const ecs::Entity e = needEntity(L, 1, "game.castCard");
    if (!h->act.castCard) return luaL_error(L, "game.castCard: cards not installed");
    lua_pushnumber(L, double(h->act.castCard(e, aInt(L, 2))));
    return 1;
}

int l_game_drawCards(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.drawCards: no host");
    const ecs::Entity e = needEntity(L, 1, "game.drawCards");
    if (!h->act.drawCards) return luaL_error(L, "game.drawCards: cards not installed");
    lua_pushinteger(L, h->act.drawCards(e, aInt(L, 2)));
    return 1;
}

int l_game_tableNum(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h || !h->act.tableNum) { lua_pushnumber(L, 0.0); return 1; }
    lua_pushnumber(L, h->act.tableNum(aStr(L, 1)));
    return 1;
}
int l_game_tableStr(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h || !h->act.tableStr) { lua_pushstring(L, ""); return 1; }
    const std::string s = h->act.tableStr(aStr(L, 1));
    lua_pushlstring(L, s.c_str(), s.size());
    return 1;
}
int l_game_tableHas(lua_State* L) {
    ScriptHost* h = hostOf(L);
    lua_pushboolean(L, (h && h->act.tableHas && h->act.tableHas(aStr(L, 1))) ? 1 : 0);
    return 1;
}
int l_game_tableRows(lua_State* L) {
    ScriptHost* h = hostOf(L);
    lua_pushinteger(L, (h && h->act.tableRows) ? h->act.tableRows(aStr(L, 1)) : -1);
    return 1;
}

// ---- 关节九件。全部**显式**传 self（见 ScriptJointActions 的注释）----
int l_game_joint(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.joint: no host");
    const ecs::Entity e = needEntity(L, 1, "game.joint");
    const ecs::Entity other = unpackEntity(uint64_t(aInt(L, 2)));
    needAction(L, h->act.joints.set, "game.joint");
    lua_pushboolean(L, h->act.joints.set(e, other, aStr(L, 3)) ? 1 : 0);
    return 1;
}
int l_game_jointAnchors(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.jointAnchors: no host");
    const ecs::Entity e = needEntity(L, 1, "game.jointAnchors");
    needAction(L, h->act.joints.anchors, "game.jointAnchors");
    lua_pushboolean(L, h->act.joints.anchors(e, aVec(L, 2), aVec(L, 3)) ? 1 : 0);
    return 1;
}
int l_game_jointAxis(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.jointAxis: no host");
    const ecs::Entity e = needEntity(L, 1, "game.jointAxis");
    needAction(L, h->act.joints.axis, "game.jointAxis");
    lua_pushboolean(L, h->act.joints.axis(e, aVec(L, 2)) ? 1 : 0);
    return 1;
}
int l_game_jointLimit(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.jointLimit: no host");
    const ecs::Entity e = needEntity(L, 1, "game.jointLimit");
    needAction(L, h->act.joints.limit, "game.jointLimit");
    lua_pushboolean(L, h->act.joints.limit(e, aBool(L, 2), aNum(L, 3), aNum(L, 4)) ? 1 : 0);
    return 1;
}
int l_game_jointMotor(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.jointMotor: no host");
    const ecs::Entity e = needEntity(L, 1, "game.jointMotor");
    needAction(L, h->act.joints.motor, "game.jointMotor");
    lua_pushboolean(L, h->act.joints.motor(e, aNum(L, 2), aNum(L, 3)) ? 1 : 0);
    return 1;
}
int l_game_jointOn(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.jointOn: no host");
    const ecs::Entity e = needEntity(L, 1, "game.jointOn");
    needAction(L, h->act.joints.enable, "game.jointOn");
    lua_pushboolean(L, h->act.joints.enable(e, aBool(L, 2)) ? 1 : 0);
    return 1;
}
int l_game_jointRemove(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.jointRemove: no host");
    const ecs::Entity e = needEntity(L, 1, "game.jointRemove");
    needAction(L, h->act.joints.remove, "game.jointRemove");
    lua_pushboolean(L, h->act.joints.remove(e) ? 1 : 0);
    return 1;
}
int l_game_jointKind(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.jointKind: no host");
    const ecs::Entity e = needEntity(L, 1, "game.jointKind");
    const char* k = h->act.joints.kindOf ? h->act.joints.kindOf(e) : nullptr;
    if (k) lua_pushstring(L, k); else lua_pushnil(L);
    return 1;
}
int l_game_jointTarget(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.jointTarget: no host");
    const ecs::Entity e = needEntity(L, 1, "game.jointTarget");
    const ecs::Entity t = h->act.joints.target ? h->act.joints.target(e) : ecs::Entity{};
    lua_pushinteger(L, lua_Integer(packEntity(t)));
    return 1;
}

