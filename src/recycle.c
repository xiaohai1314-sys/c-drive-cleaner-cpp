/* recycle.c — 移植自 Rust 版 desktop/src/recycle.rs。 */
#include "recycle.h"

#include <stdlib.h>
#include <string.h>

#include "safety.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/stat.h>
#endif

int cdc_send_to_recycle(const char **paths, size_t npaths, char ***out_failed,
                        size_t *out_nfailed, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!out_failed || !out_nfailed) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "NULL out param");
        return CDC_ERR_INVALID_ARG;
    }
    *out_failed = NULL;
    *out_nfailed = 0;
    cdc_vec_t failed;
    if (cdc_vec_init(&failed, sizeof(char *)) != CDC_OK) {
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    cdc_vec_t eligible;
    if (cdc_vec_init(&eligible, sizeof(char *)) != CDC_OK) {
        cdc_vec_free(&failed);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    for (size_t i = 0; i < npaths; i++) {
        const char *path = paths[i];
        int is_file = 0;
#ifdef _WIN32
        wchar_t *w = cdc_utf8_to_wide(path);
        if (w) {
            DWORD a = GetFileAttributesW(w);
            is_file = a != INVALID_FILE_ATTRIBUTES &&
                      (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
            free(w);
        }
#else
        struct stat st;
        is_file = path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
        char *dup = strdup(path ? path : "");
        if (!dup) continue;
        if (!is_file || cdc_explicit_delete_forbidden(path)) {
            if (cdc_vec_push(&failed, &dup) != CDC_OK) free(dup);
        } else {
            if (cdc_vec_push(&eligible, &dup) != CDC_OK) free(dup);
        }
    }
#ifdef _WIN32
    /* 每批 40 个：双 NUL 结尾的多串缓冲。 */
    for (size_t base = 0; base < eligible.len; base += 40) {
        size_t chunk_n = eligible.len - base < 40 ? eligible.len - base : 40;
        cdc_vec_t buf; /* wchar_t */
        if (cdc_vec_init(&buf, sizeof(wchar_t)) != CDC_OK) break;
        int bad = 0;
        for (size_t i = 0; i < chunk_n; i++) {
            const char *p = ((char **)eligible.data)[base + i];
            wchar_t *w = cdc_utf8_to_wide(p);
            if (!w) {
                bad = 1;
                break;
            }
            size_t wl = wcslen(w);
            for (size_t k = 0; k <= wl; k++) {
                if (cdc_vec_push(&buf, &w[k]) != CDC_OK) {
                    bad = 1;
                    break;
                }
            }
            free(w);
            if (bad) break;
        }
        if (!bad) {
            wchar_t zero = 0;
            if (cdc_vec_push(&buf, &zero) != CDC_OK) bad = 1;
        }
        if (bad) {
            /* 本批转换失败：整批计入失败（与 Rust 整批失败语义一致）。 */
            for (size_t i = 0; i < chunk_n; i++) {
                char *dup = strdup(((char **)eligible.data)[base + i]);
                if (dup && cdc_vec_push(&failed, &dup) != CDC_OK) free(dup);
            }
            cdc_vec_free(&buf);
            continue;
        }
        SHFILEOPSTRUCTW op;
        memset(&op, 0, sizeof(op));
        op.hwnd = NULL;
        op.wFunc = FO_DELETE;
        op.pFrom = (const wchar_t *)buf.data;
        op.pTo = NULL;
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI;
        int code = SHFileOperationW(&op);
        if (code != 0 || op.fAnyOperationsAborted) {
            for (size_t i = 0; i < chunk_n; i++) {
                char *dup = strdup(((char **)eligible.data)[base + i]);
                if (dup && cdc_vec_push(&failed, &dup) != CDC_OK) free(dup);
            }
        }
        cdc_vec_free(&buf);
    }
#else
    /* Linux 下没有回收站 API：全部计入失败，不做永久删除。 */
    for (size_t i = 0; i < eligible.len; i++) {
        char *dup = ((char **)eligible.data)[i];
        if (cdc_vec_push(&failed, &dup) != CDC_OK) free(dup);
    }
    /* 字符串指针已移交 failed；cdc_vec_free 只释放指针数组本身，不碰
     * 元素（字符串现归 failed 所有）。注意：不能只写
     * eligible.data = NULL（会丢掉指针数组的堆分配，ASan 报 128 字节泄漏）。 */
    cdc_vec_free(&eligible);
#endif
    for (size_t i = 0; i < eligible.len; i++) free(((char **)eligible.data)[i]);
    cdc_vec_free(&eligible);
    *out_failed = (char **)failed.data;
    *out_nfailed = failed.len;
    failed.data = NULL;
    failed.len = failed.cap = 0;
    cdc_vec_free(&failed);
    return CDC_OK;
}
