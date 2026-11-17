/* mft_raw.c — 只在 Windows 下编译：卷直读的最小骨架。
 *
 * 对应 Rust 版 mft.rs 里的 Win32 部分（CreateFileW 打开 \\.\C:，
 * FSCTL_GET_NTFS_VOLUME_DATA 拿 bytes_per_sector / bytes_per_cluster /
 * mft_start_lcn）。这里只做到“打开卷 + 读卷信息 + 打印”，
 * 证明 Win32 API 面在 MinGW 下能编过、常量/结构体对得上；
 * 真正的 $MFT 块读取 + parse_record_in_place 移植是下一步，
 * 不在这次 pilot 里（见 README 的“没做什么”）。
 */
#ifdef _WIN32

#include <stdio.h>
#include <windows.h>
#include <winioctl.h>

#ifndef FSCTL_GET_NTFS_VOLUME_DATA
#define FSCTL_GET_NTFS_VOLUME_DATA CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 25, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#pragma pack(push, 1)
typedef struct {
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
} NTFS_VOLUME_DATA_BUFFER_CDC;
#pragma pack(pop)

int cdc_mft_probe_volume(const wchar_t *drive_letter) {
    wchar_t path[16];
    swprintf(path, 16, L"\\\\.\\%c:", (wchar_t)drive_letter[0]);
    HANDLE h = CreateFileW(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "CreateFileW(%ls) failed: %lu\n", path, GetLastError());
        return 1;
    }
    NTFS_VOLUME_DATA_BUFFER_CDC vd = {};
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0,
                              &vd, sizeof vd, &ret, NULL);
    if (!ok) {
        fprintf(stderr, "FSCTL_GET_NTFS_VOLUME_DATA failed: %lu\n", GetLastError());
        CloseHandle(h);
        return 1;
    }
    printf("bytes_per_sector=%lu bytes_per_cluster=%lu\n",
           (unsigned long)vd.BytesPerSector, (unsigned long)vd.BytesPerCluster);
    printf("bytes_per_file_record=%lu mft_start_lcn=%lld mft_valid_bytes=%lld\n",
           (unsigned long)vd.BytesPerFileRecordSegment,
           (long long)vd.MftStartLcn.QuadPart,
           (long long)vd.MftValidDataLength.QuadPart);
    CloseHandle(h);
    return 0;
}

#endif /* _WIN32 */
