/* test_safety.c — src/safety.{c,h} 的自测。
 *
 *   gcc -O2 -std=c11 -Wall -Wextra -Werror -Isrc -pthread \
 *       -o build/test_safety tests/test_safety.c src/safety.c src/util.c \
 *       src/logs.c src/timeutil.c
 *   ./build/test_safety     # 退出码 0=全过
 *   x86_64-w64-mingw32-gcc ... -o build/test_safety.exe ... # Windows 交叉只编不过
 *
 * 断言与 Rust 版 desktop/src/safety.rs 的 #[cfg(test)] 用例逐一对照
 *（见各测试函数头的注释）；纯路径判定在 Linux 下也能跑——测试入口
 * 先把 USERPROFILE / SystemRoot 等环境变量设成 Windows 形态的值，
 * 使 is_user_library / is_system_path 的行为与 Windows 一致。
 * 句柄删除 / junction / 改名三组测试是 Windows 专属（Linux 下句柄
 * API 是返回错误的桩），在 Linux 下只打印 SKIP，不计失败。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "safety.h"
#include "util.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

static int failures = 0;
static int checks = 0;
static int skipped = 0;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define SKIP(msg) do { skipped++; printf("SKIP: %s\n", msg); } while (0)

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    SetEnvironmentVariableA(name, value);
#else
    setenv(name, value, 1);
#endif
}

/* ---------------- 纯路径判定（对照 Rust safety.rs tests） ---------------- */

static void test_normalize(void) {
    /* Rust: normalize_keeps_windows_shape */
    char *n = cdc_normalize("C:/Windows/Temp/");
    CHECK(n && strcmp(n, "C:\\Windows\\Temp") == 0, "normalize C:/Windows/Temp/ -> %s", n ? n : "(null)");
    free(n);
    n = cdc_normalize("c:\\Windows\\\\Temp");
    CHECK(n && strcmp(n, "C:\\Windows\\Temp") == 0, "normalize c:\\Windows\\\\Temp -> %s", n ? n : "(null)");
    free(n);
    n = cdc_normalize("C:\\");
    CHECK(n && strcmp(n, "C:\\") == 0, "normalize C:\\ -> %s", n ? n : "(null)");
    free(n);
}

static void test_protected_paths(void) {
    /* Rust: protected_paths_are_forbidden */
    CHECK(cdc_is_system_path("C:\\Windows\\System32\\kernel32.dll"), "system path kernel32.dll");
    CHECK(cdc_execution_forbidden("C:\\Windows\\System32\\kernel32.dll", 1), "execution_forbidden system32 untrusted");
    CHECK(!cdc_execution_forbidden("C:\\Windows\\Temp\\a.tmp", 0), "execution_forbidden Windows\\Temp trusted=false allowed");
    CHECK(cdc_execution_forbidden("C:\\pagefile.sys", 0), "execution_forbidden pagefile.sys");
    CHECK(cdc_execution_forbidden("C:\\Program Files\\App\\a.dll", 1), "execution_forbidden Program Files untrusted");
    CHECK(cdc_execution_forbidden("C:\\$Recycle.Bin\\x", 1), "execution_forbidden $Recycle.Bin untrusted");
    CHECK(cdc_execution_forbidden("C:\\Users\\Administrator\\Documents\\note.txt", 0), "execution_forbidden Documents untrusted=false");
    /* 内置规则也一律 untrusted：Temp 下的文件在 allowlist 内才放行。 */
    CHECK(!cdc_execution_forbidden("C:\\Users\\Administrator\\AppData\\Local\\Temp\\a.tmp", 1),
          "execution_forbidden Local Temp untrusted allowed");
}

static void test_password_store(void) {
    /* Rust: password_store_is_forbidden + 全表 */
    CHECK(cdc_is_password_store("C:\\x\\Login Data"), "password Login Data");
    CHECK(cdc_is_password_store("C:\\x\\logins.json"), "password logins.json");
    CHECK(!cdc_is_password_store("C:\\x\\data.bin"), "not password data.bin");
    const char *names[8] = { "Login Data", "Login Data-journal",
                             "Login Data for Account",
                             "Login Data for Account-journal", "logins.json",
                             "logins-backup.json", "key4.db", "key3.db" };
    for (int i = 0; i < 8; i++) {
        char path[128];
        snprintf(path, sizeof(path), "C:\\x\\%s", names[i]);
        CHECK(cdc_is_password_store(path), "password table %s", names[i]);
    }
    CHECK(cdc_is_password_store("C:\\x\\LOGIN DATA"), "password case-insensitive");
}

static void test_never_clean(void) {
    /* NEVER_CLEAN_FILES = ["usmt.ppkg"]；NEVER_CLEAN_ROOTS =
     * ["\\recovery", "\\system volume information"] */
    CHECK(cdc_is_never_clean("C:\\Recovery\\Customizations\\usmt.ppkg"), "never_clean usmt.ppkg");
    CHECK(cdc_is_never_clean("usmt.ppkg"), "never_clean bare usmt.ppkg");
    CHECK(cdc_is_never_clean("C:\\USMT.PPKG"), "never_clean case-insensitive");
    CHECK(cdc_is_never_clean("C:\\Recovery\\a.txt"), "never_clean Recovery root");
    CHECK(cdc_is_never_clean("C:\\System Volume Information\\x"), "never_clean SVI root");
    CHECK(!cdc_is_never_clean("C:\\Temp\\a.txt"), "not never_clean Temp");
    CHECK(cdc_is_never_clean(""), "never_clean empty");
}

static void test_critical_files(void) {
    const char *names[7] = { "pagefile.sys", "hiberfil.sys", "swapfile.sys",
                             "bootmgr", "ntldr", "ntuser.dat",
                             "ntuser.dat.log" };
    for (int i = 0; i < 7; i++) {
        char path[128];
        snprintf(path, sizeof(path), "C:\\%s", names[i]);
        CHECK(cdc_is_critical_file(path), "critical %s", names[i]);
    }
    CHECK(!cdc_is_critical_file("C:\\pagefile.sys.bak"), "not critical pagefile.sys.bak");
    CHECK(cdc_is_critical_file("C:\\PAGEFILE.SYS"), "critical case-insensitive");
}

static void test_path_syntax_error(void) {
    CHECK(cdc_path_syntax_error(""), "syntax empty");
    CHECK(cdc_path_syntax_error("   "), "syntax blank");
    CHECK(cdc_path_syntax_error("\\\\server\\share"), "syntax UNC \\\\");
    CHECK(cdc_path_syntax_error("//server/share"), "syntax UNC //");
    CHECK(cdc_path_syntax_error("C:\\a\\..\\b"), "syntax ..");
    CHECK(cdc_path_syntax_error("..\\a"), "syntax leading ..");
    CHECK(cdc_path_syntax_error("C:\\a:b"), "syntax colon in body");
    CHECK(!cdc_path_syntax_error("C:\\Windows\\Temp"), "syntax ok Windows Temp");
    CHECK(!cdc_path_syntax_error("C:foo"), "syntax ok C:foo");
    CHECK(!cdc_path_syntax_error("C:\\Users\\u\\a.txt"), "syntax ok user file");
}

static void test_user_library_and_system(void) {
    /* Rust: user_library_and_system_detection */
    CHECK(cdc_is_system_path("C:\\Windows\\Temp"), "system Windows\\Temp");
    CHECK(!cdc_is_system_path("C:\\Users\\Administrator\\AppData\\Local\\Temp"),
          "not system Local Temp");
    const char *roots[1] = { "C:\\Users\\Administrator\\AppData\\Local\\Temp" };
    CHECK(cdc_is_within("C:\\Users\\Administrator\\AppData\\Local\\Temp\\a\\b.txt", roots, 1),
          "is_within Temp subpath");
    CHECK(!cdc_is_within("C:\\Users\\Administrator\\AppData\\Local\\Other\\a.txt", roots, 1),
          "not is_within outside root");
    CHECK(cdc_is_user_library("C:\\Users\\Administrator\\Documents\\a.txt"), "user library Documents");
    CHECK(cdc_is_user_library("C:\\Users\\Administrator\\Downloads\\a.mp4"), "user library Downloads");
    CHECK(cdc_is_user_library("C:\\Users\\Administrator\\桌面\\a.txt"), "user library 桌面");
    CHECK(!cdc_is_user_library("C:\\Windows\\Temp\\a.txt"), "not user library Windows Temp");
}

static void test_explicit_delete(void) {
    /* Rust: explicit_delete_allows_user_folders_but_blocks_system */
    CHECK(!cdc_explicit_delete_forbidden("C:\\Users\\Administrator\\Downloads\\movie.mp4"),
          "explicit allow Downloads movie");
    CHECK(!cdc_explicit_delete_forbidden("C:\\Users\\Administrator\\Documents\\WeChat Files\\a.jpg"),
          "explicit allow Documents WeChat file");
    CHECK(cdc_explicit_delete_forbidden("C:\\Windows\\System32\\kernel32.dll"),
          "explicit block System32");
    CHECK(cdc_explicit_delete_forbidden("C:\\pagefile.sys"), "explicit block pagefile");
    CHECK(cdc_explicit_delete_forbidden("C:\\Users\\Administrator\\AppData\\Local\\Google\\Chrome\\User Data\\Default\\Login Data"),
          "explicit block Login Data");
    CHECK(cdc_explicit_delete_forbidden("C:\\Program Files\\App\\a.dll"), "explicit block Program Files");
    CHECK(cdc_explicit_delete_forbidden("C:\\"), "explicit block drive root");
}

static void test_protected_path(void) {
    CHECK(cdc_is_protected_path("C:\\Windows\\System32\\a.dll"), "protected System32");
    CHECK(cdc_is_protected_path("C:\\Program Files\\App\\a.dll"), "protected Program Files");
    CHECK(cdc_is_protected_path("C:\\ProgramData\\App\\a.txt"), "protected ProgramData");
    /* 注意：Rust 原版里 Documents 不属于 is_protected_path（它只认
     * 系统目录 + Program Files/ProgramData/Perflogs + 环境根 + profile
     * 本身）；用户库的保护在 is_user_library / execution_forbidden 里
     *（对照 Rust safety.rs tests：execution_forbidden 断言
     * <USERPROFILE>\Documents\note.txt 为真）。 */
    CHECK(!cdc_is_protected_path("C:\\Users\\Administrator\\Documents\\a.txt"),
          "not protected user Documents (is_protected_path)");
    CHECK(cdc_is_user_library("C:\\Users\\Administrator\\Documents\\a.txt"),
          "user library Documents");
    CHECK(cdc_execution_forbidden("C:\\Users\\Administrator\\Documents\\a.txt", 0),
          "execution forbidden user Documents");
    CHECK(!cdc_is_protected_path("C:\\Users\\Administrator\\AppData\\Local\\Temp\\a.tmp"),
          "not protected Local Temp");
}

/* ---------------- Linux 可跑的 reparse 测试 ---------------- */

#ifndef _WIN32
static int collect_cb(void *ctx, const char *path, int is_dir, int is_symlink,
                      uint64_t size, uint64_t mtime, uint32_t attrs) {
    (void)size;
    (void)mtime;
    (void)attrs;
    (void)is_dir;
    cdc_vec_t *v = (cdc_vec_t *)ctx;
    if (is_symlink) {
        /* 遍历永不跟随链接：链接本身出现即记一笔，供断言用。 */
        char *dup = strdup(path);
        if (dup && cdc_vec_push(v, &dup) != CDC_OK) free(dup);
    }
    return 0;
}

static void test_reparse_linux(void) {
    char base[256];
    snprintf(base, sizeof(base), "/tmp/cdc-test-safety-%ld", (long)getpid());
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", base);
    { int rc = system(cmd); (void)rc; }
    mkdir(base, 0755);
    char realdir[300], realfile[300], linkfile[300], dirlink[300];
    snprintf(realdir, sizeof(realdir), "%s/realdir", base);
    snprintf(realfile, sizeof(realfile), "%s/real.txt", base);
    snprintf(linkfile, sizeof(linkfile), "%s/link.txt", base);
    snprintf(dirlink, sizeof(dirlink), "%s/dirlink", base);
    mkdir(realdir, 0755);
    FILE *f = fopen(realfile, "w");
    if (f) {
        fputs("hello", f);
        fclose(f);
    }
    char inner[320];
    snprintf(inner, sizeof(inner), "%s/inner.txt", realdir);
    f = fopen(inner, "w");
    if (f) {
        fputs("inner", f);
        fclose(f);
    }
    { int rc = symlink(realfile, linkfile); (void)rc; }
    { int rc = symlink(realdir, dirlink); (void)rc; }

    CHECK(cdc_is_reparse_point(linkfile), "reparse symlink file");
    CHECK(cdc_is_reparse_point(dirlink), "reparse symlink dir");
    CHECK(!cdc_is_reparse_point(realfile), "not reparse regular file");
    CHECK(!cdc_is_reparse_point(realdir), "not reparse regular dir");
    int is_rep = 0;
    cdc_err_t e;
    CHECK(cdc_reparse_point_status(linkfile, &is_rep, &e) == CDC_OK && is_rep,
          "reparse_point_status symlink ok");
    CHECK(cdc_reparse_point_status("/tmp/cdc-test-safety-no-such-file-xyz", &is_rep, &e) == CDC_ERR_NOT_FOUND,
          "reparse_point_status missing -> NOT_FOUND");
    /* 遍历不跟随目录链接：inner.txt 只应出现一次（realdir 下），
     * 不应通过 dirlink 再出现一次。 */
    cdc_vec_t seen;
    cdc_vec_init(&seen, sizeof(char *));
    cdc_walk_dir(base, 1, collect_cb, &seen, NULL);
    int link_seen = 0;
    for (size_t i = 0; i < seen.len; i++) {
        if (strstr(((char **)seen.data)[i], "dirlink")) link_seen++;
    }
    CHECK(link_seen == 1, "walk sees dirlink itself once, never descends (%d)", link_seen);
    for (size_t i = 0; i < seen.len; i++) free(((char **)seen.data)[i]);
    cdc_vec_free(&seen);
    /* Linux 下句柄删除是桩：必须返回错误、绝不能静默成功，文件还在。 */
    const char *roots[1] = { base };
    cdc_delete_outcome_t out = cdc_delete_file_checked(realfile, roots, 1, 1, &e);
    CHECK(out != CDC_DELETE_DELETED, "linux delete stub must not report Deleted");
    FILE *check = fopen(realfile, "r");
    CHECK(check != NULL, "linux delete stub leaves file in place");
    if (check) fclose(check);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", base);
    { int rc = system(cmd); (void)rc; }
}
#endif

/* ---------------- Windows 专属：句柄删除 / junction / 改名 ---------------- */

#ifdef _WIN32
static void test_temp_delete_windows(void) {
    /* Rust: temp_file_can_be_deleted_but_only_inside_roots */
    char tmp[MAX_PATH];
    GetTempPathA(sizeof(tmp), tmp);
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s\\cdc-delete-test", tmp);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\"", dir);
    (void)system(cmd);
    CreateDirectoryA(dir, NULL);
    char file[MAX_PATH];
    snprintf(file, sizeof(file), "%s\\sample.bin", dir);
    FILE *f = fopen(file, "wb");
    if (f) {
        fputs("hello", f);
        fclose(f);
    }
    const char *other[1] = { "C:\\cdc-other" };
    cdc_err_t e;
    CHECK(cdc_delete_file_checked(file, other, 1, 1, &e) == CDC_DELETE_SKIPPED,
          "delete outside roots -> Skipped");
    DWORD a = GetFileAttributesA(file);
    CHECK(a != INVALID_FILE_ATTRIBUTES, "file still exists after skipped delete");
    const char *roots[1] = { dir };
    CHECK(cdc_delete_file_checked(file, roots, 1, 1, &e) == CDC_DELETE_DELETED,
          "delete inside roots -> Deleted");
    a = GetFileAttributesA(file);
    CHECK(a == INVALID_FILE_ATTRIBUTES, "file gone after delete");
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\"", dir);
    (void)system(cmd);
}

static void test_junction_windows(void) {
    /* Rust: file_and_directory_operations_refuse_junction_components */
    char tmp[MAX_PATH];
    GetTempPathA(sizeof(tmp), tmp);
    char stamp[64];
    snprintf(stamp, sizeof(stamp), "cdc-handle-relative-%lu", (unsigned long)GetCurrentProcessId());
    char base[MAX_PATH], root[MAX_PATH], outside[MAX_PATH], junction[MAX_PATH];
    snprintf(base, sizeof(base), "%s\\%s", tmp, stamp);
    snprintf(root, sizeof(root), "%s\\root", base);
    snprintf(outside, sizeof(outside), "%s\\outside", base);
    snprintf(junction, sizeof(junction), "%s\\redirect", root);
    CreateDirectoryA(root, NULL);
    CreateDirectoryA(outside, NULL);
    char keep[MAX_PATH], journal[MAX_PATH];
    snprintf(keep, sizeof(keep), "%s\\keep.txt", outside);
    snprintf(journal, sizeof(journal), "%s\\im_move_pending.json", outside);
    FILE *f = fopen(keep, "wb");
    if (f) {
        fputs("must survive", f);
        fclose(f);
    }
    f = fopen(journal, "wb");
    if (f) {
        fputs("must not be read or overwritten", f);
        fclose(f);
    }
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cmd.exe /D /C mklink /J \"%s\" \"%s\"", junction, outside);
    int rc = system(cmd);
    CHECK(rc == 0, "mklink /J succeeds");
    const char *roots[1] = { root };
    cdc_handle_chain_t *chain = NULL;
    cdc_handle_t fh = NULL;
    cdc_err_t e;
    char jjournal[MAX_PATH];
    snprintf(jjournal, sizeof(jjournal), "%s\\im_move_pending.json", junction);
    CHECK(cdc_open_read_file_checked(jjournal, roots, 1, &chain, &fh, &e) != CDC_OK,
          "open_read through junction refused");
    if (chain) {
        cdc_handle_chain_free(chain);
        free(chain);
        chain = NULL;
    }
    char jnew[MAX_PATH];
    snprintf(jnew, sizeof(jnew), "%s\\new-journal.json", junction);
    CHECK(cdc_create_new_file_checked(jnew, roots, 1, &chain, &fh, &e) != CDC_OK,
          "create_new through junction refused");
    if (chain) {
        cdc_handle_chain_free(chain);
        free(chain);
        chain = NULL;
    }
    char target[MAX_PATH];
    snprintf(target, sizeof(target), "%s\\must-not-create", junction);
    cdc_handle_chain_t *tc = NULL;
    CHECK(cdc_create_directory_tree_checked(target, &tc, &e) != CDC_OK,
          "create_directory_tree through junction refused");
    if (tc) {
        cdc_handle_chain_free(tc);
        free(tc);
    }
    char mustnot[MAX_PATH];
    snprintf(mustnot, sizeof(mustnot), "%s\\must-not-create", outside);
    CHECK(GetFileAttributesA(mustnot) == INVALID_FILE_ATTRIBUTES,
          "outside must-not-create absent");
    char jkeep[MAX_PATH];
    snprintf(jkeep, sizeof(jkeep), "%s\\keep.txt", junction);
    CHECK(cdc_delete_file_checked(jkeep, roots, 1, 0, &e) == CDC_DELETE_SKIPPED,
          "delete through junction -> Skipped");
    int deleted = -1;
    CHECK(cdc_remove_directory_checked(junction, roots, 1, &deleted, &e) == CDC_OK && deleted == 0,
          "remove_directory junction -> Ok(false)");
    CHECK(GetFileAttributesA(keep) != INVALID_FILE_ATTRIBUTES, "outside keep.txt survives");
    CHECK(GetFileAttributesA(junction) != INVALID_FILE_ATTRIBUTES, "junction itself survives");
    snprintf(cmd, sizeof(cmd), "rmdir \"%s\"", junction);
    (void)system(cmd);
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\"", base);
    (void)system(cmd);
}

static void test_rename_windows(void) {
    /* Rust: directory_rename_uses_sibling_names_under_a_plain_parent */
    char tmp[MAX_PATH];
    GetTempPathA(sizeof(tmp), tmp);
    char base[MAX_PATH], source[MAX_PATH], backup[MAX_PATH];
    snprintf(base, sizeof(base), "%s\\cdc-handle-rename-%lu", tmp, (unsigned long)GetCurrentProcessId());
    snprintf(source, sizeof(source), "%s\\source", base);
    snprintf(backup, sizeof(backup), "%s\\source_backup_20261116_200000", base);
    CreateDirectoryA(source, NULL);
    char data[MAX_PATH];
    snprintf(data, sizeof(data), "%s\\data.bin", source);
    FILE *f = fopen(data, "wb");
    if (f) {
        fputs("keep data", f);
        fclose(f);
    }
    const char *allowed[1] = { source };
    cdc_err_t e;
    CHECK(cdc_rename_directory_sibling_checked(source, backup, allowed, 1, &e) == CDC_OK,
          "rename source -> backup ok");
    CHECK(GetFileAttributesA(source) == INVALID_FILE_ATTRIBUTES, "source gone after rename");
    char bdata[MAX_PATH];
    snprintf(bdata, sizeof(bdata), "%s\\data.bin", backup);
    CHECK(GetFileAttributesA(bdata) != INVALID_FILE_ATTRIBUTES, "backup data exists");
    CHECK(cdc_rename_directory_sibling_checked(backup, source, allowed, 1, &e) == CDC_OK,
          "rename backup -> source ok");
    CHECK(GetFileAttributesA(data) != INVALID_FILE_ATTRIBUTES, "source data back");
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\"", base);
    (void)system(cmd);
}
#endif

int main(void) {
    /* Windows 形态的环境变量，使纯路径判定与 Windows 一致。 */
    set_env("USERPROFILE", "C:\\Users\\Administrator");
    set_env("LOCALAPPDATA", "C:\\Users\\Administrator\\AppData\\Local");
    set_env("APPDATA", "C:\\Users\\Administrator\\AppData\\Roaming");
    set_env("SystemRoot", "C:\\Windows");
    set_env("WINDIR", "C:\\Windows");

    test_normalize();
    test_protected_paths();
    test_password_store();
    test_never_clean();
    test_critical_files();
    test_path_syntax_error();
    test_user_library_and_system();
    test_explicit_delete();
    test_protected_path();
#ifdef _WIN32
    test_temp_delete_windows();
    test_junction_windows();
    test_rename_windows();
#else
    test_reparse_linux();
    SKIP("temp_file_can_be_deleted_but_only_inside_roots (Windows-only)");
    SKIP("file_and_directory_operations_refuse_junction_components (Windows-only)");
    SKIP("directory_rename_uses_sibling_names_under_a_plain_parent (Windows-only)");
#endif

    printf("test_safety: %d checks, %d failures, %d skipped\n", checks, failures, skipped);
    return failures == 0 ? 0 : 1;
}
