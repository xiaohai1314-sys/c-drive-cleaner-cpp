/* fastio.c — 高速文件读取原语，逐行对照 desktop/src/fastio.rs。
 *
 * Windows：CreateFileW / OpenFileById / IOCP 均为真实实现。
 * Linux：open/pread/read + stat 的等价实现（frn 被忽略，硬链接标识用
 * st_nlink/st_ino），保证 scan 流水线在 Linux 下可跑、可测。
 */
/* _GNU_SOURCE 必须在任何系统头之前：Linux 分支用 O_CLOEXEC /
 * posix_fadvise / pread，-std=c11 下没有 feature 宏会隐式声明。 */
#define _GNU_SOURCE 1
#include "fastio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mft.h" /* CDC_MFT_SKIP_FILE_ATTRS */
#include "sampling.h" /* 头摘要是 XXH3-64（cdc_xxh3_buf_to_digest32） */
#include "util.h"     /* cdc_utf8_to_wide */

/* on_tick 串行化：回调可能非线程安全（如测试的计数器），并行路径下
 * 多个 worker 会同时调它；用 cdc_shared_lock() 包住这次调用。
 * 单线程路径多一次锁也无妨（极短临界区）。 */
static void call_tick(void (*on_tick)(void *), void *ctx) {
    if (!on_tick) return;
    cdc_shared_lock();
    on_tick(ctx);
    cdc_shared_unlock();
}

static int is_cancelled(volatile int *cancel) {
    return cancel && *cancel;
}

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>

struct CdcFile {
    HANDLE h;
};

void *cdc_open_drive_hint(char drive_letter) {
    char letter = drive_letter;
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 'a' + 'A');
    char root[4];
    root[0] = letter;
    root[1] = ':';
    root[2] = '\\';
    root[3] = '\0';
    wchar_t *w = cdc_utf8_to_wide(root);
    if (!w) return NULL;
    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    return h;
}

static HANDLE open_raw_handle_by_id_or_path(void *hint, uint64_t frn, const char *path,
                                            DWORD flags) {
    if (frn != 0 && hint) {
        FILE_ID_DESCRIPTOR desc;
        memset(&desc, 0, sizeof desc);
        desc.dwSize = (DWORD)sizeof desc;
        desc.Type = FileIdType;
        desc.FileId.QuadPart = (LONGLONG)frn;
        HANDLE h = OpenFileById(hint, &desc, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, flags);
        if (h && h != INVALID_HANDLE_VALUE) return h;
    }
    wchar_t *w = cdc_utf8_to_wide(path);
    if (!w) return NULL;
    HANDLE h = CreateFileW(w, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, flags, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    return h;
}

static int info_to_file_id(const BY_HANDLE_FILE_INFORMATION *info, CdcFileId *out) {
    uint64_t index = ((uint64_t)info->nFileIndexHigh << 32) | info->nFileIndexLow;
    if (info->nNumberOfLinks > 1 && index != 0) {
        out->volume_serial = info->dwVolumeSerialNumber;
        out->file_index = index;
        return 1;
    }
    return 0;
}

CdcFile *cdc_file_open_checked(const char *path, uint64_t frn, uint32_t flags, void *hint,
                               int *has_file_id, CdcFileId *file_id_out,
                               uint64_t *actual_size_out) {
    HANDLE h = open_raw_handle_by_id_or_path(hint, frn, path, (DWORD)flags);
    if (!h) return NULL;
    if (has_file_id) *has_file_id = 0;
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileInformationByHandle(h, &info)) {
        if (info.dwFileAttributes & CDC_MFT_SKIP_FILE_ATTRS) {
            CloseHandle(h);
            return NULL;
        }
        if (has_file_id && file_id_out) *has_file_id = info_to_file_id(&info, file_id_out);
        if (actual_size_out)
            *actual_size_out =
                ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    } else if (actual_size_out) {
        LARGE_INTEGER sz;
        if (GetFileSizeEx(h, &sz))
            *actual_size_out = (uint64_t)sz.QuadPart;
        else
            *actual_size_out = 0;
    }
    CdcFile *f = malloc(sizeof *f);
    if (!f) {
        CloseHandle(h);
        return NULL;
    }
    f->h = h;
    return f;
}

void cdc_file_close(CdcFile *f) {
    if (!f) return;
    CloseHandle(f->h);
    free(f);
}

int cdc_file_read_at(CdcFile *f, uint64_t offset, uint8_t *buf, size_t len, size_t *out_n) {
    *out_n = 0;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(f->h, li, NULL, FILE_BEGIN)) return -1;
    size_t total = 0;
    while (total < len) {
        DWORD want = (DWORD)(len - total);
        DWORD read = 0;
        if (!ReadFile(f->h, buf + total, want, &read, NULL)) return -1;
        if (read == 0) break;
        total += read;
    }
    *out_n = total;
    return 0;
}

int cdc_file_read_seq(CdcFile *f, uint8_t *buf, size_t len, size_t *out_n) {
    *out_n = 0;
    size_t total = 0;
    while (total < len) {
        DWORD want = (DWORD)(len - total);
        DWORD read = 0;
        if (!ReadFile(f->h, buf + total, want, &read, NULL)) return -1;
        if (read == 0) break;
        total += read;
    }
    *out_n = total;
    return 0;
}

int cdc_file_identity(CdcFile *f, CdcFileId *out) {
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(f->h, &info)) return 0;
    return info_to_file_id(&info, out);
}

int cdc_query_file_identity(const char *path, CdcFileId *out) {
    wchar_t *w = cdc_utf8_to_wide(path);
    if (!w) return 0;
    HANDLE h = CreateFileW(w, FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return 0;
    BY_HANDLE_FILE_INFORMATION info;
    int ok = 0;
    if (GetFileInformationByHandle(h, &info)) ok = info_to_file_id(&info, out);
    CloseHandle(h);
    return ok;
}

#else /* ! _WIN32 */

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

struct CdcFile {
    int fd;
};

void *cdc_open_drive_hint(char drive_letter) {
    (void)drive_letter;
    return NULL;
}

CdcFile *cdc_file_open_checked(const char *path, uint64_t frn, uint32_t flags, void *hint,
                               int *has_file_id, CdcFileId *file_id_out,
                               uint64_t *actual_size_out) {
    (void)frn;
    (void)hint;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    if (flags & CDC_FILE_FLAG_RANDOM_ACCESS)
        posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);
    else if (flags & CDC_FILE_FLAG_SEQUENTIAL_SCAN)
        posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    struct stat st;
    if (has_file_id) *has_file_id = 0;
    if (fstat(fd, &st) == 0) {
        if (has_file_id && file_id_out && st.st_nlink > 1 && st.st_ino != 0) {
            *has_file_id = 1;
            file_id_out->volume_serial = (uint32_t)st.st_dev;
            file_id_out->file_index = (uint64_t)st.st_ino;
        }
        if (actual_size_out) *actual_size_out = (uint64_t)st.st_size;
    } else if (actual_size_out) {
        *actual_size_out = 0;
    }
    CdcFile *f = malloc(sizeof *f);
    if (!f) {
        close(fd);
        return NULL;
    }
    f->fd = fd;
    return f;
}

void cdc_file_close(CdcFile *f) {
    if (!f) return;
    close(f->fd);
    free(f);
}

int cdc_file_read_at(CdcFile *f, uint64_t offset, uint8_t *buf, size_t len, size_t *out_n) {
    *out_n = 0;
    size_t total = 0;
    while (total < len) {
        ssize_t r = pread(f->fd, buf + total, len - total, (off_t)(offset + total));
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;
        total += (size_t)r;
    }
    *out_n = total;
    return 0;
}

int cdc_file_read_seq(CdcFile *f, uint8_t *buf, size_t len, size_t *out_n) {
    *out_n = 0;
    size_t total = 0;
    while (total < len) {
        ssize_t r = read(f->fd, buf + total, len - total);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;
        total += (size_t)r;
    }
    *out_n = total;
    return 0;
}

int cdc_file_identity(CdcFile *f, CdcFileId *out) {
    struct stat st;
    if (fstat(f->fd, &st) != 0) return 0;
    if (st.st_nlink > 1 && st.st_ino != 0) {
        out->volume_serial = (uint32_t)st.st_dev;
        out->file_index = (uint64_t)st.st_ino;
        return 1;
    }
    return 0;
}

int cdc_query_file_identity(const char *path, CdcFileId *out) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (st.st_nlink > 1 && st.st_ino != 0) {
        out->volume_serial = (uint32_t)st.st_dev;
        out->file_index = (uint64_t)st.st_ino;
        return 1;
    }
    return 0;
}

#endif /* _WIN32 */

/* ---------------- 卷句柄 LCN 合并直读（仅 Windows） ---------------- */

#ifdef _WIN32
static int cmp_raw_target(const void *a, const void *b) {
    uint64_t x = ((const CdcRawHeadTarget *)a)->offset;
    uint64_t y = ((const CdcRawHeadTarget *)b)->offset;
    return (x > y) - (x < y);
}
#endif /* _WIN32 */

size_t cdc_coalesced_raw_volume_heads(char drive_letter, const CdcRawHeadTarget *targets,
                                      size_t n, int is_ssd, volatile int *cancel,
                                      void (*on_tick)(void *), void *tick_ctx,
                                      CdcHeadResult *out) {
#ifdef _WIN32
    if (n == 0) return 0;
    char letter = drive_letter;
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 'a' + 'A');
    char vol_path[8];
    snprintf(vol_path, sizeof vol_path, "\\\\.\\%c:", letter);
    wchar_t *w = cdc_utf8_to_wide(vol_path);
    if (!w) return 0;
    HANDLE vol = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING,
                             is_ssd ? FILE_FLAG_RANDOM_ACCESS : FILE_FLAG_SEQUENTIAL_SCAN,
                             NULL);
    free(w);
    if (vol == INVALID_HANDLE_VALUE) return 0;

    CdcRawHeadTarget *sorted = malloc(n * sizeof(CdcRawHeadTarget));
    if (!sorted) {
        CloseHandle(vol);
        return 0;
    }
    memcpy(sorted, targets, n * sizeof(CdcRawHeadTarget));
    qsort(sorted, n, sizeof(CdcRawHeadTarget), cmp_raw_target);

    uint64_t max_gap = is_ssd ? 16u * 1024 : 256u * 1024;
    uint64_t max_span = is_ssd ? 256u * 1024 : 512u * 1024;
    uint8_t *buf = malloc((size_t)max_span);
    if (!buf) {
        free(sorted);
        CloseHandle(vol);
        return 0;
    }
    size_t n_out = 0;
    size_t i = 0;
    while (i < n) {
        if (is_cancelled(cancel)) break;
        uint64_t start_off = sorted[i].offset;
        uint64_t end_off = start_off + 4096;
        size_t j = i + 1;
        while (j < n) {
            uint64_t next_off = sorted[j].offset;
            uint64_t next_end = next_off + 4096;
            if (next_off <= end_off + max_gap && next_end - start_off <= max_span) {
                if (next_end > end_off) end_off = next_end;
                j++;
            } else {
                break;
            }
        }
        size_t span_len = (size_t)(end_off - start_off);
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)start_off;
        if (SetFilePointerEx(vol, li, NULL, FILE_BEGIN)) {
            size_t got = 0;
            while (got < span_len) {
                DWORD want = (DWORD)(span_len - got);
                DWORD read = 0;
                if (!ReadFile(vol, buf + got, want, &read, NULL) || read == 0) break;
                got += read;
            }
            if (got == span_len) {
                for (size_t k = i; k < j; k++) {
                    size_t rel = (size_t)(sorted[k].offset - start_off);
                    size_t end = rel + sorted[k].want;
                    if (end <= span_len) {
                        cdc_xxh3_buf_to_digest32(buf + rel, sorted[k].want, out[n_out].digest);
                        out[n_out].idx = sorted[k].idx;
                        n_out++;
                        call_tick(on_tick, tick_ctx);
                    }
                }
            }
        }
        i = j;
    }
    free(buf);
    free(sorted);
    CloseHandle(vol);
    return n_out;
#else
    (void)drive_letter;
    (void)targets;
    (void)n;
    (void)is_ssd;
    (void)cancel;
    (void)on_tick;
    (void)tick_ctx;
    (void)out;
    return 0;
#endif
}

/* ---------------- IOCP 批量读头 ---------------- */

#ifdef _WIN32

typedef struct {
    OVERLAPPED ov;
    HANDLE handle;
    size_t item_idx;
    size_t want;
    int active;
    uint8_t buf[4096];
} IocpSlot;

typedef struct {
    HANDLE port;
    IocpSlot *slots;
    const CdcIocpRequest *reqs;
    size_t n_reqs;
    size_t next_req;
    volatile int *cancel;
    void (*on_tick)(void *);
    void *tick_ctx;
    CdcIocpResult *out;
    size_t n_out;
    HANDLE hints[26];
    int hints_tried[26];
} IocpCtx;

/* 1=投递了一路在途 I/O，0=请求耗尽或已取消。 */
static int iocp_submit_next(IocpCtx *ctx, size_t slot_idx) {
    IocpSlot *slot = &ctx->slots[slot_idx];
    while (ctx->next_req < ctx->n_reqs) {
        const CdcIocpRequest *req = &ctx->reqs[ctx->next_req++];
        if (is_cancelled(ctx->cancel)) return 0;
        char drive_ch = req->path && req->path[0] ? req->path[0] : 'C';
        if (drive_ch >= 'a' && drive_ch <= 'z') drive_ch = (char)(drive_ch - 'a' + 'A');
        void *hint = NULL;
        if (req->frn != 0 && drive_ch >= 'A' && drive_ch <= 'Z') {
            int di = drive_ch - 'A';
            if (!ctx->hints_tried[di]) {
                ctx->hints_tried[di] = 1;
                ctx->hints[di] = cdc_open_drive_hint(drive_ch);
            }
            hint = ctx->hints[di];
        }
        HANDLE h = open_raw_handle_by_id_or_path(
            hint, req->frn, req->path, FILE_FLAG_OVERLAPPED | FILE_FLAG_RANDOM_ACCESS);
        if (!h) {
            call_tick(ctx->on_tick, ctx->tick_ctx);
            continue;
        }
        if (req->frn == 0) {
            BY_HANDLE_FILE_INFORMATION info;
            if (GetFileInformationByHandle(h, &info)) {
                if (info.dwFileAttributes & CDC_MFT_SKIP_FILE_ATTRS) {
                    CloseHandle(h);
                    call_tick(ctx->on_tick, ctx->tick_ctx);
                    continue;
                }
                /* 提交时算出的 file_id 被丢弃（Rust 的 let _ = file_id）；
                 * 返回的 file_id 只取完成时的那次查询。 */
            }
        }
        HANDLE bound = CreateIoCompletionPort(h, ctx->port, (ULONG_PTR)slot_idx, 0);
        if (!bound) {
            CloseHandle(h);
            call_tick(ctx->on_tick, ctx->tick_ctx);
            continue;
        }
        size_t want = req->want < 4096 ? req->want : 4096;
        if (want == 0) {
            cdc_xxh3_buf_to_digest32(NULL, 0, ctx->out[ctx->n_out].digest);
            ctx->out[ctx->n_out].idx = req->idx;
            ctx->out[ctx->n_out].has_file_id = 0;
            ctx->n_out++;
            CloseHandle(h);
            call_tick(ctx->on_tick, ctx->tick_ctx);
            continue;
        }
        memset(&slot->ov, 0, sizeof slot->ov);
        slot->handle = h;
        slot->item_idx = req->idx;
        slot->want = want;
        slot->active = 1;
        DWORD dummy = 0;
        if (!ReadFile(h, slot->buf, (DWORD)want, &dummy, &slot->ov)) {
            if (GetLastError() != ERROR_IO_PENDING) {
                slot->active = 0;
                slot->handle = NULL;
                CloseHandle(h);
                call_tick(ctx->on_tick, ctx->tick_ctx);
                continue;
            }
        }
        return 1;
    }
    return 0;
}

size_t cdc_iocp_read_heads(const CdcIocpRequest *reqs, size_t n, size_t concurrency,
                           volatile int *cancel, void (*on_tick)(void *), void *tick_ctx,
                           CdcIocpResult *out) {
    if (n == 0) return 0;
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!port) return 0;
    size_t pool_size = concurrency;
    if (pool_size < 1) pool_size = 1;
    /* 上限 128：NVMe 上 SSD 批放宽到 64 在途仍有余量（见 scan.c 调用处）。 */
    if (pool_size > 128) pool_size = 128;
    if (pool_size > n) pool_size = n;
    IocpSlot *slots = calloc(pool_size, sizeof(IocpSlot));
    size_t *free_slots = malloc(pool_size * sizeof(size_t));
    if (!slots || !free_slots) {
        free(slots);
        free(free_slots);
        CloseHandle(port);
        return 0;
    }
    IocpCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.port = port;
    ctx.slots = slots;
    ctx.reqs = reqs;
    ctx.n_reqs = n;
    ctx.cancel = cancel;
    ctx.on_tick = on_tick;
    ctx.tick_ctx = tick_ctx;
    ctx.out = out;
    size_t n_free = pool_size;
    for (size_t i = 0; i < pool_size; i++) free_slots[i] = pool_size - 1 - i;
    size_t in_flight = 0;
    while (n_free > 0) {
        size_t slot_idx = free_slots[--n_free];
        if (iocp_submit_next(&ctx, slot_idx)) {
            in_flight++;
        } else {
            free_slots[n_free++] = slot_idx;
            break;
        }
    }
    while (in_flight > 0) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED ov = NULL;
        BOOL ok = GetQueuedCompletionStatus(port, &bytes, &key, &ov, INFINITE);
        if (!ov) break;
        size_t slot_idx = (size_t)key;
        IocpSlot *slot = &slots[slot_idx];
        if (ok && bytes > 0) {
            size_t m = (size_t)bytes < slot->want ? (size_t)bytes : slot->want;
            cdc_xxh3_buf_to_digest32(slot->buf, m, ctx.out[ctx.n_out].digest);
            ctx.out[ctx.n_out].idx = slot->item_idx;
            ctx.out[ctx.n_out].has_file_id = 0;
            BY_HANDLE_FILE_INFORMATION info;
            if (GetFileInformationByHandle(slot->handle, &info)) {
                ctx.out[ctx.n_out].has_file_id =
                    info_to_file_id(&info, &ctx.out[ctx.n_out].file_id);
            }
            ctx.n_out++;
        }
        if (slot->handle) {
            CloseHandle(slot->handle);
            slot->handle = NULL;
        }
        slot->active = 0;
        in_flight--;
        call_tick(on_tick, tick_ctx);
        if (!is_cancelled(cancel) && iocp_submit_next(&ctx, slot_idx)) {
            in_flight++;
        } else if (is_cancelled(cancel)) {
            for (size_t s = 0; s < pool_size; s++) {
                if (slots[s].active && slots[s].handle)
                    CancelIoEx(slots[s].handle, NULL);
            }
        }
    }
    size_t done = ctx.n_out;
    for (size_t s = 0; s < pool_size; s++) {
        if (slots[s].handle) CloseHandle(slots[s].handle);
    }
    for (int d = 0; d < 26; d++) {
        if (ctx.hints[d]) CloseHandle(ctx.hints[d]);
    }
    free(slots);
    free(free_slots);
    CloseHandle(port);
    return done;
}

#else /* ! _WIN32 */

/* Linux 等价路径的并行化：每个请求独立（打开、读 ≤4KB、哈希），
 * 用 cdc_par_for 按下标分给 worker；结果先写 out[i]（下标互斥），
 * 再按请求顺序压缩，保证输出顺序与顺序版一致。
 * 每个请求恰好回调一次 on_tick（成功、失败、跳过都算）。 */
typedef struct {
    const CdcIocpRequest *reqs;
    CdcIocpResult *out;
    uint8_t *valid; /* 每请求成功标志（仅 worker i 写 valid[i]） */
    volatile int *cancel;
    void (*on_tick)(void *);
    void *tick_ctx;
} IocpLinuxCtx;

static void iocp_linux_work(void *ctx, size_t i) {
    IocpLinuxCtx *c = (IocpLinuxCtx *)ctx;
    if (is_cancelled(c->cancel)) return; /* 未 tick，与顺序版 break 一致 */
    const CdcIocpRequest *req = &c->reqs[i];
    int has_id = 0;
    CdcFileId fid = {0, 0};
    uint64_t actual = 0;
    CdcFile *f = cdc_file_open_checked(req->path, req->frn,
                                       CDC_FILE_FLAG_OVERLAPPED |
                                           CDC_FILE_FLAG_RANDOM_ACCESS,
                                       NULL, &has_id, &fid, &actual);
    if (!f) {
        call_tick(c->on_tick, c->tick_ctx);
        return;
    }
    size_t want = req->want < 4096 ? req->want : 4096;
    uint8_t buf[4096];
    if (want == 0) {
        cdc_xxh3_buf_to_digest32(NULL, 0, c->out[i].digest);
        c->out[i].idx = req->idx;
        c->out[i].has_file_id = 0;
        c->valid[i] = 1;
    } else {
        size_t got = 0;
        if (cdc_file_read_seq(f, buf, want, &got) == 0 && got > 0) {
            cdc_xxh3_buf_to_digest32(buf, got, c->out[i].digest);
            c->out[i].idx = req->idx;
            c->out[i].has_file_id = has_id;
            c->out[i].file_id = fid;
            c->valid[i] = 1;
        }
    }
    cdc_file_close(f);
    call_tick(c->on_tick, c->tick_ctx);
}

/* calloc 失败时的顺序回退（与改前逻辑逐行一致）。 */
static size_t iocp_linux_sequential(const CdcIocpRequest *reqs, size_t n,
                                    volatile int *cancel, void (*on_tick)(void *),
                                    void *tick_ctx, CdcIocpResult *out) {
    size_t n_out = 0;
    for (size_t i = 0; i < n; i++) {
        if (is_cancelled(cancel)) break;
        const CdcIocpRequest *req = &reqs[i];
        int has_id = 0;
        CdcFileId fid = {0, 0};
        uint64_t actual = 0;
        CdcFile *f = cdc_file_open_checked(req->path, req->frn,
                                           CDC_FILE_FLAG_OVERLAPPED |
                                               CDC_FILE_FLAG_RANDOM_ACCESS,
                                           NULL, &has_id, &fid, &actual);
        if (!f) {
            call_tick(on_tick, tick_ctx);
            continue;
        }
        size_t want = req->want < 4096 ? req->want : 4096;
        uint8_t buf[4096];
        if (want == 0) {
            cdc_xxh3_buf_to_digest32(NULL, 0, out[n_out].digest);
            out[n_out].idx = req->idx;
            out[n_out].has_file_id = 0;
            n_out++;
        } else {
            size_t got = 0;
            if (cdc_file_read_seq(f, buf, want, &got) == 0 && got > 0) {
                cdc_xxh3_buf_to_digest32(buf, got, out[n_out].digest);
                out[n_out].idx = req->idx;
                out[n_out].has_file_id = has_id;
                out[n_out].file_id = fid;
                n_out++;
            }
        }
        cdc_file_close(f);
        call_tick(on_tick, tick_ctx);
    }
    return n_out;
}

size_t cdc_iocp_read_heads(const CdcIocpRequest *reqs, size_t n, size_t concurrency,
                           volatile int *cancel, void (*on_tick)(void *), void *tick_ctx,
                           CdcIocpResult *out) {
    if (n == 0) return 0;
    uint8_t *valid = calloc(n, sizeof(uint8_t));
    if (!valid)
        return iocp_linux_sequential(reqs, n, cancel, on_tick, tick_ctx, out);
    IocpLinuxCtx ctx;
    ctx.reqs = reqs;
    ctx.out = out;
    ctx.valid = valid;
    ctx.cancel = cancel;
    ctx.on_tick = on_tick;
    ctx.tick_ctx = tick_ctx;
    /* 并发数沿用调用方的 SSD/HDD 分档（32/8）；为 0 时退回 CPU 数。 */
    size_t workers = concurrency ? concurrency : cdc_par_cpu_count();
    cdc_par_for(n, workers, iocp_linux_work, &ctx);
    size_t n_out = 0;
    for (size_t i = 0; i < n; i++) {
        if (valid[i]) {
            if (n_out != i) out[n_out] = out[i];
            n_out++;
        }
    }
    free(valid);
    return n_out;
}

#endif /* _WIN32 */

/* ---------------- 磁盘类型探测 ---------------- */

#ifdef _WIN32

int cdc_is_ssd(const char *drive) {
    if (!drive || !drive[0]) return -1;
    char letter = drive[0];
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 'a' + 'A');
    if (letter < 'A' || letter > 'Z') return -1;
    char name[8];
    snprintf(name, sizeof name, "\\\\.\\%c:", letter);
    wchar_t *w = cdc_utf8_to_wide(name);
    if (!w) return -1;
    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, 0, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return -1;
    STORAGE_PROPERTY_QUERY query;
    memset(&query, 0, sizeof query);
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR desc;
    memset(&desc, 0, sizeof desc);
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, (DWORD)sizeof query,
                              &desc, (DWORD)sizeof desc, &ret, NULL);
    CloseHandle(h);
    if (!ok) return -1;
    return desc.IncursSeekPenalty == 0 ? 1 : 0;
}

int cdc_physical_disk_id(const char *drive, uint32_t *dev_type, uint32_t *dev_num) {
    if (!drive || !drive[0]) return -1;
    char letter = drive[0];
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 'a' + 'A');
    if (letter < 'A' || letter > 'Z') return -1;
    char name[8];
    snprintf(name, sizeof name, "\\\\.\\%c:", letter);
    wchar_t *w = cdc_utf8_to_wide(name);
    if (!w) return -1;
    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, 0, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return -1;
    STORAGE_DEVICE_NUMBER info;
    memset(&info, 0, sizeof info);
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &info,
                              (DWORD)sizeof info, &ret, NULL);
    CloseHandle(h);
    if (!ok) return -1;
    *dev_type = info.DeviceType;
    *dev_num = info.DeviceNumber;
    return 0;
}

#else /* ! _WIN32 */

int cdc_is_ssd(const char *drive) {
    (void)drive;
    return -1;
}

int cdc_physical_disk_id(const char *drive, uint32_t *dev_type, uint32_t *dev_num) {
    (void)drive;
    (void)dev_type;
    (void)dev_num;
    return -1;
}

#endif /* _WIN32 */

/* ---------------- 并行（线程池式 parallel-for） ---------------- */
/* 读与哈希的重叠主要靠跨文件并行：worker A 在读盘时 worker B 在哈希，
 * 另有内核 readahead（SEQUENTIAL_SCAN 的 posix_fadvise）做单文件内的
 * 预读。单文件内再开读线程的收益很小（队尾只剩一个大文件时才有），
 * 暂不做；见 scan.c 顶部的说明。
 *
 * 同步原语与 util.c 的日志锁同一模式：Windows 用 SRWLOCK、
 * POSIX 用 pthread_mutex_t，均可静态初始化。
 * 线程入口复用 mft.c 的模式：调用线程也参与干活（par_run），
 * 建线程失败时退化为更少线程，不丢下标、不报错。
 */
#ifdef _WIN32
/* windows.h 已在上面引入。 */
#else
#include <pthread.h>
#include <unistd.h>
#endif

#define CDC_PAR_MAX_THREADS 32
#define CDC_PAR_SSD_MAX_WORKERS 32
#define CDC_PAR_HDD_MAX_WORKERS 4

#ifdef _WIN32
static SRWLOCK g_shared_lock = SRWLOCK_INIT;
#else
static pthread_mutex_t g_shared_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

void cdc_shared_lock(void) {
#ifdef _WIN32
    AcquireSRWLockExclusive(&g_shared_lock);
#else
    pthread_mutex_lock(&g_shared_lock);
#endif
}

void cdc_shared_unlock(void) {
#ifdef _WIN32
    ReleaseSRWLockExclusive(&g_shared_lock);
#else
    pthread_mutex_unlock(&g_shared_lock);
#endif
}

size_t cdc_par_cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? (size_t)si.dwNumberOfProcessors : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (size_t)n : 1;
#endif
}

size_t cdc_par_hash_workers(int is_ssd) {
    size_t ncpu = cdc_par_cpu_count();
    size_t cap = is_ssd ? (size_t)CDC_PAR_SSD_MAX_WORKERS : (size_t)CDC_PAR_HDD_MAX_WORKERS;
    size_t w = ncpu < cap ? ncpu : cap;
    return w < 1 ? 1 : w;
}

typedef struct {
    cdc_par_work_fn fn;
    void *ctx;
    size_t n;
    size_t next; /* 下一个待取下标（互斥锁保护） */
#ifdef _WIN32
    SRWLOCK lock;
#else
    pthread_mutex_t lock;
#endif
} CdcParState;

static void par_state_lock(CdcParState *st) {
#ifdef _WIN32
    AcquireSRWLockExclusive(&st->lock);
#else
    pthread_mutex_lock(&st->lock);
#endif
}

static void par_state_unlock(CdcParState *st) {
#ifdef _WIN32
    ReleaseSRWLockExclusive(&st->lock);
#else
    pthread_mutex_unlock(&st->lock);
#endif
}

/* 取下标、干活，直到取完；调用线程与 worker 线程都跑这个循环。 */
static void par_run(CdcParState *st) {
    for (;;) {
        size_t idx;
        par_state_lock(st);
        if (st->next >= st->n) {
            par_state_unlock(st);
            return;
        }
        idx = st->next++;
        par_state_unlock(st);
        st->fn(st->ctx, idx);
    }
}

#ifdef _WIN32
static DWORD WINAPI par_worker_win(LPVOID p) {
    par_run((CdcParState *)p);
    return 0;
}
#else
static void *par_worker_posix(void *p) {
    par_run((CdcParState *)p);
    return NULL;
}
#endif

int cdc_par_for(size_t n, size_t nthreads, cdc_par_work_fn fn, void *ctx) {
    if (!fn) return -1;
    if (n == 0) return 0;
    if (nthreads <= 1 || n <= 1) {
        for (size_t i = 0; i < n; i++) fn(ctx, i);
        return 0;
    }
    if (nthreads > n) nthreads = n;
    if (nthreads > CDC_PAR_MAX_THREADS) nthreads = CDC_PAR_MAX_THREADS;
    if (nthreads <= 1) {
        for (size_t i = 0; i < n; i++) fn(ctx, i);
        return 0;
    }
    CdcParState st;
    st.fn = fn;
    st.ctx = ctx;
    st.n = n;
    st.next = 0;
#ifdef _WIN32
    InitializeSRWLock(&st.lock);
#else
    pthread_mutex_init(&st.lock, NULL);
#endif
    size_t nworkers = nthreads - 1; /* 其余一个由调用线程承担 */
#ifdef _WIN32
    HANDLE hs[CDC_PAR_MAX_THREADS - 1];
    size_t nspawn = 0;
    for (size_t i = 0; i < nworkers; i++) {
        HANDLE h = CreateThread(NULL, 0, par_worker_win, &st, 0, NULL);
        if (!h) break; /* 建失败：用已建好的线程 + 调用线程跑完 */
        hs[nspawn++] = h;
    }
    par_run(&st);
    if (nspawn > 0) {
        WaitForMultipleObjects((DWORD)nspawn, hs, TRUE, INFINITE);
        for (size_t i = 0; i < nspawn; i++) CloseHandle(hs[i]);
    }
#else
    pthread_t th[CDC_PAR_MAX_THREADS - 1];
    size_t nspawn = 0;
    for (size_t i = 0; i < nworkers; i++) {
        if (pthread_create(&th[nspawn], NULL, par_worker_posix, &st) != 0)
            break; /* 建失败：用已建好的线程 + 调用线程跑完 */
        nspawn++;
    }
    par_run(&st);
    for (size_t i = 0; i < nspawn; i++) pthread_join(th[i], NULL);
#endif
#ifndef _WIN32
    pthread_mutex_destroy(&st.lock);
#endif
    return 0;
}

