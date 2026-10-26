#pragma once

#include <Windows.h>

#include <bit>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

namespace modelcustom::native
{
struct Vector3
{
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

bool can_invoke_natives();





class ScriptContextScope
{
public:
    ScriptContextScope();
    ~ScriptContextScope();

    ScriptContextScope(const ScriptContextScope&) = delete;
    ScriptContextScope& operator=(const ScriptContextScope&) = delete;

    bool ready() const { return ready_; }

private:
    void* previous_ = nullptr;
    bool changed_ = false;
    bool ready_ = false;
};

std::uint64_t invoke(std::uint64_t hash, std::initializer_list<std::uint64_t> arguments = {});
void invoke_void(std::uint64_t hash, std::initializer_list<std::uint64_t> arguments = {});
bool invoke_vector3(
    std::uint64_t hash,
    std::initializer_list<std::uint64_t> arguments,
    Vector3& result);

void subtitle(std::string_view message, int durationMilliseconds = 3500);

inline std::uint64_t float_arg(float value)
{
    return std::bit_cast<std::uint32_t>(value);
}

constexpr std::uint64_t kPlayerPedId = 0xD80958FC74E988A6ULL;
constexpr std::uint64_t kDisableAllControlActions = 0x5F4B6931816E599BULL;
constexpr std::uint64_t kPlayerId = 0x4F8644AF03D0E0D6ULL;
constexpr std::uint64_t kDoesEntityExist = 0x7239B21A38F536BAULL;
constexpr std::uint64_t kGetEntityModel = 0x9F47B058362C84B5ULL;
constexpr std::uint64_t kSetPlayerModel = 0x00A1CADD00108836ULL;
constexpr std::uint64_t kHasModelLoaded = 0x98A4EB5D89A0C952ULL;
constexpr std::uint64_t kSetModelAsNoLongerNeeded = 0xE532F5D78798DAABULL;
constexpr std::uint64_t kIsModelInCdimage = 0x35B9E0803292B641ULL;
constexpr std::uint64_t kIsModelValid = 0xC0296A2EDF545E92ULL;
constexpr std::uint64_t kIsModelAPed = 0x75816577FEA6DAD5ULL;
constexpr std::uint64_t kSetPedDefaultComponentVariation = 0x45EEE61580806D63ULL;
constexpr std::uint64_t kGetPedDrawableVariation = 0x67F3780DD425D4FCULL;
constexpr std::uint64_t kGetPedTextureVariation = 0x04A355E041E004E6ULL;
constexpr std::uint64_t kGetPedPaletteVariation = 0xE3DD5F2A84B42281ULL;
constexpr std::uint64_t kSetPedComponentVariation = 0x262B14F48D29DE80ULL;
constexpr std::uint64_t kGetPedPropIndex = 0x898CC20EA75BACD8ULL;
constexpr std::uint64_t kGetPedPropTextureIndex = 0xE131A28626F81AB2ULL;
constexpr std::uint64_t kSetPedPropIndex = 0x93376B65A266EB5FULL;
constexpr std::uint64_t kClearPedProp = 0x0943E5B8E078E76EULL;
constexpr std::uint64_t kGiveWeaponToPed = 0xBF0FD6E56C964FCBULL;
constexpr std::uint64_t kSetCurrentPedWeapon = 0xADF692B254977C0CULL;
constexpr std::uint64_t kIsPedAPlayer = 0x125BF4AB723DA911ULL;
constexpr std::uint64_t kCreatePed = 0xD49F9B0955C367DEULL;
constexpr std::uint64_t kDeletePed = 0x9614299DCB53E54BULL;
constexpr std::uint64_t kAttachEntityToEntity = 0x6B9BBD38AB0796DFULL;
constexpr std::uint64_t kIsEntityAttachedToEntity = 0xEFBE71898A993728ULL;
constexpr std::uint64_t kDetachEntity = 0x961AC54BF0613F5DULL;
constexpr std::uint64_t kSetEntityAsMissionEntity = 0xAD738C3085FE7E11ULL;
constexpr std::uint64_t kSetEntityCollision = 0x1A9205C1B9EE827FULL;
constexpr std::uint64_t kSetEntityInvincible = 0x3882114BDE571AD4ULL;
constexpr std::uint64_t kSetEntityVisible = 0xEA1C610A04DB6BBBULL;
constexpr std::uint64_t kIsEntityVisible = 0x47D6F43D77935C75ULL;
constexpr std::uint64_t kSetEntityLocallyInvisible = 0xE135A9FF3F5D05D8ULL;
constexpr std::uint64_t kFreezeEntityPosition = 0x428CA6DBD1094446ULL;
constexpr std::uint64_t kSetEntityAlpha = 0x44A0870B7E92D7C0ULL;
constexpr std::uint64_t kResetEntityAlpha = 0x9B1E824FFBB7027AULL;
constexpr std::uint64_t kGetEntityCoords = 0x3FEF770D40960D5AULL;
constexpr std::uint64_t kGetEntityHeading = 0xE83D4F9BA2A38914ULL;
constexpr std::uint64_t kSetEntityCoordsNoOffset = 0x239A3351AC1DA385ULL;
constexpr std::uint64_t kSetEntityHeading = 0x8E2530AA8ADA980EULL;
constexpr std::uint64_t kSetBlockingOfNonTemporaryEvents = 0x9F8AA94D6D97DBF4ULL;
constexpr std::uint64_t kSetPedCanRagdoll = 0xB128377056A54E2AULL;
constexpr std::uint64_t kIsPedInAnyVehicle = 0x997ABD671D25CA0BULL;
constexpr std::uint64_t kDisablePlayerFiring = 0x5E6CC07646BBEAB8ULL;
constexpr std::uint64_t kEnableControlAction = 0x351220255D64C155ULL;
constexpr std::uint64_t kSetPedCombatAttributes = 0x9F7794730795E019ULL;
constexpr std::uint64_t kSetPlayerControl = 0x8D32347D6D4C40A2ULL;
constexpr std::uint64_t kSetPedConfigFlag = 0x1913FE4CBF41C463ULL;
constexpr std::uint64_t kSetEntityNoCollisionEntity = 0xA53ED5520C070143ULL;
constexpr std::uint64_t kGetSelectedPedWeapon = 0x0A6DB4965674D243ULL;
constexpr std::uint64_t kHasPedGotWeapon = 0x8DECB02F88F428BCULL;
constexpr std::uint64_t kWeaponUnarmed = 0xA2719263U;
constexpr std::uint64_t kIsEntityDead = 0x5F9532F3B5CC2551ULL;
constexpr std::uint64_t kIsPauseMenuActive = 0xB0034A223497FFD8ULL;

constexpr std::uint64_t kBeginTextCommandPrint = 0xB87A37EEB7FAA67DULL;
constexpr std::uint64_t kAddTextComponentSubstringPlayerName = 0x6C188BE134E074AAULL;
constexpr std::uint64_t kEndTextCommandPrint = 0x9D77056A530643F6ULL;

constexpr std::uint64_t kGetCurrentServerEndpoint = 0xEA11BFBAULL;
constexpr std::uint64_t kGetPlayerServerId = 0x4D97BCC7ULL;
constexpr std::uint64_t kGetPlayerFromServerId = 0x344EA166ULL;
constexpr std::uint64_t kGetPlayerPed = 0x43A66C31C68491C0ULL;
constexpr std::uint64_t kPedToNet = 0x0EDEC3C276198689ULL;
constexpr std::uint64_t kNetworkGetNetworkIdFromEntity = 0xA11700682F3AD45CULL;
constexpr std::uint64_t kNetworkIsPlayerActive = 0xB8DFD30D6973E135ULL;

std::string get_current_server_endpoint();
bool is_player_active(int playerId);
int get_player_server_id(int playerId);
int get_player_from_server_id(int serverId);
int resolve_player_from_server_id(int serverId);
int get_player_ped(int playerId);
}
