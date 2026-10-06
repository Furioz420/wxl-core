// String accessor companion for wxl.db2 v1 opaque table/row handles.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#ifndef WXL_DB2_STRING_API_H
#define WXL_DB2_STRING_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_DB2_STRING_API_VERSION 1

typedef struct WXL_Db2StringApi
{
    uint32_t structSize;
    uint32_t apiVersion;

    /**
     * Returns a table-owned, null-terminated UTF-8 string. The pointer remains
     * valid until the table handle is released. Empty/missing values return "".
     */
    const char*(__cdecl* String)(void* table, const void* row,
                                 const char* field, uint32_t element);
} WXL_Db2StringApi;

#ifdef __cplusplus
}
#endif

#endif // WXL_DB2_STRING_API_H
