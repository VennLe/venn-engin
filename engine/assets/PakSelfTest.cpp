#include "assets/Pak.h"

#include "core/Logger.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <iterator>
#include <fstream>
#include <string>
#include <vector>

namespace assets {

namespace {

namespace fs = std::filesystem;

int g_pass = 0;
int g_fail = 0;

void kCheck(const char* what, bool ok, const std::string& detail = {}) {
    if (ok) {
        ++g_pass;
        std::printf("[PASS] K %s\n", what);
    } else {
        ++g_fail;
        std::printf("[FAIL] K %s   %s\n", what, detail.c_str());
    }
}

bool writeFileBytes(const fs::path& p, const std::string& body) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::FILE* f = std::fopen(p.string().c_str(), "wb");
    if (!f) return false;
    const std::size_t n = std::fwrite(body.data(), 1, body.size(), f);
    std::fclose(f);
    return n == body.size();
}

bool readFileBytes(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
    return true;
}

// 可压缩的样本：JSON 那种重复度（真实资产里最常见的形态）
std::string jsonSample(std::size_t times) {
    std::string s;
    for (std::size_t i = 0; i < times; ++i) {
        s += "{\"entity\":\"tree_" + std::to_string(i) +
             "\",\"mesh\":\"models/tree.vmesh\",\"material\":\"materials/leaf\"}\n";
    }
    return s;
}

// 不可压缩的样本：伪随机（模拟 png / 已压缩数据）
std::string randomSample(std::size_t n) {
    std::string s;
    s.resize(n);
    std::uint32_t x = 2463534242u;
    for (std::size_t i = 0; i < n; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s[i] = static_cast<char>(x & 0xFF);
    }
    return s;
}

} // namespace

int runPakSelfTest() {
    g_pass = 0;
    g_fail = 0;
    std::printf("== selftest: pak (K 组：编解码 / 容器 / 坏包) ==\n");

    // ---------------- 编解码 ----------------
    {
        const std::string src = jsonSample(400);
        std::string packed;
        const bool smaller = lz4CompressBlock(src, packed);
        kCheck("lz4 对重复文本能压小", smaller && packed.size() < src.size() / 4,
               "src=" + std::to_string(src.size()) + " packed=" +
                   std::to_string(packed.size()));
        std::string back;
        const bool ok = lz4DecompressBlock(packed.data(), packed.size(),
                                           src.size(), back);
        kCheck("lz4 往返一致（重复文本）", ok && back == src,
               "ok=" + std::to_string(ok ? 1 : 0) +
                   " size=" + std::to_string(back.size()));
    }
    {
        const std::string src = randomSample(20000);
        std::string packed;
        const bool smaller = lz4CompressBlock(src, packed);
        // 随机数据压不动 —— 这是**正常**的，调用方会改存原文
        kCheck("lz4 对随机数据如实报「压不动」（返回 false）", !smaller,
               "packed=" + std::to_string(packed.size()) +
                   " src=" + std::to_string(src.size()));
        // 即使压不动也要能解回来（"压完更大"不等于"压坏了"）
        std::string back;
        const bool ok = lz4DecompressBlock(packed.data(), packed.size(),
                                           src.size(), back);
        kCheck("压不动的那份也能正确解回来", ok && back == src,
               "ok=" + std::to_string(ok ? 1 : 0));
    }
    {
        const std::string src = "a";
        std::string packed;
        lz4CompressBlock(src, packed);
        std::string back;
        const bool ok = lz4DecompressBlock(packed.data(), packed.size(), 1, back);
        kCheck("1 字节也能往返", ok && back == "a", "back=" + back);
    }
    {
        // 长匹配（>19 字节，走 matchLen 的 255 扩展分支）
        const std::string src = std::string(1000, 'x') + "tail";
        std::string packed;
        const bool smaller = lz4CompressBlock(src, packed);
        std::string back;
        const bool ok = lz4DecompressBlock(packed.data(), packed.size(),
                                          src.size(), back);
        kCheck("超长匹配（走 255 扩展分支）往返一致",
               smaller && ok && back == src,
               "packed=" + std::to_string(packed.size()));
    }
    {
        // 坏输入：解压器必须**拒绝**而不是读越界
        const std::string bogus = "\xff\xff\xff\xff\xff";
        std::string out;
        const bool ok = lz4DecompressBlock(bogus.data(), bogus.size(), 4, out);
        kCheck("坏 lz4 流被拒绝（不崩、不超界）", !ok,
               "out=" + std::to_string(out.size()));
    }

    // ---------------- 容器 ----------------
    const fs::path base =
        fs::temp_directory_path() / ("venn_paksel_" + std::to_string(
            static_cast<unsigned long long>(
                std::hash<std::string>{}(std::to_string(
                    static_cast<long long>(std::time(nullptr)))))));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);

    const std::string textA = jsonSample(50);
    const std::string textB = jsonSample(300);
    const std::string blob = randomSample(4096);

    std::vector<PakInput> inputs;
    inputs.push_back({"scenes/main.json", textA});
    inputs.push_back({"materials/ground.vmat", textB});
    inputs.push_back({"textures/noise.png", blob});

    PakWriteStats st;
    std::string err;
    const fs::path pakPath = base / "assets.pak";
    const bool wrote = writePak(pakPath.string(), inputs, st, err, true);
    kCheck("写 pak 成功", wrote, err);
    kCheck("文本条目走了 lz4（至少 1 条）", st.lz4 >= 1,
           "lz4=" + std::to_string(st.lz4) + " store=" +
               std::to_string(st.stored));
    kCheck("随机数据条目走 store（压不动）", st.stored >= 1,
           "stored=" + std::to_string(st.stored));
    kCheck("统计字节自洽（packed <= raw）", st.packedBytes <= st.rawBytes,
           "raw=" + std::to_string(st.rawBytes) + " packed=" +
               std::to_string(st.packedBytes));

    {
        Pak p;
        std::string e;
        const bool opened = p.open(pakPath.string(), e);
        kCheck("打开 pak 成功", opened, e);
        if (opened) {
            kCheck("条目数对", p.fileCount() == 3,
                   std::to_string(p.fileCount()));
            std::string out;
            bool allOk = true;
            std::string first;
            if (!p.read("scenes/main.json", out, e) || out != textA) {
                allOk = false;
                first = "scenes/main.json " + e;
            }
            if (!p.read("materials/ground.vmat", out, e) || out != textB) {
                allOk = false;
                if (first.empty()) first = "materials/ground.vmat " + e;
            }
            if (!p.read("textures/noise.png", out, e) || out != blob) {
                allOk = false;
                if (first.empty()) first = "textures/noise.png " + e;
            }
            kCheck("三条内容逐字节一致（含 store 与 lz4 两种）", allOk, first);
            kCheck("大小写不敏感查找（SCENES/MAIN.JSON）",
                   p.has("SCENES/MAIN.JSON"), "");
            kCheck("反斜杠路径也能查（scenes\\main.json）",
                   p.has("scenes\\main.json"), "");
            kCheck("不存在的条目 → has=false", !p.has("nope.txt"), "");
            std::string miss;
            kCheck("读不存在的条目 → 失败并给原因",
                   !p.read("nope.txt", out, miss) && !miss.empty(), miss);
            kCheck("路径上跳被拒绝（../escape）", !p.has("../escape"), "");
            // 条目数据必须落在 [32, indexOffset) 内
            bool inBounds = true;
            for (const PakEntry& pe : p.entries()) {
                if (pe.offset < 32 || pe.packedSize == 0) inBounds = false;
            }
            kCheck("所有条目偏移都在数据区", inBounds, "");
        }
    }

    // 确定性：同样的输入 → 同样的字节（发布对账要用）
    {
        const fs::path pak2 = base / "assets2.pak";
        std::vector<PakInput> again;
        again.push_back({"textures/noise.png", blob});
        again.push_back({"scenes/main.json", textA});
        again.push_back({"materials/ground.vmat", textB});   // 顺序打乱
        PakWriteStats st2;
        std::string e2;
        const bool ok = writePak(pak2.string(), again, st2, e2, true);
        std::string a, b;
        readFileBytes(pakPath, a);
        readFileBytes(pak2, b);
        kCheck("同样的内容 → 逐字节相同的 pak（顺序无关）",
               ok && !a.empty() && a == b,
               "ok=" + std::to_string(ok ? 1 : 0) + " lenA=" +
                   std::to_string(a.size()) + " lenB=" + std::to_string(b.size()));
    }

    // 坏包：三种坏法都必须**拒绝**（而不是崩）
    {
        std::string bytes;
        readFileBytes(pakPath, bytes);
        // ① 改 magic
        {
            std::string bad = bytes;
            bad[0] = 'X';
            const fs::path p = base / "bad_magic.pak";
            writeFileBytes(p, bad);
            Pak pk;
            std::string e;
            kCheck("magic 被改 → 打开失败", !pk.open(p.string(), e), e);
        }
        // ② 截断（索引区被切掉）
        {
            const fs::path p = base / "truncated.pak";
            writeFileBytes(p, bytes.substr(0, bytes.size() / 2));
            Pak pk;
            std::string e;
            kCheck("文件被截断 → 打开失败", !pk.open(p.string(), e), e);
        }
        // ③ 数据区改一个字节（哈希必须抓到）
        {
            Pak pk;
            std::string e;
            std::uint64_t off = 0;
            std::uint64_t sz = 0;
            if (pk.open(pakPath.string(), e)) {
                for (const PakEntry& pe : pk.entries()) {
                    if (pe.path == "scenes/main.json") {
                        off = pe.offset;
                        sz = pe.packedSize;
                    }
                }
            }
            pk.close();
            kCheck("取到被篡改条目的偏移", off > 0 && sz > 0,
                   "off=" + std::to_string(off));
            if (off > 0 && off < bytes.size()) {
                std::string bad = bytes;
                bad[static_cast<std::size_t>(off)] =
                    static_cast<char>(bad[static_cast<std::size_t>(off)] ^ 0xFF);
                const fs::path p = base / "tampered.pak";
                writeFileBytes(p, bad);
                Pak pk2;
                std::string e2;
                const bool opened = pk2.open(p.string(), e2);
                std::string out;
                std::string readErr;
                bool readOk = false;
                if (opened) readOk = pk2.read("scenes/main.json", out, readErr);
                kCheck("数据被改一个字节 → 读该条目失败（哈希兜住）",
                       opened && !readOk,
                       !opened ? std::string("open failed: ") + e2 : readErr);
            }
        }
    }

    // 写侧的拒绝：路径上跳 / 大小写重名
    {
        std::vector<PakInput> bad1;
        bad1.push_back({"../evil.txt", "x"});
        PakWriteStats s1;
        std::string e1;
        kCheck("写侧拒绝 ../ 路径",
               !writePak((base / "bad1.pak").string(), bad1, s1, e1, true), "");

        std::vector<PakInput> bad2;
        bad2.push_back({"Textures/A.png", "x"});
        bad2.push_back({"textures/a.png", "y"});
        PakWriteStats s2;
        std::string e2;
        kCheck("写侧拒绝大小写重名（Windows 上会互相覆盖）",
               !writePak((base / "bad2.pak").string(), bad2, s2, e2, true), e2);
    }

    fs::remove_all(base, ec);
    std::printf("PAK-TOTAL pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}

} // namespace assets
