// The disc: the PS2's drive (Platform::Disc) and the native build's reading of the disc image (ISO 9660), which stands for it
#include "native.h"
#include "platform/disc.h"

#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <unistd.h>

namespace
{
constexpr u32 SectorSize = 2048;
// The primary volume descriptor's sector, and its root directory record's place in it
constexpr u32 VolumeDescriptorSector = 16;
constexpr u32 RootRecordOffset = 156;

int g_Image = -1;
std::mutex g_Lock;
u32 g_RootSector = 0;
u32 g_RootSize = 0;

u32 Le32(const u8* bytes)
{
    return static_cast<u32>(bytes[0]) | static_cast<u32>(bytes[1]) << 8 | static_cast<u32>(bytes[2]) << 16 |
           static_cast<u32>(bytes[3]) << 24;
}

// A name as ISO 9660 compares it: upper case, without the version
std::string Normalised(const char* name, size_t length)
{
    std::string out;
    for (size_t i = 0; i < length && name[i] != ';'; i++)
    {
        out += static_cast<char>(std::toupper(static_cast<unsigned char>(name[i])));
    }

    // A file without an extension is recorded with a trailing dot
    if (!out.empty() && out.back() == '.')
    {
        out.pop_back();
    }

    return out;
}

// A directory's entry by name: its extent and size, and whether it's a directory
bool FindInDirectory(u32 sector, u32 size, const std::string& name, u32* foundSector, u32* foundSize, bool* directory)
{
    std::string buffer(size, '\0');
    if (Native::Disc::Read(static_cast<u64>(sector) * SectorSize, buffer.data(), size) != size)
    {
        return false;
    }

    const u8* bytes = reinterpret_cast<const u8*>(buffer.data());
    for (u32 at = 0; at < size;)
    {
        u8 length = bytes[at];
        if (length == 0)
        {
            // Records don't cross sectors: the rest of this one is empty
            at = (at / SectorSize + 1) * SectorSize;
            continue;
        }

        const u8* record = bytes + at;
        u8 nameLength = record[32];
        const char* recordName = reinterpret_cast<const char*>(record + 33);
        bool special = nameLength == 1 && (recordName[0] == 0 || recordName[0] == 1);
        if (!special && Normalised(recordName, nameLength) == name)
        {
            *foundSector = Le32(record + 2);
            *foundSize = Le32(record + 10);
            *directory = (record[25] & 2) != 0;
            return true;
        }

        at += length;
    }

    return false;
}
}

bool Native::Disc::Open(const char* path)
{
    std::lock_guard lock(g_Lock);
    if (g_Image >= 0)
    {
        return true;
    }

    g_Image = ::open(path, O_RDONLY);
    if (g_Image < 0)
    {
        return false;
    }

    u8 descriptor[SectorSize];
    if (::pread(g_Image, descriptor, SectorSize, static_cast<off_t>(VolumeDescriptorSector) * SectorSize) != SectorSize ||
        descriptor[0] != 1 || std::memcmp(descriptor + 1, "CD001", 5) != 0)
    {
        ::close(g_Image);
        g_Image = -1;
        return false;
    }

    g_RootSector = Le32(descriptor + RootRecordOffset + 2);
    g_RootSize = Le32(descriptor + RootRecordOffset + 10);
    return true;
}

bool Native::Disc::Find(const char* path, Entry* entry)
{
    // The device and leading separators go ("cdrom0:\CRASH6\..." as the PS2 side would have made it)
    const char* colon = std::strchr(path, ':');
    if (colon != nullptr)
    {
        path = colon + 1;
    }

    u32 sector = g_RootSector;
    u32 size = g_RootSize;
    bool directory = true;
    const char* part = path;
    while (*part != '\0')
    {
        while (*part == '\\' || *part == '/')
        {
            part++;
        }

        const char* end = part;
        while (*end != '\0' && *end != '\\' && *end != '/')
        {
            end++;
        }

        if (end == part)
        {
            break;
        }

        if (!directory || !FindInDirectory(sector, size, Normalised(part, static_cast<size_t>(end - part)), &sector, &size,
                                           &directory))
        {
            return false;
        }

        part = end;
    }

    if (directory)
    {
        return false;
    }

    entry->offset = static_cast<u64>(sector) * SectorSize;
    entry->size = size;
    return true;
}

u32 Native::Disc::Read(u64 offset, void* destination, u32 size)
{
    u32 done = 0;
    while (done < size)
    {
        ssize_t read = ::pread(g_Image, static_cast<u8*>(destination) + done, size - done, static_cast<off_t>(offset + done));
        if (read <= 0)
        {
            break;
        }

        done += static_cast<u32>(read);
    }

    return done;
}

// The PS2's drive: the native build's image is always in and ready
s32 Platform::Disc::Initialise()
{
    return 1;
}

s32 Platform::Disc::SetMedia(Media)
{
    return 1;
}

Platform::Disc::Readiness Platform::Disc::WaitReady()
{
    return Ready;
}
