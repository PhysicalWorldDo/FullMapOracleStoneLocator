#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/interop.h"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/ue5.h"
#include "plugins/common/localization.hpp"

#include "oracle_stone_profile.hpp"
#include "oracle_stone_runtime.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace oracle_stone_locator_profile;
using oracle_stone_locator::CatalogScanStage;
using oracle_stone_locator::DivinationCrystalOracleMarkerContract;
using oracle_stone_locator::OracleMarkerContract;
using oracle_stone_locator::OracleStoneAvailable;
using oracle_stone_locator::OracleStoneCollected;
using oracle_stone_locator::OracleStoneSnapshot;
using oracle_stone_locator::OracleStoneUnknown;
using oracle_stone_locator::TranslateOracleStoneState;

constexpr std::size_t kMaximumNameBytes = 1024;
constexpr std::size_t kMaximumOracleStones = 4096;
// Per-tick budgets matching the validated host implementation.
constexpr std::size_t kMaximumStateQueriesPerTick = 64;
constexpr std::size_t kMaximumAddsPerTick = 16;
constexpr std::size_t kMaximumRemovesPerTick = 32;
constexpr std::uint32_t kMaximumObjectCount = 16U * 1024U * 1024U;
constexpr std::uint32_t kMaximumObjectChunks = 4096;

struct FNameValue {
    std::uint32_t comparison_index{};
    std::uint32_t number{};
};

struct NativeUtf16StringHeader {
    wchar_t* data{};
    std::int32_t count{};
    std::int32_t capacity{};
};

struct ObjectRegistry {
    std::uintptr_t items{};
    std::uint32_t count{};
    std::uint32_t max_count{};
    std::uint32_t max_chunks{};
    std::uint32_t num_chunks{};
};

struct OracleStoneRecord {
    FNameValue id_name{};
    std::string oracle_stone_id;
    std::string level;
    std::int32_t floor{};
    double world_position[3]{};
    std::uint32_t state{OracleStoneUnknown};
    bool map_explore{};
};

struct Context {
    const AnomalyHostApiV1* host{};
    anomaly::plugins::Localizer localizer;
    const AnomalyCoreServiceV1* core{};
    const AnomalySignatureServiceV1* signature{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyUe5FrameworkServiceV1* framework{};
    const AnomalyNteSessionServiceV1* session{};
    const AnomalyNtePlayerServiceV1* player{};

    std::uintptr_t g_world_address{};
    std::uintptr_t g_objects_address{};
    std::uintptr_t process_event{};
    std::uintptr_t state_query{};
    ObjectRegistry registry{};
    std::uintptr_t treasure_asset{};
    std::uintptr_t data_table{};
    std::uintptr_t player_state{};

    std::uintptr_t add_icon_function{};
    std::uintptr_t remove_icon_function{};
    std::uintptr_t string_to_name_function{};
    std::uintptr_t string_library_receiver{};
    FNameValue map_icon_id{};
    std::array<std::uint8_t, 16> empty_text{};
    bool binding_ready{};

    std::mutex mutex;
    std::vector<OracleStoneRecord> records;
    std::vector<std::string> marker_ids;
    bool scan_attempted{};
    bool scan_ready{};
    bool markers_enabled{true};
    int window_open{1};
    std::atomic<CatalogScanStage> scan_stage{CatalogScanStage::NotAttempted};
    // Throttling state for the state refresh sweep (mirrors the host's
    // batched refresh with a pause between full sweeps).
    std::uint64_t update_sequence{};
    std::uint64_t next_state_refresh_sequence{};
    std::size_t state_refresh_cursor{};
};

Context g_context;

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

template <typename Service>
const Service* Query(const AnomalyHostApiV1* host,
                     const std::string_view id,
                     const std::uint32_t version) noexcept {
    if (host == nullptr ||
        !HasField<AnomalyHostApiV1, decltype(AnomalyHostApiV1::query_service)>(
            host, offsetof(AnomalyHostApiV1, query_service)) ||
        host->query_service == nullptr) return nullptr;
    const void* table{};
    if (host->query_service(host->host_context, anomaly::sdk::StringView(id),
                            version, &table).code != ANOMALY_STATUS_V1_OK ||
        table == nullptr) return nullptr;
    const auto* service = static_cast<const Service*>(table);
    constexpr std::size_t prefix = offsetof(Service, user) + sizeof(void*);
    return service->struct_size >= prefix && service->service_version >= version
               ? service : nullptr;
}

bool CoreReady(const AnomalyCoreServiceV1* core) noexcept {
    return HasField<AnomalyCoreServiceV1,
                    decltype(AnomalyCoreServiceV1::read_memory)>(
               core, offsetof(AnomalyCoreServiceV1, read_memory)) &&
           core->read_memory != nullptr;
}

bool SignatureReady(const AnomalySignatureServiceV1* signature) noexcept {
    return HasField<AnomalySignatureServiceV1,
                    decltype(AnomalySignatureServiceV1::resolve)>(
               signature, offsetof(AnomalySignatureServiceV1, resolve)) &&
           signature->resolve != nullptr;
}

bool NamesReady(const AnomalyUe5NamesServiceV1* names) noexcept {
    return HasField<AnomalyUe5NamesServiceV1,
                    decltype(AnomalyUe5NamesServiceV1::resolve_utf8)>(
               names, offsetof(AnomalyUe5NamesServiceV1, resolve_utf8)) &&
           names->resolve_utf8 != nullptr;
}

bool ObjectsReady(const AnomalyUe5ObjectsServiceV1* objects) noexcept {
    return HasField<AnomalyUe5ObjectsServiceV1,
                    decltype(AnomalyUe5ObjectsServiceV1::find_exact)>(
               objects, offsetof(AnomalyUe5ObjectsServiceV1, find_exact)) &&
           objects->count != nullptr && objects->find_exact != nullptr;
}

bool Read(Context& context, const std::uintptr_t address, void* value,
          const std::size_t size) noexcept {
    if (!CoreReady(context.core) || address == 0 || value == nullptr || size == 0)
        return false;
    AnomalyMutableByteSpanV1 destination{
        static_cast<std::uint8_t*>(value), size};
    return context.core->read_memory(
               context.core->user, address, destination).code == ANOMALY_STATUS_V1_OK;
}

template <typename T>
bool Read(Context& context, const std::uintptr_t address, T& value) noexcept {
    return Read(context, address, &value, sizeof(value));
}

bool AddSignedAddress(const std::uintptr_t base, const std::ptrdiff_t offset,
                      std::uintptr_t& result) noexcept {
    if (offset < 0) {
        const auto magnitude = static_cast<std::uintptr_t>(-(offset + 1)) + 1U;
        if (base <= magnitude) return false;
        result = base - magnitude;
        return true;
    }
    const auto unsigned_offset = static_cast<std::uintptr_t>(offset);
    if (base > (std::numeric_limits<std::uintptr_t>::max)() - unsigned_offset)
        return false;
    result = base + unsigned_offset;
    return true;
}

bool ReadPointerAt(Context& context, const std::uintptr_t base,
                   const std::ptrdiff_t offset, std::uintptr_t& value) noexcept {
    std::uintptr_t address{};
    return AddSignedAddress(base, offset, address) && Read(context, address, value) &&
           value != 0;
}

bool ResolveSignature(Context& context, const std::string_view pattern,
                      std::uintptr_t& address) noexcept {
    address = 0;
    return SignatureReady(context.signature) &&
           context.signature->resolve(
               context.signature->user, anomaly::sdk::StringView(kModuleName),
               anomaly::sdk::StringView(kTextSection), anomaly::sdk::StringView(pattern),
               &address).code == ANOMALY_STATUS_V1_OK && address != 0;
}

bool ResolveRipRelative(Context& context, const std::string_view pattern,
                        const std::ptrdiff_t addend,
                        std::uintptr_t& address) noexcept {
    std::uintptr_t instruction{};
    if (!ResolveSignature(context, pattern, instruction)) return false;
    std::int32_t displacement{};
    std::uintptr_t displacement_address{};
    if (!AddSignedAddress(instruction, kRipDisplacementOffset, displacement_address) ||
        !Read(context, displacement_address, displacement)) return false;
    const auto resolved = static_cast<std::intptr_t>(instruction) +
        static_cast<std::intptr_t>(kRipInstructionSize) + displacement;
    return resolved > 0 && AddSignedAddress(static_cast<std::uintptr_t>(resolved),
                                            addend, address);
}

std::string ResolveName(Context& context, const std::uint32_t name_id) {
    if (!NamesReady(context.names) || name_id == 0) return {};
    std::array<char, 128> local{};
    std::size_t size = local.size();
    AnomalyStatusV1 status = context.names->resolve_utf8(
        context.names->user, name_id, local.data(), &size);
    if (status.code == ANOMALY_STATUS_V1_OK && size > 1 && size <= local.size())
        return std::string(local.data(), size - 1U);
    if (status.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL || size <= 1 ||
        size > kMaximumNameBytes) return {};
    std::string value(size, '\0');
    status = context.names->resolve_utf8(
        context.names->user, name_id, value.data(), &size);
    if (status.code != ANOMALY_STATUS_V1_OK || size <= 1 || size > value.size())
        return {};
    value.resize(size - 1U);
    return value;
}

std::string RenderFName(Context& context, const FNameValue value) {
    std::string result = ResolveName(context, value.comparison_index);
    if (result.empty() || value.number == 0) return result;
    result.push_back('_');
    result += std::to_string(value.number - 1U);
    return result;
}

bool ReadUtf16CString(Context& context, const std::uintptr_t address,
                      std::string& result) {
    result.clear();
    if (address == 0) return false;
    std::array<char16_t, 256> value{};
    for (std::size_t offset{}; offset < value.size(); offset += 32U) {
        const auto size = (std::min)(std::size_t{32}, value.size() - offset);
        if (!Read(context, address + offset * sizeof(char16_t),
                  value.data() + offset, size * sizeof(char16_t))) return false;
        const auto end = std::find(value.begin() + static_cast<std::ptrdiff_t>(offset),
                                   value.begin() + static_cast<std::ptrdiff_t>(offset + size),
                                   u'\0');
        if (end != value.begin() + static_cast<std::ptrdiff_t>(offset + size)) {
            for (auto current = value.begin(); current != end; ++current) {
                const auto code_point = static_cast<std::uint32_t>(*current);
                if (code_point <= 0x7FU) result.push_back(static_cast<char>(code_point));
                else if (code_point <= 0x7FFU) {
                    result.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
                    result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
                } else {
                    result.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
                    result.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
                    result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
                }
            }
            return !result.empty();
        }
    }
    return false;
}

bool ReadFString(Context& context, const std::uintptr_t address,
                 std::string& result) {
    NativeUtf16StringHeader header{};
    if (!Read(context, address, header) || header.data == nullptr ||
        header.count <= 0 || header.count > 8192 || header.capacity < header.count)
        return false;
    return ReadUtf16CString(context, reinterpret_cast<std::uintptr_t>(header.data), result);
}

bool RefreshObjectRegistry(Context& context) noexcept {
    if (context.g_objects_address == 0 &&
        !ResolveRipRelative(context, kGObjectsPattern, kGObjectsAddend,
                            context.g_objects_address)) return false;
    ObjectRegistry next{};
    if (!ReadPointerAt(context, context.g_objects_address, kObjectItemsOffset, next.items) ||
        !Read(context, context.g_objects_address + kObjectCountOffset, next.count) ||
        !Read(context, context.g_objects_address + kObjectMaxCountOffset, next.max_count) ||
        !Read(context, context.g_objects_address + kObjectMaxChunksOffset, next.max_chunks) ||
        !Read(context, context.g_objects_address + kObjectNumChunksOffset, next.num_chunks) ||
        next.count == 0 || next.count > kMaximumObjectCount || next.max_count < next.count ||
        next.num_chunks == 0 || next.num_chunks > next.max_chunks ||
        next.num_chunks > kMaximumObjectChunks) return false;
    context.registry = next;
    return true;
}

bool ObjectFromHandle(Context& context, const AnomalyGenerationHandleV1 handle,
                      std::uintptr_t& object) noexcept {
    object = 0;
    if (!RefreshObjectRegistry(context)) return false;
    const std::uint32_t index = ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle);
    const std::uint32_t chunk_index = index / kObjectChunkSize;
    const std::uint32_t within_chunk = index % kObjectChunkSize;
    if (index >= context.registry.count || chunk_index >= context.registry.num_chunks)
        return false;
    std::uintptr_t chunk{};
    std::uintptr_t item{};
    return ReadPointerAt(context, context.registry.items,
                         static_cast<std::ptrdiff_t>(chunk_index * sizeof(void*)), chunk) &&
        AddSignedAddress(chunk, static_cast<std::ptrdiff_t>(within_chunk) * kObjectItemStride,
                         item) &&
        Read(context, item, object) && object != 0;
}

bool FindExactObject(Context& context, const std::string_view path,
                     std::uintptr_t& object) noexcept {
    object = 0;
    if (!ObjectsReady(context.objects)) return false;
    AnomalyGenerationHandleV1 handle{};
    if (context.objects->find_exact(
            context.objects->user, anomaly::sdk::StringView(path), &handle).code !=
            ANOMALY_STATUS_V1_OK || handle.id == 0) return false;
    return ObjectFromHandle(context, handle, object);
}

bool ResolvePlayerState(Context& context) noexcept {
    if (context.player_state != 0) return true;
    if (context.g_world_address == 0 &&
        !ResolveRipRelative(context, kGWorldPattern, 0, context.g_world_address))
        return false;
    std::uintptr_t world{};
    std::uintptr_t game_instance{};
    std::uintptr_t local_players{};
    std::uintptr_t local_player{};
    std::uintptr_t controller{};
    return Read(context, context.g_world_address, world) && world != 0 &&
        ReadPointerAt(context, world, kWorldGameInstanceOffset, game_instance) &&
        ReadPointerAt(context, game_instance, kGameInstanceLocalPlayersOffset, local_players) &&
        Read(context, local_players, local_player) && local_player != 0 &&
        ReadPointerAt(context, local_player, kLocalPlayerControllerOffset, controller) &&
        ReadPointerAt(context, controller, kControllerPlayerStateOffset, context.player_state);
}

bool InvokeOracleStateQuery(const std::uintptr_t function,
                            const std::uintptr_t state_context,
                            const FNameValue& id,
                            std::int32_t& result) noexcept {
    if (function == 0 || state_context == 0) return false;
    using QueryFn = std::int32_t(__fastcall*)(void*, const void*);
    const auto query = reinterpret_cast<QueryFn>(function);
    __try {
        result = query(reinterpret_cast<void*>(state_context), &id);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ResolveProcessEvent(Context& context) noexcept {
    return context.process_event != 0 ||
        ResolveSignature(context, kProcessEventPattern, context.process_event);
}

bool InvokeProcessEvent(Context& context, const std::uintptr_t object,
                        const std::uintptr_t function, void* parameters,
                        const std::size_t parameter_size) noexcept {
    if (!ResolveProcessEvent(context) || object == 0 || function == 0 ||
        parameter_size > 4096 || (parameter_size != 0 && parameters == nullptr))
        return false;
    using ProcessEventFn = void(__fastcall*)(void*, void*, void*);
    const auto invoke = reinterpret_cast<ProcessEventFn>(context.process_event);
    __try {
        invoke(reinterpret_cast<void*>(object), reinterpret_cast<void*>(function), parameters);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool EncodeMarkerId(const std::string& marker_id,
                    std::vector<wchar_t>& storage,
                    NativeUtf16StringHeader& native) noexcept;

// Resolve the "CrystalOracle" map icon FName exactly like the original host
// implementation: invoke the game's KismetStringLibrary.Conv_StringToName and
// read the returned FName. Reading the icon map from the treasure asset is not
// how the validated implementation works and fails on empty maps.
bool ResolveMapIconId(Context& context) noexcept {
    if (context.map_icon_id.comparison_index != 0) return true;
    if (context.string_to_name_function == 0) {
        if (!FindExactObject(context, "/Script/Engine.KismetStringLibrary.Conv_StringToName",
                             context.string_to_name_function)) {
            return false;
        }
        std::uintptr_t function_class{};
        if (!ReadPointerAt(context, context.string_to_name_function, kObjectOuterOffset,
                           function_class) ||
            !ReadPointerAt(context, function_class, kUClassClassDefaultObjectOffset,
                           context.string_library_receiver)) {
            return false;
        }
    }
    // Conv_StringToName(InString: FString, ReturnValue: FName). InString is the
    // first parameter at offset 0; the FName return value follows at offset 16.
    std::vector<std::uint8_t> parameters(24, 0);
    std::vector<wchar_t> icon_storage;
    NativeUtf16StringHeader native_icon{};
    const std::string icon_name{DivinationCrystalOracleMarkerContract().map_icon_id};
    if (!EncodeMarkerId(icon_name, icon_storage, native_icon)) return false;
    std::memcpy(parameters.data(), &native_icon, sizeof(native_icon));
    if (!InvokeProcessEvent(context, context.string_library_receiver,
                            context.string_to_name_function, parameters.data(),
                            parameters.size())) {
        return false;
    }
    std::memcpy(&context.map_icon_id, parameters.data() + 16, sizeof(context.map_icon_id));
    return context.map_icon_id.comparison_index != 0;
}

bool PrepareMarkerBinding(Context& context) noexcept {
    if (context.binding_ready) return true;
    if (!FindExactObject(context, "/Script/HTGame.HTPlayerState.AddMapIcon",
                         context.add_icon_function) ||
        !FindExactObject(context, "/Script/HTGame.HTPlayerState.RemoveMapIconByUniqueID",
                         context.remove_icon_function) ||
        !ResolveMapIconId(context)) {
        return false;
    }
    // AddMapIcon requires valid empty FText for OverrideIconName,
    // OverrideIconDesc and IconRegion. Read the empty text from the
    // UMG EditableText default object (same approach as the original host
    // implementation); a zeroed FText is invalid and the game rejects the
    // marker.
    std::uintptr_t editable_text{};
    if (!FindExactObject(context, "UMG.Default__EditableText", editable_text) ||
        !Read(context, editable_text + kEditableTextTextOffset,
              context.empty_text.data(), context.empty_text.size())) {
        return false;
    }
    context.binding_ready = true;
    return true;
}

bool EncodeMarkerId(const std::string& marker_id,
                    std::vector<wchar_t>& storage,
                    NativeUtf16StringHeader& native) noexcept {
    storage.assign(marker_id.begin(), marker_id.end());
    storage.push_back(L'\0');
    native.data = storage.data();
    native.count = static_cast<std::int32_t>(marker_id.size()) + 1;
    native.capacity = static_cast<std::int32_t>(storage.size());
    return !storage.empty();
}

bool AddOracleMarker(Context& context, const OracleStoneRecord& record,
                     const std::string& marker_id) {
    if (!PrepareMarkerBinding(context) || !ResolvePlayerState(context)) return false;
    const auto contract = DivinationCrystalOracleMarkerContract();
    std::vector<std::uint8_t> parameters(kOracleAddMapIconParmsSize, 0);
    auto* const add = parameters.data();

    std::vector<wchar_t> id_storage;
    NativeUtf16StringHeader native_id;
    if (!EncodeMarkerId(marker_id, id_storage, native_id)) return false;

    constexpr double kOne = 1.0;
    const std::array<double, 3> scale{kOne, kOne, kOne};
    const auto write = [&](const std::ptrdiff_t offset, const void* source,
                           const std::size_t size) {
        if (source == nullptr || offset < 0 ||
            static_cast<std::size_t>(offset) > kOracleAddParamsSize ||
            size > kOracleAddParamsSize - static_cast<std::size_t>(offset))
            return false;
        std::memcpy(add + offset, source, size);
        return true;
    };
    if (!write(kOracleAddMapIconIdOffset, &context.map_icon_id, sizeof(context.map_icon_id)) ||
        !write(kOracleAddIconTypeOffset, &contract.icon_type, sizeof(contract.icon_type)) ||
        !write(kOracleAddTransformTypeOffset, &contract.transform_type, sizeof(contract.transform_type)) ||
        !write(kOracleAddTransformQuatWOffset, &kOne, sizeof(kOne)) ||
        !write(kOracleAddTransformTranslationOffset, record.world_position,
               sizeof(record.world_position)) ||
        !write(kOracleAddTransformScaleOffset, scale.data(), sizeof(scale)) ||
        !write(kOracleAddOverrideNameOffset, context.empty_text.data(),
               context.empty_text.size()) ||
        !write(kOracleAddOverrideDescOffset, context.empty_text.data(),
               context.empty_text.size()) ||
        !write(kOracleAddIconRegionOffset, context.empty_text.data(),
               context.empty_text.size()) ||
        !write(kOracleAddUniqueIdOffset, &native_id, sizeof(native_id)) ||
        !write(kOracleAddIconInfoTypeOffset, &contract.icon_info_type,
               sizeof(contract.icon_info_type)) ||
        !write(kOracleAddIconStateOffset, &contract.icon_state, sizeof(contract.icon_state))) {
        return false;
    }

    // Set bOnlyShowInBelongsFloor and bUnderFog packed booleans.
    if constexpr (kOracleAddOnlyShowBelongsFloorOffset < kOracleAddParamsSize)
        add[kOracleAddOnlyShowBelongsFloorOffset] = contract.only_show_in_belongs_floor ? 1 : 0;
    if constexpr (kOracleAddUnderFogOffset < kOracleAddParamsSize)
        add[kOracleAddUnderFogOffset] = contract.under_fog ? 1 : 0;

    // bBroadcastChangeEvent follows the AddParams struct. This offset is part
    // of the validated NTE MiniMapIconAddParams ABI; addParamsSize is 352 and
    // the full AddMapIcon parameter buffer is 376 bytes.
    constexpr std::ptrdiff_t kBroadcastOffset = kOracleAddParamsSize;
    if (kBroadcastOffset >= 0 &&
        static_cast<std::size_t>(kBroadcastOffset) < parameters.size()) {
        parameters[static_cast<std::size_t>(kBroadcastOffset)] = 1;
    }

    return InvokeProcessEvent(context, context.player_state,
                              context.add_icon_function, parameters.data(),
                              parameters.size());
}

bool RemoveOracleMarker(Context& context, const std::string& marker_id) {
    if (!PrepareMarkerBinding(context) || !ResolvePlayerState(context)) return false;
    std::vector<std::uint8_t> parameters(kOracleRemoveMapIconParmsSize, 0);
    std::vector<wchar_t> id_storage;
    NativeUtf16StringHeader native_id;
    if (!EncodeMarkerId(marker_id, id_storage, native_id)) return false;
    std::memcpy(parameters.data(), &native_id, sizeof(native_id));
    if (parameters.size() >= 17) parameters[16] = 1; // bBroadcastChangeEvent
    if (parameters.size() >= 18) parameters[17] = 1; // bNeedRemoveIndicator
    return InvokeProcessEvent(context, context.player_state,
                              context.remove_icon_function, parameters.data(),
                              parameters.size());
}

bool ScanOracleCatalog(Context& context) {
    if (context.scan_attempted) return context.scan_ready;
    context.scan_stage.store(CatalogScanStage::CatalogTreasureAsset, std::memory_order_release);
    if (!FindExactObject(context, kTreasureboxDataAssetPath, context.treasure_asset) ||
        !ReadPointerAt(context, context.treasure_asset, kOracleStoneDataAssetTableOffset,
                       context.data_table)) {
        context.scan_stage.store(CatalogScanStage::Exception, std::memory_order_release);
        return false;
    }

    context.scan_stage.store(CatalogScanStage::CatalogRowMapHeader, std::memory_order_release);
    std::uintptr_t data{};
    std::int32_t num{};
    std::int32_t max{};
    std::uintptr_t flags_data{};
    std::int32_t flags_num{};
    std::int32_t flags_max{};
    if (!Read(context, context.data_table + kDataTableRowMapOffset + kDataTableRowMapData, data) ||
        !Read(context, context.data_table + kDataTableRowMapOffset + kDataTableRowMapNum, num) ||
        !Read(context, context.data_table + kDataTableRowMapOffset + kDataTableRowMapMax, max) ||
        !Read(context, context.data_table + kDataTableRowMapOffset + kDataTableRowMapFlagsData,
              flags_data) ||
        !Read(context, context.data_table + kDataTableRowMapOffset + kDataTableRowMapFlagsNum,
              flags_num) ||
        !Read(context, context.data_table + kDataTableRowMapOffset + kDataTableRowMapFlagsMax,
              flags_max) ||
        data == 0 || num <= 0 || num > kDataTableMaxRows || max < num ||
        flags_num < num || flags_max < flags_num) {
        context.scan_stage.store(CatalogScanStage::Exception, std::memory_order_release);
        return false;
    }

    context.scan_stage.store(CatalogScanStage::CatalogRowMapFlags, std::memory_order_release);
    const auto word_count = static_cast<std::size_t>((flags_num + 31) / 32);
    if (word_count == 0 || word_count > 128) {
        context.scan_stage.store(CatalogScanStage::Exception, std::memory_order_release);
        return false;
    }
    std::vector<std::uint32_t> flags(word_count);
    if (flags_data != 0) {
        if (!Read(context, flags_data, flags.data(), flags.size() * sizeof(std::uint32_t))) {
            context.scan_stage.store(CatalogScanStage::Exception, std::memory_order_release);
            return false;
        }
    } else if (word_count > 4 ||
        !Read(context, context.data_table + kDataTableRowMapOffset + kDataTableRowMapInlineFlags,
              flags.data(), flags.size() * sizeof(std::uint32_t))) {
        context.scan_stage.store(CatalogScanStage::Exception, std::memory_order_release);
        return false;
    }

    context.scan_stage.store(CatalogScanStage::CatalogRowMapElements, std::memory_order_release);
    std::vector<OracleStoneRecord> discovered;
    discovered.reserve(static_cast<std::size_t>(num));
    const std::size_t element_stride = static_cast<std::size_t>(kDataTableRowMapElementStride);
    const std::size_t row_offset = static_cast<std::size_t>(kDataTableRowMapRowOffset);
    for (std::int32_t index{}; index < num; ++index) {
        const std::size_t element_offset = static_cast<std::size_t>(index) * element_stride;
        FNameValue id_name{};
        std::uintptr_t row{};
        if (!Read(context, data + element_offset, id_name) ||
            !Read(context, data + element_offset + row_offset, row) ||
            id_name.comparison_index == 0 || row == 0) continue;

        OracleStoneRecord record;
        record.id_name = id_name;
        record.oracle_stone_id = RenderFName(context, id_name);
        if (record.oracle_stone_id.empty()) continue;

        std::string level;
        if (ReadFString(context, row + kOracleStoneLevelOffset, level))
            record.level = std::move(level);
        if (!Read(context, row + kOracleStoneFloorOffset, record.floor) ||
            !Read(context, row + kOracleStoneLocationOffset, record.world_position) ||
            !std::isfinite(record.world_position[0]) ||
            !std::isfinite(record.world_position[1]) ||
            !std::isfinite(record.world_position[2])) {
            continue;
        }
        std::uint8_t explore{};
        if (Read(context, row + kOracleStoneMapExploreOffset, explore))
            record.map_explore = (explore & 1U) != 0;
        discovered.push_back(std::move(record));
        if (discovered.size() >= kMaximumOracleStones) break;
    }

    if (discovered.empty()) {
        context.scan_stage.store(CatalogScanStage::CatalogEmpty, std::memory_order_release);
        return false;
    }

    context.scan_stage.store(CatalogScanStage::Ready, std::memory_order_release);
    std::scoped_lock lock(context.mutex);
    context.records = std::move(discovered);
    context.scan_ready = true;
    context.scan_attempted = true;
    return true;
}

void RefreshStates(Context& context) {
    if (!context.scan_ready || context.state_query == 0) {
        if (context.state_query == 0)
            ResolveSignature(context, kOracleStoneStateQueryPattern, context.state_query);
        if (context.state_query == 0 || !ResolvePlayerState(context)) return;
    }
    // Throttle: query at most kMaximumStateQueriesPerTick records per update
    // and pause for ~2 seconds (120 ticks) after a full sweep, matching the
    // validated host refresh loop.
    if (context.update_sequence < context.next_state_refresh_sequence) return;
    const std::uintptr_t state_context = context.player_state + kOracleStoneStateContextOffset;
    std::scoped_lock lock(context.mutex);
    const std::size_t count = context.records.size();
    if (count == 0) {
        context.state_refresh_cursor = 0;
        context.next_state_refresh_sequence = context.update_sequence + 120;
        return;
    }
    const std::size_t end = context.state_refresh_cursor < count
        ? (std::min)(count, context.state_refresh_cursor + kMaximumStateQueriesPerTick)
        : count;
    while (context.state_refresh_cursor < end) {
        auto& record = context.records[context.state_refresh_cursor++];
        std::int32_t native_state{};
        if (InvokeOracleStateQuery(context.state_query, state_context,
                                   record.id_name, native_state)) {
            record.state = TranslateOracleStoneState(native_state);
        }
    }
    if (context.state_refresh_cursor >= count) {
        context.state_refresh_cursor = 0;
        context.next_state_refresh_sequence = context.update_sequence + 120;
    }
}

void SyncMarkers(Context& context) {
    if (!context.scan_ready || !context.markers_enabled) return;
    if (!ResolvePlayerState(context)) return;
    if (!PrepareMarkerBinding(context)) return;

    std::vector<std::string> desired;
    {
        std::scoped_lock lock(context.mutex);
        desired.reserve(context.records.size());
        for (const auto& record : context.records) {
            if (record.state == OracleStoneAvailable) {
                desired.push_back("AnomalyOracleStone_" + record.oracle_stone_id);
            }
        }
    }

    std::vector<std::string> current;
    {
        std::scoped_lock lock(context.mutex);
        current = context.marker_ids;
    }

    std::sort(desired.begin(), desired.end());
    std::sort(current.begin(), current.end());

    std::vector<std::string> to_add;
    std::vector<std::string> to_remove;
    std::set_difference(desired.begin(), desired.end(), current.begin(), current.end(),
                        std::back_inserter(to_add));
    std::set_difference(current.begin(), current.end(), desired.begin(), desired.end(),
                        std::back_inserter(to_remove));

    std::size_t remove_budget = kMaximumRemovesPerTick;
    for (const auto& id : to_remove) {
        if (remove_budget == 0) break;
        if (RemoveOracleMarker(context, id)) {
            std::scoped_lock lock(context.mutex);
            std::erase(context.marker_ids, id);
            --remove_budget;
        }
    }
    std::size_t add_budget = kMaximumAddsPerTick;
    for (const auto& id : to_add) {
        if (add_budget == 0) break;
        const OracleStoneRecord* record{};
        {
            std::scoped_lock lock(context.mutex);
            const auto found = std::ranges::find_if(context.records, [&](const auto& candidate) {
                return ("AnomalyOracleStone_" + candidate.oracle_stone_id) == id;
            });
            if (found != context.records.end()) record = &*found;
        }
        if (record == nullptr) continue;
        if (AddOracleMarker(context, *record, id)) {
            std::scoped_lock lock(context.mutex);
            context.marker_ids.push_back(id);
            --add_budget;
        }
    }
}

void Update(Context& context, double /*delta_seconds*/) {
    if (!CoreReady(context.core) || !ObjectsReady(context.objects) ||
        !NamesReady(context.names) || !SignatureReady(context.signature))
        return;

    ++context.update_sequence;
    if (context.update_sequence == 0) ++context.update_sequence;

    if (!context.scan_attempted) {
        ScanOracleCatalog(context);
    }
    if (context.scan_ready) {
        RefreshStates(context);
        SyncMarkers(context);
    }
}

void Draw(Context& context, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr || ui->begin_window == nullptr || ui->end_window == nullptr) return;
    const std::string title = context.localizer.Text(
        "window.title", "Full-map uncollected Oracle Stone locator");
    if (!ui->begin_window(ui->user, anomaly::sdk::StringView(title),
                          &context.window_open, 0)) {
        ui->end_window(ui->user);
        return;
    }

    if (ui->checkbox != nullptr) {
        int enabled = context.markers_enabled ? 1 : 0;
        const std::string label = context.localizer.Label(
            "option.enabled", "Show uncollected Oracle Stones on the map", "enabled");
        if (ui->checkbox(ui->user, anomaly::sdk::StringView(label), &enabled)) {
            context.markers_enabled = enabled != 0;
        }
    }

    if (!context.scan_ready || !context.scan_attempted) {
        if (ui->text != nullptr) {
            const std::string unavailable = context.localizer.Text(
                "summary.unavailable",
                "The validated Oracle Stone service is unavailable for this build.");
            ui->text(ui->user, anomaly::sdk::StringView(unavailable));
        }
        ui->end_window(ui->user);
        return;
    }

    std::size_t total{};
    std::size_t uncollected{};
    std::size_t collected{};
    std::size_t visible{};
    {
        std::scoped_lock lock(context.mutex);
        total = context.records.size();
        for (const auto& record : context.records) {
            if (record.state == OracleStoneAvailable) ++uncollected;
            else if (record.state == OracleStoneCollected) ++collected;
        }
        visible = context.marker_ids.size();
    }
    if (ui->text != nullptr) {
        const std::string total_text = std::to_string(total);
        const std::string uncollected_text = std::to_string(uncollected);
        const std::string collected_text = std::to_string(collected);
        const std::array<std::string_view, 3> arguments{
            total_text, uncollected_text, collected_text};
        const std::string summary = context.localizer.Format(
            "summary.counts", "Total {0} / uncollected {1} / collected {2}", arguments);
        ui->text(ui->user, anomaly::sdk::StringView(summary));

        const std::string visible_text = std::to_string(visible);
        const std::string marker_state = context.markers_enabled
            ? context.localizer.Text("state.active", "Map markers active")
            : context.localizer.Text("state.disabled", "Disabled");
        const std::array<std::string_view, 2> marker_arguments{
            visible_text, marker_state};
        const std::string markers = context.localizer.Format(
            "summary.markers", "Visible markers {0} / {1}", marker_arguments);
        ui->text(ui->user, anomaly::sdk::StringView(markers));
    }

    if (ui->begin_table != nullptr && ui->table_next_row != nullptr &&
        ui->table_next_column != nullptr && ui->end_table != nullptr) {
        if (ui->begin_table(ui->user, anomaly::sdk::StringView("oracle-stones"), 4,
                            ANOMALY_UI_TABLE_V1_SIZING_FIXED_FIT, 0.0F, 0.0F)) {
            ui->table_next_row(ui->user);
            static_cast<void>(ui->table_next_column(ui->user));
            ui->text(ui->user, anomaly::sdk::StringView(context.localizer.Text("table.id", "ID")));
            static_cast<void>(ui->table_next_column(ui->user));
            ui->text(ui->user, anomaly::sdk::StringView(context.localizer.Text("table.level", "Map")));
            static_cast<void>(ui->table_next_column(ui->user));
            ui->text(ui->user, anomaly::sdk::StringView(context.localizer.Text("table.floor", "Floor")));
            static_cast<void>(ui->table_next_column(ui->user));
            ui->text(ui->user, anomaly::sdk::StringView(context.localizer.Text("table.position", "Coordinates")));

            std::scoped_lock lock(context.mutex);
            for (const auto& record : context.records) {
                if (record.state != OracleStoneAvailable) continue;
                ui->table_next_row(ui->user);
                static_cast<void>(ui->table_next_column(ui->user));
                ui->text(ui->user, anomaly::sdk::StringView(record.oracle_stone_id));
                static_cast<void>(ui->table_next_column(ui->user));
                ui->text(ui->user, anomaly::sdk::StringView(
                    record.level.empty() ? std::string_view{"<unknown>"}
                                         : std::string_view{record.level}));
                static_cast<void>(ui->table_next_column(ui->user));
                const std::string floor = std::to_string(record.floor);
                ui->text(ui->user, anomaly::sdk::StringView(floor));
                static_cast<void>(ui->table_next_column(ui->user));
                char position[128]{};
                std::snprintf(position, sizeof(position), "(%.1f, %.1f, %.1f)",
                              record.world_position[0], record.world_position[1],
                              record.world_position[2]);
                ui->text(ui->user, anomaly::sdk::StringView(position));
            }
            ui->end_table(ui->user);
        }
    }
    ui->end_window(ui->user);
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr)
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    try {
        Context& context = g_context;
        context.host = nullptr;
        context.localizer = anomaly::plugins::Localizer{};
        context.core = nullptr;
        context.signature = nullptr;
        context.names = nullptr;
        context.objects = nullptr;
        context.framework = nullptr;
        context.session = nullptr;
        context.player = nullptr;
        context.g_world_address = 0;
        context.g_objects_address = 0;
        context.process_event = 0;
        context.state_query = 0;
        context.registry = {};
        context.treasure_asset = 0;
        context.data_table = 0;
        context.player_state = 0;
        context.add_icon_function = 0;
        context.remove_icon_function = 0;
        context.string_to_name_function = 0;
        context.string_library_receiver = 0;
        context.map_icon_id = {};
        context.empty_text = {};
        context.binding_ready = false;
        {
            std::scoped_lock lock(context.mutex);
            context.records.clear();
            context.marker_ids.clear();
        }
        context.scan_attempted = false;
        context.scan_ready = false;
        context.markers_enabled = true;
        context.update_sequence = 0;
        context.next_state_refresh_sequence = 0;
        context.state_refresh_cursor = 0;
        context.scan_stage.store(CatalogScanStage::NotAttempted, std::memory_order_release);
        context.host = host;
        context.localizer = anomaly::plugins::Localizer(host);
        context.core = Query<AnomalyCoreServiceV1>(host, ANOMALY_CORE_SERVICE_V1_ID,
                                                   ANOMALY_CORE_SERVICE_V1_VERSION);
        context.signature = Query<AnomalySignatureServiceV1>(
            host, ANOMALY_SIGNATURE_SERVICE_V1_ID, ANOMALY_SIGNATURE_SERVICE_V1_VERSION);
        context.names = Query<AnomalyUe5NamesServiceV1>(
            host, ANOMALY_UE5_NAMES_SERVICE_V1_ID, ANOMALY_UE5_NAMES_SERVICE_V1_VERSION);
        context.objects = Query<AnomalyUe5ObjectsServiceV1>(
            host, ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION);
        context.framework = Query<AnomalyUe5FrameworkServiceV1>(
            host, ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID, ANOMALY_UE5_FRAMEWORK_SERVICE_V1_VERSION);
        context.session = Query<AnomalyNteSessionServiceV1>(
            host, ANOMALY_NTE_SESSION_SERVICE_V1_ID, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION);
        context.player = Query<AnomalyNtePlayerServiceV1>(
            host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION);
        *plugin_context = &context;
        return {ANOMALY_STATUS_V1_OK, 0, {}};
    } catch (...) {
        return {ANOMALY_STATUS_V1_FAILED, 0, {}};
    }
}

AnomalyStatusV1 ANOMALY_CALL Start(void* value) {
    if (value == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    auto& context = *static_cast<Context*>(value);
    context.markers_enabled = true;
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* value, const std::uint32_t deadline_milliseconds) {
    if (value == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    auto& context = *static_cast<Context*>(value);
    context.markers_enabled = false;
    std::vector<std::string> marker_ids;
    {
        std::scoped_lock lock(context.mutex);
        marker_ids = std::move(context.marker_ids);
        context.marker_ids.clear();
    }
    const auto started = std::chrono::steady_clock::now();
    for (const auto& id : marker_ids) {
        if (std::chrono::steady_clock::now() - started >=
            std::chrono::milliseconds(deadline_milliseconds)) break;
        static_cast<void>(RemoveOracleMarker(context, id));
    }
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

void ANOMALY_CALL Unload(void* value) {
    static_cast<void>(value);
}

void ANOMALY_CALL Update(void* value, const double delta_seconds) {
    if (value == nullptr) return;
    Update(*static_cast<Context*>(value), delta_seconds);
}

void ANOMALY_CALL Draw(void* value, const AnomalyUiServiceV1* ui) {
    if (value == nullptr) return;
    Draw(*static_cast<Context*>(value), ui);
}

} // namespace

extern "C" ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("physicalworlddo.oracle-stone-locator"),
        anomaly::sdk::StringView("全地图未获取石头定位"),
        anomaly::sdk::StringView("PhysicalWorldDo"), anomaly::sdk::StringView("1.0.1"),
        Load, Start, Stop, Unload, Update, Draw};
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}
