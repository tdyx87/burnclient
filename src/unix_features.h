/* ---------------------------------------------------------------------------
 * unix_features.h - libburn/libisofs implementation of the API v3 surface.
 *
 * Kept separate from unix_platform.cpp for the same reason the Windows side
 * has windows_features.cpp: the v3 entry points are a large, cohesive block
 * and the plain ISO paths in unix_platform.cpp are already verified.
 *
 * Honest limitations of this stack (verified against the installed headers):
 *   - libisofs has NO UDF support at all (no symbol mentions UDF), so
 *     BURN_CAP_UDF reports 0 and a UDF request fails with a clear message.
 *   - libburn's read API has no documented output sink, so read-back
 *     verification reads the device node directly instead.
 * ------------------------------------------------------------------------- */
#ifndef BURNCLIENT_UNIX_FEATURES_H
#define BURNCLIENT_UNIX_FEATURES_H

#include "burning_utility.h"

#if !defined(_WIN32) && !defined(__APPLE__)

#include <string>
#include <vector>

namespace burnunix {

/* BURN_CAP_* -> 1 when this backend implements it, else 0. */
int Capability(int capability);

/* Next writable address of the disc, or -1 with LastError explaining why. */
long long NextSessionStartBlock(const std::string& devicePath);

/* Build an image file honouring file systems, ISO level, El Torito and a
 * continuation session start block. */
bool CreateImage(const BurnImageSpec& spec);

/* Write an image file to a disc. `finalize` controls whether the media stays
 * appendable (multi-session); a non-zero sessionStartBlock selects append
 * mode. */
bool BurnImageFile(const std::string& imagePath, const std::string& devicePath,
                   const std::string& discType, int speed, int verifyLevel,
                   bool finalize, unsigned long long sessionStartBlock);

/* CD-DA in track-at-once mode. */
bool BurnAudioTracks(const std::vector<BurnAudioTrackSpec>& tracks,
                     const std::string& devicePath, int speed,
                     int verifyLevel, bool finalizeDisc);

bool EraseDisc(const std::string& devicePath, bool full);

/* Build a RAW 96-byte-subcode image (2448 bytes per sector). */
bool CreateRawImage(const std::vector<BurnAudioTrackSpec>& tracks,
                    const std::string& outputPath, const std::string& mcn,
                    int startingTrackNumber);

/* Write such an image with BURN_WRITE_RAW / BURN_BLOCK_RAW96R. */
bool BurnRawImage(const std::string& imagePath, const std::string& devicePath,
                  int speed, int verifyLevel);

/* Sector-level comparison of the disc against a reference ISO image. */
bool VerifyWrite(const std::string& devicePath, const std::string& expectedImagePath,
                 int level);

int EjectTray(const std::string& devicePath);
int CloseTray(const std::string& devicePath);

std::string DriveInformation(const std::string& devicePath);

} /* namespace burnunix */

#endif /* !_WIN32 && !__APPLE__ */

#endif /* BURNCLIENT_UNIX_FEATURES_H */
