// Optional filtered-load companion for wxl.db2. Kept separate from Db2Api.h so the live v1
// service remains ABI compatible with every stock Hub extension.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#ifndef WXL_DB2_FILTER_API_H
#define WXL_DB2_FILTER_API_H

#include "wxl/Db2Api.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_DB2_FILTER_API_VERSION 1

#define WXL_DB2_FILTER_ROW_ID    0
#define WXL_DB2_FILTER_PARENT_ID 1
#define WXL_DB2_FILTER_FIELD     2

typedef struct WXL_Db2Filter
{
    uint32_t        source;       ///< one of WXL_DB2_FILTER_*
    const char*     field;        ///< required only for WXL_DB2_FILTER_FIELD
    uint16_t        element;
    const uint32_t* values;
    uint32_t        valueCount;
} WXL_Db2Filter;

typedef struct WXL_Db2FilterApi
{
    uint32_t structSize;
    uint32_t apiVersion;

    /**
     * Decodes only rows selected by @p filter. The returned handle is consumed through the
     * ordinary wxl.db2 v1 API and released through WXL_Db2Api::Release.
     */
    void*(__cdecl* LoadFiltered)(const WXL_Db2Definition* definition,
                                const WXL_Db2Filter* filter,
                                char* errorBuf,
                                size_t errorBufSize);
} WXL_Db2FilterApi;

#ifdef __cplusplus
}
#endif

#endif // WXL_DB2_FILTER_API_H
