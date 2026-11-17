/* bigfile.c — 阈值表与 Rust 版 main.rs:697 一一对应，分组注释同款。*/
#include "bigfile.h"

#include <ctype.h>
#include <string.h>

/* 取 basename 里最后一个 '.' 后的扩展名，小写进 buf；无扩展名则 buf 为 ""。
 * Rust Path::extension() 对 ".gitignore" 这类名前置点返回 None，这里同款处理。 */
static void ext_lower(const char *path, char *buf, size_t bufsz) {
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    const char *dot = NULL;
    for (const char *p = base; *p; p++)
        if (*p == '.') dot = p;
    if (!dot || dot == base || dot[1] == '\0') { buf[0] = '\0'; return; }
    size_t i = 0;
    for (const char *p = dot + 1; *p && i + 1 < bufsz; p++, i++)
        buf[i] = (char)tolower((unsigned char)*p);
    buf[i] = '\0';
}

static int in_list(const char *ext, const char *const *list, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(ext, list[i]) == 0) return 1;
    return 0;
}

uint64_t cdc_big_file_threshold_bytes(const char *path) {
    char ext[32];
    ext_lower(path, ext, sizeof ext);

    static const char *const installer[] = {"exe", "msi"};
    static const char *const video[] = {"mp4", "avi", "mov", "mkv", "wmv", "flv",
        "m4v", "3gp", "mpeg", "mpg", "ts", "webm", "rmvb", "vob", "dv", "swf"};
    static const char *const audio[] = {"mp3", "ape", "wav", "flac", "aac", "ogg",
        "m4a", "ac3", "dts", "mka", "opus", "wma", "mid"};
    static const char *const archive[] = {"zip", "rar", "7z", "gz", "iso", "img",
        "tar", "bz2", "xz", "cab", "fzip", "rar4"};
    static const char *const mobile_pkg[] = {"apk", "ipa", "ipsw"};
    static const char *const pack_dbg[] = {"pack", "wim", "pima", "lib", "pdb",
        "rb", "torrent"};
    static const char *const image[] = {"jpg", "jpeg", "png", "gif", "bmp", "tiff",
        "tif", "webp", "heic", "ico", "raw", "psd", "svg"};
    static const char *const doc[] = {"txt", "csv", "pdf", "doc", "docx", "xls",
        "xlsx", "ppt", "pptx", "odf", "wps", "wpt", "dot", "dotx", "docm",
        "xlsm", "rtf", "odt", "dwg", "dxf", "md", "et", "dps", "ofd",
        "xmind", "vsdx"};
    static const char *const log[] = {"log", "xlog", "dump", "dmp", "lg", "tlg",
        "ttd", "xltd"};

    if (in_list(ext, installer, 2)) return 100 * CDC_MB;
    if (in_list(ext, video, 16)) return 20 * CDC_MB;
    if (strcmp(ext, "tmp") == 0) return 20 * CDC_MB;
    if (in_list(ext, audio, 13)) return 5 * CDC_MB;
    if (in_list(ext, archive, 12)) return 5 * CDC_MB;
    if (in_list(ext, mobile_pkg, 3)) return 5 * CDC_MB;
    if (in_list(ext, pack_dbg, 7)) return 5 * CDC_MB;
    if (in_list(ext, image, 13)) return 2 * CDC_MB;
    if (in_list(ext, doc, 26)) return 2 * CDC_MB;
    if (in_list(ext, log, 8)) return 2 * CDC_MB;
    return 50 * CDC_MB; /* 兜底（毒霸 id 120 的 `*` 规则） */
}

int cdc_is_system_file_name(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    char lower[64];
    size_t i = 0;
    for (const char *p = base; *p && i + 1 < sizeof lower; p++, i++)
        lower[i] = (char)tolower((unsigned char)*p);
    lower[i] = '\0';
    return strcmp(lower, "pagefile.sys") == 0 ||
           strcmp(lower, "hiberfil.sys") == 0 ||
           strcmp(lower, "swapfile.sys") == 0 ||
           strcmp(lower, "dumpstack.log.tmp") == 0 ||
           strcmp(lower, "memory.dmp") == 0;
}
