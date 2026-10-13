// The disc: the PS2's drive (Platform::Disc) and the native build's reading of the disc image (ISO 9660), which stands for it.
// The mods folder (the settings folder's "mods"): a file there at an archive file's path (mods/Levels/Earth/Hub/beach.rm2 for
// CRASH.BD's Levels\Earth\Hub\beach.rm2, any case) is read in its place. CRASH.BH and CRASH.BD are then found at made-up places
// past the image: the table with the mods' entries pointing after the archive's data, and the data followed by the mods' files
#include "native.h"
#include "platform/disc.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

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

namespace
{
bool FindOnDisc(const char* path, Native::Disc::Entry* entry);
u32 ReadImage(u64 offset, void* destination, u32 size);

// The archive with the mods: where the table and the data are made to be, and the mods' files after the archive's data
constexpr u64 TablePlace = 1ull << 40;
constexpr u64 DataPlace = 2ull << 40;
struct ModFile
{
    u64 start;
    std::vector<u8> bytes;
};
struct Mods
{
    std::once_flag built;
    bool any = false;
    std::vector<u8> table;
    Native::Disc::Entry data{};
    u64 dataSize = 0;
    std::vector<ModFile> files;
};
Mods g_Mods;

// A path as the archive's table compares it: upper case, backslashes
std::string ArchiveKey(std::string path)
{
    std::replace(path.begin(), path.end(), '/', '\\');
    for (char& c : path)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    return path;
}

void BuildMods()
{
    std::error_code error;
    std::filesystem::path folder = std::filesystem::path(Native::SettingsFolder()) / "mods";
    if (!std::filesystem::is_directory(folder, error))
    {
        return;
    }

    std::unordered_map<std::string, std::filesystem::path> found;
    for (auto it = std::filesystem::recursive_directory_iterator(folder, error); !error && it != std::filesystem::recursive_directory_iterator();
         it.increment(error))
    {
        if (it->is_regular_file(error) && it->path().filename().string()[0] != '.')
        {
            found[ArchiveKey(std::filesystem::relative(it->path(), folder, error).generic_string())] = it->path();
        }
    }

    Native::Disc::Entry table;
    Native::Disc::Entry data;
    if (found.empty() || !FindOnDisc("CRASH6/CRASH.BH", &table) || !FindOnDisc("CRASH6/CRASH.BD", &data))
    {
        return;
    }

    // CRASH.BH: a word, then each file's name (its length first), its offset in CRASH.BD and its size
    std::vector<u8> bytes(table.size);
    if (ReadImage(table.offset, bytes.data(), table.size) != table.size)
    {
        return;
    }

    u64 end = (static_cast<u64>(data.size) + 0x7FF) & ~0x7FFull;
    u32 used = 0;
    for (u32 at = 4; at + 4 <= bytes.size();)
    {
        u32 length;
        std::memcpy(&length, bytes.data() + at, 4);
        if (length == 0 || at + 12 + length > bytes.size())
        {
            break;
        }

        std::string name(reinterpret_cast<const char*>(bytes.data() + at + 4), length);
        name = name.c_str();
        auto mod = found.find(ArchiveKey(name));
        if (mod != found.end())
        {
            std::ifstream file(mod->second, std::ios::binary);
            ModFile read{end, std::vector<u8>(std::istreambuf_iterator<char>(file), {})};
            u32 offset = static_cast<u32>(end);
            u32 size = static_cast<u32>(read.bytes.size());
            std::memcpy(bytes.data() + at + 4 + length, &offset, 4);
            std::memcpy(bytes.data() + at + 8 + length, &size, 4);
            Native::Log("mods: %s from %s (%u bytes)", name.c_str(), mod->second.c_str(), size);
            end = (end + size + 0x7FF) & ~0x7FFull;
            g_Mods.files.push_back(std::move(read));
            used++;
        }

        at += 12 + length;
    }

    if (used == 0)
    {
        Native::Log("mods: nothing in %s is a file of the game's archive", folder.c_str());
        return;
    }

    g_Mods.any = true;
    g_Mods.table = std::move(bytes);
    g_Mods.data = data;
    g_Mods.dataSize = end;
}

bool FindMod(const char* path, Native::Disc::Entry* entry)
{
    std::call_once(g_Mods.built, BuildMods);
    if (!g_Mods.any)
    {
        return false;
    }

    std::string key = ArchiveKey(path);
    key = key.substr(0, key.find(';'));
    key.erase(0, key.find_first_not_of('\\'));
    if (key == "CRASH6\\CRASH.BH")
    {
        *entry = {TablePlace, static_cast<u32>(g_Mods.table.size())};
        return true;
    }

    if (key == "CRASH6\\CRASH.BD")
    {
        *entry = {DataPlace, static_cast<u32>(g_Mods.dataSize)};
        return true;
    }

    return false;
}

// What's read past the image: the mods' table, or the archive's data and the mods' files after it
u32 ReadMods(u64 offset, u8* destination, u32 size)
{
    if (offset < DataPlace)
    {
        u64 at = offset - TablePlace;
        u32 count = at < g_Mods.table.size() ? static_cast<u32>(std::min<u64>(size, g_Mods.table.size() - at)) : 0;
        std::memcpy(destination, g_Mods.table.data() + at, count);
        return count;
    }

    u32 done = 0;
    while (done < size)
    {
        u64 at = offset - DataPlace + done;
        u32 count = 0;
        if (at < g_Mods.data.size)
        {
            count = ReadImage(g_Mods.data.offset + at, destination + done, static_cast<u32>(std::min<u64>(size - done, g_Mods.data.size - at)));
        }
        else
        {
            // A mod's file, or the padding between them (zeros)
            auto file = std::find_if(g_Mods.files.begin(), g_Mods.files.end(),
                                     [at](const ModFile& mod) { return at >= mod.start && at < mod.start + ((mod.bytes.size() + 0x7FF) & ~size_t{0x7FF}); });
            if (file == g_Mods.files.end() || at >= g_Mods.dataSize)
            {
                break;
            }

            u64 within = at - file->start;
            u64 span = ((file->bytes.size() + 0x7FF) & ~size_t{0x7FF}) - within;
            count = static_cast<u32>(std::min<u64>(size - done, span));
            for (u32 i = 0; i < count; i++)
            {
                destination[done + i] = within + i < file->bytes.size() ? file->bytes[within + i] : 0;
            }
        }

        if (count == 0)
        {
            break;
        }

        done += count;
    }

    return done;
}
}

bool Native::Disc::Find(const char* path, Entry* entry)
{
    return FindMod(path, entry) || FindOnDisc(path, entry);
}

u32 Native::Disc::Read(u64 offset, void* destination, u32 size)
{
    return offset >= TablePlace ? ReadMods(offset, static_cast<u8*>(destination), size) : ReadImage(offset, destination, size);
}

namespace
{
bool FindOnDisc(const char* path, Native::Disc::Entry* entry)
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

u32 ReadImage(u64 offset, void* destination, u32 size)
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
