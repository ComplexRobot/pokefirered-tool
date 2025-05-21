#include "global.h"
#include "gflib.h"
#include "m4a.h"
#include "link.h"
#include "link_rfu.h"
#include "load_save.h"
#include "m4a.h"
#include "random.h"
#include "gba/flash_internal.h"
#include "help_system.h"
#include "new_menu_helpers.h"
#include "overworld.h"
#include "play_time.h"
#include "intro.h"
#include "battle_controllers.h"
#include "scanline_effect.h"
#include "save_failed_screen.h"
#include "quest_log.h"
#include "constants/songs.h"
#include "constants/sound.h"

extern u32 intr_main[];

static void VBlankIntr(void);
static void HBlankIntr(void);
static void VCountIntr(void);
static void SerialIntr(void);
static void IntrDummy(void);

const u8 gGameVersion = GAME_VERSION;

const u8 gGameLanguage = GAME_LANGUAGE;

#if MODERN
const char BuildDateTime[] = __DATE__ " " __TIME__;
#else
#if REVISION == 0
const char BuildDateTime[] = "2004 04 26 11:20";
#else
const char BuildDateTime[] = "2004 07 20 09:30";
#endif //REVISION
#endif //MODERN

const IntrFunc gIntrTableTemplate[] =
{
    VCountIntr, // V-count interrupt
    SerialIntr, // Serial interrupt
    Timer3Intr, // Timer 3 interrupt
    HBlankIntr, // H-blank interrupt
    VBlankIntr, // V-blank interrupt
    IntrDummy,  // Timer 0 interrupt
    IntrDummy,  // Timer 1 interrupt
    IntrDummy,  // Timer 2 interrupt
    IntrDummy,  // DMA 0 interrupt
    IntrDummy,  // DMA 1 interrupt
    IntrDummy,  // DMA 2 interrupt
    IntrDummy,  // DMA 3 interrupt
    IntrDummy,  // Key interrupt
    IntrDummy,  // Game Pak interrupt
};

#define INTR_COUNT ((int)(sizeof(gIntrTableTemplate)/sizeof(IntrFunc)))

COMMON_DATA u16 gKeyRepeatStartDelay = 0;
COMMON_DATA u8 gLinkTransferringData = 0;
COMMON_DATA struct Main gMain = {0};
COMMON_DATA u16 gKeyRepeatContinueDelay = 0;
COMMON_DATA u8 gSoftResetDisabled = 0;
COMMON_DATA IntrFunc gIntrTable[INTR_COUNT] = {0};
COMMON_DATA u8 sVcountAfterSound = 0;
COMMON_DATA bool8 gLinkVSyncDisabled = 0;
COMMON_DATA u32 IntrMain_Buffer[0x200] = {0};
COMMON_DATA u8 sVcountAtIntr = 0;
COMMON_DATA u8 sVcountBeforeSound = 0;
COMMON_DATA u8 gPcmDmaCounter = 0;

static IntrFunc * const sTimerIntrFunc = gIntrTable + 0x7;

EWRAM_DATA u8 gDecompressionBuffer[0x4000] = {0};
EWRAM_DATA u16 gTrainerId = 0;
// EWRAM_DATA u16 currentSong = MUS_GAME_FREAK;


static void UpdateLinkAndCallCallbacks(void);
static void InitMainCallbacks(void);
static void CallCallbacks(void);
static void ReadKeys(void);
void InitIntrHandlers(void);
static void WaitForVBlank(void);
void EnableVCountIntrAtLine150(void);

#define B_START_SELECT (B_BUTTON | START_BUTTON | SELECT_BUTTON)

void AgbMain()
{
#if MODERN
    // Modern compilers are liberal with the stack on entry to this function,
    // so RegisterRamReset may crash if it resets IWRAM.
    RegisterRamReset(RESET_ALL & ~RESET_IWRAM);
    asm("mov\tr1, #0xC0\n"
        "\tlsl\tr1, r1, #0x12\n"
        "\tmov\tr2, #0xFC\n"
        "\tlsl\tr2, r2, #0x7\n"
        "\tadd\tr2, r1, r2\n"
        "\tmov\tr0, #0\n"
        "\tmov\tr3, r0\n"
        "\tmov\tr4, r0\n"
        "\tmov\tr5, r0\n"
        ".LCU%=:\n"
        "\tstmia\tr1!, {r0, r3, r4, r5}\n"
        "\tstmia\tr1!, {r0, r3, r4, r5}\n"
        "\tstmia\tr1!, {r0, r3, r4, r5}\n"
        "\tstmia\tr1!, {r0, r3, r4, r5}\n"
        "\tstmia\tr1!, {r0, r3, r4, r5}\n"
        "\tstmia\tr1!, {r0, r3, r4, r5}\n"
        "\tstmia\tr1!, {r0, r3, r4, r5}\n"
        "\tstmia\tr1!, {r0, r3, r4, r5}\n"
        "\tcmp\tr1, r2\n"
        "\tbcc\t.LCU%=\n"
        :
        :
        : "r0", "r1", "r2", "r3", "r4", "r5", "memory"
    );
#else
    RegisterRamReset(RESET_ALL);
#endif //MODERN
    *(vu16 *)BG_PLTT = RGB_WHITE;
    InitGpuRegManager();
    REG_WAITCNT = WAITCNT_PREFETCH_ENABLE | WAITCNT_WS0_S_1 | WAITCNT_WS0_N_3;
    InitKeys();
    InitIntrHandlers();
    m4aSoundInit();
    EnableVCountIntrAtLine150();
    InitRFU();
    CheckForFlashMemory();
    InitMainCallbacks();
    InitMapMusic();
    ClearDma3Requests();
    ResetBgs();
    InitHeap(gHeap, HEAP_SIZE);
    SetDefaultFontsPointer();

    gSoftResetDisabled = FALSE;
    gHelpSystemEnabled = FALSE;

    SetNotInSaveFailedScreen();

#ifndef NDEBUG
#if (LOG_HANDLER == LOG_HANDLER_MGBA_PRINT)
    (void) MgbaOpen();
#elif (LOG_HANDLER == LOG_HANDLER_AGB_PRINT)
    AGBPrintInit();
#endif
#endif

#if REVISION == 1
    if (gFlashMemoryPresent != TRUE)
        SetMainCallback2(NULL);
#endif

    gLinkTransferringData = FALSE;

    gMain.currentSong = SE_USE_ITEM;

    for (;;)
    {
        ReadKeys();

        if (gSoftResetDisabled == FALSE
         && (gMain.heldKeysRaw & A_BUTTON)
         && (gMain.heldKeysRaw & B_START_SELECT) == B_START_SELECT)
        {
            rfu_REQ_stopMode();
            rfu_waitREQComplete();
            DoSoftReset();
        }

        if (Overworld_SendKeysToLinkIsRunning() == TRUE)
        {
            gLinkTransferringData = TRUE;
            UpdateLinkAndCallCallbacks();
            gLinkTransferringData = FALSE;
        }
        else
        {
            gLinkTransferringData = FALSE;
            UpdateLinkAndCallCallbacks();

            if (Overworld_RecvKeysFromLinkIsRunning() == 1)
            {
                gMain.newKeys = 0;
                ClearSpriteCopyRequests();
                gLinkTransferringData = TRUE;
                UpdateLinkAndCallCallbacks();
                gLinkTransferringData = FALSE;
            }
        }

        PlayTimeCounter_Update();
        // MapMusicMain();
        WaitForVBlank();
    }
}

const struct
{
    u16 duration;
    u8 looping;
} musicData[] =
{
    // Used regex on song-data.csv (convert to all  uppercase first):
    // Find:
    // ^[^,]+,([^,]+),([^,]+),([^,]+),[^,]+,[^,]+$
    // Replacement:
    //     [\1] =\n    {\n        .duration = \2 * 60 + 0.999,\n        .looping = \3,\n    },
    //
    [MUS_BERRY_PICK] =
    {
        .duration = 42.567567567567565 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_CAUGHT] =
    {
        .duration = 13.928571428571429 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_CAUGHT_INTRO] =
    {
        .duration = 3.5569852941176472 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_CELADON] =
    {
        .duration = 39.272727272727273 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_CINNABAR] =
    {
        .duration = 36 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_CREDITS] =
    {
        .duration = 249.48861022508871 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_CYCLING] =
    {
        .duration = 34.253731343283583 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_DEX_RATING] =
    {
        .duration = 3.3065878378378377 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_DUMMY] =
    {
        .duration = 0 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_ENCOUNTER_BOY] =
    {
        .duration = 15.584415584415584 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_ENCOUNTER_DEOXYS] =
    {
        .duration = 15 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_ENCOUNTER_GIRL] =
    {
        .duration = 6.7241379310344831 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_ENCOUNTER_GYM_LEADER] =
    {
        .duration = 8.7804878048780495 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_ENCOUNTER_RIVAL] =
    {
        .duration = 20 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_ENCOUNTER_ROCKET] =
    {
        .duration = 8.6301369863013697 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_EVOLUTION] =
    {
        .duration = 32 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_EVOLUTION_INTRO] =
    {
        .duration = 1 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_EVOLVED] =
    {
        .duration = 3.6000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_FOLLOW_ME] =
    {
        .duration = 16.438356164383563 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_FUCHSIA] =
    {
        .duration = 34 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_GAME_CORNER] =
    {
        .duration = 43.150684931506852 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_GAME_FREAK] =
    {
        .duration = 8.6999999999999993 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_GYM] =
    {
        .duration = 33.75 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_HALL_OF_FAME] =
    {
        .duration = 28.421052631578949 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_HEAL] =
    {
        .duration = 2.2727272727272729 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_HEAL_UNUSED] =
    {
        .duration = 2.0547945205479454 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_INTRO_FIGHT] =
    {
        .duration = 12.127659574468085 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_JIGGLYPUFF] =
    {
        .duration = 6.5753424657534243 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_LAVENDER] =
    {
        .duration = 82.5 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_LEVEL_UP] =
    {
        .duration = 1.0344827586206897 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_MOVE_DELETED] =
    {
        .duration = 2.6000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_MT_MOON] =
    {
        .duration = 92.093658585818872 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_MYSTERY_GIFT] =
    {
        .duration = 17.419354838709676 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_NET_CENTER] =
    {
        .duration = 33.684210526315788 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_NEW_GAME_EXIT] =
    {
        .duration = 2 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_NEW_GAME_INSTRUCT] =
    {
        .duration = 8.25 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_NEW_GAME_INTRO] =
    {
        .duration = 12 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_OAK] =
    {
        .duration = 37.5 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_OAK_LAB] =
    {
        .duration = 17 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_OBTAIN_BADGE] =
    {
        .duration = 5 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_OBTAIN_BERRY] =
    {
        .duration = 1.7142857142857142 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_OBTAIN_ITEM] =
    {
        .duration = 2.2105263157894739 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_OBTAIN_KEY_ITEM] =
    {
        .duration = 2.6325757575757573 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_OBTAIN_TMHM] =
    {
        .duration = 3 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_PALLET] =
    {
        .duration = 87.272727272727266 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_PEWTER] =
    {
        .duration = 61.428571428571431 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_PHOTO] =
    {
        .duration = 1.25 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_POKE_CENTER] =
    {
        .duration = 33.103448275862071 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_POKE_FLUTE] =
    {
        .duration = 51 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_POKE_JUMP] =
    {
        .duration = 45.652173913043477 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_POKE_MANSION] =
    {
        .duration = 116.36363636363636 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_POKE_TOWER] =
    {
        .duration = 59.104477611940297 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_RIVAL_EXIT] =
    {
        .duration = 18.571428571428573 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_ROCKET_HIDEOUT] =
    {
        .duration = 73.84615384615384 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_ROUTE1] =
    {
        .duration = 49.91379310344827 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_ROUTE11] =
    {
        .duration = 65.806451612903217 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_ROUTE24] =
    {
        .duration = 20.847457627118644 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_ROUTE3] =
    {
        .duration = 36.048387096774192 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_RS_VS_GYM_LEADER] =
    {
        .duration = 75.91836734693878 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_RS_VS_TRAINER] =
    {
        .duration = 89.696969696969703 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_SCHOOL] =
    {
        .duration = 37.241379310344826 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_SEVII_123] =
    {
        .duration = 61.428571428571431 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_SEVII_45] =
    {
        .duration = 52.25 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_SEVII_67] =
    {
        .duration = 43.448275862068968 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_SEVII_CAVE] =
    {
        .duration = 92.093658585818872 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_SEVII_DUNGEON] =
    {
        .duration = 105.234375 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_SEVII_ROUTE] =
    {
        .duration = 37.258064516129032 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_SILPH] =
    {
        .duration = 59.113645230203623 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_SLOTS_JACKPOT] =
    {
        .duration = 3.75 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_SLOTS_WIN] =
    {
        .duration = 2.5 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_SLOW_PALLET] =
    {
        .duration = 106.66666666666667 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_SS_ANNE] =
    {
        .duration = 71.578947368421055 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_SURF] =
    {
        .duration = 32.399999999999999 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_TEACHY_TV_MENU] =
    {
        .duration = 3.75 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_TEACHY_TV_SHOW] =
    {
        .duration = 16.438356164383563 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_TITLE] =
    {
        .duration = 45 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_TOO_BAD] =
    {
        .duration = 2.3333333333333335 * 60 + 0.999,
        .looping = FALSE,
    },
    [MUS_TRAINER_TOWER] =
    {
        .duration = 33.75 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_UNION_ROOM] =
    {
        .duration = 47.014925373134325 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_VERMILLION] =
    {
        .duration = 37.241379310344826 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_VICTORY_GYM_LEADER] =
    {
        .duration = 47.826086956521742 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_VICTORY_ROAD] =
    {
        .duration = 60.441176470588232 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_VICTORY_TRAINER] =
    {
        .duration = 16.176470588235293 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_VICTORY_WILD] =
    {
        .duration = 15.589285714285714 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_VIRIDIAN_FOREST] =
    {
        .duration = 105.234375 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_VS_CHAMPION] =
    {
        .duration = 73.95348837209302 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_VS_DEOXYS] =
    {
        .duration = 81.44125964603856 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_VS_GYM_LEADER] =
    {
        .duration = 110.96774193548387 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_VS_LEGEND] =
    {
        .duration = 44.835164835164832 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_VS_MEWTWO] =
    {
        .duration = 44.835164835164832 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_VS_TRAINER] =
    {
        .duration = 101.86046511627907 * 60 + 0.999,
        .looping = TRUE,
    },
    [MUS_VS_WILD] =
    {
        .duration = 44.835164835164832 * 60 + 0.999,
        .looping = TRUE,
    },
    [SE_APPLAUSE] =
    {
        .duration = 4.7999999999999998 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_BALL] =
    {
        .duration = 0.1171875 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_BALLOON_BLUE] =
    {
        .duration = 0.1875 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_BALLOON_RED] =
    {
        .duration = 0.25 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_BALLOON_YELLOW] =
    {
        .duration = 0.1875 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_BALL_BOUNCE_1] =
    {
        .duration = 0.20000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_BALL_BOUNCE_2] =
    {
        .duration = 0.20000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_BALL_BOUNCE_3] =
    {
        .duration = 0.20000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_BALL_BOUNCE_4] =
    {
        .duration = 0.20000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_BALL_OPEN] =
    {
        .duration = 0.41666666666666669 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_BALL_THROW] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_BALL_TRADE] =
    {
        .duration = 0 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_BALL_TRAY_BALL] =
    {
        .duration = 0.10000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_BALL_TRAY_ENTER] =
    {
        .duration = 0.20000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_BALL_TRAY_EXIT] =
    {
        .duration = 0.050000000000000003 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_BANG] =
    {
        .duration = 0.75 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_BERRY_BLENDER] =
    {
        .duration = 4 * 60 + 0.999,
        .looping = TRUE,
    },
    [SE_BIKE_BELL] =
    {
        .duration = 2 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_BIKE_HOP] =
    {
        .duration = 0.083333333333333329 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_BOO] =
    {
        .duration = 0.29999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_BREAKABLE_DOOR] =
    {
        .duration = 0.3125 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_BRIDGE_WALK] =
    {
        .duration = 0.14999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_CLICK] =
    {
        .duration = 0.14999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_CONTEST_CONDITION_LOSE] =
    {
        .duration = 0.14999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_CONTEST_CURTAIN_FALL] =
    {
        .duration = 0.20000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_CONTEST_CURTAIN_RISE] =
    {
        .duration = 0.20000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_CONTEST_HEART] =
    {
        .duration = 0.20000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_CONTEST_ICON_CHANGE] =
    {
        .duration = 0.10000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_CONTEST_ICON_CLEAR] =
    {
        .duration = 0.10000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_CONTEST_MONS_TURN] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_CONTEST_PLACE] =
    {
        .duration = 0.625 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_DEX_SEARCH] =
    {
        .duration = 1.25 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_DING_DONG] =
    {
        .duration = 1.05 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_DOOR] =
    {
        .duration = 0.54545454545454541 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_DOWNPOUR] =
    {
        .duration = 0.68181818181818177 * 60 + 0.999,
        .looping = TRUE,
    },
    [SE_DOWNPOUR_STOP] =
    {
        .duration = 0.54545454545454541 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_EFFECTIVE] =
    {
        .duration = 1.1666666666666667 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_EGG_HATCH] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_ELEVATOR] =
    {
        .duration = 3.6000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_ESCALATOR] =
    {
        .duration = 2 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_EXIT] =
    {
        .duration = 0.625 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_EXP] =
    {
        .duration = 1.625 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_EXP_MAX] =
    {
        .duration = 0.29999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_FAILURE] =
    {
        .duration = 0.34375 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_FAINT] =
    {
        .duration = 0.83333333333333337 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_FALL] =
    {
        .duration = 0.70588235294117652 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_FIELD_POISON] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_FLEE] =
    {
        .duration = 0.65000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_FU_ZAKU] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_GLASS_FLUTE] =
    {
        .duration = 0.375 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_ICE_BREAK] =
    {
        .duration = 0.3125 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_ICE_CRACK] =
    {
        .duration = 0.14999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_ICE_STAIRS] =
    {
        .duration = 0.29999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_INTRO_BLAST] =
    {
        .duration = 3 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_ITEMFINDER] =
    {
        .duration = 0.5859375 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_LAVARIDGE_FALL_WARP] =
    {
        .duration = 1.125 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_LEDGE] =
    {
        .duration = 0.1875 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_LOW_HEALTH] =
    {
        .duration = 0.59999999999999998 * 60 + 0.999,
        .looping = TRUE,
    },
    [SE_MUD_BALL] =
    {
        .duration = 0.14999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_MUGSHOT] =
    {
        .duration = 2.4545454545454546 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_BIND] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_COMET_PUNCH] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_CUT] =
    {
        .duration = 0.54545454545454541 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_DOUBLE_SLAP] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_FIRE_PUNCH] =
    {
        .duration = 0.81818181818181823 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_FLY] =
    {
        .duration = 0.54545454545454541 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_GUST] =
    {
        .duration = 1.2 * 60 + 0.999,
        .looping = TRUE,
    },
    [SE_M_GUST2] =
    {
        .duration = 0.80000000000000004 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_HEADBUTT] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_HORN_ATTACK] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_JUMP_KICK] =
    {
        .duration = 0.33333333333333331 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_LEER] =
    {
        .duration = 1 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_MEGA_KICK] =
    {
        .duration = 1.2 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_MEGA_KICK2] =
    {
        .duration = 0.54545454545454541 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_PAY_DAY] =
    {
        .duration = 0.59999999999999998 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_RAZOR_WIND] =
    {
        .duration = 0.54545454545454541 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_RAZOR_WIND2] =
    {
        .duration = 0.23999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_SAND_ATTACK] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_SCRATCH] =
    {
        .duration = 0.27272727272727271 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_SWORDS_DANCE] =
    {
        .duration = 1 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_TAIL_WHIP] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_TAKE_DOWN] =
    {
        .duration = 0.80000000000000004 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_VICEGRIP] =
    {
        .duration = 0.27272727272727271 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_WING_ATTACK] =
    {
        .duration = 0.27272727272727271 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_NOTE_A] =
    {
        .duration = 0.29999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_NOTE_B] =
    {
        .duration = 0.29999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_NOTE_C] =
    {
        .duration = 0.29999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_NOTE_C_HIGH] =
    {
        .duration = 0.29999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_NOTE_D] =
    {
        .duration = 0.29999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_NOTE_E] =
    {
        .duration = 0.29999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_NOTE_F] =
    {
        .duration = 0.29999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_NOTE_G] =
    {
        .duration = 0.29999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_NOT_EFFECTIVE] =
    {
        .duration = 0.25 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_ORB] =
    {
        .duration = 3.6818181818181817 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_PC_LOGIN] =
    {
        .duration = 0.41666666666666669 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_PC_OFF] =
    {
        .duration = 0.375 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_PC_ON] =
    {
        .duration = 0.98958333333333337 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_PIN] =
    {
        .duration = 0.375 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_POKENAV_OFF] =
    {
        .duration = 0.3125 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_POKENAV_ON] =
    {
        .duration = 0.3125 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_PUDDLE] =
    {
        .duration = 0.20000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_RAIN] =
    {
        .duration = 0.95454545454545459 * 60 + 0.999,
        .looping = TRUE,
    },
    [SE_RAIN_STOP] =
    {
        .duration = 0.54545454545454541 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_REPEL] =
    {
        .duration = 0.40909090909090912 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_ROTATING_GATE] =
    {
        .duration = 0.34090909090909088 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_ROULETTE_BALL] =
    {
        .duration = 8 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_ROULETTE_BALL2] =
    {
        .duration = 1.2 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_RS_DOOR] =
    {
        .duration = 0.54545454545454541 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_RS_SHOP] =
    {
        .duration = 0.46875 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_SAVE] =
    {
        .duration = 0.69999999999999996 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_SELECT] =
    {
        .duration = 0.14999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_SHINY] =
    {
        .duration = 0.54545454545454541 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_SHIP] =
    {
        .duration = 1.9090909090909092 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_SLIDING_DOOR] =
    {
        .duration = 0.25 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_SUCCESS] =
    {
        .duration = 0.25 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_SUPER_EFFECTIVE] =
    {
        .duration = 1.4166666666666667 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_SWITCH] =
    {
        .duration = 0.14999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_TAILLOW_WING_FLAP] =
    {
        .duration = 0.54545454545454541 * 60 + 0.999,
        .looping = TRUE,
    },
    [SE_THUNDER] =
    {
        .duration = 1.6363636363636365 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_THUNDER2] =
    {
        .duration = 1.3636363636363635 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_THUNDERSTORM] =
    {
        .duration = 0.95454545454545459 * 60 + 0.999,
        .looping = TRUE,
    },
    [SE_THUNDERSTORM_STOP] =
    {
        .duration = 0.54545454545454541 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_TRUCK_DOOR] =
    {
        .duration = 0.20000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_TRUCK_MOVE] =
    {
        .duration = 6.4000000000000004 * 60 + 0.999,
        .looping = TRUE,
    },
    [SE_TRUCK_STOP] =
    {
        .duration = 2.3999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_TRUCK_UNLOAD] =
    {
        .duration = 0.45000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_UNLOCK] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_USE_ITEM] =
    {
        .duration = 0.45454545454545453 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_VEND] =
    {
        .duration = 0.20000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_WARP_IN] =
    {
        .duration = 0.75 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_WARP_OUT] =
    {
        .duration = 0.81818181818181823 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_BAG_CURSOR] =
    {
        .duration = 0.072580645161290328 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_BAG_POCKET] =
    {
        .duration = 0.035545023696682464 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_BALL_CLICK] =
    {
        .duration = 0.16935483870967741 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_CARD_FLIP] =
    {
        .duration = 0.65934065934065933 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_CARD_FLIPPING] =
    {
        .duration = 1.6483516483516483 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_CARD_OPEN] =
    {
        .duration = 1.7241379310344827 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_DEOXYS_MOVE] =
    {
        .duration = 1.83206106870229 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_DEX_PAGE] =
    {
        .duration = 0.22500000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_DEX_SCROLL] =
    {
        .duration = 0.074999999999999997 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_HELP_CLOSE] =
    {
        .duration = 0.5714285714285714 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_HELP_ERROR] =
    {
        .duration = 0.2857142857142857 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_HELP_OPEN] =
    {
        .duration = 0.5714285714285714 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_ABSORB] =
    {
        .duration = 0.20000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_ABSORB_2] =
    {
        .duration = 0.84999999999999998 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_ACID_ARMOR] =
    {
        .duration = 1.2 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_ATTRACT] =
    {
        .duration = 0.8214285714285714 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_ATTRACT2] =
    {
        .duration = 3.6000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_BARRIER] =
    {
        .duration = 0.90000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_BATON_PASS] =
    {
        .duration = 0 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_BELLY_DRUM] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_BITE] =
    {
        .duration = 0.27272727272727271 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_BLIZZARD] =
    {
        .duration = 1.2 * 60 + 0.999,
        .looping = TRUE,
    },
    [SE_M_BLIZZARD2] =
    {
        .duration = 0.80000000000000004 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_BONEMERANG] =
    {
        .duration = 0.20000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_BRICK_BREAK] =
    {
        .duration = 0.59999999999999998 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_BUBBLE] =
    {
        .duration = 0.27272727272727271 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_BUBBLE2] =
    {
        .duration = 0.27272727272727271 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_BUBBLE3] =
    {
        .duration = 0.27272727272727271 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_BUBBLE_BEAM] =
    {
        .duration = 1.3 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_BUBBLE_BEAM2] =
    {
        .duration = 0.59999999999999998 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_CHARGE] =
    {
        .duration = 1.2 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_CHARM] =
    {
        .duration = 0.29999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_CONFUSE_RAY] =
    {
        .duration = 0.54545454545454541 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_COSMIC_POWER] =
    {
        .duration = 3.2999999999999998 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_CRABHAMMER] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_DETECT] =
    {
        .duration = 0.47727272727272729 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_DIG] =
    {
        .duration = 0.25 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_DIVE] =
    {
        .duration = 0.80000000000000004 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_DIZZY_PUNCH] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_DOUBLE_TEAM] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_DRAGON_RAGE] =
    {
        .duration = 1.6000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_EARTHQUAKE] =
    {
        .duration = 3.3999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_EMBER] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_ENCORE] =
    {
        .duration = 0.20000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_ENCORE2] =
    {
        .duration = 2.3999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_EXPLOSION] =
    {
        .duration = 1.2 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_FAINT_ATTACK] =
    {
        .duration = 0.5 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_FLAMETHROWER] =
    {
        .duration = 3.2000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_FLAME_WHEEL] =
    {
        .duration = 0.55000000000000004 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_FLAME_WHEEL2] =
    {
        .duration = 0.80000000000000004 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_FLATTER] =
    {
        .duration = 0.20000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_GIGA_DRAIN] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_GRASSWHISTLE] =
    {
        .duration = 3.2999999999999998 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_HAIL] =
    {
        .duration = 0.5 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_HARDEN] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_HAZE] =
    {
        .duration = 2.763157894736842 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_HEAL_BELL] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_HEAT_WAVE] =
    {
        .duration = 2.2000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_HYDRO_PUMP] =
    {
        .duration = 2 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_HYPER_BEAM] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_HYPER_BEAM2] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_ICY_WIND] =
    {
        .duration = 0.20000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_LICK] =
    {
        .duration = 0.59999999999999998 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_LOCK_ON] =
    {
        .duration = 0.29999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_METRONOME] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_MILK_DRINK] =
    {
        .duration = 0.41666666666666669 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_MINIMIZE] =
    {
        .duration = 0.90000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_MIST] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_MOONLIGHT] =
    {
        .duration = 4.7999999999999998 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_MORNING_SUN] =
    {
        .duration = 1.25 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_NIGHTMARE] =
    {
        .duration = 1.0909090909090908 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_PERISH_SONG] =
    {
        .duration = 3.1499999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_PETAL_DANCE] =
    {
        .duration = 2.25 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_POISON_POWDER] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_PSYBEAM] =
    {
        .duration = 0.55263157894736847 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_PSYBEAM2] =
    {
        .duration = 0.78947368421052633 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_RAIN_DANCE] =
    {
        .duration = 1.9090909090909092 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_REFLECT] =
    {
        .duration = 0.90000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_REVERSAL] =
    {
        .duration = 1.1000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_ROCK_THROW] =
    {
        .duration = 0.80000000000000004 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_SACRED_FIRE] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_SACRED_FIRE2] =
    {
        .duration = 1.6000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_SANDSTORM] =
    {
        .duration = 2.3999999999999999 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_SAND_TOMB] =
    {
        .duration = 1.6000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_SCREECH] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_SELF_DESTRUCT] =
    {
        .duration = 0.80000000000000004 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_SING] =
    {
        .duration = 4.3499999999999996 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_SKETCH] =
    {
        .duration = 0.16666666666666666 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_SKY_UPPERCUT] =
    {
        .duration = 0.40909090909090912 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_SNORE] =
    {
        .duration = 0.34090909090909088 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_SOLAR_BEAM] =
    {
        .duration = 3.2000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_SPIT_UP] =
    {
        .duration = 0.25 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_STAT_DECREASE] =
    {
        .duration = 1.1470588235294117 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_STAT_INCREASE] =
    {
        .duration = 1.1470588235294117 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_STRENGTH] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_STRING_SHOT] =
    {
        .duration = 0.47999999999999998 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_STRING_SHOT2] =
    {
        .duration = 0.95999999999999996 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_SUPERSONIC] =
    {
        .duration = 0.69999999999999996 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_SURF] =
    {
        .duration = 1.6000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_SWAGGER] =
    {
        .duration = 0.23684210526315788 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_SWAGGER2] =
    {
        .duration = 0.20000000000000001 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_SWEET_SCENT] =
    {
        .duration = 2.7999999999999998 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_SWIFT] =
    {
        .duration = 0.25 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_TEETER_DANCE] =
    {
        .duration = 0.88235294117647056 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_TELEPORT] =
    {
        .duration = 0.90000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_THUNDERBOLT] =
    {
        .duration = 0.54545454545454541 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_THUNDERBOLT2] =
    {
        .duration = 0.54545454545454541 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_THUNDER_WAVE] =
    {
        .duration = 0.80000000000000004 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_TOXIC] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_TRI_ATTACK] =
    {
        .duration = 0.27272727272727271 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_TRI_ATTACK2] =
    {
        .duration = 1.0909090909090908 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_TWISTER] =
    {
        .duration = 2.7999999999999998 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_UPROAR] =
    {
        .duration = 0.40000000000000002 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_VITAL_THROW] =
    {
        .duration = 0.27272727272727271 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_VITAL_THROW2] =
    {
        .duration = 0.27272727272727271 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_WATERFALL] =
    {
        .duration = 1 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_WHIRLPOOL] =
    {
        .duration = 2 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_M_YAWN] =
    {
        .duration = 0.80000000000000004 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_POKE_JUMP_FAILURE] =
    {
        .duration = 0.34375 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_POKE_JUMP_SUCCESS] =
    {
        .duration = 0.84999999999999998 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_SHOP] =
    {
        .duration = 0.60483870967741937 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_SS_ANNE_HORN] =
    {
        .duration = 3.6585365853658538 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_WALL_HIT] =
    {
        .duration = 0.27272727272727271 * 60 + 0.999,
        .looping = FALSE,
    },
    [SE_WIN_OPEN] =
    {
        .duration = 0.27272727272727271 * 60 + 0.999,
        .looping = FALSE,
    },
};

#define WAIT_FRAMES 120

void CB_PlayAllSongs(void)
{
    const struct MusicPlayer *mplayTable = gMPlayTable;
    const struct Song *songTable = gSongTable;
    const struct Song *song = &songTable[gMain.currentSong];
    const struct MusicPlayer *mplay = &mplayTable[song->ms];
    struct MusicPlayerInfo *mplayInfo = mplay->info;
    // struct SongHeader *songHeader = song->header;

    switch (gMain.state)
    {
    default:
        if (gMain.state < WAIT_FRAMES)
        {
            ++gMain.state;
        }
        break;
    case WAIT_FRAMES:
        gMain.songPlayTime = 0;
        m4aSongNumStart(gMain.currentSong);
        ++gMain.state;
        break;
    case WAIT_FRAMES + 1:
        if (musicData[gMain.currentSong].looping)
        {
            // Wait 2 seconds after the song loops
            if (++gMain.songPlayTime < musicData[gMain.currentSong].duration + 2 * 60)
            {
                break;
            }
            else
            {
                m4aSongNumStop(gMain.currentSong);
            }
        }
        else if (!(mplayInfo->status & MUSICPLAYER_STATUS_PAUSE))
        {
            break;
        }

        if (gMain.currentSong < MUS_TEACHY_TV_MENU)
        {
            ++gMain.currentSong;
            gMain.state = 0;
        }
        else
        {
            ++gMain.state;
        }
        break;
    }
}

static void UpdateLinkAndCallCallbacks(void)
{
    if (!HandleLinkConnection())
        CallCallbacks();
}

static void InitMainCallbacks(void)
{
    gMain.vblankCounter1 = 0;
    gMain.vblankCounter2 = 0;
    gMain.callback1 = NULL;
    // SetMainCallback2(CB2_InitCopyrightScreenAfterBootup);
    SetMainCallback2(CB_PlayAllSongs);
    gSaveBlock2Ptr = &gSaveBlock2;
    gSaveBlock1Ptr = &gSaveBlock1;
    gSaveBlock2.encryptionKey = 0;
    gQuestLogPlaybackState = QL_PLAYBACK_STATE_STOPPED;
}

static void CallCallbacks(void)
{
    if (!RunSaveFailedScreen() && !RunHelpSystemCallback())
    {
        if (gMain.callback1)
            gMain.callback1();

        if (gMain.callback2)
            gMain.callback2();
    }
}

void SetMainCallback2(MainCallback callback)
{
    gMain.callback2 = callback;
    gMain.state = 0;
}

void StartTimer1(void)
{
    REG_TM1CNT_H = 0x80;
}

void SeedRngAndSetTrainerId(void)
{
    u16 val = REG_TM1CNT_L;
    SeedRng(val);
    REG_TM1CNT_H = 0;
    gTrainerId = val;
}

u16 GetGeneratedTrainerIdLower(void)
{
    return gTrainerId;
}

void EnableVCountIntrAtLine150(void)
{
    u16 gpuReg = (GetGpuReg(REG_OFFSET_DISPSTAT) & 0xFF) | (150 << 8);
    SetGpuReg(REG_OFFSET_DISPSTAT, gpuReg | DISPSTAT_VCOUNT_INTR);
    EnableInterrupts(INTR_FLAG_VCOUNT);
}

void InitKeys(void)
{
    gKeyRepeatContinueDelay = 5;
    gKeyRepeatStartDelay = 40;

    gMain.heldKeys = 0;
    gMain.newKeys = 0;
    gMain.newAndRepeatedKeys = 0;
    gMain.heldKeysRaw = 0;
    gMain.newKeysRaw = 0;
}

static void ReadKeys(void)
{
    u16 keyInput = REG_KEYINPUT ^ KEYS_MASK;
    gMain.newKeysRaw = keyInput & ~gMain.heldKeysRaw;
    gMain.newKeys = gMain.newKeysRaw;
    gMain.newAndRepeatedKeys = gMain.newKeysRaw;

    // BUG: Key repeat won't work when pressing L using L=A button mode
    // because it compares the raw key input with the remapped held keys.
    // Note that newAndRepeatedKeys is never remapped either.

    if (keyInput != 0 && gMain.heldKeys == keyInput)
    {
        gMain.keyRepeatCounter--;

        if (gMain.keyRepeatCounter == 0)
        {
            gMain.newAndRepeatedKeys = keyInput;
            gMain.keyRepeatCounter = gKeyRepeatContinueDelay;
        }
    }
    else
    {
        // If there is no input or the input has changed, reset the counter.
        gMain.keyRepeatCounter = gKeyRepeatStartDelay;
    }

    gMain.heldKeysRaw = keyInput;
    gMain.heldKeys = gMain.heldKeysRaw;

    // Remap L to A if the L=A option is enabled.
    if (gSaveBlock2Ptr->optionsButtonMode == OPTIONS_BUTTON_MODE_L_EQUALS_A)
    {
        if (JOY_NEW(L_BUTTON))
            gMain.newKeys |= A_BUTTON;

        if (JOY_HELD(L_BUTTON))
            gMain.heldKeys |= A_BUTTON;
    }

    if (JOY_NEW(gMain.watchedKeysMask))
        gMain.watchedKeysPressed = TRUE;
}

void InitIntrHandlers(void)
{
    int i;

    for (i = 0; i < INTR_COUNT; i++)
        gIntrTable[i] = gIntrTableTemplate[i];

    DmaCopy32(3, intr_main, IntrMain_Buffer, sizeof(IntrMain_Buffer));

    INTR_VECTOR = IntrMain_Buffer;

    SetVBlankCallback(NULL);
    SetHBlankCallback(NULL);
    SetSerialCallback(NULL);

    REG_IME = 1;

    EnableInterrupts(INTR_FLAG_VBLANK);
}

void SetVBlankCallback(IntrCallback callback)
{
    gMain.vblankCallback = callback;
}

void SetHBlankCallback(IntrCallback callback)
{
    gMain.hblankCallback = callback;
}

void SetVCountCallback(IntrCallback callback)
{
    gMain.vcountCallback = callback;
}

void SetSerialCallback(IntrCallback callback)
{
    gMain.serialCallback = callback;
}

extern void CopyBufferedValuesToGpuRegs(void);
extern void ProcessDma3Requests(void);

static void VBlankIntr(void)
{
    if (gWirelessCommType)
        RfuVSync();
    else if (!gLinkVSyncDisabled)
        LinkVSync();

    if (gMain.vblankCounter1)
        (*gMain.vblankCounter1)++;

    if (gMain.vblankCallback)
        gMain.vblankCallback();

    gMain.vblankCounter2++;

    CopyBufferedValuesToGpuRegs();
    ProcessDma3Requests();

    gPcmDmaCounter = gSoundInfo.pcmDmaCounter;

#ifndef NDEBUG
    sVcountBeforeSound = REG_VCOUNT;
#endif
    m4aSoundMain();
#ifndef NDEBUG
    sVcountAfterSound = REG_VCOUNT;
#endif

    TryReceiveLinkBattleData();
    Random();
    UpdateWirelessStatusIndicatorSprite();

    INTR_CHECK |= INTR_FLAG_VBLANK;
    gMain.intrCheck |= INTR_FLAG_VBLANK;
}

void InitFlashTimer(void)
{
    IntrFunc **func = (IntrFunc **)&sTimerIntrFunc;
    SetFlashTimerIntr(2, *func);
}

static void HBlankIntr(void)
{
    if (gMain.hblankCallback)
        gMain.hblankCallback();

    INTR_CHECK |= INTR_FLAG_HBLANK;
    gMain.intrCheck |= INTR_FLAG_HBLANK;
}

static void VCountIntr(void)
{
#ifndef NDEBUG
    sVcountAtIntr = REG_VCOUNT;
#endif
    m4aSoundVSync();
    INTR_CHECK |= INTR_FLAG_VCOUNT;
    gMain.intrCheck |= INTR_FLAG_VCOUNT;
}

static void SerialIntr(void)
{
    if (gMain.serialCallback)
        gMain.serialCallback();

    INTR_CHECK |= INTR_FLAG_SERIAL;
    gMain.intrCheck |= INTR_FLAG_SERIAL;
}

void RestoreSerialTimer3IntrHandlers(void)
{
    gIntrTable[1] = SerialIntr;
    gIntrTable[2] = Timer3Intr;
}

static void IntrDummy(void)
{}

static void WaitForVBlank(void)
{
    gMain.intrCheck &= ~INTR_FLAG_VBLANK;

    while (!(gMain.intrCheck & INTR_FLAG_VBLANK))
        ;
}

void SetVBlankCounter1Ptr(u32 *ptr)
{
    gMain.vblankCounter1 = ptr;
}

void DisableVBlankCounter1(void)
{
    gMain.vblankCounter1 = NULL;
}

void DoSoftReset(void)
{
    REG_IME = 0;
    m4aSoundVSyncOff();
    ScanlineEffect_Stop();
    DmaStop(1);
    DmaStop(2);
    DmaStop(3);
    SoftReset(RESET_ALL & ~RESET_SIO_REGS);
}

void ClearPokemonCrySongs(void)
{
    CpuFill16(0, gPokemonCrySongs, MAX_POKEMON_CRIES * sizeof(struct PokemonCrySong));
}
