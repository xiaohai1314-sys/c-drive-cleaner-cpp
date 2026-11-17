// bigfile.cpp — 阈值表与 Rust 版 main.rs:697 一一对应，分组注释同款。
#include "bigfile.h"

#include <array>
#include <cctype>
#include <string>

namespace cdc {
namespace {

std::string_view base_name(std::string_view path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// 取 basename 里最后一个 '.' 后的扩展名（小写，最多 31 字节）；无扩展名返回 ""。
// Rust Path::extension() 对 ".gitignore" 这类名前置点返回 None，这里同款处理。
std::string ext_lower(std::string_view path) {
    std::string_view base = base_name(path);
    size_t dot = base.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == base.size()) return {};
    std::string ext;
    for (char c : base.substr(dot + 1)) {
        if (ext.size() == 31) break;
        ext.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return ext;
}

template <size_t N>
bool in_list(std::string_view ext, const std::array<std::string_view, N> &list) {
    for (std::string_view e : list)
        if (ext == e) return true;
    return false;
}

constexpr std::array<std::string_view, 2> kInstaller{"exe", "msi"};
constexpr std::array<std::string_view, 16> kVideo{"mp4", "avi",  "mov",  "mkv", "wmv", "flv",
                                                  "m4v", "3gp",  "mpeg", "mpg", "ts",  "webm",
                                                  "rmvb", "vob", "dv",   "swf"};
constexpr std::array<std::string_view, 13> kAudio{"mp3", "ape", "wav", "flac", "aac",
                                                  "ogg", "m4a", "ac3", "dts",  "mka",
                                                  "opus", "wma", "mid"};
constexpr std::array<std::string_view, 12> kArchive{"zip", "rar", "7z",  "gz",  "iso",  "img",
                                                    "tar", "bz2", "xz", "cab", "fzip", "rar4"};
constexpr std::array<std::string_view, 3> kMobilePkg{"apk", "ipa", "ipsw"};
constexpr std::array<std::string_view, 7> kPackDbg{"pack", "wim", "pima", "lib",
                                                   "pdb",  "rb",  "torrent"};
constexpr std::array<std::string_view, 13> kImage{"jpg", "jpeg", "png", "gif",  "bmp",
                                                  "tiff", "tif", "webp", "heic", "ico",
                                                  "raw", "psd",  "svg"};
constexpr std::array<std::string_view, 26> kDoc{
    "txt",  "csv",  "pdf", "doc", "docx", "xls", "xlsx", "ppt", "pptx",
    "odf",  "wps",  "wpt", "dot", "dotx", "docm", "xlsm", "rtf", "odt",
    "dwg",  "dxf",  "md",  "et",  "dps",  "ofd", "xmind", "vsdx"};
constexpr std::array<std::string_view, 8> kLog{"log", "xlog", "dump", "dmp",
                                               "lg",  "tlg",  "ttd",  "xltd"};

}  // namespace

uint64_t big_file_threshold_bytes(std::string_view path) {
    std::string ext = ext_lower(path);
    if (in_list(ext, kInstaller)) return 100 * kMB;
    if (in_list(ext, kVideo)) return 20 * kMB;
    if (ext == "tmp") return 20 * kMB;
    if (in_list(ext, kAudio)) return 5 * kMB;
    if (in_list(ext, kArchive)) return 5 * kMB;
    if (in_list(ext, kMobilePkg)) return 5 * kMB;
    if (in_list(ext, kPackDbg)) return 5 * kMB;
    if (in_list(ext, kImage)) return 2 * kMB;
    if (in_list(ext, kDoc)) return 2 * kMB;
    if (in_list(ext, kLog)) return 2 * kMB;
    return 50 * kMB;  // 兜底（毒霸 id 120 的 `*` 规则）
}

bool is_system_file_name(std::string_view path) {
    std::string lower;
    for (char c : base_name(path)) {
        if (lower.size() == 63) break;  // 与 C 版 char lower[64] 同样截断
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lower == "pagefile.sys" || lower == "hiberfil.sys" || lower == "swapfile.sys" ||
           lower == "dumpstack.log.tmp" || lower == "memory.dmp";
}

}  // namespace cdc
