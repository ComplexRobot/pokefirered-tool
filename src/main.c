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
    u8 tempo;
    u32 duration;
    u8 looping;
} musicData[] =
{
    // Used regex on song-data.csv (convert to all  uppercase first):
    // Find:
    // ^[^,]+,([^,]+),([^,]+),([^,]+),([^,]+),[^,]+,[^,]+$
    // Replacement:
    //     [\1] =\n    {\n        .tempo = \2,\n        .duration = \3 * 80,\n        .looping = \4,\n    },
    //
    [MUS_BERRY_PICK] =
    {
        .tempo = 74,
        .duration = 2520 * 80,
        .looping = TRUE,
    },
    [MUS_CAUGHT] =
    {
        .tempo = 70,
        .duration = 780 * 80,
        .looping = TRUE,
    },
    [MUS_CAUGHT_INTRO] =
    {
        .tempo = 68,
        .duration = 180 * 80,
        .looping = FALSE,
    },
    [MUS_CELADON] =
    {
        .tempo = 55,
        .duration = 1728 * 80,
        .looping = TRUE,
    },
    [MUS_CINNABAR] =
    {
        .tempo = 60,
        .duration = 1728 * 80,
        .looping = TRUE,
    },
    [MUS_CREDITS] =
    {
        .tempo = 65,
        .duration = 11664 * 80,
        .looping = FALSE,
    },
    [MUS_CYCLING] =
    {
        .tempo = 67,
        .duration = 1836 * 80,
        .looping = TRUE,
    },
    [MUS_DEX_RATING] =
    {
        .tempo = 37,
        .duration = 96 * 80,
        .looping = FALSE,
    },
    [MUS_DUMMY] =
    {
        .tempo = -1,
        .duration = 0 * 80,
        .looping = FALSE,
    },
    [MUS_ENCOUNTER_BOY] =
    {
        .tempo = 77,
        .duration = 576 * 80,
        .looping = TRUE,
    },
    [MUS_ENCOUNTER_DEOXYS] =
    {
        .tempo = 64,
        .duration = 768 * 80,
        .looping = TRUE,
    },
    [MUS_ENCOUNTER_GIRL] =
    {
        .tempo = 87,
        .duration = 468 * 80,
        .looping = TRUE,
    },
    [MUS_ENCOUNTER_GYM_LEADER] =
    {
        .tempo = 82,
        .duration = 576 * 80,
        .looping = TRUE,
    },
    [MUS_ENCOUNTER_RIVAL] =
    {
        .tempo = 84,
        .duration = 1344 * 80,
        .looping = TRUE,
    },
    [MUS_ENCOUNTER_ROCKET] =
    {
        .tempo = 73,
        .duration = 504 * 80,
        .looping = TRUE,
    },
    [MUS_EVOLUTION] =
    {
        .tempo = 60,
        .duration = 1536 * 80,
        .looping = TRUE,
    },
    [MUS_EVOLUTION_INTRO] =
    {
        .tempo = 60,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [MUS_EVOLVED] =
    {
        .tempo = 75,
        .duration = 228 * 80,
        .looping = FALSE,
    },
    [MUS_FOLLOW_ME] =
    {
        .tempo = 73,
        .duration = 960 * 80,
        .looping = TRUE,
    },
    [MUS_FUCHSIA] =
    {
        .tempo = 60,
        .duration = 1632 * 80,
        .looping = TRUE,
    },
    [MUS_GAME_CORNER] =
    {
        .tempo = 73,
        .duration = 2520 * 80,
        .looping = TRUE,
    },
    [MUS_GAME_FREAK] =
    {
        .tempo = 75,
        .duration = 522 * 80,
        .looping = FALSE,
    },
    [MUS_GYM] =
    {
        .tempo = 64,
        .duration = 1728 * 80,
        .looping = TRUE,
    },
    [MUS_HALL_OF_FAME] =
    {
        .tempo = 76,
        .duration = 1728 * 80,
        .looping = TRUE,
    },
    [MUS_HEAL] =
    {
        .tempo = 66,
        .duration = 120 * 80,
        .looping = FALSE,
    },
    [MUS_HEAL_UNUSED] =
    {
        .tempo = 73,
        .duration = 120 * 80,
        .looping = FALSE,
    },
    [MUS_INTRO_FIGHT] =
    {
        .tempo = 94,
        .duration = 912 * 80,
        .looping = FALSE,
    },
    [MUS_JIGGLYPUFF] =
    {
        .tempo = 73,
        .duration = 384 * 80,
        .looping = FALSE,
    },
    [MUS_LAVENDER] =
    {
        .tempo = 64,
        .duration = 4224 * 80,
        .looping = TRUE,
    },
    [MUS_LEVEL_UP] =
    {
        .tempo = 87,
        .duration = 72 * 80,
        .looping = FALSE,
    },
    [MUS_MOVE_DELETED] =
    {
        .tempo = 75,
        .duration = 156 * 80,
        .looping = FALSE,
    },
    [MUS_MT_MOON] =
    {
        .tempo = 55,
        .duration = 3900 * 80,
        .looping = TRUE,
    },
    [MUS_MYSTERY_GIFT] =
    {
        .tempo = 62,
        .duration = 864 * 80,
        .looping = TRUE,
    },
    [MUS_NET_CENTER] =
    {
        .tempo = 57,
        .duration = 1536 * 80,
        .looping = TRUE,
    },
    [MUS_NEW_GAME_EXIT] =
    {
        .tempo = 60,
        .duration = 96 * 80,
        .looping = FALSE,
    },
    [MUS_NEW_GAME_INSTRUCT] =
    {
        .tempo = 60,
        .duration = 396 * 80,
        .looping = TRUE,
    },
    [MUS_NEW_GAME_INTRO] =
    {
        .tempo = 60,
        .duration = 576 * 80,
        .looping = TRUE,
    },
    [MUS_OAK] =
    {
        .tempo = 80,
        .duration = 1632 * 80,
        .looping = TRUE,
    },
    [MUS_OAK_LAB] =
    {
        .tempo = 60,
        .duration = 816 * 80,
        .looping = TRUE,
    },
    [MUS_OBTAIN_BADGE] =
    {
        .tempo = 72,
        .duration = 300 * 80,
        .looping = FALSE,
    },
    [MUS_OBTAIN_BERRY] =
    {
        .tempo = 70,
        .duration = 108 * 80,
        .looping = FALSE,
    },
    [MUS_OBTAIN_ITEM] =
    {
        .tempo = 95,
        .duration = 180 * 80,
        .looping = FALSE,
    },
    [MUS_OBTAIN_KEY_ITEM] =
    {
        .tempo = 36,
        .duration = 72 * 80,
        .looping = FALSE,
    },
    [MUS_OBTAIN_TMHM] =
    {
        .tempo = 70,
        .duration = 180 * 80,
        .looping = FALSE,
    },
    [MUS_PALLET] =
    {
        .tempo = 44,
        .duration = 1536 * 80,
        .looping = TRUE,
    },
    [MUS_PEWTER] =
    {
        .tempo = 63,
        .duration = 3096 * 80,
        .looping = TRUE,
    },
    [MUS_PHOTO] =
    {
        .tempo = 96,
        .duration = 96 * 80,
        .looping = FALSE,
    },
    [MUS_POKE_CENTER] =
    {
        .tempo = 58,
        .duration = 1536 * 80,
        .looping = TRUE,
    },
    [MUS_POKE_FLUTE] =
    {
        .tempo = 40,
        .duration = 1632 * 80,
        .looping = FALSE,
    },
    [MUS_POKE_JUMP] =
    {
        .tempo = 69,
        .duration = 2520 * 80,
        .looping = TRUE,
    },
    [MUS_POKE_MANSION] =
    {
        .tempo = 66,
        .duration = 3456 * 80,
        .looping = TRUE,
    },
    [MUS_POKE_TOWER] =
    {
        .tempo = 67,
        .duration = 3168 * 80,
        .looping = TRUE,
    },
    [MUS_RIVAL_EXIT] =
    {
        .tempo = 84,
        .duration = 1248 * 80,
        .looping = TRUE,
    },
    [MUS_ROCKET_HIDEOUT] =
    {
        .tempo = 65,
        .duration = 3840 * 80,
        .looping = TRUE,
    },
    [MUS_ROUTE1] =
    {
        .tempo = 58,
        .duration = 1164 * 80,
        .looping = TRUE,
    },
    [MUS_ROUTE11] =
    {
        .tempo = 62,
        .duration = 1728 * 80,
        .looping = TRUE,
    },
    [MUS_ROUTE24] =
    {
        .tempo = 59,
        .duration = 984 * 80,
        .looping = TRUE,
    },
    [MUS_ROUTE3] =
    {
        .tempo = 62,
        .duration = 1788 * 80,
        .looping = TRUE,
    },
    [MUS_RS_VS_GYM_LEADER] =
    {
        .tempo = 98,
        .duration = 5952 * 80,
        .looping = TRUE,
    },
    [MUS_RS_VS_TRAINER] =
    {
        .tempo = 99,
        .duration = 7104 * 80,
        .looping = TRUE,
    },
    [MUS_SCHOOL] =
    {
        .tempo = 58,
        .duration = 1728 * 80,
        .looping = TRUE,
    },
    [MUS_SEVII_123] =
    {
        .tempo = 63,
        .duration = 3096 * 80,
        .looping = TRUE,
    },
    [MUS_SEVII_45] =
    {
        .tempo = 60,
        .duration = 2508 * 80,
        .looping = TRUE,
    },
    [MUS_SEVII_67] =
    {
        .tempo = 58,
        .duration = 2016 * 80,
        .looping = TRUE,
    },
    [MUS_SEVII_CAVE] =
    {
        .tempo = 55,
        .duration = 3900 * 80,
        .looping = TRUE,
    },
    [MUS_SEVII_DUNGEON] =
    {
        .tempo = 64,
        .duration = 5388 * 80,
        .looping = TRUE,
    },
    [MUS_SEVII_ROUTE] =
    {
        .tempo = 62,
        .duration = 1848 * 80,
        .looping = TRUE,
    },
    [MUS_SILPH] =
    {
        .tempo = 57,
        .duration = 3336 * 80,
        .looping = TRUE,
    },
    [MUS_SLOTS_JACKPOT] =
    {
        .tempo = 72,
        .duration = 252 * 80,
        .looping = FALSE,
    },
    [MUS_SLOTS_WIN] =
    {
        .tempo = 72,
        .duration = 144 * 80,
        .looping = FALSE,
    },
    [MUS_SLOW_PALLET] =
    {
        .tempo = 36,
        .duration = 1536 * 80,
        .looping = TRUE,
    },
    [MUS_SS_ANNE] =
    {
        .tempo = 57,
        .duration = 3264 * 80,
        .looping = TRUE,
    },
    [MUS_SURF] =
    {
        .tempo = 50,
        .duration = 1296 * 80,
        .looping = TRUE,
    },
    [MUS_TEACHY_TV_MENU] =
    {
        .tempo = 72,
        .duration = 216 * 80,
        .looping = TRUE,
    },
    [MUS_TEACHY_TV_SHOW] =
    {
        .tempo = 73,
        .duration = 960 * 80,
        .looping = TRUE,
    },
    [MUS_TITLE] =
    {
        .tempo = 70,
        .duration = 2520 * 80,
        .looping = TRUE,
    },
    [MUS_TOO_BAD] =
    {
        .tempo = 90,
        .duration = 168 * 80,
        .looping = FALSE,
    },
    [MUS_TRAINER_TOWER] =
    {
        .tempo = 64,
        .duration = 1728 * 80,
        .looping = TRUE,
    },
    [MUS_UNION_ROOM] =
    {
        .tempo = 67,
        .duration = 2520 * 80,
        .looping = TRUE,
    },
    [MUS_VERMILLION] =
    {
        .tempo = 58,
        .duration = 1728 * 80,
        .looping = TRUE,
    },
    [MUS_VICTORY_GYM_LEADER] =
    {
        .tempo = 69,
        .duration = 2649 * 80,
        .looping = TRUE,
    },
    [MUS_VICTORY_ROAD] =
    {
        .tempo = 68,
        .duration = 1752 * 80,
        .looping = TRUE,
    },
    [MUS_VICTORY_TRAINER] =
    {
        .tempo = 68,
        .duration = 880 * 80,
        .looping = TRUE,
    },
    [MUS_VICTORY_WILD] =
    {
        .tempo = 70,
        .duration = 873 * 80,
        .looping = TRUE,
    },
    [MUS_VIRIDIAN_FOREST] =
    {
        .tempo = 64,
        .duration = 5388 * 80,
        .looping = TRUE,
    },
    [MUS_VS_CHAMPION] =
    {
        .tempo = 86,
        .duration = 5088 * 80,
        .looping = TRUE,
    },
    [MUS_VS_DEOXYS] =
    {
        .tempo = 81,
        .duration = 5184 * 80,
        .looping = TRUE,
    },
    [MUS_VS_GYM_LEADER] =
    {
        .tempo = 93,
        .duration = 4224 * 80,
        .looping = TRUE,
    },
    [MUS_VS_LEGEND] =
    {
        .tempo = 91,
        .duration = 3264 * 80,
        .looping = TRUE,
    },
    [MUS_VS_MEWTWO] =
    {
        .tempo = 91,
        .duration = 3264 * 80,
        .looping = TRUE,
    },
    [MUS_VS_TRAINER] =
    {
        .tempo = 86,
        .duration = 7008 * 80,
        .looping = TRUE,
    },
    [MUS_VS_WILD] =
    {
        .tempo = 91,
        .duration = 3264 * 80,
        .looping = TRUE,
    },
    [SE_APPLAUSE] =
    {
        .tempo = 75,
        .duration = 288 * 80,
        .looping = FALSE,
    },
    [SE_BALL] =
    {
        .tempo = 64,
        .duration = 6 * 80,
        .looping = FALSE,
    },
    [SE_BALLOON_BLUE] =
    {
        .tempo = 120,
        .duration = 18 * 80,
        .looping = FALSE,
    },
    [SE_BALLOON_RED] =
    {
        .tempo = 120,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_BALLOON_YELLOW] =
    {
        .tempo = 120,
        .duration = 18 * 80,
        .looping = FALSE,
    },
    [SE_BALL_BOUNCE_1] =
    {
        .tempo = 75,
        .duration = 12 * 80,
        .looping = FALSE,
    },
    [SE_BALL_BOUNCE_2] =
    {
        .tempo = 75,
        .duration = 12 * 80,
        .looping = FALSE,
    },
    [SE_BALL_BOUNCE_3] =
    {
        .tempo = 75,
        .duration = 12 * 80,
        .looping = FALSE,
    },
    [SE_BALL_BOUNCE_4] =
    {
        .tempo = 75,
        .duration = 12 * 80,
        .looping = FALSE,
    },
    [SE_BALL_OPEN] =
    {
        .tempo = 45,
        .duration = 15 * 80,
        .looping = FALSE,
    },
    [SE_BALL_THROW] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_BALL_TRADE] =
    {
        .tempo = -1,
        .duration = 12 * 80,
        .looping = FALSE,
    },
    [SE_BALL_TRAY_BALL] =
    {
        .tempo = 75,
        .duration = 6 * 80,
        .looping = FALSE,
    },
    [SE_BALL_TRAY_ENTER] =
    {
        .tempo = 75,
        .duration = 12 * 80,
        .looping = FALSE,
    },
    [SE_BALL_TRAY_EXIT] =
    {
        .tempo = 75,
        .duration = 3 * 80,
        .looping = FALSE,
    },
    [SE_BANG] =
    {
        .tempo = 120,
        .duration = 72 * 80,
        .looping = FALSE,
    },
    [SE_BERRY_BLENDER] =
    {
        .tempo = 75,
        .duration = 240 * 80,
        .looping = TRUE,
    },
    [SE_BIKE_BELL] =
    {
        .tempo = 60,
        .duration = 96 * 80,
        .looping = FALSE,
    },
    [SE_BIKE_HOP] =
    {
        .tempo = 90,
        .duration = 6 * 80,
        .looping = FALSE,
    },
    [SE_BOO] =
    {
        .tempo = 50,
        .duration = 12 * 80,
        .looping = FALSE,
    },
    [SE_BREAKABLE_DOOR] =
    {
        .tempo = 120,
        .duration = 30 * 80,
        .looping = FALSE,
    },
    [SE_BRIDGE_WALK] =
    {
        .tempo = 75,
        .duration = 9 * 80,
        .looping = FALSE,
    },
    [SE_CLICK] =
    {
        .tempo = 75,
        .duration = 9 * 80,
        .looping = FALSE,
    },
    [SE_CONTEST_CONDITION_LOSE] =
    {
        .tempo = 75,
        .duration = 9 * 80,
        .looping = FALSE,
    },
    [SE_CONTEST_CURTAIN_FALL] =
    {
        .tempo = 75,
        .duration = 12 * 80,
        .looping = FALSE,
    },
    [SE_CONTEST_CURTAIN_RISE] =
    {
        .tempo = 75,
        .duration = 12 * 80,
        .looping = FALSE,
    },
    [SE_CONTEST_HEART] =
    {
        .tempo = 75,
        .duration = 12 * 80,
        .looping = FALSE,
    },
    [SE_CONTEST_ICON_CHANGE] =
    {
        .tempo = 75,
        .duration = 6 * 80,
        .looping = FALSE,
    },
    [SE_CONTEST_ICON_CLEAR] =
    {
        .tempo = 75,
        .duration = 6 * 80,
        .looping = FALSE,
    },
    [SE_CONTEST_MONS_TURN] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_CONTEST_PLACE] =
    {
        .tempo = 72,
        .duration = 36 * 80,
        .looping = FALSE,
    },
    [SE_DEX_SEARCH] =
    {
        .tempo = 72,
        .duration = 72 * 80,
        .looping = FALSE,
    },
    [SE_DING_DONG] =
    {
        .tempo = 75,
        .duration = 63 * 80,
        .looping = FALSE,
    },
    [SE_DOOR] =
    {
        .tempo = 55,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_DOWNPOUR] =
    {
        .tempo = 110,
        .duration = 60 * 80,
        .looping = TRUE,
    },
    [SE_DOWNPOUR_STOP] =
    {
        .tempo = 110,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_EFFECTIVE] =
    {
        .tempo = 45,
        .duration = 42 * 80,
        .looping = FALSE,
    },
    [SE_EGG_HATCH] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_ELEVATOR] =
    {
        .tempo = 75,
        .duration = 216 * 80,
        .looping = FALSE,
    },
    [SE_ESCALATOR] =
    {
        .tempo = 75,
        .duration = 120 * 80,
        .looping = FALSE,
    },
    [SE_EXIT] =
    {
        .tempo = 60,
        .duration = 30 * 80,
        .looping = FALSE,
    },
    [SE_EXP] =
    {
        .tempo = 120,
        .duration = 156 * 80,
        .looping = FALSE,
    },
    [SE_EXP_MAX] =
    {
        .tempo = 75,
        .duration = 18 * 80,
        .looping = FALSE,
    },
    [SE_FAILURE] =
    {
        .tempo = 120,
        .duration = 33 * 80,
        .looping = FALSE,
    },
    [SE_FAINT] =
    {
        .tempo = 45,
        .duration = 30 * 80,
        .looping = FALSE,
    },
    [SE_FALL] =
    {
        .tempo = 85,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_FIELD_POISON] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_FLEE] =
    {
        .tempo = 75,
        .duration = 39 * 80,
        .looping = FALSE,
    },
    [SE_FU_ZAKU] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_GLASS_FLUTE] =
    {
        .tempo = 60,
        .duration = 18 * 80,
        .looping = FALSE,
    },
    [SE_ICE_BREAK] =
    {
        .tempo = 60,
        .duration = 15 * 80,
        .looping = FALSE,
    },
    [SE_ICE_CRACK] =
    {
        .tempo = 75,
        .duration = 9 * 80,
        .looping = FALSE,
    },
    [SE_ICE_STAIRS] =
    {
        .tempo = 75,
        .duration = 18 * 80,
        .looping = FALSE,
    },
    [SE_INTRO_BLAST] =
    {
        .tempo = 55,
        .duration = 132 * 80,
        .looping = FALSE,
    },
    [SE_ITEMFINDER] =
    {
        .tempo = 64,
        .duration = 30 * 80,
        .looping = FALSE,
    },
    [SE_LAVARIDGE_FALL_WARP] =
    {
        .tempo = 60,
        .duration = 54 * 80,
        .looping = FALSE,
    },
    [SE_LEDGE] =
    {
        .tempo = 60,
        .duration = 9 * 80,
        .looping = FALSE,
    },
    [SE_LOW_HEALTH] =
    {
        .tempo = 75,
        .duration = 36 * 80,
        .looping = TRUE,
    },
    [SE_MUD_BALL] =
    {
        .tempo = 75,
        .duration = 9 * 80,
        .looping = FALSE,
    },
    [SE_MUGSHOT] =
    {
        .tempo = 55,
        .duration = 108 * 80,
        .looping = FALSE,
    },
    [SE_M_BIND] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_COMET_PUNCH] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_CUT] =
    {
        .tempo = 110,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_M_DOUBLE_SLAP] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_FIRE_PUNCH] =
    {
        .tempo = 110,
        .duration = 72 * 80,
        .looping = FALSE,
    },
    [SE_M_FLY] =
    {
        .tempo = 110,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_M_GUST] =
    {
        .tempo = 75,
        .duration = 72 * 80,
        .looping = TRUE,
    },
    [SE_M_GUST2] =
    {
        .tempo = 75,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_M_HEADBUTT] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_HORN_ATTACK] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_JUMP_KICK] =
    {
        .tempo = 90,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_LEER] =
    {
        .tempo = 75,
        .duration = 60 * 80,
        .looping = FALSE,
    },
    [SE_M_MEGA_KICK] =
    {
        .tempo = 75,
        .duration = 72 * 80,
        .looping = FALSE,
    },
    [SE_M_MEGA_KICK2] =
    {
        .tempo = 110,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_M_PAY_DAY] =
    {
        .tempo = 75,
        .duration = 36 * 80,
        .looping = FALSE,
    },
    [SE_M_RAZOR_WIND] =
    {
        .tempo = 110,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_M_RAZOR_WIND2] =
    {
        .tempo = 125,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_SAND_ATTACK] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_SCRATCH] =
    {
        .tempo = 110,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_SWORDS_DANCE] =
    {
        .tempo = 75,
        .duration = 60 * 80,
        .looping = FALSE,
    },
    [SE_M_TAIL_WHIP] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_TAKE_DOWN] =
    {
        .tempo = 75,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_M_VICEGRIP] =
    {
        .tempo = 110,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_WING_ATTACK] =
    {
        .tempo = 110,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_NOTE_A] =
    {
        .tempo = 75,
        .duration = 18 * 80,
        .looping = FALSE,
    },
    [SE_NOTE_B] =
    {
        .tempo = 75,
        .duration = 18 * 80,
        .looping = FALSE,
    },
    [SE_NOTE_C] =
    {
        .tempo = 75,
        .duration = 18 * 80,
        .looping = FALSE,
    },
    [SE_NOTE_C_HIGH] =
    {
        .tempo = 75,
        .duration = 18 * 80,
        .looping = FALSE,
    },
    [SE_NOTE_D] =
    {
        .tempo = 75,
        .duration = 18 * 80,
        .looping = FALSE,
    },
    [SE_NOTE_E] =
    {
        .tempo = 75,
        .duration = 18 * 80,
        .looping = FALSE,
    },
    [SE_NOTE_F] =
    {
        .tempo = 75,
        .duration = 18 * 80,
        .looping = FALSE,
    },
    [SE_NOTE_G] =
    {
        .tempo = 75,
        .duration = 18 * 80,
        .looping = FALSE,
    },
    [SE_NOT_EFFECTIVE] =
    {
        .tempo = 45,
        .duration = 9 * 80,
        .looping = FALSE,
    },
    [SE_ORB] =
    {
        .tempo = 55,
        .duration = 162 * 80,
        .looping = FALSE,
    },
    [SE_PC_LOGIN] =
    {
        .tempo = 72,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_PC_OFF] =
    {
        .tempo = 60,
        .duration = 18 * 80,
        .looping = FALSE,
    },
    [SE_PC_ON] =
    {
        .tempo = 72,
        .duration = 57 * 80,
        .looping = FALSE,
    },
    [SE_PIN] =
    {
        .tempo = 50,
        .duration = 15 * 80,
        .looping = FALSE,
    },
    [SE_POKENAV_OFF] =
    {
        .tempo = 72,
        .duration = 18 * 80,
        .looping = FALSE,
    },
    [SE_POKENAV_ON] =
    {
        .tempo = 72,
        .duration = 18 * 80,
        .looping = FALSE,
    },
    [SE_PUDDLE] =
    {
        .tempo = 75,
        .duration = 12 * 80,
        .looping = FALSE,
    },
    [SE_RAIN] =
    {
        .tempo = 110,
        .duration = 84 * 80,
        .looping = TRUE,
    },
    [SE_RAIN_STOP] =
    {
        .tempo = 110,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_REPEL] =
    {
        .tempo = 110,
        .duration = 36 * 80,
        .looping = FALSE,
    },
    [SE_ROTATING_GATE] =
    {
        .tempo = 110,
        .duration = 30 * 80,
        .looping = FALSE,
    },
    [SE_ROULETTE_BALL] =
    {
        .tempo = 75,
        .duration = 480 * 80,
        .looping = FALSE,
    },
    [SE_ROULETTE_BALL2] =
    {
        .tempo = 75,
        .duration = 72 * 80,
        .looping = FALSE,
    },
    [SE_RS_DOOR] =
    {
        .tempo = 55,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_RS_SHOP] =
    {
        .tempo = 64,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_SAVE] =
    {
        .tempo = 75,
        .duration = 42 * 80,
        .looping = FALSE,
    },
    [SE_SELECT] =
    {
        .tempo = 150,
        .duration = 18 * 80,
        .looping = FALSE,
    },
    [SE_SHINY] =
    {
        .tempo = 110,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_SHIP] =
    {
        .tempo = 110,
        .duration = 168 * 80,
        .looping = FALSE,
    },
    [SE_SLIDING_DOOR] =
    {
        .tempo = 60,
        .duration = 12 * 80,
        .looping = FALSE,
    },
    [SE_SUCCESS] =
    {
        .tempo = 120,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_SUPER_EFFECTIVE] =
    {
        .tempo = 45,
        .duration = 51 * 80,
        .looping = FALSE,
    },
    [SE_SWITCH] =
    {
        .tempo = 75,
        .duration = 9 * 80,
        .looping = FALSE,
    },
    [SE_TAILLOW_WING_FLAP] =
    {
        .tempo = 110,
        .duration = 48 * 80,
        .looping = TRUE,
    },
    [SE_THUNDER] =
    {
        .tempo = 110,
        .duration = 144 * 80,
        .looping = FALSE,
    },
    [SE_THUNDER2] =
    {
        .tempo = 110,
        .duration = 120 * 80,
        .looping = FALSE,
    },
    [SE_THUNDERSTORM] =
    {
        .tempo = 110,
        .duration = 84 * 80,
        .looping = TRUE,
    },
    [SE_THUNDERSTORM_STOP] =
    {
        .tempo = 110,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_TRUCK_DOOR] =
    {
        .tempo = 75,
        .duration = 12 * 80,
        .looping = FALSE,
    },
    [SE_TRUCK_MOVE] =
    {
        .tempo = 75,
        .duration = 384 * 80,
        .looping = TRUE,
    },
    [SE_TRUCK_STOP] =
    {
        .tempo = 75,
        .duration = 144 * 80,
        .looping = FALSE,
    },
    [SE_TRUCK_UNLOAD] =
    {
        .tempo = 75,
        .duration = 27 * 80,
        .looping = FALSE,
    },
    [SE_UNLOCK] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_USE_ITEM] =
    {
        .tempo = 66,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_VEND] =
    {
        .tempo = 75,
        .duration = 12 * 80,
        .looping = FALSE,
    },
    [SE_WARP_IN] =
    {
        .tempo = 55,
        .duration = 33 * 80,
        .looping = FALSE,
    },
    [SE_WARP_OUT] =
    {
        .tempo = 55,
        .duration = 36 * 80,
        .looping = FALSE,
    },
    [SE_BAG_CURSOR] =
    {
        .tempo = 155,
        .duration = 9 * 80,
        .looping = FALSE,
    },
    [SE_BAG_POCKET] =
    {
        .tempo = 211,
        .duration = 6 * 80,
        .looping = FALSE,
    },
    [SE_BALL_CLICK] =
    {
        .tempo = 155,
        .duration = 21 * 80,
        .looping = FALSE,
    },
    [SE_CARD_FLIP] =
    {
        .tempo = 91,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_CARD_FLIPPING] =
    {
        .tempo = 91,
        .duration = 120 * 80,
        .looping = FALSE,
    },
    [SE_CARD_OPEN] =
    {
        .tempo = 87,
        .duration = 120 * 80,
        .looping = FALSE,
    },
    [SE_DEOXYS_MOVE] =
    {
        .tempo = 131,
        .duration = 192 * 80,
        .looping = FALSE,
    },
    [SE_DEX_PAGE] =
    {
        .tempo = 50,
        .duration = 9 * 80,
        .looping = FALSE,
    },
    [SE_DEX_SCROLL] =
    {
        .tempo = 50,
        .duration = 3 * 80,
        .looping = FALSE,
    },
    [SE_HELP_CLOSE] =
    {
        .tempo = 210,
        .duration = 96 * 80,
        .looping = FALSE,
    },
    [SE_HELP_ERROR] =
    {
        .tempo = 210,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_HELP_OPEN] =
    {
        .tempo = 210,
        .duration = 96 * 80,
        .looping = FALSE,
    },
    [SE_M_ABSORB] =
    {
        .tempo = 75,
        .duration = 12 * 80,
        .looping = FALSE,
    },
    [SE_M_ABSORB_2] =
    {
        .tempo = 75,
        .duration = 51 * 80,
        .looping = FALSE,
    },
    [SE_M_ACID_ARMOR] =
    {
        .tempo = 75,
        .duration = 72 * 80,
        .looping = FALSE,
    },
    [SE_M_ATTRACT] =
    {
        .tempo = 105,
        .duration = 69 * 80,
        .looping = FALSE,
    },
    [SE_M_ATTRACT2] =
    {
        .tempo = 75,
        .duration = 216 * 80,
        .looping = FALSE,
    },
    [SE_M_BARRIER] =
    {
        .tempo = 75,
        .duration = 54 * 80,
        .looping = FALSE,
    },
    [SE_M_BATON_PASS] =
    {
        .tempo = -1,
        .duration = 45 * 80,
        .looping = FALSE,
    },
    [SE_M_BELLY_DRUM] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_BITE] =
    {
        .tempo = 110,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_BLIZZARD] =
    {
        .tempo = 75,
        .duration = 72 * 80,
        .looping = TRUE,
    },
    [SE_M_BLIZZARD2] =
    {
        .tempo = 75,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_M_BONEMERANG] =
    {
        .tempo = 75,
        .duration = 12 * 80,
        .looping = FALSE,
    },
    [SE_M_BRICK_BREAK] =
    {
        .tempo = 75,
        .duration = 36 * 80,
        .looping = FALSE,
    },
    [SE_M_BUBBLE] =
    {
        .tempo = 110,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_BUBBLE2] =
    {
        .tempo = 110,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_BUBBLE3] =
    {
        .tempo = 110,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_BUBBLE_BEAM] =
    {
        .tempo = 75,
        .duration = 78 * 80,
        .looping = FALSE,
    },
    [SE_M_BUBBLE_BEAM2] =
    {
        .tempo = 75,
        .duration = 36 * 80,
        .looping = FALSE,
    },
    [SE_M_CHARGE] =
    {
        .tempo = 75,
        .duration = 72 * 80,
        .looping = FALSE,
    },
    [SE_M_CHARM] =
    {
        .tempo = 75,
        .duration = 18 * 80,
        .looping = FALSE,
    },
    [SE_M_CONFUSE_RAY] =
    {
        .tempo = 110,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_M_COSMIC_POWER] =
    {
        .tempo = 75,
        .duration = 198 * 80,
        .looping = FALSE,
    },
    [SE_M_CRABHAMMER] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_DETECT] =
    {
        .tempo = 110,
        .duration = 42 * 80,
        .looping = FALSE,
    },
    [SE_M_DIG] =
    {
        .tempo = 75,
        .duration = 15 * 80,
        .looping = FALSE,
    },
    [SE_M_DIVE] =
    {
        .tempo = 75,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_M_DIZZY_PUNCH] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_DOUBLE_TEAM] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_DRAGON_RAGE] =
    {
        .tempo = 75,
        .duration = 96 * 80,
        .looping = FALSE,
    },
    [SE_M_EARTHQUAKE] =
    {
        .tempo = 75,
        .duration = 204 * 80,
        .looping = FALSE,
    },
    [SE_M_EMBER] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_ENCORE] =
    {
        .tempo = 75,
        .duration = 12 * 80,
        .looping = FALSE,
    },
    [SE_M_ENCORE2] =
    {
        .tempo = 75,
        .duration = 144 * 80,
        .looping = FALSE,
    },
    [SE_M_EXPLOSION] =
    {
        .tempo = 75,
        .duration = 72 * 80,
        .looping = FALSE,
    },
    [SE_M_FAINT_ATTACK] =
    {
        .tempo = 75,
        .duration = 30 * 80,
        .looping = FALSE,
    },
    [SE_M_FLAMETHROWER] =
    {
        .tempo = 75,
        .duration = 192 * 80,
        .looping = FALSE,
    },
    [SE_M_FLAME_WHEEL] =
    {
        .tempo = 75,
        .duration = 33 * 80,
        .looping = FALSE,
    },
    [SE_M_FLAME_WHEEL2] =
    {
        .tempo = 75,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_M_FLATTER] =
    {
        .tempo = 75,
        .duration = 12 * 80,
        .looping = FALSE,
    },
    [SE_M_GIGA_DRAIN] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_GRASSWHISTLE] =
    {
        .tempo = 50,
        .duration = 132 * 80,
        .looping = FALSE,
    },
    [SE_M_HAIL] =
    {
        .tempo = 75,
        .duration = 30 * 80,
        .looping = FALSE,
    },
    [SE_M_HARDEN] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_HAZE] =
    {
        .tempo = 95,
        .duration = 210 * 80,
        .looping = FALSE,
    },
    [SE_M_HEAL_BELL] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_HEAT_WAVE] =
    {
        .tempo = 75,
        .duration = 132 * 80,
        .looping = FALSE,
    },
    [SE_M_HYDRO_PUMP] =
    {
        .tempo = 75,
        .duration = 120 * 80,
        .looping = FALSE,
    },
    [SE_M_HYPER_BEAM] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_HYPER_BEAM2] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_ICY_WIND] =
    {
        .tempo = 75,
        .duration = 12 * 80,
        .looping = FALSE,
    },
    [SE_M_LICK] =
    {
        .tempo = 75,
        .duration = 36 * 80,
        .looping = FALSE,
    },
    [SE_M_LOCK_ON] =
    {
        .tempo = 75,
        .duration = 18 * 80,
        .looping = FALSE,
    },
    [SE_M_METRONOME] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_MILK_DRINK] =
    {
        .tempo = 90,
        .duration = 30 * 80,
        .looping = FALSE,
    },
    [SE_M_MINIMIZE] =
    {
        .tempo = 75,
        .duration = 54 * 80,
        .looping = FALSE,
    },
    [SE_M_MIST] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_MOONLIGHT] =
    {
        .tempo = 75,
        .duration = 288 * 80,
        .looping = FALSE,
    },
    [SE_M_MORNING_SUN] =
    {
        .tempo = 75,
        .duration = 75 * 80,
        .looping = FALSE,
    },
    [SE_M_NIGHTMARE] =
    {
        .tempo = 110,
        .duration = 96 * 80,
        .looping = FALSE,
    },
    [SE_M_PERISH_SONG] =
    {
        .tempo = 50,
        .duration = 126 * 80,
        .looping = FALSE,
    },
    [SE_M_PETAL_DANCE] =
    {
        .tempo = 75,
        .duration = 135 * 80,
        .looping = FALSE,
    },
    [SE_M_POISON_POWDER] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_PSYBEAM] =
    {
        .tempo = 95,
        .duration = 42 * 80,
        .looping = FALSE,
    },
    [SE_M_PSYBEAM2] =
    {
        .tempo = 95,
        .duration = 60 * 80,
        .looping = FALSE,
    },
    [SE_M_RAIN_DANCE] =
    {
        .tempo = 110,
        .duration = 168 * 80,
        .looping = FALSE,
    },
    [SE_M_REFLECT] =
    {
        .tempo = 75,
        .duration = 54 * 80,
        .looping = FALSE,
    },
    [SE_M_REVERSAL] =
    {
        .tempo = 75,
        .duration = 66 * 80,
        .looping = FALSE,
    },
    [SE_M_ROCK_THROW] =
    {
        .tempo = 75,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_M_SACRED_FIRE] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_SACRED_FIRE2] =
    {
        .tempo = 75,
        .duration = 96 * 80,
        .looping = FALSE,
    },
    [SE_M_SANDSTORM] =
    {
        .tempo = 100,
        .duration = 192 * 80,
        .looping = FALSE,
    },
    [SE_M_SAND_TOMB] =
    {
        .tempo = 75,
        .duration = 96 * 80,
        .looping = FALSE,
    },
    [SE_M_SCREECH] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_SELF_DESTRUCT] =
    {
        .tempo = 75,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_M_SING] =
    {
        .tempo = 50,
        .duration = 174 * 80,
        .looping = FALSE,
    },
    [SE_M_SKETCH] =
    {
        .tempo = 90,
        .duration = 12 * 80,
        .looping = FALSE,
    },
    [SE_M_SKY_UPPERCUT] =
    {
        .tempo = 110,
        .duration = 36 * 80,
        .looping = FALSE,
    },
    [SE_M_SNORE] =
    {
        .tempo = 110,
        .duration = 30 * 80,
        .looping = FALSE,
    },
    [SE_M_SOLAR_BEAM] =
    {
        .tempo = 75,
        .duration = 192 * 80,
        .looping = FALSE,
    },
    [SE_M_SPIT_UP] =
    {
        .tempo = 75,
        .duration = 15 * 80,
        .looping = FALSE,
    },
    [SE_M_STAT_DECREASE] =
    {
        .tempo = 85,
        .duration = 78 * 80,
        .looping = FALSE,
    },
    [SE_M_STAT_INCREASE] =
    {
        .tempo = 85,
        .duration = 78 * 80,
        .looping = FALSE,
    },
    [SE_M_STRENGTH] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_STRING_SHOT] =
    {
        .tempo = 125,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_M_STRING_SHOT2] =
    {
        .tempo = 125,
        .duration = 96 * 80,
        .looping = FALSE,
    },
    [SE_M_SUPERSONIC] =
    {
        .tempo = 75,
        .duration = 42 * 80,
        .looping = FALSE,
    },
    [SE_M_SURF] =
    {
        .tempo = 75,
        .duration = 96 * 80,
        .looping = FALSE,
    },
    [SE_M_SWAGGER] =
    {
        .tempo = 95,
        .duration = 18 * 80,
        .looping = FALSE,
    },
    [SE_M_SWAGGER2] =
    {
        .tempo = 75,
        .duration = 12 * 80,
        .looping = FALSE,
    },
    [SE_M_SWEET_SCENT] =
    {
        .tempo = 75,
        .duration = 168 * 80,
        .looping = FALSE,
    },
    [SE_M_SWIFT] =
    {
        .tempo = 90,
        .duration = 18 * 80,
        .looping = FALSE,
    },
    [SE_M_TEETER_DANCE] =
    {
        .tempo = 85,
        .duration = 60 * 80,
        .looping = FALSE,
    },
    [SE_M_TELEPORT] =
    {
        .tempo = 75,
        .duration = 54 * 80,
        .looping = FALSE,
    },
    [SE_M_THUNDERBOLT] =
    {
        .tempo = 110,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_M_THUNDERBOLT2] =
    {
        .tempo = 110,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_M_THUNDER_WAVE] =
    {
        .tempo = 75,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_M_TOXIC] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_TRI_ATTACK] =
    {
        .tempo = 110,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_TRI_ATTACK2] =
    {
        .tempo = 110,
        .duration = 96 * 80,
        .looping = FALSE,
    },
    [SE_M_TWISTER] =
    {
        .tempo = 75,
        .duration = 168 * 80,
        .looping = FALSE,
    },
    [SE_M_UPROAR] =
    {
        .tempo = 75,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_VITAL_THROW] =
    {
        .tempo = 110,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_VITAL_THROW2] =
    {
        .tempo = 110,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_M_WATERFALL] =
    {
        .tempo = 75,
        .duration = 60 * 80,
        .looping = FALSE,
    },
    [SE_M_WHIRLPOOL] =
    {
        .tempo = 75,
        .duration = 120 * 80,
        .looping = FALSE,
    },
    [SE_M_YAWN] =
    {
        .tempo = 75,
        .duration = 48 * 80,
        .looping = FALSE,
    },
    [SE_POKE_JUMP_FAILURE] =
    {
        .tempo = 120,
        .duration = 33 * 80,
        .looping = FALSE,
    },
    [SE_POKE_JUMP_SUCCESS] =
    {
        .tempo = 75,
        .duration = 51 * 80,
        .looping = FALSE,
    },
    [SE_SHOP] =
    {
        .tempo = 62,
        .duration = 30 * 80,
        .looping = FALSE,
    },
    [SE_SS_ANNE_HORN] =
    {
        .tempo = 82,
        .duration = 240 * 80,
        .looping = FALSE,
    },
    [SE_WALL_HIT] =
    {
        .tempo = 110,
        .duration = 24 * 80,
        .looping = FALSE,
    },
    [SE_WIN_OPEN] =
    {
        .tempo = 110,
        .duration = 24 * 80,
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
        gMain.accumulator = 0;
        gMain.songPlayTime = 0;
        m4aSongNumStart(gMain.currentSong);
        ++gMain.state;
        break;
    case WAIT_FRAMES + 1:
        if (musicData[gMain.currentSong].looping)
        {
            gMain.songPlayTime += musicData[gMain.currentSong].tempo;
            // do 16 audio ticks per 15 frames (64 audio ticks/s with 60 frames/s)
            if (++gMain.accumulator == 15)
            {
                gMain.accumulator = 0;
                gMain.songPlayTime += musicData[gMain.currentSong].tempo;
            }
            // Wait 2 seconds after the song loops
            if (gMain.songPlayTime < musicData[gMain.currentSong].duration + 2 * 64 * musicData[gMain.currentSong].tempo)
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
