/* test_safety.cpp — src/safety.{cpp,h} 的自测（make check 构建并运行；
 * Windows 交叉构建产出 test_safety.exe，在 Windows/CI 上跑句柄测试）。
 *
 * 断言与 Rust 版 desktop/src/safety.rs 的 #[cfg(test)] 用例逐一对照
 *（见各测试函数头的注释）；纯路径判定在 Linux 下也能跑——测试入口
 * 先把 USERPROFILE / SystemRoot 等环境变量设成 Windows 形态的值，
 * 使 is_user_library / is_system_path 的行为与 Windows 一致。
 * 句柄删除 / junction / 改名三组测试是 Windows 专属（Linux 下句柄
 * API 是返回错误的桩），在 Linux 下只打印 SKIP，不计失败。
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

static void test_normalize() {
    /* Rust: normalize_keeps_windows_shape */
    std::string n = cdc::normalize("C:/Windows/Temp/");
    CHECK(n == "C:\\Windows\\Temp", "normalize C:/Windows/Temp/ -> %s", n.c_str());
    n = cdc::normalize("c:\\Windows\\\\Temp");
    CHECK(n == "C:\\Windows\\Temp", "normalize c:\\Windows\\\\Temp -> %s", n.c_str());
    n = cdc::normalize("C:\\");
    CHECK(n == "C:\\", "normalize C:\\ -> %s", n.c_str());
}

static void test_protected_paths() {
    /* Rust: protected_paths_are_forbidden */
    CHECK(cdc::is_system_path("C:\\Windows\\System32\\kernel32.dll"), "system path kernel32.dll");
    CHECK(cdc::execution_forbidden("C:\\Windows\\System32\\kernel32.dll", true), "execution_forbidden system32 untrusted");
    CHECK(!cdc::execution_forbidden("C:\\Windows\\Temp\\a.tmp", false), "execution_forbidden Windows\\Temp trusted=false allowed");
    CHECK(cdc::execution_forbidden("C:\\pagefile.sys", false), "execution_forbidden pagefile.sys");
    CHECK(cdc::execution_forbidden("C:\\Program Files\\App\\a.dll", true), "execution_forbidden Program Files untrusted");
    CHECK(cdc::execution_forbidden("C:\\$Recycle.Bin\\x", true), "execution_forbidden $Recycle.Bin untrusted");
    CHECK(cdc::execution_forbidden("C:\\Users\\Administrator\\Documents\\note.txt", false), "execution_forbidden Documents untrusted=false");
    /* 内置规则也一律 untrusted：Temp 下的文件在 allowlist 内才放行。 */
    CHECK(!cdc::execution_forbidden("C:\\Users\\Administrator\\AppData\\Local\\Temp\\a.tmp", true),
          "execution_forbidden Local Temp untrusted allowed");
}

static void test_password_store() {
    /* Rust: password_store_is_forbidden + 全表 */
    CHECK(cdc::is_password_store("C:\\x\\Login Data"), "password Login Data");
    CHECK(cdc::is_password_store("C:\\x\\logins.json"), "password logins.json");
    CHECK(!cdc::is_password_store("C:\\x\\data.bin"), "not password data.bin");
    const char *names[8] = { "Login Data", "Login Data-journal",
                             "Login Data for Account",
                             "Login Data for Account-journal", "logins.json",
                             "logins-backup.json", "key4.db", "key3.db" };
    for (int i = 0; i < 8; i++) {
        char path[128];
        snprintf(path, sizeof(path), "C:\\x\\%s", names[i]);
        CHECK(cdc::is_password_store(path), "password table %s", names[i]);
    }
    CHECK(cdc::is_password_store("C:\\x\\LOGIN DATA"), "password case-insensitive");
}

static void test_never_clean() {
    /* NEVER_CLEAN_FILES = ["usmt.ppkg"]；NEVER_CLEAN_ROOTS =
     * ["\\recovery", "\\system volume information"] */
    CHECK(cdc::is_never_clean("C:\\Recovery\\Customizations\\usmt.ppkg"), "never_clean usmt.ppkg");
    CHECK(cdc::is_never_clean("usmt.ppkg"), "never_clean bare usmt.ppkg");
    CHECK(cdc::is_never_clean("C:\\USMT.PPKG"), "never_clean case-insensitive");
    CHECK(cdc::is_never_clean("C:\\Recovery\\a.txt"), "never_clean Recovery root");
    CHECK(cdc::is_never_clean("C:\\System Volume Information\\x"), "never_clean SVI root");
    CHECK(!cdc::is_never_clean("C:\\Temp\\a.txt"), "not never_clean Temp");
    CHECK(cdc::is_never_clean(""), "never_clean empty");
}

static void test_critical_files() {
    const char *names[7] = { "pagefile.sys", "hiberfil.sys", "swapfile.sys",
                             "bootmgr", "ntldr", "ntuser.dat",
                             "ntuser.dat.log" };
    for (int i = 0; i < 7; i++) {
        char path[128];
        snprintf(path, sizeof(path), "C:\\%s", names[i]);
        CHECK(cdc::is_critical_file(path), "critical %s", names[i]);
    }
    CHECK(!cdc::is_critical_file("C:\\pagefile.sys.bak"), "not critical pagefile.sys.bak");
    CHECK(cdc::is_critical_file("C:\\PAGEFILE.SYS"), "critical case-insensitive");
}

static void test_path_syntax_error() {
    CHECK(cdc::path_syntax_error(""), "syntax empty");
    CHECK(cdc::path_syntax_error("   "), "syntax blank");
    CHECK(cdc::path_syntax_error("\\\\server\\share"), "syntax UNC \\\\");
    CHECK(cdc::path_syntax_error("//server/share"), "syntax UNC //");
    CHECK(cdc::path_syntax_error("C:\\a\\..\\b"), "syntax ..");
    CHECK(cdc::path_syntax_error("..\\a"), "syntax leading ..");
    CHECK(cdc::path_syntax_error("C:\\a:b"), "syntax colon in body");
    CHECK(!cdc::path_syntax_error("C:\\Windows\\Temp"), "syntax ok Windows Temp");
    CHECK(!cdc::path_syntax_error("C:foo"), "syntax ok C:foo");
    CHECK(!cdc::path_syntax_error("C:\\Users\\u\\a.txt"), "syntax ok user file");
}

static void test_user_library_and_system() {
    /* Rust: user_library_and_system_detection */
    CHECK(cdc::is_system_path("C:\\Windows\\Temp"), "system Windows\\Temp");
    CHECK(!cdc::is_system_path("C:\\Users\\Administrator\\AppData\\Local\\Temp"),
          "not system Local Temp");
    const char *roots[1] = { "C:\\Users\\Administrator\\AppData\\Local\\Temp" };
    CHECK(cdc::is_within("C:\\Users\\Administrator\\AppData\\Local\\Temp\\a\\b.txt", roots),
          "is_within Temp subpath");
    CHECK(!cdc::is_within("C:\\Users\\Administrator\\AppData\\Local\\Other\\a.txt", roots),
          "not is_within outside root");
    CHECK(cdc::is_user_library("C:\\Users\\Administrator\\Documents\\a.txt"), "user library Documents");
    CHECK(cdc::is_user_library("C:\\Users\\Administrator\\Downloads\\a.mp4"), "user library Downloads");
    CHECK(cdc::is_user_library("C:\\Users\\Administrator\\桌面\\a.txt"), "user library 桌面");
    CHECK(!cdc::is_user_library("C:\\Windows\\Temp\\a.txt"), "not user library Windows Temp");
}

static void test_explicit_delete() {
    /* Rust: explicit_delete_allows_user_folders_but_blocks_system */
    CHECK(!cdc::explicit_delete_forbidden("C:\\Users\\Administrator\\Downloads\\movie.mp4"),
          "explicit allow Downloads movie");
    CHECK(!cdc::explicit_delete_forbidden("C:\\Users\\Administrator\\Documents\\WeChat Files\\a.jpg"),
          "explicit allow Documents WeChat file");
    CHECK(cdc::explicit_delete_forbidden("C:\\Windows\\System32\\kernel32.dll"),
          "explicit block System32");
    CHECK(cdc::explicit_delete_forbidden("C:\\pagefile.sys"), "explicit block pagefile");
    CHECK(cdc::explicit_delete_forbidden("C:\\Users\\Administrator\\AppData\\Local\\Google\\Chrome\\User Data\\Default\\Login Data"),
          "explicit block Login Data");
    CHECK(cdc::explicit_delete_forbidden("C:\\Program Files\\App\\a.dll"), "explicit block Program Files");
    CHECK(cdc::explicit_delete_forbidden("C:\\"), "explicit block drive root");
}

static void test_protected_path() {
    CHECK(cdc::is_protected_path("C:\\Windows\\System32\\a.dll"), "protected System32");
    CHECK(cdc::is_protected_path("C:\\Program Files\\App\\a.dll"), "protected Program Files");
    CHECK(cdc::is_protected_path("C:\\ProgramData\\App\\a.txt"), "protected ProgramData");
    /* 注意：Rust 原版里 Documents 不属于 is_protected_path（它只认
     * 系统目录 + Program Files/ProgramData/Perflogs + 环境根 + profile
     * 本身）；用户库的保护在 is_user_library / execution_forbidden 里
     *（对照 Rust safety.rs tests：execution_forbidden 断言
     * <USERPROFILE>\Documents\note.txt 为真）。 */
    CHECK(!cdc::is_protected_path("C:\\Users\\Administrator\\Documents\\a.txt"),
          "not protected user Documents (is_protected_path)");
    CHECK(cdc::is_user_library("C:\\Users\\Administrator\\Documents\\a.txt"),
          "user library Documents");
    CHECK(cdc::execution_forbidden("C:\\Users\\Administrator\\Documents\\a.txt", false),
          "execution forbidden user Documents");
    CHECK(!cdc::is_protected_path("C:\\Users\\Administrator\\AppData\\Local\\Temp\\a.tmp"),
          "not protected Local Temp");
}

/* ---------------- Linux 可跑的 reparse 测试 ---------------- */

#ifndef _WIN32
static void test_reparse_linux() {
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

    CHECK(cdc::is_reparse_point(linkfile), "reparse symlink file");
    CHECK(cdc::is_reparse_point(dirlink), "reparse symlink dir");
    CHECK(!cdc::is_reparse_point(realfile), "not reparse regular file");
    CHECK(!cdc::is_reparse_point(realdir), "not reparse regular dir");
    bool is_rep = false;
    cdc::Error e;
    CHECK(cdc::reparse_point_status(linkfile, is_rep, &e) == cdc::kOk && is_rep,
          "reparse_point_status symlink ok");
    CHECK(cdc::reparse_point_status("/tmp/cdc-test-safety-no-such-file-xyz", is_rep, &e) ==
              cdc::kErrNotFound,
          "reparse_point_status missing -> NOT_FOUND");
    /* 遍历不跟随目录链接：inner.txt 只应出现一次（realdir 下），
     * 不应通过 dirlink 再出现一次。 */
    std::vector<std::string> seen;
    cdc::walk_dir(base, 1, [&](const cdc::WalkEntry &w) {
        /* 遍历永不跟随链接：链接本身出现即记一笔，供断言用。 */
        if (w.is_symlink) seen.emplace_back(w.path);
        return 0;
    }, nullptr);
    int link_seen = 0;
    for (const std::string &p : seen)
        if (p.find("dirlink") != std::string::npos) link_seen++;
    CHECK(link_seen == 1, "walk sees dirlink itself once, never descends (%d)", link_seen);
    /* Linux 下句柄删除是桩：必须返回错误、绝不能静默成功，文件还在。 */
    const char *roots[1] = { base };
    cdc::DeleteOutcome out = cdc::delete_file_checked(realfile, roots, true, &e);
    CHECK(out != cdc::DeleteOutcome::Deleted, "linux delete stub must not report Deleted");
    FILE *check = fopen(realfile, "r");
    CHECK(check != nullptr, "linux delete stub leaves file in place");
    if (check) fclose(check);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", base);
    { int rc = system(cmd); (void)rc; }
}
#endif

/* ---------------- Windows 专属：句柄删除 / junction / 改名 ---------------- */

#ifdef _WIN32
static void remove_test_tree(const char *path) {
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return;
    char cmd[MAX_PATH + 64];
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\"", path);
    (void)system(cmd);
}

static void test_temp_delete_windows() {
    /* Rust: temp_file_can_be_deleted_but_only_inside_roots */
    char tmp[MAX_PATH];
    GetTempPathA(sizeof(tmp), tmp);
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s\\cdc-delete-test", tmp);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\"", dir);
    (void)system(cmd);
    CreateDirectoryA(dir, nullptr);
    char file[MAX_PATH];
    snprintf(file, sizeof(file), "%s\\sample.bin", dir);
    FILE *f = fopen(file, "wb");
    if (f) {
        fputs("hello", f);
        fclose(f);
    }
    const char *other[1] = { "C:\\cdc-other" };
    cdc::Error e;
    CHECK(cdc::delete_file_checked(file, other, true, &e) == cdc::DeleteOutcome::Skipped,
          "delete outside roots -> Skipped");
    DWORD a = GetFileAttributesA(file);
    CHECK(a != INVALID_FILE_ATTRIBUTES, "file still exists after skipped delete");
    const char *roots[1] = { dir };
    CHECK(cdc::delete_file_checked(file, roots, true, &e) == cdc::DeleteOutcome::Deleted,
          "delete inside roots -> Deleted");
    a = GetFileAttributesA(file);
    CHECK(a == INVALID_FILE_ATTRIBUTES, "file gone after delete");
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\"", dir);
    (void)system(cmd);
}

static void test_junction_windows() {
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
    remove_test_tree(base);
    CHECK(CreateDirectoryA(base, nullptr) != 0, "create junction test base");
    if (GetFileAttributesA(base) == INVALID_FILE_ATTRIBUTES) return;
    CHECK(CreateDirectoryA(root, nullptr) != 0, "create junction test root");
    if (GetFileAttributesA(root) == INVALID_FILE_ATTRIBUTES) {
        remove_test_tree(base);
        return;
    }
    CHECK(CreateDirectoryA(outside, nullptr) != 0, "create junction target");
    if (GetFileAttributesA(outside) == INVALID_FILE_ATTRIBUTES) {
        remove_test_tree(base);
        return;
    }
    char keep[MAX_PATH], journal[MAX_PATH];
    snprintf(keep, sizeof(keep), "%s\\keep.txt", outside);
    snprintf(journal, sizeof(journal), "%s\\im_move_pending.json", outside);
    FILE *f = fopen(keep, "wb");
    CHECK(f != nullptr, "create outside keep file");
    if (!f) {
        remove_test_tree(base);
        return;
    }
    fputs("must survive", f);
    fclose(f);
    f = fopen(journal, "wb");
    CHECK(f != nullptr, "create outside journal file");
    if (!f) {
        remove_test_tree(base);
        return;
    }
    fputs("must not be read or overwritten", f);
    fclose(f);
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cmd.exe /D /C mklink /J \"%s\" \"%s\"", junction, outside);
    int rc = system(cmd);
    CHECK(rc == 0, "mklink /J succeeds");
    if (rc != 0) {
        remove_test_tree(base);
        return;
    }
    const char *roots[1] = { root };
    cdc::HandleChain chain;
    cdc::OwnedHandle fh;
    cdc::Error e;
    char jjournal[MAX_PATH];
    snprintf(jjournal, sizeof(jjournal), "%s\\im_move_pending.json", junction);
    CHECK(cdc::open_read_file_checked(jjournal, roots, chain, fh, &e) != cdc::kOk,
          "open_read through junction refused");
    chain.clear();
    fh.reset();
    char jnew[MAX_PATH];
    snprintf(jnew, sizeof(jnew), "%s\\new-journal.json", junction);
    CHECK(cdc::create_new_file_checked(jnew, roots, chain, fh, &e) != cdc::kOk,
          "create_new through junction refused");
    chain.clear();
    fh.reset();
    char target[MAX_PATH];
    snprintf(target, sizeof(target), "%s\\must-not-create", junction);
    cdc::HandleChain tc;
    CHECK(cdc::create_directory_tree_checked(target, tc, &e) != cdc::kOk,
          "create_directory_tree through junction refused");
    tc.clear();
    char mustnot[MAX_PATH];
    snprintf(mustnot, sizeof(mustnot), "%s\\must-not-create", outside);
    CHECK(GetFileAttributesA(mustnot) == INVALID_FILE_ATTRIBUTES,
          "outside must-not-create absent");
    char jkeep[MAX_PATH];
    snprintf(jkeep, sizeof(jkeep), "%s\\keep.txt", junction);
    CHECK(cdc::delete_file_checked(jkeep, roots, false, &e) == cdc::DeleteOutcome::Skipped,
          "delete through junction -> Skipped");
    bool deleted = true;
    CHECK(cdc::remove_directory_checked(junction, roots, deleted, &e) == cdc::kOk && !deleted,
          "remove_directory junction -> Ok(false)");
    CHECK(GetFileAttributesA(keep) != INVALID_FILE_ATTRIBUTES, "outside keep.txt survives");
    CHECK(GetFileAttributesA(junction) != INVALID_FILE_ATTRIBUTES, "junction itself survives");
    snprintf(cmd, sizeof(cmd), "rmdir \"%s\"", junction);
    (void)system(cmd);
    remove_test_tree(base);
}

static void test_rename_windows() {
    /* Rust: directory_rename_uses_sibling_names_under_a_plain_parent */
    char tmp[MAX_PATH];
    GetTempPathA(sizeof(tmp), tmp);
    char base[MAX_PATH], source[MAX_PATH], backup[MAX_PATH];
    snprintf(base, sizeof(base), "%s\\cdc-handle-rename-%lu", tmp, (unsigned long)GetCurrentProcessId());
    snprintf(source, sizeof(source), "%s\\source", base);
    snprintf(backup, sizeof(backup), "%s\\source_backup_20261116_200000", base);
    remove_test_tree(base);
    CHECK(CreateDirectoryA(base, nullptr) != 0, "create rename test base");
    if (GetFileAttributesA(base) == INVALID_FILE_ATTRIBUTES) return;
    CHECK(CreateDirectoryA(source, nullptr) != 0, "create rename test source");
    if (GetFileAttributesA(source) == INVALID_FILE_ATTRIBUTES) {
        remove_test_tree(base);
        return;
    }
    char data[MAX_PATH];
    snprintf(data, sizeof(data), "%s\\data.bin", source);
    FILE *f = fopen(data, "wb");
    CHECK(f != nullptr, "create rename test data");
    if (!f) {
        remove_test_tree(base);
        return;
    }
    fputs("keep data", f);
    fclose(f);
    const char *allowed[1] = { source };
    cdc::Error e;
    int rename_rc = cdc::rename_directory_sibling_checked(source, backup, allowed, &e);
    CHECK(rename_rc == cdc::kOk,
          "rename source -> backup ok");
    if (rename_rc != cdc::kOk) {
        remove_test_tree(base);
        return;
    }
    CHECK(GetFileAttributesA(source) == INVALID_FILE_ATTRIBUTES, "source gone after rename");
    char bdata[MAX_PATH];
    snprintf(bdata, sizeof(bdata), "%s\\data.bin", backup);
    CHECK(GetFileAttributesA(bdata) != INVALID_FILE_ATTRIBUTES, "backup data exists");
    int restore_rc = cdc::rename_directory_sibling_checked(backup, source, allowed, &e);
    CHECK(restore_rc == cdc::kOk,
          "rename backup -> source ok");
    CHECK(GetFileAttributesA(data) != INVALID_FILE_ATTRIBUTES, "source data back");
    remove_test_tree(base);
}
#endif

int main() {
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
