#ifndef BURN_WINDOWS_FEATURES_H
#define BURN_WINDOWS_FEATURES_H

/* ---------------------------------------------------------------------------
 * Windows (IMAPI2) implementation of the API version 3 capability surface:
 * capability discovery, multisession inquiry, rich image creation (UDF,
 * El Torito, ISO level, continuation sessions), write options (session
 * closing, read-back verification), audio CD track-at-once, CD-RW erase /
 * RW formatting, RAW PQ image building and RAW writing, sector-exact
 * verification, tray control and MMC drive reporting.
 *
 * These are free functions in namespace burnwin; windows_platform.cpp owns
 * the IPlatform subclass and delegates to them. The engine only reaches them
 * through the IPlatform virtuals whose default bodies report
 * BURN_ERROR_UNSUPPORTED_FORMAT, so the libburn and drutil backends stay
 * exactly as they are.
 * ------------------------------------------------------------------------- */

#include "burning_utility.h"   /* IPlatform, BurnImageSpec, BurnAudioTrackSpec */
#include <string>
#include <vector>

#ifdef _WIN32

namespace burnwin {

int Capability(int capability);

/* >=0 on success, -1 not appendable, -2 unsupported. */
long long NextSessionStartBlock(const std::string& devicePath);

bool CreateImage(const BurnImageSpec& spec);

bool BurnImageFile(const std::string& imagePath, const std::string& devicePath,
                   const std::string& discType, int speed, int verifyLevel,
                   bool finalize, unsigned long long sessionStartBlock);

bool BurnAudioTracks(const std::vector<BurnAudioTrackSpec>& tracks,
                     const std::string& devicePath, int speed,
                     int verifyLevel, bool finalizeDisc);

bool EraseDisc(const std::string& devicePath, bool full);

bool CreateRawImage(const std::vector<BurnAudioTrackSpec>& tracks,
                    const std::string& outputPath, const std::string& mcn,
                    int startingTrackNumber);

bool BurnRawImage(const std::string& imagePath, const std::string& devicePath,
                  int speed, int verifyLevel);

bool VerifyWrite(const std::string& devicePath, const std::string& expectedImagePath,
                 int level);

int EjectTray(const std::string& devicePath);
int CloseTray(const std::string& devicePath);
std::string DriveInformation(const std::string& devicePath);

/* Physical disc currently inserted: short name and BURN_MEDIA_* code. */
std::string MediaType(const std::string& devicePath);
int MediaTypeCode(const std::string& devicePath);

/* Live BURN_MEDIA_STATE_* bitmask (or -1) and a short reason sentence. */
int MediaState(const std::string& devicePath);
std::string MediaStateText(const std::string& devicePath);

} /* namespace burnwin */

#endif /* _WIN32 */
#endif /* BURN_WINDOWS_FEATURES_H */
