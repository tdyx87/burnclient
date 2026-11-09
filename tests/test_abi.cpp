/* ABI and API consistency check.
 *
 * It links against the installed library, exactly like an external caller
 * would, and verifies that:
 *   - every symbol the public header declares is actually exported,
 *   - the version macro and the runtime version agree,
 *   - the v1 and v3 option structs have the layout the header promises,
 *   - a v1 caller still works after the v2/v3 additions.
 *
 * Compiled as plain C on purpose: the public header must stay consumable
 * from C. */
#include "burning_utility.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>   /* offsetof */

static int g_failures = 0;
#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            printf("CHECK FAILED: %s (line %d)\n", #expr, __LINE__);           \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

/* Every function the header promises. Taking the address forces the linker
 * to resolve the symbol; if it is missing, this file will not link. */
static void* const kRequired[] = {
    (void*)&Burn_Start, (void*)&Burn_GetStatus, (void*)&Burn_Cancel,
    (void*)&Burn_Wait, (void*)&Burn_Release, (void*)&Burn_Simple,
    (void*)&Burn_GetJobErrorMessage, (void*)&Burn_GetApiVersion,
    (void*)&ListBurningDevices, (void*)&FreeDeviceList,
    (void*)&Burn_GetCapability, (void*)&Burn_GetNextSessionBlock,
    (void*)&BurnAudioCd, (void*)&BurnEraseDisc, (void*)&BurnAppendSession,
    (void*)&Burn_VerifyWrite, (void*)&BurnCreateRawImage,
    (void*)&BurnRawImageToDisc, (void*)&BurnEjectTray, (void*)&BurnCloseTray,
    (void*)&Burn_GetDriveInformation,
    (void*)&Burn_GetMediaType, (void*)&Burn_GetMediaTypeCode,
    (void*)&Burn_GetMediaState, (void*)&Burn_GetMediaStateText,
};

static void progressCb(int percent, const char* status, void* userData) {
    (void)percent; (void)status; (void)userData;
}

int main(void) {
    printf("=== ABI and API consistency ===\n");
    printf("  required symbols: %zu\n", sizeof(kRequired) / sizeof(kRequired[0]));

    /* The runtime must report the version the header was compiled with. */
    const int runtime = Burn_GetApiVersion();
    printf("  header BURN_API_VERSION = %d, runtime = %d\n", BURN_API_VERSION, runtime);
    CHECK(runtime == BURN_API_VERSION);
    CHECK(runtime == 3);

    /* The v1 struct must stay exactly as it was: the new struct is a strict
     * extension, so the first eleven fields have to line up.
     *
     * These offsets are compile-time constants, which MSVC would flag as a
     * constant condition; the comparison below is the point of the test, so
     * the values are routed through a volatile to keep the check real. */
    BurnJobOptions v1;
    BurnJobOptions3 v3;
    memset(&v1, 0, sizeof(v1));
    memset(&v3, 0, sizeof(v3));
    printf("  sizeof(BurnJobOptions)   = %u\n", (unsigned)sizeof(v1));
    printf("  sizeof(BurnJobOptions3)  = %u\n", (unsigned)sizeof(v3));
    {
        static volatile size_t v1Size = sizeof(v1);
        static volatile size_t v3Size = sizeof(v3);
        CHECK(v3Size > v1Size);
    }

    static volatile size_t v1Mode = offsetof(BurnJobOptions, mode);
    static volatile size_t v3Mode = offsetof(BurnJobOptions3, mode);
    static volatile size_t v1Src = offsetof(BurnJobOptions, sourcePath);
    static volatile size_t v3Src = offsetof(BurnJobOptions3, sourcePath);
    static volatile size_t v1Out = offsetof(BurnJobOptions, outputPath);
    static volatile size_t v3Out = offsetof(BurnJobOptions3, outputPath);
    static volatile size_t v1Dev = offsetof(BurnJobOptions, devicePath);
    static volatile size_t v3Dev = offsetof(BurnJobOptions3, devicePath);
    static volatile size_t v1Type = offsetof(BurnJobOptions, discType);
    static volatile size_t v3Type = offsetof(BurnJobOptions3, discType);
    static volatile size_t v1Label = offsetof(BurnJobOptions, volumeLabel);
    static volatile size_t v3Label = offsetof(BurnJobOptions3, volumeLabel);
    static volatile size_t v1Speed = offsetof(BurnJobOptions, speed);
    static volatile size_t v3Speed = offsetof(BurnJobOptions3, speed);
    static volatile size_t v1Cb = offsetof(BurnJobOptions, progressCallback);
    static volatile size_t v3Cb = offsetof(BurnJobOptions3, progressCallback);
    static volatile size_t v1Ud = offsetof(BurnJobOptions, userData);
    static volatile size_t v3Ud = offsetof(BurnJobOptions3, userData);

    CHECK(v1Mode == v3Mode);
    CHECK(v1Src == v3Src);
    CHECK(v1Out == v3Out);
    CHECK(v1Dev == v3Dev);
    CHECK(v1Type == v3Type);
    CHECK(v1Label == v3Label);
    CHECK(v1Speed == v3Speed);
    CHECK(v1Cb == v3Cb);
    CHECK(v1Ud == v3Ud);

    /* The capability ids must be dense and start at zero, so a caller can
     * iterate them. */
    for (int i = 0; i <= BURN_CAP_DRIVE_INFO; ++i) {
        const int cap = Burn_GetCapability(i);
        if (cap != 0 && cap != 1) {
            printf("  capability %d returned %d\n", i, cap);
            ++g_failures;
        }
    }
    CHECK(Burn_GetCapability(BURN_CAP_DRIVE_INFO + 1) == 0);

    /* A v1 caller must still work. */
    v1.size = (int)sizeof(v1);
    v1.mode = BURN_JOB_MODE_SELFTEST;
    v1.progressCallback = progressCb;
    {
        const BurnJobHandle h = Burn_Start(&v1);
        CHECK(h != BURN_INVALID_JOB_HANDLE);
        if (h != BURN_INVALID_JOB_HANDLE) {
            CHECK(Burn_Wait(h, 30000) == BURN_SUCCESS);
            BurnJobStatus st;
            memset(&st, 0, sizeof(st));
            st.size = (int)sizeof(st);
            CHECK(Burn_GetStatus(h, &st) == BURN_SUCCESS);
            CHECK(st.state == BURN_JOB_COMPLETED);
            printf("  v1 job: state=%d elapsed=%llu ms\n", (int)st.state,
                   (unsigned long long)st.elapsedMs);
            Burn_Release(h);
        }
    }

    /* Releasing twice must be refused, not crash. */
    {
        BurnJobOptions3 o;
        memset(&o, 0, sizeof(o));
        o.size = (int)sizeof(o);
        o.mode = BURN_JOB_MODE_SELFTEST;
        const BurnJobHandle h = Burn_Start((const BurnJobOptions*)&o);
        CHECK(h != BURN_INVALID_JOB_HANDLE);
        if (h != BURN_INVALID_JOB_HANDLE) {
            Burn_Wait(h, 30000);
            CHECK(Burn_Release(h) == BURN_SUCCESS);
            CHECK(Burn_Release(h) != BURN_SUCCESS);   /* second release refused */
            CHECK(Burn_GetStatus(h, NULL) != BURN_SUCCESS);
        }
    }

    /* Invalid arguments must be refused with a specific code. */
    CHECK(Burn_Start(NULL) == BURN_INVALID_JOB_HANDLE);
    CHECK(Burn_GetStatus(BURN_INVALID_JOB_HANDLE, NULL) != BURN_SUCCESS);
    CHECK(Burn_Release(BURN_INVALID_JOB_HANDLE) != BURN_SUCCESS);
    CHECK(Burn_Wait(BURN_INVALID_JOB_HANDLE, 0) != BURN_SUCCESS);

    /* The device list must be consistent with its own count. */
    {
        DeviceInfo* devices = NULL;
        int count = 0;
        if (ListBurningDevices(&devices, &count) == BURN_SUCCESS) {
            printf("  devices: %d\n", count);
            CHECK(count >= 0);
            if (count > 0) CHECK(devices != NULL);
            for (int i = 0; i < count; ++i) {
                CHECK(devices[i].devicePath[0] != '\0');
                printf("    %s  writable=%d\n", devices[i].devicePath,
                       devices[i].isWritable);
            }
            FreeDeviceList(devices, count);
        } else {
            printf("  device enumeration failed: %s\n", GetLastErrorMessage());
            CHECK(strlen(GetLastErrorMessage()) > 0);
        }
    }

    if (g_failures) {
        printf("\n%d CHECK(s) FAILED\n", g_failures);
        return 1;
    }
    printf("\n=== ABI check: SUCCESS ===\n");
    return 0;
}
