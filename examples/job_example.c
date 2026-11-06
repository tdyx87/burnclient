/* Asynchronous job API example (plain C).
 *
 * Build against the installed SDK:
 *   Windows : cl /I include job_example.c /link lib/burning_dll.lib
 *   Linux   : cc -I include job_example.c -L lib -l burning_dll -Wl,-rpath,lib
 *
 * The example uses BURN_JOB_MODE_SELFTEST, a diagnostic job that needs no
 * optical drive. Replace it with BURN_JOB_MODE_IMAGE / _DIRECTORY for real
 * burning; the status/cancel/wait logic is identical.
 */
#include "../src/burning_utility.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  define SLEEP_MS(ms) Sleep(ms)
#else
#  include <unistd.h>
#  define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

static void onProgress(int percent, const char* status, void* userData) {
    int* last = (int*)userData;
    if (last) *last = percent;
    printf("  [callback] %3d%%  %s\n", percent, status ? status : "");
}

static void configure(BurnJobOptions* options) {
    memset(options, 0, sizeof(*options));
    options->size = (int)sizeof(*options);
    options->mode = BURN_JOB_MODE_SELFTEST;
    options->progressCallback = onProgress;
}

int main(int argc, char** argv) {
    BurnJobOptions options;
    BurnJobStatus status;
    BurnJobHandle job;
    int lastPercent = -1;
    int cancelHalfway = (argc > 1 && strcmp(argv[1], "cancel") == 0);

    printf("BurningDLL job API version: %d\n", Burn_GetApiVersion());

    configure(&options);
    options.userData = &lastPercent;
    job = Burn_Start(&options);
    if (job == BURN_INVALID_JOB_HANDLE) {
        printf("Burn_Start failed: %s\n", GetLastErrorMessage());
        return 1;
    }
    printf("Started job %llu\n", (unsigned long long)job);

    /* Poll the state machine. Note that Burn_GetStatus() is a plain snapshot
     * with no pointers, so it is safe to call from any thread. */
    for (;;) {
        memset(&status, 0, sizeof(status));
        status.size = (int)sizeof(status);
        if (Burn_GetStatus(job, &status) != BURN_SUCCESS) {
            printf("Burn_GetStatus failed\n");
            break;
        }
        printf("  [status]   state=%d %3d%%  phase=%-18s %s\n",
               status.state, status.percent, status.phase, status.message);

        if (status.state != BURN_JOB_PENDING && status.state != BURN_JOB_RUNNING) {
            break;
        }
        if (cancelHalfway && status.percent >= 20 && !status.cancelRequested) {
            printf("  Cancelling...\n");
            Burn_Cancel(job);
        }
        SLEEP_MS(100);
    }

    /* Burn_Wait() with a negative timeout blocks until the job is terminal. */
    if (Burn_Wait(job, 5000) != BURN_SUCCESS) {
        printf("Job did not finish within 5 seconds\n");
    }

    memset(&status, 0, sizeof(status));
    status.size = (int)sizeof(status);
    if (Burn_GetStatus(job, &status) == BURN_SUCCESS) {
        printf("Final: state=%d errorCode=%d elapsed=%llums error=%s\n",
               status.state, status.errorCode,
               (unsigned long long)status.elapsedMs, status.errorMessage);
    }
    printf("Job error string: %s\n", Burn_GetJobErrorMessage(job));

    /* Always release. A second release returns BURN_ERROR_INVALID_HANDLE. */
    printf("Release: %d (second release: %d)\n",
           Burn_Release(job), Burn_Release(job));
    return status.state == BURN_JOB_COMPLETED || status.state == BURN_JOB_CANCELLED ? 0 : 1;
}
