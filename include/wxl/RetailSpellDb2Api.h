// Optional retail spell-visual data service published by wxl-db2.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#ifndef WXL_RETAIL_SPELL_DB2_API_H
#define WXL_RETAIL_SPELL_DB2_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_RETAIL_SPELL_DB2_API_VERSION 2

enum WXL_RetailSpellModelRole
{
    WXL_RETAIL_SPELL_MODEL_ATTACH = 0,
    WXL_RETAIL_SPELL_MODEL_MISSILE = 1,
    WXL_RETAIL_SPELL_MODEL_AREA = 2,
};

typedef struct WXL_RetailSpellModel
{
    uint32_t fileDataId;
    float    scale;
    int32_t  attachmentId;
    uint32_t positionerId;
    uint32_t spellVisualKitId;
    uint32_t startEvent;
    uint32_t endEvent;
    uint32_t targetType;
    uint32_t role;
} WXL_RetailSpellModel;

typedef struct WXL_RetailSpellDb2Api
{
    uint32_t structSize;
    uint32_t apiVersion;

    /// Reports whether retail spell-table parsing is enabled in wxl-db2.cfg.
    int(__cdecl* Enabled)(void);

    /**
     * Resolves one spell through SpellXSpellVisual -> kits -> model attachments.
     * The returned opaque lease is owned and released by wxl-db2.
     */
    void*(__cdecl* Acquire)(uint32_t spellId);
    void(__cdecl* Release)(void* lease);
    uint32_t(__cdecl* ModelCount)(void* lease);
    int(__cdecl* ModelAt)(void* lease, uint32_t index, WXL_RetailSpellModel* out);
    const char*(__cdecl* Error)(void* lease);
} WXL_RetailSpellDb2Api;

#ifdef __cplusplus
}
#endif

#endif // WXL_RETAIL_SPELL_DB2_API_H
