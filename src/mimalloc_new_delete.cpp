// mimalloc_new_delete.cpp — 把全局 operator new/delete 替换为 mimalloc。
//
// 只在链接主程序 / 测试时作为独立对象加入（不进 libcdc.a，避免被链接器
// 丢弃）。std::string / std::vector 等容器的分配因此与 C 第三方库
//（sqlite3 / cJSON 通过 -include 宏替换）共用同一个分配器。
// CDC_MIMALLOC=0 时不链接本文件。
#include <mimalloc-new-delete.h>
