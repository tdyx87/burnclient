#include "burn_internal.h"
#include <string>
#include <vector>

/* ---------------------------------------------------------------------------
 * IPlatform default implementations for the version 3 capability surface.
 *
 * Every extended feature is an OPTIONAL virtual: a backend that does not
 * override it here reports "not supported on this platform" with a precise
 * message instead of pretending to succeed. The Windows/IMAPI2 backend
 * overrides all of them in windows_platform.cpp; the libburn and drutil
 * backends inherit these bodies, so their behaviour is a clean
 * BURN_ERROR_UNSUPPORTED_FORMAT.
 * ------------------------------------------------------------------------- */

namespace {

std::string Unsupported(const char* feature) {
    return std::string("Not supported by this platform backend: ") + feature;
}

} /* namespace */

int IPlatform::Capability(int /*capability*/) {
    return 0;
}

long long IPlatform::NextSessionStartBlock(const std::string& /*devicePath*/) {
    SetBurnLastError("Multisession inquiry is not supported by this platform backend");
    return -2;
}

bool IPlatform::CreateImage(const BurnImageSpec& spec) {
    /* Baseline images still work: fall back to the legacy ISO creation so a
     * v3 caller asking for plain ISO9660/Joliet succeeds everywhere. Only
     * the advanced parts of the spec are refused. */
    const bool plain =
        (spec.fileSystems == 0 ||
         (spec.fileSystems & ~(BURN_FS_ISO9660 | BURN_FS_JOLIET)) == 0) &&
        spec.bootImagePath.empty() && spec.udfRevision == 0 &&
        spec.sessionStartBlock == 0 && spec.interchangeLevel == 0;
    if (plain) {
        return CreateISO(spec.sourcePath, spec.outputPath, spec.volumeLabel);
    }
    SetBurnLastError(
        "Advanced image options (UDF, El Torito boot, ISO level, "
        "multisession start block) are not supported by this platform backend");
    return false;
}

bool IPlatform::BurnImageFile(const std::string& imagePath,
                              const std::string& devicePath,
                              const std::string& discType, int speed,
                              int verifyLevel, bool finalize,
                              unsigned long long sessionStartBlock) {
    /* Same reasoning: a plain first-session write is exactly BurnISO. */
    if ((verifyLevel == BURN_VERIFY_NONE || verifyLevel == BURN_VERIFY_QUICK) &&
        finalize && sessionStartBlock == 0) {
        (void)verifyLevel;
        return BurnISO(imagePath, devicePath, discType, speed);
    }
    if (verifyLevel == BURN_VERIFY_QUICK) {
        return BurnISO(imagePath, devicePath, discType, speed);
    }
    SetBurnLastError(
        "Write options beyond plain image burning (append sessions, "
        "session-open writes, byte-level read-back verification) are not "
        "supported by this platform backend");
    return false;
}

bool IPlatform::BurnAudioTracks(const std::vector<BurnAudioTrackSpec>& /*tracks*/,
                                const std::string& /*devicePath*/, int /*speed*/,
                                int /*verifyLevel*/, bool /*finalizeDisc*/) {
    SetBurnLastError(Unsupported("audio CD (track-at-once) burning"));
    return false;
}

bool IPlatform::EraseDisc(const std::string& /*devicePath*/, bool /*full*/) {
    SetBurnLastError(Unsupported("disc erase / media formatting"));
    return false;
}

bool IPlatform::CreateRawImage(const std::vector<BurnAudioTrackSpec>& /*tracks*/,
                               const std::string& /*outputPath*/,
                               const std::string& /*mcn*/, int /*startingTrackNumber*/) {
    SetBurnLastError(Unsupported("RAW (2352-byte sector) image creation"));
    return false;
}

bool IPlatform::BurnRawImage(const std::string& /*imagePath*/,
                             const std::string& /*devicePath*/,
                             int /*speed*/, int /*verifyLevel*/) {
    SetBurnLastError(Unsupported("RAW (2352-byte sector) writing"));
    return false;
}

bool IPlatform::VerifyWrite(const std::string& /*devicePath*/,
                            const std::string& /*expectedImagePath*/, int /*level*/) {
    SetBurnLastError(Unsupported("disc read-back verification"));
    return false;
}

int IPlatform::EjectTray(const std::string& /*devicePath*/) {
    SetBurnLastError(Unsupported("tray ejection"));
    return BURN_ERROR_UNSUPPORTED_FORMAT;
}

int IPlatform::CloseTray(const std::string& /*devicePath*/) {
    SetBurnLastError(Unsupported("tray closing"));
    return BURN_ERROR_UNSUPPORTED_FORMAT;
}

std::string IPlatform::DriveInformation(const std::string& /*devicePath*/) {
    SetBurnLastError(Unsupported("MMC drive information reporting"));
    return "";
}

std::string IPlatform::MediaType(const std::string& /*devicePath*/) {
    SetBurnLastError(Unsupported("inserted media type reporting"));
    return "";
}

int IPlatform::MediaTypeCode(const std::string& /*devicePath*/) {
    SetBurnLastError(Unsupported("inserted media type reporting"));
    return BURN_MEDIA_UNKNOWN;
}

int IPlatform::MediaState(const std::string& /*devicePath*/) {
    SetBurnLastError(Unsupported("media readiness reporting"));
    return BURN_MEDIA_STATE_QUERY_UNSUPPORTED;
}

std::string IPlatform::MediaStateText(const std::string& /*devicePath*/) {
    SetBurnLastError(Unsupported("media readiness reporting"));
    return "Media readiness reporting is not supported by this backend";
}
