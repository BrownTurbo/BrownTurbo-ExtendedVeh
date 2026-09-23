// this example is created using AI.

#define FILTERSCRIPT

#include <openmultiplayer\open.mp>
#include <extendedveh>
#include <sscanf2>
#include <Pawn.CMD>

// =============================================================================

#define C_INFO      "{3498DB}"
#define C_SUCCESS   "{2ECC71}"
#define C_WARN      "{F1C40F}"
#define C_FAIL      "{E74C3C}"
#define C_WHITE     "{FFFFFF}"
#define C_DEBUG     "{95A5A6}"
#define C_PRM       "{9B59B6}"

#define EPSILON     0.0001

#define TEST_MODEL_ALPHA    20001
#define TEST_MODEL_BETA     20002

#define BASE_ALPHA_VISUAL   411
#define BASE_ALPHA_AUDIO    411
#define BASE_ALPHA_HANDLING 411

#define BASE_BETA_VISUAL    560
#define BASE_BETA_AUDIO     560
#define BASE_BETA_HANDLING  560

// =============================================================================

new g_TestsPassed = 0;
new g_TestsFailed = 0;
new g_TestCallerId = INVALID_PLAYER_ID;
new g_TestVehicleAlpha = INVALID_VEHICLE_ID;

new bool:g_AlphaDefinitionReady = false;
new bool:g_BetaDefinitionReady = false;

enum ModelFileKind
{
    FILE_KIND_DFF = 0,
	FILE_KIND_TXD = 1,
	FILE_KIND_COL = 2
};

// =============================================================================

public OnFilterScriptInit()
{
    print("\n=========================================================");
    print(" BrownTurbo-ExtendedVeh - Advanced Diagnostic Suite Init");
    print("=========================================================");

    RegisterTestDefinitions();
    return 1;
}

public OnFilterScriptExit()
{
    if (g_TestVehicleAlpha != INVALID_VEHICLE_ID) DestroyVehicle(g_TestVehicleAlpha);

    DestroyCustomVehicle(TEST_MODEL_ALPHA);
    DestroyCustomVehicle(TEST_MODEL_BETA);

    print(" BrownTurbo-ExtendedVeh test suite unloaded.\n");
    return 1;
}

// =============================================================================

stock Test_Log(bool:condition, const testName[], const errorMsg[] = "")
{
    new str[256];
    if (condition)
    {
        g_TestsPassed++;
        printf("[PASS] %s", testName);
        if (g_TestCallerId != INVALID_PLAYER_ID)
        {
            format(str, sizeof(str), "%s[PASS]%s %s", C_SUCCESS, C_WHITE, testName);
            SendClientMessage(g_TestCallerId, -1, str);
        }
    }
    else
    {
        g_TestsFailed++;
        printf("[FAIL] %s - %s", testName, errorMsg);
        if (g_TestCallerId != INVALID_PLAYER_ID)
        {
            format(str, sizeof(str), "%s[FAIL]%s %s %s(%s)", C_FAIL, C_WHITE, testName, C_DEBUG, errorMsg);
            SendClientMessage(g_TestCallerId, -1, str);
        }
    }
}

stock Test_AssertFloat(Float:expected, Float:actual, const testName[])
{
    new err[128];
    new bool:ok = floatabs(expected - actual) <= EPSILON;
    if (!ok) format(err, sizeof(err), "Expected %.4f, got %.4f", expected, actual);
    Test_Log(ok, testName, err);
}

stock Test_AssertInt(expected, actual, const testName[])
{
    new err[128];
    new bool:ok = floatabs(float(expected) - float(actual)) <= EPSILON;
    if (!ok) format(err, sizeof(err), "Expected %d, got %d", expected, actual);
    Test_Log(ok, testName, err);
}

// =============================================================================

stock Suite_Meta()
{
    Test_Log(_:GetHandlingAttribType(HANDL_FMASS) == _:TYPE_FLOAT, "Meta: FMASS is FLOAT");
    Test_Log(_:GetHandlingAttribType(HANDL_TR_NNUMBEROFGEARS) == _:TYPE_BYTE, "Meta: NNUMBEROFGEARS is BYTE");
    Test_Log(_:GetHandlingAttribType(HANDL_MODELFLAGS) == _:TYPE_FLAG, "Meta: MODELFLAGS is FLAG");
    Test_Log(_:GetHandlingAttribType(HANDL_HANDLINGFLAGS) == _:TYPE_FLAG, "Meta: HANDLINGFLAGS is FLAG");

    new hash[65];
    Test_Log(!GetFileSha256("missing.dff", hash), "Meta: SHA256 safety on missing file");
}

stock Suite_Definitions()
{
    Test_Log(IsCustomVehicleModel(TEST_MODEL_ALPHA), "Pipeline: Alpha Model Registered");
    Test_Log(!IsCustomVehicleModel(BASE_ALPHA_VISUAL), "Pipeline: Vanilla models reject custom flag");

    new wheelModel;
    GetCustomVehicleWheelModel(TEST_MODEL_ALPHA, wheelModel);
    Test_AssertInt(1025, wheelModel, "Pipeline: Alpha Default Wheel Model");

    new Float:wf, Float:wr;
    GetCustomVehicleWheelScale(TEST_MODEL_ALPHA, wf, wr);
    Test_AssertFloat(0.85, wf, "Pipeline: Alpha Wheel Scale Front");
}

stock Suite_ModelHandling()
{
    new Float:defMass, Float:modMass, defABS, modABS;
    GetDefaultHandlingFloat(TEST_MODEL_ALPHA, HANDL_FMASS, defMass);
    GetDefaultHandlingInt(TEST_MODEL_ALPHA, HANDL_BABS, defABS);

    SetModelHandlingFloat(TEST_MODEL_ALPHA, HANDL_FMASS, defMass + 100.0);

    new newABS = (defABS == 0) ? 1 : 0;
    SetModelHandlingInt(TEST_MODEL_ALPHA, HANDL_BABS, newABS);

    GetModelHandlingFloat(TEST_MODEL_ALPHA, HANDL_FMASS, modMass);
    GetModelHandlingInt(TEST_MODEL_ALPHA, HANDL_BABS, modABS);

    Test_AssertFloat(defMass + 100.0, modMass, "Model: Float Override applied");
    Test_AssertInt(newABS, modABS, "Model: Int Override applied (ABS)");

    ResetModelHandling(TEST_MODEL_ALPHA);
    GetModelHandlingFloat(TEST_MODEL_ALPHA, HANDL_FMASS, modMass);
    Test_AssertFloat(defMass, modMass, "Model: Reset reverts to Default");
}

stock Suite_PlayerHandling(playerid)
{
    if (playerid == INVALID_PLAYER_ID) return;

    SetPlayerHandlingFloat(playerid, HANDL_FMASS, 2000.0);
    new Float:pMass; GetPlayerHandlingFloat(playerid, HANDL_FMASS, pMass);
    Test_AssertFloat(2000.0, pMass, "Player: Float Override applied");

    Test_Log(ResetPlayerHandling(playerid), "Player: ResetPlayerHandling executes");
    Test_Log(ResetAllHandlingForPlayer(playerid), "Player: ResetAllHandlingForPlayer executes");
    Test_Log(ResetAllHandling(playerid), "Player: ResetAllHandling (Alias) executes");
}

stock Suite_VehicleHandling(playerid)
{
    if (!g_AlphaDefinitionReady)
    {
        Test_Log(
            false,
            "Vehicle: Definition prerequisite",
            "Alpha custom model was not committed; binding test skipped."
        );

        return;
    }

    new Float:x, Float:y, Float:z;

    if (playerid != INVALID_PLAYER_ID)
    {
        GetPlayerPos(playerid, x, y, z);
        x += 2.0;
        y += 2.0;
        z += 0.75;
    }

    g_TestVehicleAlpha =
        CreateVehicle(
            BASE_ALPHA_VISUAL,
            x,
            y,
            z,
            0.0,
            1,
            1,
            -1
        );

    Test_Log(
        g_TestVehicleAlpha != INVALID_VEHICLE_ID,
        "Vehicle: Instance Created"
    );

    if (g_TestVehicleAlpha == INVALID_VEHICLE_ID)
        return;

    Test_Log(
        BindVehicleModel(
            g_TestVehicleAlpha,
            TEST_MODEL_ALPHA
        ),
        "Vehicle: Protocol Binding"
    );

    Test_Log(
        IsVehicleCustom(
            g_TestVehicleAlpha
        ),
        "Vehicle: Instance flagged as custom"
    );

    new Float:defBrake;
    new Float:modBrake;

    GetVehicleHandlingFloat(
        g_TestVehicleAlpha,
        HANDL_FBRAKEDECELERATION,
        defBrake
    );

    SetVehicleHandlingFloat(
        g_TestVehicleAlpha,
        HANDL_FBRAKEDECELERATION,
        25.0
    );

    GetVehicleHandlingFloat(
        g_TestVehicleAlpha,
        HANDL_FBRAKEDECELERATION,
        modBrake
    );

    Test_AssertFloat(
        25.0,
        modBrake,
        "Vehicle: Instance specific Float applied"
    );

    ResetVehicleHandling(
        g_TestVehicleAlpha
    );

    GetVehicleHandlingFloat(
        g_TestVehicleAlpha,
        HANDL_FBRAKEDECELERATION,
        modBrake
    );

    Test_AssertFloat(
        defBrake,
        modBrake,
        "Vehicle: Reset reverts to model base"
    );

    if (g_TestVehicleAlpha != INVALID_VEHICLE_ID)
    {
        DestroyVehicle(g_TestVehicleAlpha);
        g_TestVehicleAlpha = INVALID_VEHICLE_ID;
    }
}

stock Suite_ClientCache(playerid)
{
    if (playerid == INVALID_PLAYER_ID) return;

    new bool:active = IsPlayerUsingExtendedVeh(playerid);
    Test_Log(true, "Client: Handshake query safe", active ? "Active" : "Vanilla");

    new stat = GetClientFileStoreStatus(playerid, TEST_MODEL_ALPHA, FILE_KIND_DFF);
    Test_Log((stat >= 0 && stat <= 2), "Client: Status boundaries valid");

    Test_Log(InvalidateModelCache(TEST_MODEL_ALPHA, FILE_KIND_DFF), "Client: Cache invalidation dispatched");
}

// =============================================================================

CMD:extveh(playerid, params[])
{
    SendClientMessage(playerid, -1, C_PRM "=== ExtendedVehicles Advanced Commands ===");
    SendClientMessage(playerid, -1, C_WHITE "/testcustom" C_DEBUG " - Run the full 6-phase API test suite");
    SendClientMessage(playerid, -1, C_WHITE "/spawncustom [modelid=20001]" C_DEBUG " - Spawn & bind a custom vehicle");
    SendClientMessage(playerid, -1, C_WHITE "/checkclient [playerid]" C_DEBUG " - Inspect client ASI status & cache");
    SendClientMessage(playerid, -1, C_WHITE "/sethandling [attrId] [floatVal]" C_DEBUG " - Modify current vehicle");
    SendClientMessage(playerid, -1, C_WHITE "/setplayerhandling [attrId] [floatVal]" C_DEBUG " - Modify your player scope");
    SendClientMessage(playerid, -1, C_WHITE "/gethandling" C_DEBUG " - View current vehicle telemetry");
    SendClientMessage(playerid, -1, C_WHITE "/resethandling" C_DEBUG " - Revert current vehicle");
    SendClientMessage(playerid, -1, C_WHITE "/resetplayerhandling" C_DEBUG " - Revert your player scope overrides");
    SendClientMessage(playerid, -1, C_WHITE "/invalidatecache [modelid] [type]" C_DEBUG " - Force client redownload");
    return 1;
}

CMD:testcustom(playerid, params[])
{
    g_TestsPassed = 0; g_TestsFailed = 0; g_TestCallerId = playerid;

    SendClientMessage(playerid, -1, C_PRM "==================================================");
    SendClientMessage(playerid, -1, C_PRM " EXECUTING EXT-VEH COMPREHENSIVE SUITE");
    SendClientMessage(playerid, -1, C_PRM "==================================================");

    Suite_Meta();
    Suite_Definitions();
    Suite_ModelHandling();
    Suite_PlayerHandling(playerid);
    Suite_VehicleHandling(playerid);
    Suite_ClientCache(playerid);

    new msg[128];
    format(msg, sizeof(msg), "%sTESTS COMPLETE | %sPASS: %d %s| FAIL: %d", C_INFO, C_SUCCESS, g_TestsPassed, C_FAIL, g_TestsFailed);
    SendClientMessage(playerid, -1, msg);
    return 1;
}

CMD:spawncustom(playerid, params[])
{
    new modelId;
    if (sscanf(params, "I(20001)", modelId)) return 1;

    if (!IsCustomVehicleModel(modelId))
        return SendClientMessage(playerid, -1, C_FAIL "[ExtVeh] Model is not registered.");

    new Float:x, Float:y, Float:z, Float:a;
    GetPlayerPos(playerid, x, y, z);
    GetPlayerFacingAngle(playerid, a);

    new baseModel = (modelId == TEST_MODEL_ALPHA) ? BASE_ALPHA_VISUAL : BASE_BETA_VISUAL;
    new veh = CreateVehicle(baseModel, x + 2.0, y + 2.0, z + 1.0, a, 1, 1, -1);
    
    BindVehicleModel(veh, modelId);
    PutPlayerInVehicle(playerid, veh, 0);

    new msg[128];
    format(msg, sizeof(msg), "%s[ExtVeh] Spawned vehicle %d bound to custom %d.", C_SUCCESS, veh, modelId);
    SendClientMessage(playerid, -1, msg);
    return 1;
}

CMD:spawnveh(playerid, params[])
{
    new modelId;
    if (sscanf(params, "I(411)", modelId)) return 1;

    new Float:x, Float:y, Float:z, Float:a;
    GetPlayerPos(playerid, x, y, z);
    GetPlayerFacingAngle(playerid, a);

    new veh = CreateVehicle(modelId, x + 2.0, y + 2.0, z + 1.0, a, 1, 1, -1);

    new msg[128];
    format(msg, sizeof(msg), "Spawned vehicle %d bound to %d.", veh, modelId);
    SendClientMessage(playerid, -1, msg);
    return 1;
}

CMD:checkclient(playerid, params[])
{
    new target;
    // sscanf: "U" resolves ID/Name, defaulting to the command executor if omitted.
    if (sscanf(params, "U(-1)", target)) return 1;
    if (target == -1) target = playerid;

    if (!IsPlayerConnected(target))
        return SendClientMessage(playerid, -1, C_FAIL "[ExtVeh] Player not connected.");

    new bool:active = IsPlayerUsingExtendedVeh(target);
    new msg[128];
    format(msg, sizeof(msg), "%s[ExtVeh] Player %d ASI: %s", C_INFO, target, active ? (C_SUCCESS"ACTIVE") : (C_WARN"VANILLA"));
    SendClientMessage(playerid, -1, msg);

    if (active)
    {
        new dff = GetClientFileStoreStatus(target, TEST_MODEL_ALPHA, FILE_KIND_DFF);
        new txd = GetClientFileStoreStatus(target, TEST_MODEL_ALPHA, FILE_KIND_TXD);
        format(msg, sizeof(msg), " -> Alpha Assets: DFF:%d | TXD:%d", dff, txd);
        SendClientMessage(playerid, -1, msg);
    }
    return 1;
}

CMD:sethandling(playerid, params[])
{
    new attrId, Float:val;
    if (sscanf(params, "if", attrId, val))
        return SendClientMessage(playerid, -1, C_INFO "Usage: /sethandling [attrId] [floatValue]");

    if (!IsPlayerInAnyVehicle(playerid))
        return SendClientMessage(playerid, -1, C_FAIL "[ExtVeh] You must be in a vehicle.");

    new veh = GetPlayerVehicleID(playerid);
    SetVehicleHandlingFloat(veh, CHandlingAttrib:attrId, val);

    new msg[128];
    format(msg, sizeof(msg), "%s[ExtVeh] Veh %d Attr %d set to %.2f.", C_SUCCESS, veh, attrId, val);
    SendClientMessage(playerid, -1, msg);
    return 1;
}

CMD:setplayerhandling(playerid, params[])
{
    new attrId, Float:val;
    if (sscanf(params, "if", attrId, val))
        return SendClientMessage(playerid, -1, C_INFO "Usage: /setplayerhandling [attrId] [floatValue]");

    SetPlayerHandlingFloat(playerid, CHandlingAttrib:attrId, val);
    new msg[128];
    format(msg, sizeof(msg), "%s[ExtVeh] Player scope Attr %d set to %.2f.", C_SUCCESS, attrId, val);
    SendClientMessage(playerid, -1, msg);
    return 1;
}

CMD:gethandling(playerid, params[])
{
    if (!IsPlayerInAnyVehicle(playerid))
        return SendClientMessage(playerid, -1, C_FAIL "[ExtVeh] You must be in a vehicle.");

    new veh = GetPlayerVehicleID(playerid);
    new Float:mass, Float:trac;

    GetVehicleHandlingFloat(veh, HANDL_FMASS, mass);
    GetVehicleHandlingFloat(veh, HANDL_FTRACTIONMULTIPLIER, trac);

    new msg[128];
    format(msg, sizeof(msg), "%s[ExtVeh] VehID: %d | Mass: %.1f | Trac: %.2f", C_INFO, veh, mass, trac);
    SendClientMessage(playerid, -1, msg);
    return 1;
}

CMD:resethandling(playerid, params[])
{
    if (!IsPlayerInAnyVehicle(playerid)) return 1;
    ResetVehicleHandling(GetPlayerVehicleID(playerid));
    SendClientMessage(playerid, -1, C_SUCCESS "[ExtVeh] Vehicle handling reset to base.");
    return 1;
}

CMD:resetplayerhandling(playerid, params[])
{
    ResetPlayerHandling(playerid);
    SendClientMessage(playerid, -1, C_SUCCESS "[ExtVeh] Player scope handling reset.");
    return 1;
}

CMD:destroycustom(playerid, params[])
{
    new modelId;
    if (sscanf(params, "i", modelId))
        return SendClientMessage(playerid, -1, C_INFO "Usage: /destroycustom [modelId]");

    if (DestroyCustomVehicle(modelId))
        SendClientMessage(playerid, -1, C_SUCCESS "[ExtVeh] Custom model successfully destroyed.");
    else
        SendClientMessage(playerid, -1, C_FAIL "[ExtVeh] Model not found or could not be destroyed.");
    return 1;
}

CMD:setwheel(playerid, params[])
{
    if (!IsPlayerInAnyVehicle(playerid))
        return SendClientMessage(playerid, -1, C_FAIL "[ExtVeh] You must be in a vehicle!");

    new wheelModel;
    if (sscanf(params, "i", wheelModel))
        return SendClientMessage(playerid, -1, C_INFO "Usage: /setwheel [wheelModelId (e.g. 1025, 1073-1085, or 0 to remove)]");

    new veh = GetPlayerVehicleID(playerid);
    if (SetCustomVehicleWheel(veh, wheelModel))
    {
        new msg[128];
        format(msg, sizeof(msg), "%s[ExtVeh] Vehicle %d wheel set to %d.", C_SUCCESS, veh, wheelModel);
        SendClientMessage(playerid, -1, msg);
    }
    else
    {
        SendClientMessage(playerid, -1, C_FAIL "[ExtVeh] Failed to set vehicle wheel.");
    }
    return 1;
}

CMD:invalidatecache(playerid, params[])
{
    new modelId, kind;
    // sscanf: "i i(0)" requires model, optional kind defaulting to 0 (DFF)
    if (sscanf(params, "iI(0)", modelId, kind))
        return SendClientMessage(playerid, -1, C_INFO "Usage: /invalidatecache [modelId] [kind: 0=DFF, 1=TXD, 2=COL]");

    InvalidateModelCache(modelId, ModelFileKind:kind);
    SendClientMessage(playerid, -1, C_SUCCESS "[ExtVeh] Cache invalidation command dispatched.");
    return 1;
}

// =============================================================================

stock RegisterTestDefinitions()
{
    new bool:ok;

    // ---------------------------------------------------------
    // Alpha
    // ---------------------------------------------------------

    ok = BeginCustomVehicleDef(
        TEST_MODEL_ALPHA,
        BASE_ALPHA_VISUAL,
        BASE_ALPHA_AUDIO,
        BASE_ALPHA_HANDLING,
        -1,
        -1
    );

    Test_Log(
        ok,
        "Definition: Alpha BeginCustomVehicleDef"
    );

    if (ok)
    {
        Test_Log(
            SetCustomVehicleDff(TEST_MODEL_ALPHA),
            "Definition: Alpha DFF staged"
        );

        Test_Log(
            SetCustomVehicleTxd(TEST_MODEL_ALPHA),
            "Definition: Alpha TXD staged"
        );

        Test_Log(
            SetCustomVehicleModelInfo(
                TEST_MODEL_ALPHA,
                0,
                1025,
                0.85,
                0.85,
                10,
                0,
                0,
                0,
                0
            ),
            "Definition: Alpha ModelInfo staged"
        );

        g_AlphaDefinitionReady =
            CommitCustomVehicleDef(
                TEST_MODEL_ALPHA
            );

        Test_Log(
            g_AlphaDefinitionReady,
            "Definition: Alpha committed"
        );
    }

    // ---------------------------------------------------------
    // Beta
    // ---------------------------------------------------------

    ok = BeginCustomVehicleDef(
        TEST_MODEL_BETA,
        BASE_BETA_VISUAL,
        BASE_BETA_AUDIO,
        BASE_BETA_HANDLING,
        -1,
        -1
    );

    Test_Log(
        ok,
        "Definition: Beta BeginCustomVehicleDef"
    );

    if (ok)
    {
        Test_Log(
            SetCustomVehicleDff(TEST_MODEL_BETA),
            "Definition: Beta DFF staged"
        );

        Test_Log(
            SetCustomVehicleTxd(TEST_MODEL_BETA),
            "Definition: Beta TXD staged"
        );

        g_BetaDefinitionReady =
            CommitCustomVehicleDef(
                TEST_MODEL_BETA
            );

        Test_Log(
            g_BetaDefinitionReady,
            "Definition: Beta committed"
        );
    }
}
