/* test_gui.c — gui.c 纯函数自测（两平台可跑；Windows 段不在此测）。
 *
 * 覆盖：frag 解析（Rust frag.rs 的 3 个测试向量 + U+3000 折叠）、
 * 任务坞（begin/status/finish/cancel/mutating 互斥/同 id 覆盖）、
 * dup 缓存键（确定性 + 对 min_size/fast/roots 敏感）、索引作用域解析、
 * path_under_dir、扫描状态单元、dups-*.json 缓存失效。
 *
 * 编译见 Makefile（test_gui）/ CMakeLists.txt（test_gui）：
 * 链接 gui.c + util.c + safety.c + mft_util.c + logs.c + timeutil.c
 * + cJSON + SQLite + BLAKE3；-D_GNU_SOURCE（mkdtemp/setenv）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "gui.h"
#include "util.h"

static int checks = 0;
static int failures = 0;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

#define CHECK_STR_EQ(got, want) \
    CHECK(strcmp((got), (want)) == 0, "want \"%s\", got \"%s\"", \
          (want), (got))

static cJSON *status_obj(const char *task_id) {
    char *json = cdc_task_status_json();
    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) return NULL;
    cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, task_id);
    if (obj) cJSON_DetachItemFromObjectCaseSensitive(root, task_id);
    cJSON_Delete(root);
    return obj;
}

static cJSON *scan_obj(const char *kind) {
    char *json = cdc_scan_status_json();
    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) return NULL;
    cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, kind);
    if (obj) cJSON_DetachItemFromObjectCaseSensitive(root, kind);
    cJSON_Delete(root);
    return obj;
}

static void test_frag(void) {
    /* Rust frag.rs 测试向量 1：中文。 */
    const char *zh =
        "卷 (C:)\n"
        "    卷大小 = 118.99 GB\n"
        "    可用空间 = 15.23 GB\n"
        "    碎片率 = 12%\n"
        "    碎片空间 = 0.42 GB\n"
        "    碎片文件数 = 1,234\n";
    cdc_frag_parsed_t p = cdc_frag_parse_defrag(zh);
    CHECK(p.has_percent && p.percent == 12.0, "zh percent");
    CHECK(p.has_space_gb && p.space_gb == 0.42, "zh space_gb");
    CHECK(p.has_count && p.count == 1234, "zh count");
    CHECK(p.has_free_gb && p.free_gb == 15.23, "zh free_gb");

    /* Rust frag.rs 测试向量 2：英文。 */
    const char *en =
        "Volume (C:)\n"
        "    Volume size = 118.99 GB\n"
        "    Free space = 30.5 GB\n"
        "    Percent file fragmentation = 7%\n"
        "    Total fragmented space = 0.10 GB\n"
        "    Total fragmented files = 42\n";
    p = cdc_frag_parse_defrag(en);
    CHECK(p.has_percent && p.percent == 7.0, "en percent");
    CHECK(p.has_space_gb && p.space_gb == 0.10, "en space_gb");
    CHECK(p.has_count && p.count == 42, "en count");
    CHECK(p.has_free_gb && p.free_gb == 30.5, "en free_gb");

    /* Rust frag.rs 测试向量 3：无可用数据 → 全部 None。 */
    p = cdc_frag_parse_defrag("没有可用的分析数据\n");
    CHECK(!p.has_percent && !p.has_space_gb && !p.has_count && !p.has_free_gb,
          "missing fields all none");

    /* U+3000（全角空格）折叠：标签与 = 之间是 U+3000 也应解析。 */
    const char *full =
        "碎片率\xe3\x80\x80= 9%\n"
        "碎片空间\xe3\x80\x80= 0.01 GB\n";
    p = cdc_frag_parse_defrag(full);
    CHECK(p.has_percent && p.percent == 9.0, "U+3000 percent");
    CHECK(p.has_space_gb && p.space_gb == 0.01, "U+3000 space_gb");

    /* 空输入 → 全部 None，不崩。 */
    p = cdc_frag_parse_defrag("");
    CHECK(!p.has_percent && !p.has_space_gb && !p.has_count && !p.has_free_gb,
          "empty input all none");
}

static void test_task_dock(void) {
    char *err = NULL;
    CHECK(cdc_task_begin("t1", "测试任务", 1, 0, &err) == 0, "begin t1");
    free(err);
    err = NULL;

    cJSON *o = status_obj("t1");
    CHECK(o != NULL, "status has t1");
    if (o) {
        cJSON *title = cJSON_GetObjectItemCaseSensitive(o, "title");
        CHECK(cJSON_IsString(title), "t1 title is string");
        if (cJSON_IsString(title)) CHECK_STR_EQ(title->valuestring, "测试任务");
        CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "active")),
              "t1 active");
        CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "cancellable")),
              "t1 cancellable");
        CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o, "mutating")),
              "t1 not mutating");
        CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o, "cancelling")),
              "t1 not cancelling");
        cJSON_Delete(o);
    }

    cdc_task_set_stage("t1", "阶段A");
    cdc_task_set_progress("t1", 3, 10);
    o = status_obj("t1");
    CHECK(o != NULL, "status t1 again");
    if (o) {
        cJSON *stage = cJSON_GetObjectItemCaseSensitive(o, "stage");
        CHECK(cJSON_IsString(stage) &&
                  strcmp(stage->valuestring, "阶段A") == 0,
              "t1 stage A");
        CHECK(cJSON_GetObjectItemCaseSensitive(o, "done")->valuedouble == 3,
              "t1 done 3");
        CHECK(cJSON_GetObjectItemCaseSensitive(o, "total")->valuedouble == 10,
              "t1 total 10");
        cJSON_Delete(o);
    }

    CHECK(cdc_task_cancel("t1") == 1, "cancel t1 returns 1");
    o = status_obj("t1");
    if (o) {
        CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "cancelling")),
              "t1 cancelling after cancel");
        cJSON_Delete(o);
    }
    CHECK(cdc_task_cancelled("t1") == 1, "t1 cancelled flag");

    cdc_task_finish("t1", "ok", "完成");
    o = status_obj("t1");
    if (o) {
        CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o, "active")),
              "t1 inactive after finish");
        cJSON *outcome = cJSON_GetObjectItemCaseSensitive(o, "outcome");
        CHECK(cJSON_IsString(outcome) &&
                  strcmp(outcome->valuestring, "ok") == 0,
              "t1 outcome ok");
        cJSON *summary = cJSON_GetObjectItemCaseSensitive(o, "summary");
        CHECK(cJSON_IsString(summary) &&
                  strcmp(summary->valuestring, "完成") == 0,
              "t1 summary");
        CHECK(cJSON_GetObjectItemCaseSensitive(o, "finished_ms")->valuedouble > 0,
              "t1 finished_ms set");
        cJSON_Delete(o);
    }

    /* mutating 互斥：已有活跃 mutating 任务时第二个 begin 失败。 */
    CHECK(cdc_task_begin("m1", "清理", 0, 1, &err) == 0, "begin m1 mutating");
    free(err);
    err = NULL;
    CHECK(cdc_task_begin("m2", "清理2", 0, 1, &err) == -1,
          "begin m2 conflicts");
    CHECK(err && strcmp(err, "已有清理任务正在进行，请等它结束后再试") == 0,
          "conflict message");
    free(err);
    err = NULL;
    cdc_task_finish("m1", "ok", "m1 done");
    CHECK(cdc_task_begin("m2", "清理2", 0, 1, &err) == 0,
          "begin m2 after m1 finished");
    free(err);
    err = NULL;
    cdc_task_finish("m2", "ok", "m2 done");

    /* 同 id 重复 begin 覆盖旧条目（Rust map.insert 一致）。 */
    CHECK(cdc_task_begin("t1", "新标题", 0, 0, &err) == 0, "begin t1 again");
    free(err);
    err = NULL;
    o = status_obj("t1");
    if (o) {
        cJSON *title = cJSON_GetObjectItemCaseSensitive(o, "title");
        CHECK(cJSON_IsString(title) &&
                  strcmp(title->valuestring, "新标题") == 0,
              "t1 replaced title");
        CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "active")),
              "t1 active again");
        cJSON_Delete(o);
    }
    cdc_task_finish("t1", "ok", "done");

    /* cancel_all：NULL 取消全部可取消的活跃任务。 */
    CHECK(cdc_task_begin("c1", "可取消1", 1, 0, &err) == 0, "begin c1");
    free(err);
    err = NULL;
    CHECK(cdc_task_begin("c2", "可取消2", 1, 0, &err) == 0, "begin c2");
    free(err);
    err = NULL;
    CHECK(cdc_task_begin("c3", "不可取消", 0, 0, &err) == 0, "begin c3");
    free(err);
    err = NULL;
    CHECK(cdc_task_cancel(NULL) == 1, "cancel_all returns 1");
    CHECK(cdc_task_cancelled("c1") == 1, "c1 cancelled by cancel_all");
    CHECK(cdc_task_cancelled("c2") == 1, "c2 cancelled by cancel_all");
    CHECK(cdc_task_cancelled("c3") == 0, "c3 not cancelled by cancel_all");
    cdc_task_finish("c1", "cancelled", "已取消");
    cdc_task_finish("c2", "cancelled", "已取消");
    cdc_task_finish("c3", "ok", "done");
}

static void test_dup_cache_key(void) {
    const char *roots1[] = {"C:\\"};
    const char *roots2[] = {"C:\\", "D:\\"};
    char *k1 = cdc_gui_dup_cache_key(roots1, 1, 1048576, 0);
    char *k2 = cdc_gui_dup_cache_key(roots1, 1, 1048576, 0);
    CHECK(k1 && k2 && strcmp(k1, k2) == 0, "cache key deterministic");
    /* 完整 32 字节 blake3 摘要的 hex（64 字符），与 Rust 的
     * dups-{blake3}.json 文件名一致。 */
    CHECK(k1 && strlen(k1) == 64, "cache key is 64 hex chars");
    if (k1) {
        int hex = 1;
        for (size_t i = 0; i < strlen(k1); i++) {
            char c = k1[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) hex = 0;
        }
        CHECK(hex, "cache key lowercase hex");
    }
    char *k3 = cdc_gui_dup_cache_key(roots1, 1, 1048576, 1);
    CHECK(k1 && k3 && strcmp(k1, k3) != 0, "cache key differs by fast");
    char *k4 = cdc_gui_dup_cache_key(roots1, 1, 2097152, 0);
    CHECK(k1 && k4 && strcmp(k1, k4) != 0, "cache key differs by min_size");
    char *k5 = cdc_gui_dup_cache_key(roots2, 2, 1048576, 0);
    CHECK(k1 && k5 && strcmp(k1, k5) != 0, "cache key differs by roots");
    free(k1);
    free(k2);
    free(k3);
    free(k4);
    free(k5);
}

static void test_index_scope(void) {
    char drive[8] = "";
    char *prefix = NULL;
    CHECK(cdc_gui_parse_index_scope("C:\\", drive, &prefix) == 1,
          "scope C:\\ ok");
    CHECK_STR_EQ(drive, "C:\\");
    CHECK(prefix == NULL, "scope C:\\ prefix NULL");
    free(prefix);
    prefix = NULL;

    CHECK(cdc_gui_parse_index_scope("C:\\Foo\\", drive, &prefix) == 1,
          "scope C:\\Foo\\ ok");
    CHECK_STR_EQ(drive, "C:\\");
    CHECK(prefix && strcmp(prefix, "C:\\Foo") == 0, "scope prefix trimmed");
    free(prefix);
    prefix = NULL;

    CHECK(cdc_gui_parse_index_scope("c:\\foo", drive, &prefix) == 1,
          "scope lowercase drive ok");
    CHECK_STR_EQ(drive, "C:\\");
    CHECK(prefix && strcmp(prefix, "c:\\foo") == 0, "scope prefix keeps case");
    free(prefix);
    prefix = NULL;

    CHECK(cdc_gui_parse_index_scope("D:\\Docs", drive, &prefix) == 1,
          "scope D:\\Docs ok");
    CHECK_STR_EQ(drive, "D:\\");
    free(prefix);
    prefix = NULL;

    /* "C:" 也接受（与 Rust parse_index_scope_root 一致：len==2 即通过，
     * drive "C:\\"，prefix 为 None）。 */
    CHECK(cdc_gui_parse_index_scope("C:", drive, &prefix) == 1,
          "scope C: accepted like Rust");
    CHECK_STR_EQ(drive, "C:\\");
    CHECK(prefix == NULL, "scope C: prefix NULL");
    free(prefix);
    prefix = NULL;
    CHECK(cdc_gui_parse_index_scope("", drive, &prefix) == 0,
          "scope empty rejected");
    CHECK(cdc_gui_parse_index_scope("C", drive, &prefix) == 0,
          "scope C rejected");

    CHECK(cdc_gui_path_under_dir("C:\\Foo\\bar.txt", "C:\\Foo") == 1,
          "path under dir");
    CHECK(cdc_gui_path_under_dir("C:\\Foo\\sub\\deep.txt", "C:\\Foo") == 1,
          "path under dir deep");
    CHECK(cdc_gui_path_under_dir("C:\\Foo", "C:\\Foo") == 0,
          "dir itself not under");
    CHECK(cdc_gui_path_under_dir("C:\\FooBar\\x", "C:\\Foo") == 0,
          "sibling prefix not under");
    CHECK(cdc_gui_path_under_dir("c:\\foo\\x", "C:\\Foo") == 1,
          "path under dir case-insensitive");
    CHECK(cdc_gui_path_under_dir("D:\\Foo\\x", "C:\\Foo") == 0,
          "other drive not under");
}

static void test_scan_cells(void) {
    cdc_scan_begin("dup", "枚举");
    cdc_scan_set_found("dup", 5);
    cdc_scan_add_done("dup", 3);
    cJSON *o = scan_obj("dup");
    CHECK(o != NULL, "scan status has dup");
    if (o) {
        CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "active")),
              "dup active");
        cJSON *stage = cJSON_GetObjectItemCaseSensitive(o, "stage");
        CHECK(cJSON_IsString(stage) &&
                  strcmp(stage->valuestring, "枚举") == 0,
              "dup stage");
        CHECK(cJSON_GetObjectItemCaseSensitive(o, "found")->valuedouble == 5,
              "dup found 5");
        CHECK(cJSON_GetObjectItemCaseSensitive(o, "done")->valuedouble == 3,
              "dup done 3");
        CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o, "cancelled")),
              "dup not cancelled");
        cJSON_Delete(o);
    }
    CHECK(cdc_scan_cancelled("dup") == 0, "dup scan not cancelled");
    cdc_scan_end("dup", "完成");
    o = scan_obj("dup");
    if (o) {
        CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o, "active")),
              "dup inactive after end");
        cJSON *stage = cJSON_GetObjectItemCaseSensitive(o, "stage");
        CHECK(cJSON_IsString(stage) &&
                  strcmp(stage->valuestring, "完成") == 0,
              "dup end stage");
        cJSON_Delete(o);
    }
    cdc_scan_begin("big", "扫描");
    CHECK(cdc_scan_cancelled("big") == 0, "big scan not cancelled");
    cdc_scan_end("big", "完成");
}

static void test_setenv_localappdata(const char *value) {
#ifdef _WIN32
    /* MinGW 的 CRT 没有 setenv/unsetenv，用 _putenv；value 为 NULL 时清空。 */
    char buf[4096];
    snprintf(buf, sizeof(buf), "LOCALAPPDATA=%s", value ? value : "");
    _putenv(buf);
#else
    if (value) setenv("LOCALAPPDATA", value, 1);
    else unsetenv("LOCALAPPDATA");
#endif
}

static void test_invalidate_dup_cache(void) {
    /* 把缓存目录指到临时目录：cdc_cache_dir 在 Linux 下认 LOCALAPPDATA。 */
    char tmpl[] = "/tmp/cdc-gui-test-XXXXXX";
    char *tmp = mkdtemp(tmpl);
    CHECK(tmp != NULL, "mkdtemp");
    if (!tmp) return;
    test_setenv_localappdata(tmp);

    char *dir = cdc_cache_dir();
    CHECK(dir != NULL, "cache dir created");
    if (!dir) return;
    char dups_path[4096], keep_path[4096];
    snprintf(dups_path, sizeof(dups_path), "%s/dups-abc123.json", dir);
    snprintf(keep_path, sizeof(keep_path), "%s/keep.txt", dir);
    FILE *f = fopen(dups_path, "w");
    CHECK(f != NULL, "write dups file");
    if (f) {
        fputs("[]", f);
        fclose(f);
    }
    f = fopen(keep_path, "w");
    CHECK(f != NULL, "write keep file");
    if (f) {
        fputs("keep", f);
        fclose(f);
    }
    cdc_gui_invalidate_dup_cache();
    CHECK(access(dups_path, F_OK) != 0, "dups file deleted");
    CHECK(access(keep_path, F_OK) == 0, "other file kept");
    free(dir);

    /* 清理临时目录。 */
    remove(dups_path);
    remove(keep_path);
    char sub[4096], top[4096];
    snprintf(sub, sizeof(sub), "%s/C盘清理工具/mftcache", tmp);
    snprintf(top, sizeof(top), "%s/C盘清理工具", tmp);
    rmdir(sub);
    rmdir(top);
    rmdir(tmp);
    test_setenv_localappdata(NULL);
}

int main(void) {
    test_frag();
    test_task_dock();
    test_dup_cache_key();
    test_index_scope();
    test_scan_cells();
    test_invalidate_dup_cache();
    printf("test_gui: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
