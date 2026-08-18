#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace oracle_stone_locator {

// Oracle stone states mirrored from the Anomaly SDK additions. They are local
// to this plugin so the original upstream SDK remains untouched.
enum OracleStoneState : std::uint32_t {
    OracleStoneUnknown = 0,
    OracleStoneLocked = 1,
    OracleStoneAvailable = 2,
    OracleStoneCollected = 3,
};

enum OracleStoneMarkerState : std::uint32_t {
    MarkersUnavailable = 0,
    MarkersDisabled = 1,
    MarkersSyncing = 2,
    MarkersActive = 3,
};

struct OracleStoneSnapshot {
    std::uint64_t sequence{};
    std::uint32_t state{OracleStoneUnknown};
    std::int32_t config_index{};
    std::int32_t floor{};
    std::uint32_t flags{};
    double world_position[3]{};
    char oracle_stone_id[128]{};
    char level[512]{};
};

[[nodiscard]] constexpr std::wstring_view TreasureboxDataAssetPath() noexcept {
    return L"/Game/DataAssets/TreasureboxDataAsset.TreasureboxDataAsset";
}

[[nodiscard]] constexpr std::uint8_t OracleMarkerIconInfoType() noexcept {
    return 1; // EMapIconInfoType::MapIcons
}

struct OracleMarkerContract {
    std::string_view map_icon_id;
    std::uint8_t icon_type{};
    std::uint8_t transform_type{};
    std::uint8_t icon_info_type{};
    std::uint8_t icon_state{};
    bool populate_map_context{};
    bool restrict_show_types{};
    bool restrict_show_modes{};
    bool only_show_in_belongs_floor{};
    bool under_fog{};
};

[[nodiscard]] constexpr OracleMarkerContract DivinationCrystalOracleMarkerContract() noexcept {
    return {
        "CrystalOracle",
        1, // EMiniMapIconType::Normal
        1, // EMiniMapIconTransformType::Transform
        OracleMarkerIconInfoType(),
        1, // EMiniMapIconState::Normal
        false,
        false,
        false,
        false,
        false,
    };
}

enum class CatalogScanStage : std::uint8_t {
    NotAttempted,
    Ready,
    BindingParameterSizes,
    BindingStateQuery,
    BindingStateContext,
    BindingAddMapIconFunction,
    BindingAddMapIconParameters,
    BindingAddParamsFields,
    BindingAddParamsArrays,
    BindingTransformInfo,
    BindingTransformFields,
    BindingTransformComponents,
    BindingTransformOffsets,
    BindingRemoveMapIconFunction,
    BindingRemoveMapIconParameters,
    BindingStringLeftFunction,
    BindingStringLeftParameters,
    BindingOracleIcon,
    BindingEmptyText,
    BindingTextIsEmptyFunction,
    BindingTextIsEmptyParameters,
    BindingTextIsEmptyInvoke,
    CatalogTreasureAsset,
    CatalogTable,
    CatalogRowStruct,
    CatalogRowFields,
    CatalogRowLocation,
    CatalogRowMapHeader,
    CatalogRowMapFlags,
    CatalogRowMapLayout,
    CatalogRowMapElements,
    CatalogRowData,
    CatalogRowRecord,
    CatalogEmpty,
    Exception,
};

[[nodiscard]] constexpr std::string_view CatalogScanStageName(
    const CatalogScanStage stage) noexcept {
    switch (stage) {
    case CatalogScanStage::NotAttempted: return "not-attempted";
    case CatalogScanStage::Ready: return "ready";
    case CatalogScanStage::BindingParameterSizes: return "binding.parameter-sizes";
    case CatalogScanStage::BindingStateQuery: return "binding.oracle-state.query";
    case CatalogScanStage::BindingStateContext: return "binding.oracle-state.context";
    case CatalogScanStage::BindingAddMapIconFunction: return "binding.add-map-icon.function";
    case CatalogScanStage::BindingAddMapIconParameters:
        return "binding.add-map-icon.parameters";
    case CatalogScanStage::BindingAddParamsFields: return "binding.add-params.fields";
    case CatalogScanStage::BindingAddParamsArrays: return "binding.add-params.arrays";
    case CatalogScanStage::BindingTransformInfo: return "binding.transform-info";
    case CatalogScanStage::BindingTransformFields: return "binding.transform.fields";
    case CatalogScanStage::BindingTransformComponents:
        return "binding.transform.components";
    case CatalogScanStage::BindingTransformOffsets: return "binding.transform.offsets";
    case CatalogScanStage::BindingRemoveMapIconFunction:
        return "binding.remove-map-icon.function";
    case CatalogScanStage::BindingRemoveMapIconParameters:
        return "binding.remove-map-icon.parameters";
    case CatalogScanStage::BindingStringLeftFunction: return "binding.string-left.function";
    case CatalogScanStage::BindingStringLeftParameters:
        return "binding.string-left.parameters";
    case CatalogScanStage::BindingOracleIcon: return "binding.oracle-icon";
    case CatalogScanStage::BindingEmptyText: return "binding.empty-text";
    case CatalogScanStage::BindingTextIsEmptyFunction:
        return "binding.text-is-empty.function";
    case CatalogScanStage::BindingTextIsEmptyParameters:
        return "binding.text-is-empty.parameters";
    case CatalogScanStage::BindingTextIsEmptyInvoke: return "binding.text-is-empty.invoke";
    case CatalogScanStage::CatalogTreasureAsset: return "catalog.treasure-asset";
    case CatalogScanStage::CatalogTable: return "catalog.table";
    case CatalogScanStage::CatalogRowStruct: return "catalog.row-struct";
    case CatalogScanStage::CatalogRowFields: return "catalog.row.fields";
    case CatalogScanStage::CatalogRowLocation: return "catalog.row.location";
    case CatalogScanStage::CatalogRowMapHeader: return "catalog.row-map.header";
    case CatalogScanStage::CatalogRowMapFlags: return "catalog.row-map.flags";
    case CatalogScanStage::CatalogRowMapLayout: return "catalog.row-map.layout";
    case CatalogScanStage::CatalogRowMapElements: return "catalog.row-map.elements";
    case CatalogScanStage::CatalogRowData: return "catalog.row.data";
    case CatalogScanStage::CatalogRowRecord: return "catalog.row.record";
    case CatalogScanStage::CatalogEmpty: return "catalog.empty";
    case CatalogScanStage::Exception: return "exception";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::uint32_t TranslateOracleStoneState(
    const std::int32_t native_state) noexcept {
    switch (native_state) {
    case 0: return OracleStoneAvailable;
    case 2: return OracleStoneCollected;
    default: return OracleStoneUnknown;
    }
}

[[nodiscard]] constexpr bool IsOracleIconMapPropertyShape(
    const std::string_view key_type,
    const std::int32_t key_array_dim,
    const std::int32_t key_size,
    const std::string_view key_enum,
    const std::string_view key_underlying_type,
    const std::string_view value_type,
    const std::int32_t value_array_dim,
    const std::int32_t value_size) noexcept {
    return key_type == "EnumProperty" && key_array_dim == 1 && key_size == 1 &&
        key_enum == "EAzimuthTreasureBoxType" &&
        key_underlying_type == "ByteProperty" && value_type == "NameProperty" &&
        value_array_dim == 1 && value_size == static_cast<std::int32_t>(sizeof(std::uint64_t));
}

[[nodiscard]] constexpr std::optional<std::size_t> FindOracleIconSlot(
    const std::span<const std::uint8_t> elements,
    const std::span<const std::uint32_t> allocation_flags,
    const std::size_t slot_count,
    const std::size_t element_stride,
    const std::size_t key_offset,
    const std::uint8_t oracle_key) noexcept {
    if (element_stride == 0 || key_offset >= element_stride ||
        slot_count > (std::numeric_limits<std::size_t>::max)() / element_stride ||
        elements.size() < slot_count * element_stride ||
        slot_count > (std::numeric_limits<std::size_t>::max)() - 31U ||
        allocation_flags.size() < (slot_count + 31U) / 32U) {
        return std::nullopt;
    }
    std::optional<std::size_t> result;
    for (std::size_t index{}; index < slot_count; ++index) {
        const auto mask = std::uint32_t{1} << (index & 31U);
        if ((allocation_flags[index / 32U] & mask) == 0 ||
            elements[index * element_stride + key_offset] != oracle_key) {
            continue;
        }
        if (result) return std::nullopt;
        result = index;
    }
    return result;
}

[[nodiscard]] inline std::optional<std::uint64_t> FindOracleIconValue(
    const std::span<const std::uint8_t> elements,
    const std::span<const std::uint32_t> allocation_flags,
    const std::size_t slot_count,
    const std::size_t element_stride,
    const std::size_t key_offset,
    const std::size_t value_offset,
    const std::uint8_t oracle_key) noexcept {
    if (element_stride < sizeof(std::uint64_t) ||
        value_offset > element_stride - sizeof(std::uint64_t)) {
        return std::nullopt;
    }
    const auto slot = FindOracleIconSlot(
        elements, allocation_flags, slot_count, element_stride, key_offset, oracle_key);
    if (!slot) return std::nullopt;
    std::uint64_t value{};
    std::memcpy(&value,
        elements.data() + *slot * element_stride + value_offset, sizeof(value));
    return value;
}

[[nodiscard]] constexpr bool ReadPackedBool(
    const std::uint8_t storage,
    const std::uint8_t byte_mask) noexcept {
    return (storage & byte_mask) != 0;
}

[[nodiscard]] constexpr std::uint8_t WritePackedBool(
    const std::uint8_t storage,
    const std::uint8_t field_mask,
    const std::uint8_t byte_mask,
    const bool value) noexcept {
    return static_cast<std::uint8_t>(
        (storage & static_cast<std::uint8_t>(~field_mask)) |
        (value ? byte_mask : 0U));
}

} // namespace oracle_stone_locator
