/* test_slice3.c — 切片 3 其余模块的纯逻辑自测（rules / timeutil /
 * logs / im 分类与预览 / system 纯函数 / shred 与 recycle 的
 * Linux 降级行为 / process 空集）。
 *
 *   gcc -O2 -std=c11 -Wall -Wextra -Werror -Isrc -Ithird_party/sqlite3 \
 *       -Ithird_party/cjson -pthread -o build/test_slice3 \
 *       tests/test_slice3.c src/rules.c src/timeutil.c src/logs.c \
 *       src/im.c src/system.c src/privacy.c src/shred.c src/recycle.c \
 *       src/process.c src/safety.c src/util.c \
 *       third_party/sqlite3/sqlite3.c third_party/cjson/cJSON.c \
 *       -ladvapi32 -lshell32 -lwinhttp -lbcrypt   (仅 Windows 交叉时)
 *   ./build/test_slice3    # 退出码 0=全过
 *                          #（内置 rules.json 是编译进来的 rules_data.h，
 *                          # 不依赖运行目录；logs 测试用 /tmp 做 LOCALAPPDATA）
 *   x86_64-w64-mingw32-gcc ... -o build/test_slice3.exe ... # Windows 交叉只编不过
 *
 * 断言对照：rules.rs（validate / update 语义）、im.rs tests、
 * system.rs tests（safe_for_quoted_cmd_path / valid_recovery_paths）。
 * 网络相关（rules_update 下载）不测：rules_update.json 的 url 为空，
 * update 应直接报错而不发请求。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "im.h"
#include "logs.h"
#include "privacy.h"
#include "cdc_process.h"
#include "recycle.h"
#include "rules.h"
#include "safety.h"
#include "shred.h"
#include "system.h"
#include "timeutil.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static long my_pid(void) {
#ifdef _WIN32
    return (long)GetCurrentProcessId();
#else
    return (long)getpid();
#endif
}

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    SetEnvironmentVariableA(name, value);
#else
    setenv(name, value, 1);
#endif
}

static void write_file(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    if (n > 0) fwrite(data, 1, n, f);
    fclose(f);
}

/* ---------------- rules ---------------- */

static void test_rules_load_all(void) {
    cdc_rule_t *rules = NULL;
    size_t n = 0;
    cdc_err_t e;
    int rc = cdc_rules_load_all(&rules, &n, &e);
    CHECK(rc == CDC_OK, "rules load_all rc=%d (%s)", rc, e.message);
    if (rc != CDC_OK) return;
    CHECK(n == 1074, "rules count 1074, got %zu", n);
    size_t standard = 0, deep = 0;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(rules[i].tier, "standard") == 0) standard++;
        else if (strcmp(rules[i].tier, "deep") == 0) deep++;
    }
    CHECK(standard == 1071, "rules standard 1071, got %zu", standard);
    CHECK(deep == 3, "rules deep 3, got %zu", deep);
    /* 977 条用 desc 而非 describe：Rust serde 忽略 desc，
     * 故它们的 describe 为空——C 版只读 describe，应与 Rust 一致。 */
    size_t empty_describe = 0;
    for (size_t i = 0; i < n; i++) {
        if (!rules[i].describe || rules[i].describe[0] == '\0') empty_describe++;
    }
    CHECK(empty_describe == 977, "rules empty describe 977, got %zu", empty_describe);
    cdc_rules_free(rules, n);
}

static void test_rules_validate(void) {
    cdc_err_t e;
    /* kind 必须为 "files"（空串 kind 在 Rust 里是合法的 files 规则，
     * 但 "command" 必须拒绝）。 */
    CHECK(cdc_rule_validate("k1", "command", "standard", 0, 0, NULL, 0, NULL, 0, &e) != CDC_OK,
          "validate rejects kind=command");
    CHECK(cdc_rule_validate("k1", "files", "standard", 1, 1, NULL, 0, NULL, 0, &e) != CDC_OK,
          "validate rejects command field");
    CHECK(cdc_rule_validate("k1", "files", "bogus", 0, 0, NULL, 0, NULL, 0, &e) != CDC_OK,
          "validate rejects bad tier");
    CHECK(cdc_rule_validate("", "files", "standard", 0, 0, NULL, 0, NULL, 0, &e) != CDC_OK,
          "validate rejects empty key");
    CHECK(cdc_rule_validate("k 1", "files", "standard", 0, 0, NULL, 0, NULL, 0, &e) != CDC_OK,
          "validate rejects key with space");
    const char *good_paths[1] = { "%LOCALAPPDATA%\\Temp\\a.tmp" };
    CHECK(cdc_rule_validate("k1", "files", "standard", 0, 0, good_paths, 1, NULL, 0, &e) == CDC_OK,
          "validate accepts Temp path (%s)", e.message);
    const char *bad_paths[1] = { "C:\\Windows\\System32\\a.dll" };
    CHECK(cdc_rule_validate("k1", "files", "standard", 0, 0, bad_paths, 1, NULL, 0, &e) != CDC_OK,
          "validate rejects system path");
    CHECK(cdc_rule_validate("k1", "", "standard", 0, 0, NULL, 0, NULL, 0, &e) == CDC_OK,
          "validate accepts empty kind (files default)");
    /* key 规则：Rust key_ok 只允许 ASCII 字母数字 + _-（不含 '.'）。 */
    CHECK(cdc_rule_key_ok("abc-123_X"), "key_ok abc-123_X");
    CHECK(!cdc_rule_key_ok("a.b"), "key_ok rejects dot");
    CHECK(!cdc_rule_key_ok("a b"), "key_ok rejects space");
    CHECK(!cdc_rule_key_ok(""), "key_ok rejects empty");
    /* path_allowed：%VAR% 展开、glob 段合法性。 */
    char *perr = NULL;
    CHECK(cdc_rule_path_allowed("%TEMP%\\a.tmp", &perr) == CDC_OK, "path_allowed TEMP");
    free(perr);
    CHECK(cdc_rule_path_allowed("C:\\Windows\\a.txt", &perr) != CDC_OK, "path_allowed rejects system");
    free(perr);
}

static void test_rules_update_no_url(void) {
    /* rules_update.json 的 url 为 ""：update 应直接报错，不发任何请求。 */
    cdc_rules_update_result_t out;
    cdc_err_t e;
    int rc = cdc_rules_update(&out, &e);
    CHECK(rc != CDC_OK, "update with empty url fails (rc=%d)", rc);
    CHECK(strstr(e.message, "没有配置") != NULL || strstr(e.message, "远程规则") != NULL ||
              strstr(e.message, "WinHTTP") != NULL,
          "update error mentions missing config: %s", e.message);
}

static void test_rules_config(void) {
    char *url = NULL, *hash = NULL;
    cdc_err_t e;
    int rc = cdc_rules_config(&url, &hash, &e);
    CHECK(rc == CDC_OK, "rules_config rc=%d (%s)", rc, e.message);
    CHECK(url && url[0] == '\0', "config url empty (got %s)", url ? url : "(null)");
    free(url);
    free(hash);
}

/* ---------------- timeutil / logs ---------------- */

static void test_timeutil(void) {
    char *m = cdc_month_label(0);
    CHECK(m && strcmp(m, "1970-01") == 0, "month_label(0) == 1970-01 (got %s)", m ? m : "(null)");
    free(m);
    char *s = cdc_stamp();
    CHECK(s && strlen(s) == 15 && s[8] == '_', "stamp shape YYYYMMDD_HHMMSS (got %s)", s ? s : "(null)");
    free(s);
}

static void test_logs(void) {
    const char *lines[3] = { "开始清理", "清理：测试", "完成：测试" };
    char *path = NULL;
    cdc_err_t e;
    int rc = cdc_write_log(lines, 3, &path, &e);
    CHECK(rc == CDC_OK, "write_log rc=%d (%s)", rc, e.message);
    CHECK(path != NULL, "write_log returns path");
    free(path);
    cdc_log_entry_t *entries = NULL;
    size_t n = 0;
    CHECK(cdc_logs_list(&entries, &n, &e) == CDC_OK && n >= 1, "logs_list >= 1");
    int found_test = 0;
    if (entries) {
        for (size_t i = 0; i < n; i++) {
            char *text = NULL;
            if (cdc_log_read(entries[i].name, &text, NULL) == CDC_OK && text) {
                if (strstr(text, "清理：测试")) found_test = 1;
                free(text);
            }
        }
    }
    CHECK(found_test, "logs_list/read finds the test log");
    cdc_logs_list_free(entries, n);
    /* 非法名静默丢弃。 */
    CHECK(cdc_append_line("../evil", "x") == CDC_OK, "append_line illegal name dropped silently");
    CHECK(cdc_log_read("../evil", NULL, &e) != CDC_OK, "log_read rejects traversal");
}

/* ---------------- im classify（对照 Rust im.rs tests） ---------------- */

static void check_classify(const char *path, const char *root,
                           const char *want_kind, const char *want_sub) {
    char *kind = NULL, *sub = NULL;
    int ok = cdc_im_classify(path, root, &kind, &sub);
    if (!want_kind) {
        CHECK(!ok, "classify %s -> None (got %s/%s)", path, kind ? kind : "-", sub ? sub : "-");
    } else {
        CHECK(ok && kind && strcmp(kind, want_kind) == 0,
              "classify %s -> %s (got %s)", path, want_kind, kind ? kind : "(none)");
        if (want_sub)
            CHECK(sub && strcmp(sub, want_sub) == 0,
                  "classify %s sub %s (got %s)", path, want_sub, sub ? sub : "(none)");
    }
    free(kind);
    free(sub);
}

static void test_im_classify(void) {
    const char *root = "C:\\Users\\u\\Documents\\WeChat Files";
    /* Rust: classifies_by_directory_before_extension */
    check_classify("C:\\Users\\u\\Documents\\WeChat Files\\wxid\\FileStorage\\File\\2024-01\\a.mp4",
                   root, "file", "video");
    check_classify("C:\\Users\\u\\Documents\\WeChat Files\\wxid\\FileStorage\\Cache\\a.jpg",
                   root, "cache", "");
    /* Rust: video_cover_images_are_not_counted_as_video */
    check_classify("C:\\Users\\u\\Documents\\WeChat Files\\wxid\\FileStorage\\Video\\2025-09\\0483e1f2.jpg",
                   root, "image", "");
    check_classify("C:\\Users\\u\\Documents\\WeChat Files\\wxid\\FileStorage\\Video\\2025-09\\0483e1f2.mp4",
                   root, "video", "");
    /* Rust: skips_databases_and_config */
    check_classify("C:\\Users\\u\\Documents\\WeChat Files\\wxid\\Msg\\a.db", root, NULL, NULL);
    check_classify("C:\\Users\\u\\Documents\\WeChat Files\\wxid\\db_storage\\a.dat", root, NULL, NULL);
    /* Rust: classifies_plain_media_without_directory_hint */
    check_classify("C:\\Users\\u\\Documents\\WeChat Files\\x\\a.pdf", root, "file", "doc");
    check_classify("C:\\Users\\u\\Documents\\WeChat Files\\x\\a.unknownext", root, NULL, NULL);
    /* Rust: classifies_miniprogram_assets_as_cache / client icons */
    const char *xroot = "C:\\Users\\u\\AppData\\Roaming\\Tencent\\xwechat";
    check_classify("C:\\Users\\u\\AppData\\Roaming\\Tencent\\xwechat\\radium\\users\\h\\xworker\\liteapp\\local\\packages\\p\\main\\174\\dist\\pkg\\res\\img\\lucky-bag-card-0.png",
                   xroot, "cache", "");
    check_classify("C:\\Users\\u\\AppData\\Roaming\\Tencent\\xwechat\\xplugin\\Plugins\\WinWeixinShare\\2004\\extracted\\Assets\\WeChat150.png",
                   xroot, "cache", "");
    /* 程序根目录：非日志扩展名一律 cache；日志扩展名 log。 */
    check_classify("C:\\Users\\u\\AppData\\Roaming\\Tencent\\xwechat\\radium\\a.log",
                   xroot, "log", "");
    /* 空扩展名 / .dat / .tmp 在非程序根目录下不分类。 */
    check_classify("C:\\Users\\u\\Documents\\WeChat Files\\x\\noext", root, NULL, NULL);
    check_classify("C:\\Users\\u\\Documents\\WeChat Files\\x\\a.dat", root, NULL, NULL);
    check_classify("C:\\Users\\u\\Documents\\WeChat Files\\x\\a.tmp", root, NULL, NULL);
}

static void test_im_thumbnail_preview(void) {
    char base[256];
    snprintf(base, sizeof(base), "/tmp/cdc-test-slice3-%ld", (long)my_pid());
#ifndef _WIN32
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", base);
    { int rc = system(cmd); (void)rc; }
    mkdir(base, 0755);
#endif
    /* 最小 JPEG：FF D8 FF + 任意内容。 */
    char jpg[300];
    snprintf(jpg, sizeof(jpg), "%s/a.jpg", base);
    uint8_t jpeg_head[16] = { 0xFF, 0xD8, 0xFF, 0xE0, 0, 0, 0, 0,
                              0, 0, 0, 0, 0, 0, 0, 0 };
    write_file(jpg, jpeg_head, sizeof(jpeg_head));
    const char *mime = NULL;
    uint8_t *data = NULL;
    size_t len = 0;
    cdc_err_t e;
    int rc = cdc_im_thumbnail(jpg, &mime, &data, &len, &e);
    CHECK(rc == 1 && mime && strcmp(mime, "image/jpeg") == 0 && len == 16,
          "thumbnail plain jpeg");
    free(data);
    /* XOR 0x05 加密的 .dat（微信聊天图片）。 */
    char dat[300];
    snprintf(dat, sizeof(dat), "%s/b.dat", base);
    uint8_t enc[16];
    for (int i = 0; i < 16; i++) enc[i] = (uint8_t)(jpeg_head[i] ^ 0x05);
    write_file(dat, enc, sizeof(enc));
    mime = NULL;
    data = NULL;
    len = 0;
    rc = cdc_im_thumbnail(dat, &mime, &data, &len, &e);
    CHECK(rc == 1 && mime && strcmp(mime, "image/jpeg") == 0,
          "thumbnail xor .dat decrypted");
    if (data) {
        CHECK(memcmp(data, jpeg_head, 16) == 0, "thumbnail decrypted bytes match");
        free(data);
    }
    /* 非图片。 */
    char txt[300];
    snprintf(txt, sizeof(txt), "%s/c.txt", base);
    write_file(txt, "hello world, plain text", 23);
    rc = cdc_im_thumbnail(txt, &mime, &data, &len, &e);
    CHECK(rc == 0, "thumbnail non-image -> 0");
    /* 预览：文本与二进制。 */
    cdc_im_preview_t pv;
    CHECK(cdc_im_preview(txt, &pv, &e) == CDC_OK && !pv.binary &&
              strstr(pv.text, "hello world") != NULL,
          "preview text file");
    cdc_im_preview_free(&pv);
    char bin[300];
    snprintf(bin, sizeof(bin), "%s/d.bin", base);
    uint8_t bin_data[64];
    memset(bin_data, 0x01, sizeof(bin_data));
    write_file(bin, bin_data, sizeof(bin_data));
    CHECK(cdc_im_preview(bin, &pv, &e) == CDC_OK && pv.binary &&
              strstr(pv.text, "01 01 01") != NULL,
          "preview binary -> hex");
    cdc_im_preview_free(&pv);
    /* 已扫描集合。 */
    cdc_im_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.path = strdup(txt);
    cdc_im_remember_scanned(&rec, 1);
    CHECK(cdc_im_is_scanned(txt), "is_scanned remembers");
    CHECK(!cdc_im_is_scanned("/tmp/never-scanned-xyz"), "is_scanned unknown false");
    free(rec.path);
#ifndef _WIN32
    snprintf(cmd, sizeof(cmd), "rm -rf %s", base);
    { int rc = system(cmd); (void)rc; }
#endif
}

/* ---------------- system 纯函数（对照 Rust system.rs tests） ---------------- */

static void test_system_pure(void) {
    CHECK(cdc_safe_for_quoted_cmd_path("D:\\Data folder\\WeChat"), "quoted ok plain");
    CHECK(cdc_safe_for_quoted_cmd_path("D:\\Data & files\\WeChat"), "quoted ok &");
    CHECK(!cdc_safe_for_quoted_cmd_path("D:\\Data%TEMP%\\WeChat"), "quoted rejects percent");
    CHECK(!cdc_safe_for_quoted_cmd_path("D:\\Data\\\"quoted\""), "quoted rejects quote");
    CHECK(!cdc_safe_for_quoted_cmd_path("D:\\Data\\line\nnext"), "quoted rejects newline");
    CHECK(!cdc_safe_for_quoted_cmd_path(""), "quoted rejects empty");
    const char *roots[1] = { "C:\\Users\\u\\Documents\\WeChat Files" };
    CHECK(cdc_valid_recovery_paths("C:\\Users\\u\\Documents\\WeChat Files",
                                   "C:\\Users\\u\\Documents\\WeChat Files_backup_20261116_200000",
                                   roots, 1),
          "recovery valid exact");
    CHECK(!cdc_valid_recovery_paths("C:\\Users\\u\\Documents\\WeChat Files\\nested",
                                    "C:\\Users\\u\\Documents\\WeChat Files\\nested_backup_20261116_200000",
                                    roots, 1),
          "recovery rejects nested source");
    CHECK(!cdc_valid_recovery_paths("C:\\Users\\u\\Documents\\WeChat Files",
                                    "D:\\elsewhere\\WeChat Files_backup_20261116_200000",
                                    roots, 1),
          "recovery rejects different parent");
    CHECK(!cdc_valid_recovery_paths("C:\\Users\\u\\Documents\\WeChat Files",
                                    "C:\\Users\\u\\Documents\\WeChat Files_backup_20261116-200000",
                                    roots, 1),
          "recovery rejects bad stamp");
    CHECK(!cdc_valid_recovery_paths("C:\\Windows\\System32",
                                    "C:\\Windows\\System32_backup_20261116_200000",
                                    roots, 1),
          "recovery rejects system source");
    /* hiber / 固定磁盘 / 系统清理项（Linux 下为降级行为）。 */
    cdc_hiber_info_t hi;
    CHECK(cdc_query_hiber(&hi, NULL) == CDC_OK, "query_hiber ok");
    CHECK(hi.path && strstr(hi.path, "hiberfil.sys") != NULL, "hiber path ends hiberfil.sys");
    cdc_hiber_info_free(&hi);
    char **drives = NULL;
    size_t ndrives = 0;
    CHECK(cdc_fixed_drives(&drives, &ndrives, NULL) == CDC_OK && ndrives >= 1,
          "fixed_drives >= 1");
    if (drives && ndrives > 0)
        CHECK(strcmp(drives[0], "C:\\") == 0, "linux fixed_drives fallback C:\\");
    cdc_strvec_free(drives, ndrives);
    cdc_system_item_t *items = NULL;
    size_t nitems = 0;
    CHECK(cdc_scan_system_deep(&items, &nitems, NULL) == CDC_OK, "scan_system_deep ok");
    int has_dism = 0, has_dumps = 0;
    for (size_t i = 0; i < nitems; i++) {
        if (strcmp(items[i].key, "sys_dism_winsxs") == 0) has_dism = 1;
        if (strcmp(items[i].key, "sys_crash_dumps") == 0) has_dumps = 1;
    }
    CHECK(has_dism && has_dumps, "scan_system_deep has dism + crash dumps");
    cdc_system_items_free(items, nitems);
    cdc_err_t e;
    CHECK(cdc_set_hiber("bogus", NULL, &e) == CDC_ERR_INVALID_ARG, "set_hiber bogus rejected");
    CHECK(cdc_reveal_in_explorer("/nonexistent-path-xyz", &e) == CDC_ERR_NOT_FOUND,
          "reveal nonexistent -> NOT_FOUND");
    CHECK(cdc_clean_system_item("no_such_key", NULL, NULL, NULL, &e) == CDC_ERR_INVALID_ARG,
          "clean_system_item unknown key rejected");
    /* im 迁移分析：无根目录时 source 为空、exists 为假。 */
    cdc_move_analysis_t ma;
    CHECK(cdc_analyze_im_migration("wechat", &ma, NULL) == CDC_OK, "analyze_im_migration ok");
    CHECK(!ma.exists && ma.source && ma.source[0] == '\0', "analyze no roots -> empty source");
    cdc_move_analysis_free(&ma);
}

/* ---------------- privacy 扫描结构（空环境） ---------------- */

static void test_privacy_scan_empty(void) {
    cdc_privacy_item_t *items = NULL;
    size_t n = 0;
    cdc_err_t e;
    CHECK(cdc_privacy_scan(&items, &n, &e) == CDC_OK, "privacy scan rc=%d (%s)", e.code, e.message);
    CHECK(n == 9, "privacy scan empty env -> 9 items, got %zu", n);
    const char *want_ids[9] = { "risk_file", "risk_money", "risk_shop",
                                "risk_medical", "cookies", "reg_file",
                                "reg_usb", "software", "media" };
    for (size_t i = 0; i < n && i < 9; i++) {
        CHECK(strcmp(items[i].id, want_ids[i]) == 0,
              "privacy item %zu id %s (got %s)", i, want_ids[i], items[i].id);
    }
    cdc_privacy_items_free(items, n);
}

/* ---------------- shred / recycle / process 的 Linux 降级 ---------------- */

static void test_linux_degradation(void) {
    char base[256];
    snprintf(base, sizeof(base), "/tmp/cdc-test-slice3-deg-%ld", (long)my_pid());
#ifndef _WIN32
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", base);
    { int rc = system(cmd); (void)rc; }
    mkdir(base, 0755);
    char f1[300];
    snprintf(f1, sizeof(f1), "%s/a.txt", base);
    write_file(f1, "data", 4);
    /* 粉碎：Linux 下不删文件，只计失败。 */
    cdc_shred_report_t rep;
    const char *paths[1] = { f1 };
    cdc_err_t e;
    CHECK(cdc_shred_paths(paths, 1, 1, &rep, &e) == CDC_OK, "shred rc ok");
    CHECK(rep.files_failed == 1 && rep.files_ok == 0, "shred linux fails gracefully");
    FILE *chk = fopen(f1, "r");
    CHECK(chk != NULL, "shred linux leaves file");
    if (chk) fclose(chk);
    cdc_shred_report_free(&rep);
    /* 回收站：Linux 下全部失败，不做永久删除。 */
    std::vector<std::string> failed = cdc::send_to_recycle(paths);
    CHECK(failed.size() == 1 && failed[0] == f1, "recycle failed list names the file");
    CHECK(failed.size() == 1, "recycle linux all failed");
    chk = fopen(f1, "r");
    CHECK(chk != NULL, "recycle linux leaves file");
    if (chk) fclose(chk);
    /* 进程：空集 + 0 杀。 */
    char **running = NULL;
    size_t nrun = 0;
    const char *t[1] = { "nonexistent-xyz.exe" };
    CHECK(cdc_check_running(t, 1, &running, &nrun, &e) == CDC_OK && nrun == 0,
          "check_running empty on linux");
    cdc_strvec_free(running, nrun);
    size_t killed = 99;
    CHECK(cdc_kill_process_by_name("nonexistent-xyz.exe", &killed, &e) == CDC_OK && killed == 0,
          "kill nonexistent -> 0");
    CHECK(cdc_run_command("cmd.exe", "/c echo hi", 0, NULL, NULL, NULL, &e) == CDC_ERR_IO,
          "run_command linux -> IO error");
    snprintf(cmd, sizeof(cmd), "rm -rf %s", base);
    { int rc = system(cmd); (void)rc; }
#else
    (void)base;
#endif
}

int main(void) {
    /* Windows 形态的环境变量 + Linux 下的临时应用数据目录。 */
    set_env("USERPROFILE", "C:\\Users\\u");
    set_env("LOCALAPPDATA", "C:\\Users\\u\\AppData\\Local");
    set_env("APPDATA", "C:\\Users\\u\\AppData\\Roaming");
    set_env("SystemRoot", "C:\\Windows");
    set_env("WINDIR", "C:\\Windows");
    set_env("SystemDrive", "C:");
    set_env("ProgramData", "C:\\ProgramData");
    set_env("TEMP", "C:\\Users\\u\\AppData\\Local\\Temp");
    set_env("TMP", "C:\\Users\\u\\AppData\\Local\\Temp");
    char tmpapp[256];
    snprintf(tmpapp, sizeof(tmpapp), "/tmp/cdc-test-slice3-app-%ld", (long)my_pid());
#ifndef _WIN32
    /* 让 LOCALAPPDATA 指向真实可写的 Linux 临时目录，供 logs 写日志；
     * 但 rules 的 allowlist 判定依赖 LOCALAPPDATA 的 Windows 形态值，
     * 因此 logs 测试用单独的进程级覆盖：这里先跑 rules/im/system，
     * logs 测试内部再临时改。——简化：logs 用 cdc_logs_dir 的回退
     * （LOCALAPPDATA 为 Windows 路径时 Linux 下建目录失败，write_log
     * 应返回错误而非崩溃；为测成功路径，test_logs 内临时改 env）。 */
    /* logs.c 的 join_and_create 只建一层目录，父目录须先存在。 */
    mkdir(tmpapp, 0755);
#endif

    test_rules_load_all();
    test_rules_validate();
    test_rules_update_no_url();
    test_rules_config();
    test_timeutil();
    /* logs 成功路径需要可写的 LOCALAPPDATA：临时指向 /tmp。 */
    {
        char *saved = getenv("LOCALAPPDATA");
        char *saved_copy = saved ? strdup(saved) : NULL;
        set_env("LOCALAPPDATA", tmpapp);
        test_logs();
        if (saved_copy) {
            set_env("LOCALAPPDATA", saved_copy);
            free(saved_copy);
        }
    }
    test_im_classify();
    test_im_thumbnail_preview();
    test_system_pure();
    test_privacy_scan_empty();
    test_linux_degradation();

    printf("test_slice3: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
