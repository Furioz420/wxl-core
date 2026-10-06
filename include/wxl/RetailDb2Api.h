// Eunoia retail-item graph service published by wxl-db2 and consumed by wxl-modern-m2.
// The ABI deliberately exposes only POD values and opaque snapshot leases: all STL ownership stays
// inside the extension that created it.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#ifndef WXL_RETAIL_DB2_API_H
#define WXL_RETAIL_DB2_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_RETAIL_DB2_API_VERSION 1

typedef struct WXL_RetailItemInfo
{
    uint32_t classId;
    uint32_t subclassId;
    uint32_t soundOverride;
    uint32_t itemGroupSoundsId;
    uint32_t material;
    uint32_t inventoryType;
    uint32_t sheatheType;
    uint32_t displayId;
    uint32_t appearanceId;
    uint32_t iconFileDataId;
} WXL_RetailItemInfo;

typedef struct WXL_RetailItemVariant
{
    uint32_t displayId;
    uint32_t appearanceId;
    uint32_t iconFileDataId;
} WXL_RetailItemVariant;

typedef struct WXL_RetailGeosetFilter
{
    uint16_t ids[16];
    uint32_t count;
} WXL_RetailGeosetFilter;

typedef struct WXL_RetailModelEntry
{
    uint32_t modelSlot;
    uint32_t attachId;
    uint32_t modelIndex;
    uint32_t raceId;
    uint32_t genderId;
    uint32_t modelFlags;
    uint32_t textureFlags;
    const char* folder;
    const char* model;
    const char* texture;
    WXL_RetailGeosetFilter geoFilter;
} WXL_RetailModelEntry;

typedef struct WXL_RetailMaterialEntry
{
    uint32_t modelIndex;
    uint32_t modelColumn;
    uint32_t layer;
    uint32_t textureType;
    uint32_t raceId;
    uint32_t genderId;
    const char* folder;
    const char* model;
    const char* texture;
    const char* skinSectionIds;
    const char* batchIndexes;
    const char* targetSkinSectionIds;
    const char* targetBatchIndexes;
    const char* targetMode;
} WXL_RetailMaterialEntry;

typedef struct WXL_RetailDisplayRecord
{
    uint32_t inventoryType;
    uint32_t flags;
    uint32_t itemVisual;
    uint32_t particleColor;
    const char* nativeModelNames[2];
    const char* nativeModelTextures[2];
    uint32_t geosets[6];
    uint32_t helmetVis[2];
    const char* componentTextures[8];
} WXL_RetailDisplayRecord;

typedef struct WXL_RetailHelmetGeosetRule
{
    uint32_t raceId;
    uint32_t hideGroup;
    uint32_t raceBitSelection;
    uint32_t flags;
} WXL_RetailHelmetGeosetRule;

typedef struct WXL_RetailDb2Api
{
    uint32_t structSize;
    uint32_t apiVersion;

    /// Reports whether WXL_DB2_RETAIL_ITEMS enabled the catalog/index worker.
    int(__cdecl* Enabled)(void);

    /// Catalog snapshots are immutable. Release every non-null handle in the publishing DLL.
    void*(__cdecl* AcquireCatalog)(void);
    void(__cdecl* ReleaseCatalog)(void* catalog);
    uint32_t(__cdecl* CatalogItemCount)(void* catalog);
    int(__cdecl* CatalogItemAt)(void* catalog, uint32_t index, uint32_t* itemId,
                               WXL_RetailItemInfo* out);
    uint32_t(__cdecl* CatalogVariantCount)(void* catalog);
    int(__cdecl* CatalogVariantAt)(void* catalog, uint32_t index, uint64_t* key,
                                  WXL_RetailItemVariant* out);
    uint32_t(__cdecl* CatalogIconCount)(void* catalog);
    int(__cdecl* CatalogIconAt)(void* catalog, uint32_t index, uint32_t* displayId,
                               uint32_t* fileDataId);
    uint32_t(__cdecl* CatalogSoundCount)(void* catalog);
    int(__cdecl* CatalogSoundAt)(void* catalog, uint32_t index, uint32_t* displayId,
                                uint32_t* soundId);
    uint32_t(__cdecl* CatalogDisplayCount)(void* catalog);
    int(__cdecl* CatalogDisplayAt)(void* catalog, uint32_t index, uint32_t* displayId);

    /// Queues demand resolution; the index generation changes after a new snapshot is published.
    void(__cdecl* RequestDisplays)(const uint32_t* displayIds, uint32_t count);
    uint64_t(__cdecl* IndexGeneration)(void);
    void*(__cdecl* AcquireIndex)(void);
    void(__cdecl* ReleaseIndex)(void* index);
    int(__cdecl* IndexModelsReady)(void* index);
    int(__cdecl* IndexMaterialsReady)(void* index);
    uint32_t(__cdecl* IndexResolvedDisplayCount)(void* index);
    int(__cdecl* IndexResolvedDisplayAt)(void* index, uint32_t position, uint32_t* displayId);
    int(__cdecl* IndexDisplayRecord)(void* index, uint32_t displayId,
                                    WXL_RetailDisplayRecord* out);
    uint32_t(__cdecl* IndexModelCount)(void* index, uint32_t displayId);
    int(__cdecl* IndexModelAt)(void* index, uint32_t displayId, uint32_t position,
                              WXL_RetailModelEntry* out);
    uint32_t(__cdecl* IndexMaterialCount)(void* index, uint32_t displayId);
    int(__cdecl* IndexMaterialAt)(void* index, uint32_t displayId, uint32_t position,
                                 WXL_RetailMaterialEntry* out);
    uint32_t(__cdecl* IndexHelmetRuleCount)(void* index, uint32_t visibilityId);
    int(__cdecl* IndexHelmetRuleAt)(void* index, uint32_t visibilityId, uint32_t position,
                                   WXL_RetailHelmetGeosetRule* out);
    int(__cdecl* IndexHelmetAnimScale)(void* index, uint32_t visibilityId, uint32_t raceId,
                                      float* out);
} WXL_RetailDb2Api;

#ifdef __cplusplus
}
#endif

#endif // WXL_RETAIL_DB2_API_H
