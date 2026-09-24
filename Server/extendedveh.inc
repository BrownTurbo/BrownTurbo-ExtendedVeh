/*
 * SA:MP ExtendedVeh Server plugin
 * Author: .silent & Zorono
 *
 * http://github.com/BrownTurbo/BrownTurbo-ExtendedVeh
 *
*/

#if defined _extveh_included
#endinput
#endif
#define _extveh_included

/*
 * Vehicle Model & ID Limits
 */
#define MIN_VEHICLE_MODEL               (400)
#define MAX_VEHICLE_MODEL               (611)
#define TOTAL_DEFAULT_VEHICLE_MODELS    (212)

#define MIN_CUSTOM_VEHICLE_MODEL        (20000)
#define MAX_CUSTOM_VEHICLE_MODEL        (65535)

/*
 * Handling Attributes
 */
enum CHandlingAttrib
{
	HANDL_IDENTIFIER = 1, // A
	HANDL_FMASS,	// B
	HANDL_FTURNMASS, // c
	HANDL_FDRAGMULTIPLIER, // D
	HANDL_CENTREOFMASS_X,	// F
	HANDL_CENTREOFMASS_Y,	// G
	HANDL_CENTREOFMASS_Z,	// H
	HANDL_NPERCENTSUBMERGED,	// I
	HANDL_FTRACTIONMULTIPLIER,	// J
	HANDL_FTRACTIONLOSS,	// K
	HANDL_FTRACTIONBIAS,	// L
	HANDL_TR_NNUMBEROFGEARS,	// M // transmission data, full name is too long for pawn
	HANDL_TR_FMAXVELOCITY,	// N
	HANDL_TR_FENGINEACCELERATION,	// O
	HANDL_TR_FENGINEINERTIA,	// P
	HANDL_TR_NDRIVETYPE,	// Q
	HANDL_TR_NENGINETYPE,	// R
	HANDL_FBRAKEDECELERATION,	// S
	HANDL_FBRAKEBIAS,	// T
	HANDL_BABS,	// U
	HANDL_FSTEERINGLOCK,	// V
	HANDL_FSUSPENSIONFORCELEVEL,	// a
	HANDL_FSUSPENSIONDAMPINGLEVEL,	// b
	HANDL_FSUSPENSIONHIGHSPDCOMDAMP,	// c
	HANDL_FSUSPENSIONUPPERLIMIT,	// d
	HANDL_FSUSPENSIONLOWERLIMIT,	// e
	HANDL_FSUSPENSIONBIAS,	// f
	HANDL_FSUSPENSIONANTIDIVEMULT,	// g
	HANDL_FSEATOFFSETDISTANCE,	// aa
	HANDL_FCOLLISIONDAMAGEMULT,	// ab
	HANDL_NMONETARYVALUE,	// ac
	HANDL_MODELFLAGS,	// af
	HANDL_HANDLINGFLAGS,	// ag
	HANDL_FRONTLIGHTS,	// ah
	HANDL_REARLIGHTS,	// ai
	HANDL_ANIMGROUP	 // aj
};

/* Handling flags */
enum
{
	HFLAG_1G_BOOST = 0x1,
	HFLAG_2G_BOOST = 0x2,
	HFLAG_NPC_ANTI_ROLL = 0x4,
	HFLAG_NPC_NEUTRAL_HANDL = 0x8,
	HFLAG_NO_HANDBRAKE = 0x10,
	HFLAG_STEER_REARWHEELS = 0x20,
	HFLAG_HB_REARWHEEL_STEER = 0x40,
	HFLAG_ALT_STEER_OPT = 0x80,
	HFLAG_WHEEL_F_NARROW2 = 0x100,
	HFLAG_WHEEL_F_NARROW = 0x200,
	HFLAG_WHEEL_F_WIDE = 0x400,
	HFLAG_WHEEL_F_WIDE2 = 0x800,
	HFLAG_WHEEL_R_NARROW2 = 0x1000,
	HFLAG_WHEEL_R_NARROW = 0x2000,
	HFLAG_WHEEL_R_WIDE = 0x4000,
	HFLAG_WHEEL_R_WIDE2 = 0x8000,
	HFLAG_HYDRAULIC_GEOM = 0x10000,
	HFLAG_HYDRAULIC_INST = 0x20000,
	HFLAG_HYDRAULIC_NONE = 0x40000,
	HFLAG_NOS_INST = 0x80000,
	HFLAG_OFFROAD_ABILITY = 0x100000,
	HFLAG_OFFROAD_ABILITY2 = 0x200000,
	HFLAG_HALOGEN_LIGHTS = 0x400000,
	HFLAG_PROC_REARWHEEL_1ST = 0x800000,
	HFLAG_USE_MAXSP_LIMIT = 0x1000000,
	HFLAG_LOW_RIDER = 0x2000000,
	HFLAG_STREET_RACER = 0x4000000,
	HFLAG_SWINGING_CHASSIS = 0x10000000
};

/* Model flags */
enum
{
	MFLAG_IS_VAN = 0x1,
	MFLAG_IS_BUS = 0x2,
	MFLAG_IS_LOW = 0x4,
	MFLAG_IS_BIG = 0x8,
	MFLAG_REVERSE_BONNET = 0x10,
	MFLAG_HANGING_BOOT = 0x20,
	MFLAG_TAILGATE_BOOT = 0x40,
	MFLAG_NOSWING_BOOT = 0x80,
	MFLAG_NO_DOORS = 0x100,
	MFLAG_TANDEM_SEATS = 0x200,
	MFLAG_SIT_IN_BOAT = 0x400,
	MFLAG_CONVERTIBLE = 0x800,
	MFLAG_NO_EXHAUST = 0x1000,
	MFLAG_DOUBLE_EXHAUST = 0x2000,
	MFLAG_NO1FPS_LOOK_BEHIND = 0x4000,
	MFLAG_FORCE_DOOR_CHECK = 0x8000,
	MFLAG_AXLE_F_NOTILT = 0x10000,
	MFLAG_AXLE_F_SOLID = 0x20000,
	MFLAG_AXLE_F_MCPHERSON = 0x40000,
	MFLAG_AXLE_F_REVERSE = 0x80000,
	MFLAG_AXLE_R_NOTILT = 0x100000,
	MFLAG_AXLE_R_SOLID = 0x200000,
	MFLAG_AXLE_R_MCPHERSON = 0x400000,
	MFLAG_AXLE_R_REVERSE = 0x800000,
	MFLAG_IS_BIKE = 0x1000000,
	MFLAG_IS_HELI = 0x2000000,
	MFLAG_IS_PLANE = 0x4000000,
	MFLAG_IS_BOAT = 0x8000000,
	MFLAG_BOUNCE_PANELS = 0x10000000,
	MFLAG_DOUBLE_RWHEELS = 0x20000000,
	MFLAG_FORCE_GROUND_CLEARANCE = 0x40000000,
	MFLAG_IS_HATCHBACK = 0x80000000
};

/*
 * Bit Flag Utility Macros
 */
#define FLAG_IS_ENABLED(%0,%1) (%0 & %1)
#define FLAG_ENABLE(%0,%1) (%0 |= (%1))
#define FLAG_DISABLE(%0,%1) (%0 &= ~(%1))
#define FLAG_FLIP(%0,%1) (%0 ^= %1)

enum CHandlingLights
{
	LIGHTS_LONG,
	LIGHTS_SMALL,
	LIGHTS_BIG,
	LIGHTS_TALL
};
#define eVehicleLightsSize CHandlingLights

enum eVehicleDoor
{
	VEHICLE_DOOR_BONNET = 0,
	VEHICLE_DOOR_BOOT = 1,
	VEHICLE_DOOR_FRONT_LEFT = 2,
	VEHICLE_DOOR_FRONT_RIGHT = 3,
	VEHICLE_DOOR_REAR_LEFT = 4,
	VEHICLE_DOOR_REAR_RIGHT = 5,
	VEHICLE_DOOR_ALL = 0xFF
};

enum CHandlingEngineType
{
    ENGINETYPE_PETROL  = 'P',
    ENGINETYPE_DIESEL  = 'D',
    ENGINETYPE_ELECTRIC= 'E'
};

enum CHandlingAttribType
{
	TYPE_NONE,
	TYPE_UINT,
	TYPE_FLOAT,
	TYPE_BYTE,
	TYPE_FLAG
};

/*
 * CHandling Native Declarations
 */
native CHandlingAttribType:GetHandlingAttribType(CHandlingAttrib:attribute);
native bool:IsPlayerUsingExtendedVeh(playerid);
#pragma deprecated Use IsPlayerUsingExtendedVeh instead
native bool:IsPlayerUsingCHandling(playerid);

// Model Handling Natives
native bool:ResetModelHandling(customModelId);
native bool:SetModelHandlingFloat(modelid, CHandlingAttrib:attribute, Float:value);
native bool:SetModelHandlingInt(modelid, CHandlingAttrib:attribute, value);
native bool:GetModelHandlingFloat(modelid, CHandlingAttrib:attribute, &Float:result);
native bool:GetModelHandlingInt(modelid, CHandlingAttrib:attribute, &result);
native bool:GetDefaultHandlingFloat(modelid, CHandlingAttrib:attribute, &Float:result);
native bool:GetDefaultHandlingInt(modelid, CHandlingAttrib:attribute, &result);

// Vehicle Instance Handling Natives
native bool:ResetVehicleHandling(vehicleid);
native bool:SetVehicleHandlingFloat(vehicleid, CHandlingAttrib:attribute, Float:value);
native bool:SetVehicleHandlingInt(vehicleid, CHandlingAttrib:attribute, value);
native bool:GetVehicleHandlingFloat(vehicleid, CHandlingAttrib:attribute, &Float:result);
native bool:GetVehicleHandlingInt(vehicleid, CHandlingAttrib:attribute, &result);

// Water Driving & Amphibious Floating Natives
// Allowed on: Automobiles, Quads, Monster Trucks
// Prohibited on: Planes (including Skimmer), Helicopters (including Leviathan/SeaSparrow),
//               Boats, Trains, Trailers, Bikes, Motorcycles (BMX included)
native bool:SetVehicleWaterDrive(vehicleid, bool:enable, submergedPercent = 30);
native bool:GetVehicleWaterDrive(vehicleid, &bool:enabled);
native bool:SetModelWaterDrive(modelid, bool:enable, submergedPercent = 30);
native bool:GetModelWaterDrive(modelid, &bool:enabled);

// Flying Vehicle Natives (Auto-leveling car/boat/bike flight)
// Allowed on: Automobiles, Motorcycles/BMX, Quads, Monster Trucks, Boats
// Prohibited on: Planes, Helicopters, Trains, Trailers (use native flight or rail physics)
native bool:SetVehicleFlying(vehicleid, bool:enable);
native bool:GetVehicleFlying(vehicleid, &bool:enabled);
native bool:SetModelFlying(modelid, bool:enable);
native bool:GetModelFlying(modelid, &bool:enabled);

// Player Handling Overrides
native bool:SetPlayerHandlingFloat(playerid, CHandlingAttrib:attribute, Float:value);
native bool:SetPlayerHandlingInt(playerid, CHandlingAttrib:attribute, value);
native bool:GetPlayerHandlingFloat(playerid, CHandlingAttrib:attribute, &Float:result);
native bool:GetPlayerHandlingInt(playerid, CHandlingAttrib:attribute, &result);
native bool:ResetPlayerHandling(playerid);
native bool:ResetAllHandlingForPlayer(playerid);
native bool:ResetAllHandling(playerid);

// Custom Vehicle Models & Streaming Natives
native bool:BeginCustomVehicleDef(customModelId, visualBase, audioBase, handlingBase, engineOnSoundId, engineOffSoundId);
native bool:SetCustomVehicleDff(customModelId);
native bool:SetCustomVehicleTxd(customModelId);
native bool:SetCustomVehicleCol(customModelId);
native bool:SetCustomVehicleModelInfo(customModelId, vehicleClass, wheelModelId, Float:wheelScaleFront, Float:wheelScaleRear, frequency, level, comprate, numExtras, wheelUpgradeClass = 0);
native bool:SetCustomVehicleWheelModel(customModelId, wheelModelId);
native bool:SetCustomVehicleWheelScale(customModelId, Float:wheelScaleFront, Float:wheelScaleRear);
native bool:GetCustomVehicleWheelModel(customModelId, &wheelModelId);
native bool:GetCustomVehicleWheelScale(customModelId, &Float:wheelScaleFront, &Float:wheelScaleRear);
native bool:CommitCustomVehicleDef(customModelId);
native bool:DestroyCustomVehicle(customModelId);
native bool:IsCustomVehicleModel(customModelId);
native bool:IsVehicleCustom(vehicleid);

// Custom Vehicle Model INI Configuration & Override Natives
native bool:LoadCustomVehicleConfig(customModelId);
native bool:DefineCustomVehicleFromConfig(customModelId, defaultVisualBase = 411);
native LoadAllCustomVehicles(defaultVisualBase = 411);

native bool:GetCustomVehicleName(customModelId, output[], maxlen = sizeof(output));
native bool:SetCustomVehicleName(customModelId, const name[]);

native bool:GetCustomVehicleConfigString(customModelId, const key[], output[], maxlen = sizeof(output));
native bool:SetCustomVehicleConfigString(customModelId, const key[], const value[]);

native bool:GetCustomVehicleConfigInt(customModelId, const key[], &result);
native bool:SetCustomVehicleConfigInt(customModelId, const key[], value);

native bool:GetCustomVehicleConfigFloat(customModelId, const key[], &Float:result);
native bool:SetCustomVehicleConfigFloat(customModelId, const key[], Float:value);

native GetCustomVehicleColorVariationsCount(customModelId);
native bool:GetCustomVehicleColorVariation(customModelId, variationIndex, &primary, &secondary, &tertiary, &quaternary);
native bool:GetCustomVehicleDefaultColors(customModelId, &primary, &secondary, &tertiary, &quaternary);

native GetCustomVehicleAllowedUpgradesCount(customModelId);
native GetCustomVehicleAllowedUpgrade(customModelId, index);

native bool:SetCustomVehicleLightingOffset(customModelId, Float:hlOffsetX, Float:hlOffsetY, Float:hlOffsetZ, Float:tlOffsetX = 0.0, Float:tlOffsetY = 0.0, Float:tlOffsetZ = 0.0);
native bool:GetCustomVehicleLightingOffset(customModelId, &Float:hlOffsetX, &Float:hlOffsetY, &Float:hlOffsetZ, &Float:tlOffsetX, &Float:tlOffsetY, &Float:tlOffsetZ);

/*
 * Custom Vehicle Instance Customization Natives
 *
 * NOTE: Legacy SA-MP vehicle natives work out-of-the-box on custom vehicles:
 *   - ChangeVehiclePaintjob(vehicleid, paintjobid) automatically applies custom liveries.
 *   - ChangeVehicleColor(vehicleid, color1, color2) automatically recolors custom vehicles.
 *   - AddVehicleComponent(vehicleid, componentid) / RemoveVehicleComponent(...) work seamlessly.
 */
native bool:BindCustomVehicle(vehicleid, customModelId);

native bool:SetCustomVehicleStance(vehicleid, Float:frontScale, Float:rearScale, Float:frontCamber, Float:rearCamber, Float:frontTrackWidth, Float:rearTrackWidth);
native bool:GetCustomVehicleStance(vehicleid, &Float:frontScale, &Float:rearScale, &Float:frontCamber, &Float:rearCamber, &Float:frontTrackWidth, &Float:rearTrackWidth);

native bool:SetCustomVehicleExtras(vehicleid, extrasMask);
native bool:GetCustomVehicleExtras(vehicleid, &extrasMask);

native bool:SetCustomVehiclePaintjob(vehicleid, paintjobid);
native GetCustomVehiclePaintjob(vehicleid);

native bool:SetCustomVehicleNeon(vehicleid, enabled, r, g, b, Float:size = 2.5);
native bool:GetCustomVehicleNeon(vehicleid, &enabled, &r, &g, &b, &Float:size);

native bool:SetCustomVehicleWindowTint(vehicleid, alpha, r = 0, g = 0, b = 0);
native bool:GetCustomVehicleWindowTint(vehicleid, &alpha, &r, &g, &b);

native bool:SetCustomVehicleWheelColor(vehicleid, r, g, b);
native bool:GetCustomVehicleWheelColor(vehicleid, &r, &g, &b);

native bool:SetCustomVehicleWheel(vehicleid, wheelModelId);
native bool:GetCustomVehicleWheel(vehicleid, &wheelModelId);

native bool:SetCustomVehicleBackfire(vehicleid, bool:enabled);
native bool:GetCustomVehicleBackfire(vehicleid, &bool:enabled);

native bool:SetCustomVehicleHorn(vehicleid, hornSoundId, Float:hornPitch = 1.0);
native bool:GetCustomVehicleHorn(vehicleid, &hornSoundId, &Float:hornPitch);

native bool:SetCustomVehicleSiren(vehicleid, bool:enabled, sirenType = 1);
native bool:GetCustomVehicleSiren(vehicleid, &bool:enabled, &sirenType);

/* Vehicle Lighting Categories */
enum
{
	VEHICLE_LIGHT_CAT_AUTO = 0,       // Standard Automobiles (Sedans, Sports, SUVs, etc.)
	VEHICLE_LIGHT_CAT_TWOWHEELER = 1, // Motorcycles, Dirt Bikes, BMX, Quads
	VEHICLE_LIGHT_CAT_HEAVY = 2,      // Monster Trucks, Big Rigs, Buses, Machinery
	VEHICLE_LIGHT_CAT_AIRCRAFT = 3,   // Planes, Jets, Helicopters
	VEHICLE_LIGHT_CAT_BOAT = 4,       // Speedboats, Yachts, Dinghies
	VEHICLE_LIGHT_CAT_HOVER = 5,      // Hovercraft (Vortex) / Flight-mode vehicles
	VEHICLE_LIGHT_CAT_RC = 6,         // Miniature RC models (RC Bandit, Baron, Raider, Goblin, Tiger, Cam)
	VEHICLE_LIGHT_CAT_TRAILER = 7     // Towed trailers (rear markers/taillights only)
};

native bool:SetCustomVehicleLighting(vehicleid, category = -1, Float:scale = 1.0);
native bool:GetCustomVehicleLighting(vehicleid, &category, &Float:scale);

native bool:SetCustomVehicleModelHorn(customModelId, hornSoundId, Float:hornPitch = 1.0);
native bool:SetCustomVehicleModelSiren(customModelId, bool:hasSiren, sirenType = 1);

native bool:GetFileSha256(const filename[], outputHash[]);
native bool:InvalidateModelCache(customModelId, fileKind);
native GetClientFileStoreStatus(playerid, customModelId, fileKind);

// Vehicle Door Removal / Restoration Natives
// Allowed on: Automobiles, Quads, Monster Trucks, Planes, Helicopters
// Prohibited on: Bikes, Motorcycles, Bicycles, Boats, Trailers, Trains (vehicles without standard doors)
native bool:SetCustomVehicleDoorMissing(vehicleid, doorid, bool:missing);
native bool:GetCustomVehicleDoorMissing(vehicleid, doorid, &bool:missing);
native bool:SetCustomVehicleAllDoorsMissing(vehicleid, bool:missing);

/*
 * Backward-compatibility aliases for legacy scripts
 */
#define BindVehicleModel            BindCustomVehicle
#define SetVehicleStance            SetCustomVehicleStance
#define GetVehicleStance            GetCustomVehicleStance
#define SetVehicleExtras            SetCustomVehicleExtras
#define GetVehicleExtras            GetCustomVehicleExtras
#define SetVehiclePaintjob          SetCustomVehiclePaintjob
#define SetVehicleNeon              SetCustomVehicleNeon
#define GetVehicleNeon              GetCustomVehicleNeon
#define SetVehicleWindowTint        SetCustomVehicleWindowTint
#define GetVehicleWindowTint        GetCustomVehicleWindowTint
#define SetVehicleWheelColor        SetCustomVehicleWheelColor
#define GetVehicleWheelColor        GetCustomVehicleWheelColor
#define SetVehicleWheel             SetCustomVehicleWheel
#define GetVehicleWheel             GetCustomVehicleWheel
#define SetVehicleBackfire          SetCustomVehicleBackfire
#define GetVehicleBackfire          GetCustomVehicleBackfire
#define SetVehicleHorn              SetCustomVehicleHorn
#define GetVehicleHorn              GetCustomVehicleHorn
#define SetVehicleSiren             SetCustomVehicleSiren
#define GetVehicleSiren             GetCustomVehicleSiren
#define SetVehicleLighting          SetCustomVehicleLighting
#define GetVehicleLighting          GetCustomVehicleLighting
#define SetVehicleLightingOffset    SetCustomVehicleLightingOffset
#define GetVehicleLightingOffset    GetCustomVehicleLightingOffset
#define SetCustomVehicleModelLightingOffset SetCustomVehicleLightingOffset
#define GetCustomVehicleModelLightingOffset GetCustomVehicleLightingOffset
#define SetVehicleModelLightingOffset       SetCustomVehicleLightingOffset
#define GetVehicleModelLightingOffset       GetCustomVehicleLightingOffset
#define SetVehicleModelHorn         SetCustomVehicleModelHorn
#define SetVehicleModelSiren        SetCustomVehicleModelSiren
#define SetVehicleDoorMissing       SetCustomVehicleDoorMissing
#define GetVehicleDoorMissing       GetCustomVehicleDoorMissing
#define SetVehicleAllDoorsMissing   SetCustomVehicleAllDoorsMissing

/*
 * Stock Wrapper Helper for Single-Call Custom Vehicle Creation
 */
stock bool:DefineCustomVehicle(customModelId, visualBase, audioBase, handlingBase, engineOnSoundId, engineOffSoundId)
{
    if (!BeginCustomVehicleDef(customModelId, visualBase, audioBase, handlingBase, engineOnSoundId, engineOffSoundId))
        return false;
    if (!SetCustomVehicleDff(customModelId))
        return false;
    if (!SetCustomVehicleTxd(customModelId))
        return false;
    if (!SetCustomVehicleCol(customModelId))
        return false;
    return CommitCustomVehicleDef(customModelId);
}
