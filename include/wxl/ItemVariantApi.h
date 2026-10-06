// Optional item-variant/presentation service used by model-facing extensions.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#ifndef WXL_ITEM_VARIANT_API_H
#define WXL_ITEM_VARIANT_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_ITEM_VARIANT_API_VERSION 1

typedef struct WXL_ItemVariantApi
{
    uint32_t structSize;
    uint32_t apiVersion;
    uint32_t(__cdecl* PresentationSourceForItem)(uint32_t itemId);
    uint32_t(__cdecl* VisibleModifierForItem)(uint32_t itemId);
    int(__cdecl* EquippedVariant)(uint32_t equipmentSlot,
                                 uint32_t* itemId, uint32_t* modifierId);
    uint64_t(__cdecl* VariantGeneration)(void);
} WXL_ItemVariantApi;

#ifdef __cplusplus
}
#endif

#endif // WXL_ITEM_VARIANT_API_H
