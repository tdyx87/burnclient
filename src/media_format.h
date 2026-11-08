#ifndef BURN_MEDIA_FORMAT_H
#define BURN_MEDIA_FORMAT_H

/* ---------------------------------------------------------------------------
 * Audio input format helpers (internal, header-only).
 *
 * Both the CD-audio (track-at-once) and the RAW image paths take CD-DA
 * samples: 44100 Hz, 16-bit signed PCM, 2 channels, little-endian, packed
 * with no frame padding - exactly the audio payload of a Microsoft WAV
 * file. Callers may hand us either a full .wav (we locate the "data" chunk
 * and skip the header) or headerless raw samples (we use the whole file).
 *
 * The geometry checks exist because IMAPI2 writes whatever bytes it is
 * given: 44.056 kHz input, mono input, or a 22 kHz "compact disc" rip
 * silently becomes a disc that plays fast/slow or in one speaker. Better
 * one clear error than a ruined blank disc.
 * ------------------------------------------------------------------------- */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace burn_media {

/* CD-DA geometry. */
static const unsigned kCdAudioRate = 44100;
static const unsigned kCdAudioChannels = 2;
static const unsigned kCdAudioBits = 16;
static const unsigned kCdAudioFrameBytes = kCdAudioChannels * (kCdAudioBits / 8); /* 4 */
static const unsigned kCdSectorsPerSecond = 75;
static const unsigned kCdAudioBytesPerSector = 2352;      /* 75 frames */

struct AudioPayload {
    bool isWav = false;
    bool ok = false;
    unsigned long long dataOffset = 0;  /* first sample byte in the file */
    unsigned long long dataBytes = 0;   /* sample bytes available        */
    unsigned sampleRate = 0;
    unsigned channels = 0;
    unsigned bitsPerSample = 0;
    unsigned formatTag = 1;
    std::string error;                  /* populated when ok == false    */

    /* Whole-sector payload: IMAPI2 TAO and RAW writers want a multiple of
     * 2352 bytes; the tail (0..2351 bytes) is dropped and reported. */
    unsigned long long sectorAlignedBytes() const {
        return (dataBytes / kCdAudioBytesPerSector) * kCdAudioBytesPerSector;
    }
    unsigned long long truncatedTail() const {
        return dataBytes - sectorAlignedBytes();
    }
    unsigned long long durationSeconds() const {
        if (!ok || sampleRate == 0 || channels == 0 || bitsPerSample == 0) return 0;
        unsigned long long frameBytes = channels * (bitsPerSample / 8);
        if (frameBytes == 0) return 0;
        return (dataBytes / frameBytes) / sampleRate;
    }
};

static inline unsigned ReadLe32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static inline unsigned ReadLe16(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

/* Inspect a file: detect RIFF/WAVE, otherwise treat it as raw CD-DA.
 * No audio bytes are read here; only the header is parsed. */
static inline AudioPayload Probe(const std::string& path) {
    AudioPayload out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        out.error = "cannot open audio file: " + path;
        return out;
    }

    unsigned char head[12];
    if (std::fread(head, 1, 12, f) != 12) {
        out.error = "audio file too small: " + path;
        std::fclose(f);
        return out;
    }

    bool riff = std::memcmp(head, "RIFF", 4) == 0 && std::memcmp(head + 8, "WAVE", 4) == 0;
    if (!riff) {
        /* Headerless: assume raw CD-DA. The file size must be a multiple of
         * one frame, and in practice a multiple of a sector for a clean
         * write; geometry cannot be verified without a header. */
        std::fseek(f, 0, SEEK_END);
        long long sz = std::ftell(f);
        std::fclose(f);
        if (sz <= 0) { out.error = "audio file is empty: " + path; return out; }
        out.isWav = false;
        out.ok = true;
        out.dataOffset = 0;
        out.dataBytes = (unsigned long long)sz;
        out.sampleRate = kCdAudioRate;
        out.channels = kCdAudioChannels;
        out.bitsPerSample = kCdAudioBits;
        out.formatTag = 0xFFFE; /* none: headerless */
        return out;
    }

    out.isWav = true;
    /* Walk the chunk list. Chunk layout: 4-byte id, 4-byte LE size,
     * size bytes payload, one pad byte when size is odd. */
    long long pos = 12;
    bool haveFmt = false, haveData = false;
    unsigned char chunk[8];
    for (int guard = 0; guard < 256; ++guard) {
        if (std::fseek(f, (long)pos, SEEK_SET) != 0) break;
        if (std::fread(chunk, 1, 8, f) != 8) break;
        unsigned id = ReadLe32(chunk);
        unsigned size = ReadLe32(chunk + 4);

        if (id == ReadLe32((const unsigned char*)"fmt ") && size >= 16) {
            unsigned char fmt[16];
            if (std::fread(fmt, 1, 16, f) == 16) {
                unsigned tag = ReadLe16(fmt);
                out.channels = ReadLe16(fmt + 2);
                out.sampleRate = ReadLe32(fmt + 4);
                /* WAVEFORMATEX: wFormatTag(0) nChannels(2) nSamplesPerSec(4)
                 * nAvgBytesPerSec(8) nBlockAlign(12) wBitsPerSample(14).
                 * Reading bits at offset 12 would yield nBlockAlign - a
                 * 4-byte stereo 16-bit frame really is "4", which the
                 * geometry check would then report as a 4-bit stream. */
                out.bitsPerSample = ReadLe16(fmt + 14);

                /* WAVE_FORMAT_EXTENSIBLE (0xFFFE) stores the real codec in
                 * the first two bytes of the SubFormat GUID, which sits 22
                 * bytes after the start of the fmt body. */
                if (tag == 0xFFFE && size >= 40) {
                    long long fmtBody = pos + 8;
                    unsigned char sub[2] = { 1, 0 };
                    if (std::fseek(f, (long)(fmtBody + 22), SEEK_SET) == 0 &&
                        std::fread(sub, 1, 2, f) == 2) {
                        tag = ReadLe16(sub);
                    }
                }
                out.formatTag = tag;
                haveFmt = true;
            }
        } else if (id == ReadLe32((const unsigned char*)"data")) {
            /* Some writers leave the data size as 0xFFFFFFFF when the length
             * is unknown; fall back to the remaining file bytes. */
            long long start = pos + 8;
            std::fseek(f, 0, SEEK_END);
            long long end = std::ftell(f);
            unsigned long long avail = (end > start) ? (unsigned long long)(end - start) : 0;
            out.dataOffset = (unsigned long long)start;
            out.dataBytes = (size != 0xFFFFFFFFu && size <= avail) ? size : avail;
            haveData = true;
        }

        pos += 8 + (long long)size + ((size & 1) ? 1 : 0);
        if (haveFmt && haveData) break;
        if (pos < 12) break;
    }
    std::fclose(f);

    if (!haveFmt || !haveData) {
        out.error = "not a RIFF/WAVE file with fmt+data chunks: " + path;
        return out;
    }
    if (out.formatTag != 1) {
        char b[128];
        std::snprintf(b, sizeof(b),
                      "WAV codec is not PCM (format tag 0x%04X); convert to "
                      "uncompressed PCM first: %s", out.formatTag, path.c_str());
        out.error = b;
        return out;
    }
    if (out.sampleRate != kCdAudioRate || out.channels != kCdAudioChannels ||
        out.bitsPerSample != kCdAudioBits) {
        char b[192];
        std::snprintf(b, sizeof(b),
                      "CD-audio needs 44100 Hz / 2 channels / 16-bit PCM but '%s' is "
                      "%u Hz / %u ch / %u bit",
                      path.c_str(), out.sampleRate, out.channels, out.bitsPerSample);
        out.error = b;
        return out;
    }
    if (out.dataBytes < kCdAudioBytesPerSector) {
        out.error = "audio track is shorter than one CD sector (2352 bytes): " + path;
        return out;
    }
    out.ok = true;
    return out;
}

} /* namespace burn_media */

#endif /* BURN_MEDIA_FORMAT_H */
