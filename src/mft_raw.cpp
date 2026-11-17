// mft_raw.cpp — 只在 Windows 下编译：卷直读的最小探测（--probe 命令）。
//
// 对应 Rust 版 mft.rs 里的 Win32 部分（CreateFileW 打开 \\.\C:，
// FSCTL_GET_NTFS_VOLUME_DATA 拿 bytes_per_sector / bytes_per_cluster /
// mft_start_lcn），只做"打开卷 + 读卷信息 + 打印"。
#ifdef _WIN32
#include <cstdio>
#include <cwchar>

#include "mft.h"
#include "raii.h"

#include <windows.h>
#include <winioctl.h>

#ifndef FSCTL_GET_NTFS_VOLUME_DATA
#define FSCTL_GET_NTFS_VOLUME_DATA \
    CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 25, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

namespace cdc {
namespace {

#pragma pack(push, 1)
struct NtfsVolumeDataBuffer {
    LARGE_INTEGER VolumeSerialNumber;
    LARGE_INTEGER NumberSectors;
    LARGE_INTEGER TotalClusters;
    LARGE_INTEGER FreeClusters;
    LARGE_INTEGER TotalReserved;
    DWORD BytesPerSector;
    DWORD BytesPerCluster;
    DWORD BytesPerFileRecordSegment;
    DWORD ClustersPerFileRecordSegment;
    LARGE_INTEGER MftValidDataLength;
    LARGE_INTEGER MftStartLcn;
    LARGE_INTEGER Mft2StartLcn;
    LARGE_INTEGER MftZoneStart;
    LARGE_INTEGER MftZoneEnd;
};
#pragma pack(pop)

}  // namespace

int mft_probe_volume(const wchar_t *drive_letter) {
    wchar_t path[16];
    std::swprintf(path, 16, L"\\\\.\\%c:", static_cast<wchar_t>(drive_letter[0]));
    UniqueHandle h = adopt_handle(CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                              nullptr, OPEN_EXISTING, 0, nullptr));
    if (!h) {
        std::fprintf(stderr, "CreateFileW(%ls) failed: %lu\n", path, GetLastError());
        return 1;
    }
    NtfsVolumeDataBuffer vd{};
    DWORD ret = 0;
    if (!DeviceIoControl(h.get(), FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &vd, sizeof vd, &ret,
                         nullptr)) {
        std::fprintf(stderr, "FSCTL_GET_NTFS_VOLUME_DATA failed: %lu\n", GetLastError());
        return 1;
    }
    std::printf("bytes_per_sector=%lu bytes_per_cluster=%lu\n",
                static_cast<unsigned long>(vd.BytesPerSector),
                static_cast<unsigned long>(vd.BytesPerCluster));
    std::printf("bytes_per_file_record=%lu mft_start_lcn=%lld mft_valid_bytes=%lld\n",
                static_cast<unsigned long>(vd.BytesPerFileRecordSegment),
                static_cast<long long>(vd.MftStartLcn.QuadPart),
                static_cast<long long>(vd.MftValidDataLength.QuadPart));
    return 0;
}

}  // namespace cdc
#endif  // _WIN32
