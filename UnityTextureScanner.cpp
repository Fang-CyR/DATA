#include "UnityTextureScanner.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <algorithm>

#include "lz4.h"
#include <zlib.h>


// ============================================================
// Helpers
// ============================================================

static size_t AlignUp(
    size_t value,
    size_t alignment)
{
    if (alignment == 0)
        return value;

    return (value + alignment - 1) &
           ~(alignment - 1);
}

static size_t Align4Value(size_t value)
{
    return AlignUp(value, 4);
}

static size_t Align8(size_t value)
{
    return AlignUp(value, 8);
}

static size_t Align16(size_t value)
{
    return AlignUp(value, 16);
}

static std::string BaseName(
    const std::string& path)
{
    size_t p = path.find_last_of("/\\");

    if (p == std::string::npos)
        return path;

    return path.substr(p + 1);
}

static bool EndsWith(
    const std::string& value,
    const std::string& suffix)
{
    if (value.size() < suffix.size())
        return false;

    return value.compare(
               value.size() - suffix.size(),
               suffix.size(),
               suffix) == 0;
}


// ============================================================
// Reader
// ============================================================

class Reader
{
public:
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
    bool littleEndian = true;

    Reader() = default;

    Reader(const uint8_t* p, size_t s, bool little = true)
        : data(p), size(s), pos(0), littleEndian(little)
    {
    }

    bool CanRead(size_t n) const
    {
        return pos <= size && n <= (size - pos);
    }

    size_t Remaining() const
    {
        return pos <= size ? size - pos : 0;
    }

    bool Seek(size_t p)
    {
        if (p > size)
            return false;

        pos = p;
        return true;
    }

    bool Skip(size_t n)
    {
        if (!CanRead(n))
            return false;

        pos += n;
        return true;
    }

    void Align4()
    {
        size_t p = Align4Value(pos);

        if (p <= size)
            pos = p;
        else
            pos = size;
    }

    uint8_t ReadU8()
    {
        if (!CanRead(1))
            return 0;

        return data[pos++];
    }

    int8_t ReadI8()
    {
        return static_cast<int8_t>(ReadU8());
    }

    uint16_t ReadU16()
    {
        if (!CanRead(2))
        {
            pos = size;
            return 0;
        }

        uint16_t v;

        if (littleEndian)
        {
            v =
                static_cast<uint16_t>(data[pos]) |
                (static_cast<uint16_t>(data[pos + 1]) << 8);
        }
        else
        {
            v =
                (static_cast<uint16_t>(data[pos]) << 8) |
                static_cast<uint16_t>(data[pos + 1]);
        }

        pos += 2;
        return v;
    }

    int16_t ReadI16()
    {
        return static_cast<int16_t>(ReadU16());
    }

    uint32_t ReadU32()
    {
        if (!CanRead(4))
        {
            pos = size;
            return 0;
        }

        uint32_t v;

        if (littleEndian)
        {
            v =
                static_cast<uint32_t>(data[pos]) |
                (static_cast<uint32_t>(data[pos + 1]) << 8) |
                (static_cast<uint32_t>(data[pos + 2]) << 16) |
                (static_cast<uint32_t>(data[pos + 3]) << 24);
        }
        else
        {
            v =
                (static_cast<uint32_t>(data[pos]) << 24) |
                (static_cast<uint32_t>(data[pos + 1]) << 16) |
                (static_cast<uint32_t>(data[pos + 2]) << 8) |
                static_cast<uint32_t>(data[pos + 3]);
        }

        pos += 4;
        return v;
    }

    int32_t ReadI32()
    {
        return static_cast<int32_t>(ReadU32());
    }

    uint64_t ReadU64()
    {
        if (!CanRead(8))
        {
            pos = size;
            return 0;
        }

        uint64_t v = 0;

        if (littleEndian)
        {
            v =
                static_cast<uint64_t>(data[pos]) |
                (static_cast<uint64_t>(data[pos + 1]) << 8) |
                (static_cast<uint64_t>(data[pos + 2]) << 16) |
                (static_cast<uint64_t>(data[pos + 3]) << 24) |
                (static_cast<uint64_t>(data[pos + 4]) << 32) |
                (static_cast<uint64_t>(data[pos + 5]) << 40) |
                (static_cast<uint64_t>(data[pos + 6]) << 48) |
                (static_cast<uint64_t>(data[pos + 7]) << 56);
        }
        else
        {
            v =
                (static_cast<uint64_t>(data[pos]) << 56) |
                (static_cast<uint64_t>(data[pos + 1]) << 48) |
                (static_cast<uint64_t>(data[pos + 2]) << 40) |
                (static_cast<uint64_t>(data[pos + 3]) << 32) |
                (static_cast<uint64_t>(data[pos + 4]) << 24) |
                (static_cast<uint64_t>(data[pos + 5]) << 16) |
                (static_cast<uint64_t>(data[pos + 6]) << 8) |
                static_cast<uint64_t>(data[pos + 7]);
        }

        pos += 8;
        return v;
    }

    int64_t ReadI64()
    {
        return static_cast<int64_t>(ReadU64());
    }

    float ReadFloat()
    {
        uint32_t bits = ReadU32();

        float value;
        std::memcpy(&value, &bits, sizeof(value));

        return value;
    }

    std::string ReadCString()
    {
        std::string result;

        while (pos < size)
        {
            char c = static_cast<char>(data[pos++]);

            if (c == '\0')
                break;

            result.push_back(c);
        }

        return result;
    }
};


// ============================================================
// Common TypeTree Strings
// ============================================================

static const std::map<uint32_t, std::string>
kCommonStrings =
{
    {0, "AABB"},
    {5, "AnimationClip"},
    {18, "AnimationCurve"},
    {34, "AnimationState"},
    {50, "Array"},
    {56, "Base"},
    {61, "BitField"},
    {70, "bitset"},
    {77, "bool"},
    {82, "char"},
    {87, "ColorRGBA"},
    {97, "Component"},
    {107, "data"},
    {112, "deque"},
    {118, "double"},
    {125, "FastPropertyName"},
    {142, "first"},
    {148, "float"},
    {154, "Font"},
    {159, "GameObject"},
    {170, "Generic Mono"},
    {183, "GradientNEW"},
    {195, "GUID"},
    {200, "GUIStyle"},
    {209, "GUIStyleState"},
    {224, "int"},
    {228, "list"},
    {233, "List"},
    {238, "long long"},
    {248, "map"},
    {252, "Matrix4x4f"},
    {263, "MonoBehaviour"},
    {276, "MonoScript"},
    {287, "m_ByteSize"},
    {298, "m_Curve"},
    {305, "m_EditorClassIdentifier"},
    {329, "m_EditorHideFlags"},
    {347, "m_Enabled"},
    {356, "m_ExtensionPtr"},
    {369, "m_GameObject"},
    {381, "m_Index"},
    {389, "m_IsActive"},
    {400, "m_IsEnabled"},
    {411, "m_IsReadable"},
    {423, "m_Name"},
    {430, "m_ObjectHideFlags"},
    {448, "m_PrefabInternal"},
    {464, "m_PrefabParentObject"},
    {483, "m_Script"},
    {491, "m_StaticEditorFlags"},
    {510, "m_StreamData"},
    {523, "m_Type"},
    {530, "m_Version"},
    {538, "Object"},
    {545, "pair"},
    {550, "PPtr<Component>"},
    {566, "PPtr<GameObject>"},
    {584, "PPtr<Material>"},
    {600, "PPtr<MonoBehaviour>"},
    {620, "PPtr<MonoScript>"},
    {638, "PPtr<Object>"},
    {652, "PPtr<Prefab>"},
    {665, "PPtr<Sprite>"},
    {677, "PPtr<TextAsset>"},
    {692, "PPtr<Texture>"},
    {705, "PPtr<Texture2D>"},
    {721, "PPtr<Transform>"},
    {739, "Prefab"},
    {746, "Quaternionf"},
    {758, "Rectf"},
    {764, "RectInt"},
    {772, "RectOffset"},
    {782, "second"},
    {789, "set"},
    {793, "short"},
    {799, "size"},
    {804, "SInt16"},
    {811, "SInt32"},
    {818, "SInt64"},
    {825, "SInt8"},
    {831, "staticvector"},
    {844, "string"},
    {851, "TextAsset"},
    {860, "Texture"},
    {868, "Texture2D"},
    {878, "Transform"},
    {888, "TypelessData"},
    {900, "UInt16"},
    {907, "UInt32"},
    {914, "UInt64"},
    {921, "UInt8"},
    {927, "unsigned int"},
    {940, "unsigned long long"},
    {961, "unsigned short"},
    {976, "vector"},
    {983, "Vector2f"},
    {992, "Vector3f"},
    {1001, "Vector4f"},
    {1010, "m_Width"},
    {1018, "m_Height"},
    {1026, "m_TextureFormat"},
    {1041, "m_MipCount"},
    {1052, "m_CompleteImageSize"},
    {1071, "image data"},
    {1082, "offset"},
    {1089, "path"},
    {1094, "m_StreamData"},
    {1106, "m_Lightmap"},
    {1116, "m_ColorSpace"},
    {1128, "m_IsColorData"},
    {1141, "m_IgnoreMasterTextureLimit"},
    {1152, "FileSize"},
    {1161, "Hash128"}
};

static std::string ResolveCommonString(
    const std::vector<uint8_t>& stringBuffer,
    uint32_t value)
{
    if (value & 0x80000000U)
    {
        uint32_t offset =
            value & 0x7FFFFFFFU;

        auto it =
            kCommonStrings.find(offset);

        if (it != kCommonStrings.end())
            return it->second;

        char temp[64];

        std::snprintf(
            temp,
            sizeof(temp),
            "<common:%u>",
            offset);

        return temp;
    }

    uint32_t offset = value;

    if (offset >= stringBuffer.size())
        return {};

    size_t p = offset;

    std::string result;

    while (p < stringBuffer.size())
    {
        char c =
            (char)stringBuffer[p++];

        if (c == '\0')
            break;

        result.push_back(c);
    }

    return result;
}


// ============================================================
// TypeTree
// ============================================================

struct TypeTreeNode
{
    uint16_t version = 0;
    uint8_t level = 0;
    uint8_t typeFlags = 0;

    uint32_t typeStringOffset = 0;
    uint32_t nameStringOffset = 0;

    int32_t byteSize = 0;
    int32_t index = 0;
    int32_t metaFlags = 0;

    uint64_t refTypeHash = 0;

    std::string type;
    std::string name;

    std::vector<TypeTreeNode> children;

    bool IsArray() const
    {
        return (typeFlags & 1) != 0;
    }

    bool AlignAfter() const
    {
        return (metaFlags & 0x4000) != 0;
    }
};


// ============================================================
// Serialized structures
// ============================================================

struct SerializedType
{
    int32_t classID = -1;

    bool stripped = false;
    int16_t scriptTypeIndex = -1;

    std::vector<uint8_t> scriptID;
    std::vector<uint8_t> oldTypeHash;

    TypeTreeNode typeTree;

    std::vector<int32_t> dependencies;
};

struct SerializedObject
{
    int64_t pathID = 0;

    uint64_t byteStart = 0;
    uint32_t byteSize = 0;

    int32_t typeID = -1;
    int32_t classID = -1;
};


// ============================================================
// Texture Format
// ============================================================

static const char* TextureFormatName(
    int format)
{
    switch (format)
    {
        case 0: return "None";
        case 1: return "Alpha8";
        case 2: return "ARGB4444";
        case 3: return "RGB24";
        case 4: return "RGBA32";
        case 5: return "ARGB32";
        case 6: return "ARGBFloat";
        case 7: return "RGB565";
        case 8: return "BGR24";
        case 9: return "R16";
        case 10: return "DXT1";
        case 11: return "DXT3";
        case 12: return "DXT5";
        case 13: return "RGBA4444";
        case 14: return "BGRA32";
        case 15: return "RHalf";
        case 16: return "RGHalf";
        case 17: return "RGBAHalf";
        case 18: return "RFloat";
        case 19: return "RGFloat";
        case 20: return "RGBAFloat";
        case 21: return "YUY2";
        case 22: return "RGB9e5Float";
        case 23: return "RGBFloat";
        case 24: return "BC6H";
        case 25: return "BC7";
        case 26: return "BC4";
        case 27: return "BC5";
        case 28: return "DXT1Crunched";
        case 29: return "DXT5Crunched";
        case 30: return "PVRTC_RGB2";
        case 31: return "PVRTC_RGBA2";
        case 32: return "PVRTC_RGB4";
        case 33: return "PVRTC_RGBA4";
        case 34: return "ETC_RGB4";
        case 35: return "ATC_RGB4";
        case 36: return "ATC_RGBA8";
        case 37: return "EAC_R";
        case 38: return "EAC_R_SIGNED";
        case 39: return "EAC_RG";
        case 40: return "EAC_RG_SIGNED";
        case 41: return "ETC2_RGB";
        case 42: return "ETC2_RGBA1";
        case 43: return "ETC2_RGBA8";
        case 44: return "ASTC_RGB_4x4";
        case 45: return "ASTC_RGB_5x5";
        case 46: return "ASTC_RGB_6x6";
        case 47: return "ASTC_RGB_8x8";
        case 48: return "ASTC_RGB_10x10";
        case 49: return "ASTC_RGB_12x12";
        case 50: return "ASTC_RGBA_4x4";
        case 51: return "ASTC_RGBA_5x5";
        case 52: return "ASTC_RGBA_6x6";
        case 53: return "ASTC_RGBA_8x8";
        case 54: return "ASTC_RGBA_10x10";
        case 55: return "ASTC_RGBA_12x12";
        case 56: return "ETC_RGB4_3DS";
        case 57: return "ETC_RGBA8_3DS";
        case 58: return "RG16";
        case 59: return "R8";
        case 60: return "ETC_RGB4Crunched";
        case 61: return "ETC2_RGBA8Crunched";
        case 62: return "ASTC_HDR_4x4";
        case 63: return "ASTC_HDR_5x5";
        case 64: return "ASTC_HDR_6x6";
        case 65: return "ASTC_HDR_8x8";
        case 66: return "ASTC_HDR_10x10";
        case 67: return "ASTC_HDR_12x12";
        case 68: return "RG32";
        case 69: return "RGB48";
        case 70: return "RGBA64";
        default: return "Unknown";
    }
}


// ============================================================
// UnityFS
// ============================================================

struct UnityFSHeader
{
    std::string signature;

    uint32_t formatVersion = 0;

    std::string playerVersion;
    std::string engineVersion;

    uint64_t bundleSize = 0;

    uint32_t compressedBlocksInfoSize = 0;
    uint32_t uncompressedBlocksInfoSize = 0;

    uint32_t flags = 0;

    size_t headerEnd = 0;
    size_t blocksInfoOffset = 0;
    size_t dataStart = 0;
};

struct UnityFSBlock
{
    uint32_t compressedSize = 0;
    uint32_t uncompressedSize = 0;
    uint16_t flags = 0;
};

struct UnityFSDirectoryEntry
{
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t flags = 0;
    std::string path;
};


// ============================================================
// File
// ============================================================

static bool LoadFile(
    const std::string& path,
    std::vector<uint8_t>& output)
{
    FILE* fp =
        std::fopen(
            path.c_str(),
            "rb");

    if (!fp)
        return false;

    std::fseek(
        fp,
        0,
        SEEK_END);

    long fileSize =
        std::ftell(fp);

    if (fileSize <= 0)
    {
        std::fclose(fp);
        return false;
    }

    std::fseek(
        fp,
        0,
        SEEK_SET);

    output.resize(
        (size_t)fileSize);

    size_t read =
        std::fread(
            output.data(),
            1,
            output.size(),
            fp);

    std::fclose(fp);

    if (read != output.size())
    {
        output.clear();
        return false;
    }

    return true;
}


// ============================================================
// Compression
// ============================================================

static bool DecompressLZ4(
    const uint8_t* src,
    size_t srcSize,
    uint8_t* dst,
    size_t dstSize)
{
    if (srcSize > INT32_MAX ||
        dstSize > INT32_MAX)
    {
        return false;
    }

    int result =
        LZ4_decompress_safe(
            (const char*)src,
            (char*)dst,
            (int)srcSize,
            (int)dstSize);

    return
        result >= 0 &&
        (size_t)result == dstSize;
}

static bool DecompressZlib(
    const uint8_t* src,
    size_t srcSize,
    uint8_t* dst,
    size_t dstSize)
{
    uLongf outputSize =
        (uLongf)dstSize;

    int result =
        uncompress(
            dst,
            &outputSize,
            src,
            (uLong)srcSize);

    return
        result == Z_OK &&
        outputSize == dstSize;
}

static bool DecompressUnityBlock(
    const uint8_t* src,
    size_t srcSize,
    uint8_t* dst,
    size_t dstSize,
    uint16_t flags)
{
    uint16_t compression =
        flags & 0x3F;

    switch (compression)
    {
        case 0:
        {
            if (srcSize != dstSize)
                return false;

            std::memcpy(
                dst,
                src,
                dstSize);

            return true;
        }

        case 2:
        case 3:
        {
            return DecompressLZ4(
                src,
                srcSize,
                dst,
                dstSize);
        }

        case 4:
        {
            return DecompressZlib(
                src,
                srcSize,
                dst,
                dstSize);
        }

        case 1:
        default:
            return false;
    }
}


// ============================================================
// UnityFS Bundle
// ============================================================

class UnityFSBundle
{
public:

    std::vector<uint8_t> fileData;
    std::vector<uint8_t> contentData;

    UnityFSHeader header;

    std::vector<UnityFSBlock> blocks;
    std::vector<UnityFSDirectoryEntry> directory;

    bool ParseHeader()
    {
        if (fileData.empty())
            return false;

        Reader r(
            fileData.data(),
            fileData.size(),
            false);

        header.signature =
            r.ReadCString();

        if (header.signature != "UnityFS")
            return false;

        header.formatVersion =
            r.ReadU32();

        header.playerVersion =
            r.ReadCString();

        header.engineVersion =
            r.ReadCString();

        header.bundleSize =
            r.ReadU64();

        header.compressedBlocksInfoSize =
            r.ReadU32();

        header.uncompressedBlocksInfoSize =
            r.ReadU32();

        header.flags =
            r.ReadU32();

        header.headerEnd =
            r.pos;

        if (header.flags & 0x80)
        {
            if (fileData.size() <
                header.compressedBlocksInfoSize)
            {
                return false;
            }

            header.blocksInfoOffset =
                fileData.size() -
                header.compressedBlocksInfoSize;

            header.dataStart =
                Align16(
                    header.headerEnd);
        }
        else
        {
            header.blocksInfoOffset =
                Align16(
                    header.headerEnd);

            header.dataStart =
                header.blocksInfoOffset +
                header.compressedBlocksInfoSize;
        }

        return true;
    }

    bool ParseBlocksInfo()
    {
        if (header.blocksInfoOffset +
            header.compressedBlocksInfoSize >
            fileData.size())
        {
            return false;
        }

        const uint8_t* src =
            fileData.data() +
            header.blocksInfoOffset;

        std::vector<uint8_t> decoded;

        decoded.resize(
            header.uncompressedBlocksInfoSize);

        uint16_t compression =
            header.flags & 0x3F;

        bool ok = false;

        if (compression == 0)
        {
            if (header.compressedBlocksInfoSize !=
                header.uncompressedBlocksInfoSize)
            {
                return false;
            }

            std::memcpy(
                decoded.data(),
                src,
                decoded.size());

            ok = true;
        }
        else if (compression == 2 ||
                 compression == 3)
        {
            ok =
                DecompressLZ4(
                    src,
                    header.compressedBlocksInfoSize,
                    decoded.data(),
                    decoded.size());
        }

        if (!ok)
            return false;

        Reader r(
            decoded.data(),
            decoded.size(),
            false);

        if (!r.Skip(16))
            return false;

        uint32_t blockCount =
            r.ReadU32();

        if (blockCount > 1000000)
            return false;

        blocks.clear();
        blocks.reserve(blockCount);

        for (uint32_t i = 0;
             i < blockCount;
             ++i)
        {
            UnityFSBlock block;

            block.uncompressedSize =
                r.ReadU32();

            block.compressedSize =
                r.ReadU32();

            block.flags =
                r.ReadU16();

            blocks.push_back(block);
        }

        uint32_t directoryCount =
            r.ReadU32();

        if (directoryCount > 1000000)
            return false;

        directory.clear();
        directory.reserve(directoryCount);

        for (uint32_t i = 0;
             i < directoryCount;
             ++i)
        {
            UnityFSDirectoryEntry entry;

            entry.offset =
                r.ReadU64();

            entry.size =
                r.ReadU64();

            entry.flags =
                r.ReadU32();

            entry.path =
                r.ReadCString();

            directory.push_back(
                std::move(entry));
        }

        return true;
    }

    bool DecompressContent()
    {
        size_t compressedPos =
            header.dataStart;

        uint64_t totalSize = 0;

        for (const auto& block : blocks)
        {
            totalSize +=
                block.uncompressedSize;

            if (totalSize >
                SIZE_MAX)
            {
                return false;
            }
        }

        if (totalSize == 0)
            return false;

        contentData.resize(
            (size_t)totalSize);

        size_t outputPos = 0;

        for (const auto& block : blocks)
        {
            if (compressedPos +
                block.compressedSize >
                fileData.size())
            {
                return false;
            }

            if (outputPos +
                block.uncompressedSize >
                contentData.size())
            {
                return false;
            }

            const uint8_t* src =
                fileData.data() +
                compressedPos;

            uint8_t* dst =
                contentData.data() +
                outputPos;

            if (!DecompressUnityBlock(
                    src,
                    block.compressedSize,
                    dst,
                    block.uncompressedSize,
                    block.flags))
            {
                return false;
            }

            compressedPos +=
                block.compressedSize;

            outputPos +=
                block.uncompressedSize;
        }

        return true;
    }

    const UnityFSDirectoryEntry*
    FindResourceForPath(
        const std::string& path) const
    {
        std::string wanted =
            BaseName(path);

        for (const auto& entry :
             directory)
        {
            std::string base =
                BaseName(entry.path);

            if (base == wanted)
                return &entry;

            if (EndsWith(
                    base,
                    wanted))
            {
                return &entry;
            }
        }

        return nullptr;
    }

    bool GetEntryData(
        const UnityFSDirectoryEntry& entry,
        const uint8_t*& data,
        size_t& size) const
    {
        if (entry.offset >
            contentData.size())
        {
            return false;
        }

        if (entry.size >
            contentData.size() -
            (size_t)entry.offset)
        {
            return false;
        }

        data =
            contentData.data() +
            (size_t)entry.offset;

        size =
            (size_t)entry.size;

        return true;
    }
};


// ============================================================
// SerializedFile Parser
// ============================================================

class SerializedFileParser
{
public:

    const uint8_t* data = nullptr;
    size_t size = 0;

    uint32_t version = 0;

    uint64_t metadataSize = 0;
    uint64_t fileSize = 0;
    uint64_t dataOffset = 0;

    bool littleEndian = true;

    std::vector<SerializedType> types;
    std::vector<SerializedObject> objects;

    bool Parse(
        const uint8_t* p,
        size_t s)
    {
        data = p;
        size = s;

        if (!data || size < 20)
            return false;

        uint32_t v =
            ReadBE32(8);

        if (v < 1 || v > 50)
        {
            v = ReadLE32(8);

            if (v < 1 || v > 50)
                return false;
        }

        version = v;

        if (version >= 22)
            return ParseModern();

        return ParseLegacy();
    }

private:

    uint32_t ReadBE32(size_t o) const
    {
        if (o + 4 > size)
            return 0;

        return
            ((uint32_t)data[o] << 24) |
            ((uint32_t)data[o + 1] << 16) |
            ((uint32_t)data[o + 2] << 8) |
            data[o + 3];
    }

    uint64_t ReadBE64(size_t o) const
    {
        if (o + 8 > size)
            return 0;

        uint64_t v = 0;

        for (int i = 0; i < 8; ++i)
            v = (v << 8) | data[o + i];

        return v;
    }

    uint32_t ReadLE32(size_t o) const
    {
        if (o + 4 > size)
            return 0;

        return
            ((uint32_t)data[o]) |
            ((uint32_t)data[o + 1] << 8) |
            ((uint32_t)data[o + 2] << 16) |
            ((uint32_t)data[o + 3] << 24);
    }

    bool ParseModern()
    {
        if (size < 48)
            return false;

        metadataSize =
            ReadBE64(16);

        fileSize =
            ReadBE64(24);

        dataOffset =
            ReadBE64(32);

        littleEndian =
            data[40] == 0;

        Reader r(
            data,
            size,
            littleEndian);

        if (!r.Seek(48))
            return false;

        return ParseMetadata(r);
    }

    bool ParseLegacy()
{
    std::printf("\n[SerializedFile] ParseLegacy()\n");

    Reader r(data, size, false);

    metadataSize = r.ReadU32();
    fileSize     = r.ReadU32();
    version      = r.ReadU32();
    dataOffset   = r.ReadU32();

    uint8_t endian = r.ReadU8();

    r.Skip(3);

    littleEndian = (endian == 0);

    // IMPORTANT:
    // Metadata menggunakan endian flag dari SerializedFile header.
    r.littleEndian = littleEndian;

    std::printf("[SerializedFile] Header:\n");
    std::printf("  metadataSize = %llu\n",
                (unsigned long long)metadataSize);
    std::printf("  fileSize     = %llu\n",
                (unsigned long long)fileSize);
    std::printf("  version      = %u\n", version);
    std::printf("  dataOffset   = %llu\n",
                (unsigned long long)dataOffset);
    std::printf("  endian       = %u\n", endian);
    std::printf("  littleEndian = %s\n",
                littleEndian ? "true" : "false");
    std::printf("  metadataPos  = %zu\n", r.pos);

    return ParseMetadata(r);
}

    bool ParseMetadata(Reader& r)
{
    std::printf(
        "\n[SerializedFile] ParseMetadata()\n"
        "  start pos = %zu\n"
        "  remaining = %zu\n",
        r.pos,
        r.Remaining());

    // ------------------------------------------------------------
    // Unity version
    // ------------------------------------------------------------

    if (version >= 7)
    {
        std::string unityVersion =
            r.ReadCString();
            
        std::printf("  After Unity version pos = %zu\n", r.pos);

if (r.CanRead(8))
{
    std::printf(
        "  RAW metadata bytes: "
        "%02X %02X %02X %02X "
        "%02X %02X %02X %02X\n",
        r.data[r.pos + 0],
        r.data[r.pos + 1],
        r.data[r.pos + 2],
        r.data[r.pos + 3],
        r.data[r.pos + 4],
        r.data[r.pos + 5],
        r.data[r.pos + 6],
        r.data[r.pos + 7]
    );
}

std::printf("  Reader littleEndian = %s\n",
            r.littleEndian ? "true" : "false");

        std::printf(
            "  Unity version : %s\n",
            unityVersion.c_str());
    }

    // ------------------------------------------------------------
    // Target platform
    // ------------------------------------------------------------

    if (version >= 8)
    {
        int32_t targetPlatform =
            r.ReadI32();

        std::printf(
            "  Target platform : %d\n",
            targetPlatform);
    }

    // ------------------------------------------------------------
    // TypeTree enabled
    // ------------------------------------------------------------

    bool enableTypeTree = true;

    if (version >= 13)
    {
        enableTypeTree =
            r.ReadU8() != 0;

        std::printf(
            "  Enable TypeTree : %s\n",
            enableTypeTree ? "true" : "false");
    }

    if (!enableTypeTree)
    {
        std::printf(
            "  ERROR: TypeTree disabled\n");

        return false;
    }

    // ------------------------------------------------------------
    // Type count
    // ------------------------------------------------------------

    int32_t typeCount =
        r.ReadI32();

    std::printf(
        "  Type count : %d\n"
        "  Position   : %zu\n",
        typeCount,
        r.pos);

    if (typeCount < 0 ||
        typeCount > 100000)
    {
        std::printf(
            "  ERROR: invalid typeCount\n");

        return false;
    }

    types.clear();
    types.reserve(typeCount);

    // ------------------------------------------------------------
    // Types
    // ------------------------------------------------------------

    for (int32_t i = 0;
         i < typeCount;
         ++i)
    {
        SerializedType type;

        size_t before =
            r.pos;

        if (!ReadSerializedType(
                r,
                false,
                type))
        {
            std::printf(
                "  ERROR: ReadSerializedType failed\n"
                "    type index = %d\n"
                "    position   = %zu\n"
                "    before     = %zu\n",
                i,
                r.pos,
                before);

            return false;
        }

        std::printf(
            "  Type[%d] classID=%d\n",
            i,
            type.classID);

        types.push_back(
            std::move(type));
    }

    // ------------------------------------------------------------
    // bigIDEnabled
    // ------------------------------------------------------------

    int32_t bigIDEnabled = 0;

    if (version >= 7 &&
        version < 14)
    {
        bigIDEnabled =
            r.ReadI32();

        std::printf(
            "  bigIDEnabled = %d\n",
            bigIDEnabled);
    }

    // ------------------------------------------------------------
    // Object count
    // ------------------------------------------------------------

    int32_t objectCount =
        r.ReadI32();

    std::printf(
        "  Object count : %d\n"
        "  Position     : %zu\n",
        objectCount,
        r.pos);

    if (objectCount < 0 ||
        objectCount > 10000000)
    {
        std::printf(
            "  ERROR: invalid objectCount\n");

        return false;
    }

    objects.clear();
    objects.reserve(objectCount);

    // ------------------------------------------------------------
    // Objects
    // ------------------------------------------------------------

    for (int32_t i = 0;
         i < objectCount;
         ++i)
    {
        SerializedObject obj;

        if (bigIDEnabled)
        {
            obj.pathID =
                r.ReadI64();
        }
        else if (version < 14)
        {
            obj.pathID =
                r.ReadI32();
        }
        else
        {
            r.Align4();

            obj.pathID =
                r.ReadI64();
        }

        if (version >= 22)
            obj.byteStart =
                r.ReadU64();
        else
            obj.byteStart =
                r.ReadU32();

        obj.byteStart +=
            dataOffset;

        obj.byteSize =
            r.ReadU32();

        obj.typeID =
            r.ReadI32();

        if (version < 16)
        {
            obj.classID =
                (int32_t)r.ReadU16();
        }
        else if (
            obj.typeID >= 0 &&
            obj.typeID <
                (int32_t)types.size())
        {
            obj.classID =
                types[obj.typeID].classID;
        }

        if (version >= 11 &&
            version < 17)
        {
            r.ReadI16();
        }

        if (version == 15 ||
            version == 16)
        {
            r.ReadU8();
        }

        objects.push_back(obj);
    }

    std::printf(
        "\n[SerializedFile] Metadata OK\n"
        "  Types   : %zu\n"
        "  Objects : %zu\n"
        "  End pos : %zu\n",
        types.size(),
        objects.size(),
        r.pos);

    return true;
}

    bool ReadSerializedType(
        Reader& r,
        bool isRefType,
        SerializedType& type)
    {
        type.classID =
            r.ReadI32();

        if (version >= 16)
            type.stripped =
                r.ReadU8() != 0;

        if (version >= 17)
            type.scriptTypeIndex =
                r.ReadI16();

        if (version >= 13)
        {
            bool hasScriptID = false;

            if (isRefType &&
                type.scriptTypeIndex >= 0)
            {
                hasScriptID = true;
            }

            if (!isRefType &&
                version >= 16 &&
                type.classID == 114)
            {
                hasScriptID = true;
            }

            if (!isRefType &&
                version < 16 &&
                type.classID < 0)
            {
                hasScriptID = true;
            }

            if (hasScriptID)
            {
                type.scriptID.resize(16);

                for (int i = 0; i < 16; ++i)
                    type.scriptID[i] =
                        r.ReadU8();
            }

            type.oldTypeHash.resize(16);

            for (int i = 0; i < 16; ++i)
                type.oldTypeHash[i] =
                    r.ReadU8();
        }

        if (!ReadTypeTreeBlob(
                r,
                type.typeTree))
        {
            return false;
        }

        if (version >= 21)
        {
            if (isRefType)
            {
                r.ReadCString();
                r.ReadCString();
                r.ReadCString();
            }
            else
            {
                int32_t count =
                    r.ReadI32();

                if (count < 0 ||
                    count > 100000)
                {
                    return false;
                }

                type.dependencies.resize(count);

                for (int i = 0;
                     i < count;
                     ++i)
                {
                    type.dependencies[i] =
                        r.ReadI32();
                }
            }
        }

        return true;
    }

    bool ReadTypeTreeBlob(
    Reader& r,
    TypeTreeNode& root)
{
    uint32_t nodeCount =
        r.ReadU32();

    uint32_t stringBufferSize =
        r.ReadU32();

    if (nodeCount == 0 ||
        nodeCount > 1000000)
    {
        return false;
    }

    if (stringBufferSize >
        64 * 1024 * 1024)
    {
        return false;
    }

    struct RawNode
    {
        uint16_t version;
        uint8_t level;
        uint8_t typeFlags;
        uint32_t typeOffset;
        uint32_t nameOffset;
        int32_t byteSize;
        int32_t index;
        int32_t metaFlags;
        uint64_t refTypeHash;
    };

    std::vector<RawNode> raw(
        nodeCount);

    for (uint32_t i = 0;
         i < nodeCount;
         ++i)
    {
        RawNode& n = raw[i];

        n.version =
            r.ReadU16();

        n.level =
            r.ReadU8();

        n.typeFlags =
            r.ReadU8();

        n.typeOffset =
            r.ReadU32();

        n.nameOffset =
            r.ReadU32();

        n.byteSize =
            r.ReadI32();

        n.index =
            r.ReadI32();

        n.metaFlags =
            r.ReadI32();

        if (version >= 19)
        {
            n.refTypeHash =
                r.ReadU64();
        }
        else
        {
            n.refTypeHash = 0;
        }
    }

    std::vector<uint8_t> strings(
        stringBufferSize);

    for (uint32_t i = 0;
         i < stringBufferSize;
         ++i)
    {
        strings[i] =
            r.ReadU8();
    }

    /*
     * Unity TypeTree common string table.
     *
     * If the high bit of the string offset is set:
     *
     *     offset & 0x80000000
     *
     * then the remaining 31 bits are an offset into
     * Unity's global CommonString table.
     *
     * Otherwise the offset points into the local
     * TypeTree string buffer.
     */
    auto ResolveCommonString =
        [&strings](uint32_t offset) -> std::string
    {
        /*
         * CommonString flag.
         */
        if ((offset & 0x80000000u) != 0)
        {
            const uint32_t commonOffset =
                offset & 0x7FFFFFFFu;

            /*
             * Unity common strings used by Texture2D
             * and normal Unity TypeTrees.
             *
             * This table follows Unity's CommonString
             * offsets.
             */
            switch (commonOffset)
            {
                case 0:
                    return "AABB";

                case 5:
                    return "AnimationClip";

                case 19:
                    return "AnimationCurve";

                case 34:
                    return "AnimationState";

                case 49:
                    return "Array";

                case 55:
                    return "Base";

                case 60:
                    return "BitField";

                case 69:
                    return "bitset";

                case 76:
                    return "bool";

                case 81:
                    return "char";

                case 86:
                    return "ColorRGBA";

                case 96:
                    return "Component";

                case 106:
                    return "data";

                case 111:
                    return "deque";

                case 117:
                    return "double";

                case 124:
                    return "dynamic_array";

                case 138:
                    return "FastPropertyName";

                case 155:
                    return "first";

                case 161:
                    return "float";

                case 167:
                    return "Font";

                case 172:
                    return "GameObject";

                case 183:
                    return "Generic Mono";

                case 196:
                    return "GradientNEW";

                case 208:
                    return "GUID";

                case 213:
                    return "GUIStyle";

                case 222:
                    return "int";

                case 226:
                    return "list";

                case 231:
                    return "long long";

                case 241:
                    return "map";

                case 245:
                    return "Matrix4x4f";

                case 256:
                    return "MdFour";

                case 263:
                    return "MonoBehaviour";

                case 277:
                    return "MonoScript";

                case 288:
                    return "m_ByteSize";

                case 299:
                    return "m_Curve";

                case 307:
                    return "m_EditorClassIdentifier";

                case 331:
                    return "m_EditorHideFlags";

                case 349:
                    return "m_Enabled";

                case 359:
                    return "m_ExtensionPtr";

                case 374:
                    return "m_GameObject";

                case 387:
                    return "m_Index";

                case 395:
                    return "m_IsArray";

                case 405:
                    return "m_IsStatic";

                case 416:
                    return "m_MetaFlag";

                case 427:
                    return "m_Name";

                case 434:
                    return "m_ObjectHideFlags";

                case 452:
                    return "m_PrefabInternal";

                case 469:
                    return "m_PrefabParentObject";

                case 490:
                    return "m_Script";

                case 499:
                    return "m_StaticEditorFlags";

                case 519:
                    return "m_Type";

                case 526:
                    return "m_Version";

                case 536:
                    return "Object";

                case 543:
                    return "pair";

                case 548:
                    return "PPtr<Component>";

                case 564:
                    return "PPtr<GameObject>";

                case 581:
                    return "PPtr<Material>";

                case 596:
                    return "PPtr<MonoBehaviour>";

                case 616:
                    return "PPtr<MonoScript>";

                case 633:
                    return "PPtr<Object>";

                case 646:
                    return "PPtr<Prefab>";

                case 659:
                    return "PPtr<Sprite>";

                case 672:
                    return "PPtr<TextAsset>";

                case 688:
                    return "PPtr<Texture>";

                case 702:
                    return "PPtr<Texture2D>";

                case 718:
                    return "PPtr<Transform>";

                case 734:
                    return "Prefab";

                case 741:
                    return "Quaternionf";

                case 753:
                    return "Rectf";

                case 759:
                    return "RectInt";

                case 767:
                    return "RectOffset";

                case 778:
                    return "second";

                case 785:
                    return "set";

                case 789:
                    return "short";

                case 795:
                    return "size";

                case 800:
                    return "SInt16";

                case 807:
                    return "SInt32";

                case 814:
                    return "SInt64";

                case 821:
                    return "SInt8";

                case 827:
                    return "staticvector";

                case 840:
                    return "string";

                case 847:
                    return "TextAsset";

                case 857:
                    return "TextMesh";

                case 866:
                    return "Texture";

                case 874:
                    return "Texture2D";

                case 884:
                    return "Transform";

                case 894:
                    return "TypelessData";

                case 907:
                    return "UInt16";

                case 914:
                    return "UInt32";

                case 921:
                    return "UInt64";

                case 928:
                    return "UInt8";

                case 934:
                    return "unsigned int";

                case 947:
                    return "unsigned long long";

                case 966:
                    return "unsigned short";

                case 981:
                    return "vector";

                case 988:
                    return "Vector2f";

                case 997:
                    return "Vector3f";

                case 1006:
                    return "Vector4f";

                case 1015:
                    return "m_ScriptingClassIdentifier";

                case 1042:
                    return "Gradient";

                case 1051:
                    return "Type*";

                case 1057:
                    return "int2_storage";

                case 1070:
                    return "int3_storage";

                case 1083:
                    return "BoundsInt";

                case 1093:
                    return "m_CorrespondingSourceObject";

                case 1121:
                    return "m_PrefabInstance";

                case 1138:
                    return "m_PrefabAsset";

                case 1152:
                    return "FileSize";

                case 1161:
                    return "Hash128";

                case 1169:
                    return "RenderingLayerMask";

                default:
                {
                    char buffer[64];

                    std::snprintf(
                        buffer,
                        sizeof(buffer),
                        "<common:%u>",
                        commonOffset);

                    return buffer;
                }
            }
        }

        /*
         * Local TypeTree string.
         */
        if (offset >= strings.size())
        {
            char buffer[64];

            std::snprintf(
                buffer,
                sizeof(buffer),
                "<local:%u>",
                offset);

            return buffer;
        }

        const char* str =
            reinterpret_cast<const char*>(
                strings.data() + offset);

        const size_t remaining =
            strings.size() - offset;

        size_t length = 0;

        while (length < remaining &&
               str[length] != '\0')
        {
            ++length;
        }

        if (length == remaining)
        {
            char buffer[64];

            std::snprintf(
                buffer,
                sizeof(buffer),
                "<local:%u>",
                offset);

            return buffer;
        }

        return std::string(
            str,
            length);
    };

    /*
     * Convert raw nodes into TypeTreeNode.
     */
    std::vector<TypeTreeNode> flat(
        nodeCount);

    for (uint32_t i = 0;
         i < nodeCount;
         ++i)
    {
        const RawNode& n =
            raw[i];

        TypeTreeNode& node =
            flat[i];

        node.version =
            n.version;

        node.level =
            n.level;

        node.typeFlags =
            n.typeFlags;

        node.typeStringOffset =
            n.typeOffset;

        node.nameStringOffset =
            n.nameOffset;

        node.byteSize =
            n.byteSize;

        node.index =
            n.index;

        node.metaFlags =
            n.metaFlags;

        node.refTypeHash =
            n.refTypeHash;

        node.type =
            ResolveCommonString(
                n.typeOffset);

        node.name =
            ResolveCommonString(
                n.nameOffset);
    }

    /*
     * Build hierarchical tree from Unity's
     * flat level-based TypeTree.
     */
    root =
        std::move(flat[0]);

    std::vector<TypeTreeNode*> stack;

    stack.push_back(&root);

    for (size_t i = 1;
         i < flat.size();
         ++i)
    {
        while (!stack.empty() &&
               stack.back()->level >=
                   flat[i].level)
        {
            stack.pop_back();
        }

        if (stack.empty())
        {
            continue;
        }

        TypeTreeNode* parent =
            stack.back();

        parent->children.push_back(
            std::move(flat[i]));

        stack.push_back(
            &parent->children.back());
    }

    return true;
}
};


// ============================================================
// TextureTreeReader
// ============================================================

class TextureTreeReader
{
public:

    bool littleEndian = true;

    TextureInfo info;

    bool Parse(
        const SerializedType& serializedType,
        const SerializedObject& object,
        const uint8_t* data,
        size_t size,
        bool little)
    {
        if (!data || size == 0)
            return false;

        /*
            SerializedObject::byteStart adalah offset
            absolut di dalam SerializedFile.
        */
        if (object.byteStart >= size)
        {
            std::printf(
                "[TextureTreeReader] Invalid byteStart: "
                "%llu >= %zu\n",
                (unsigned long long)object.byteStart,
                size);

            return false;
        }

        if ((uint64_t)object.byteSize >
            (uint64_t)size - object.byteStart)
        {
            std::printf(
                "[TextureTreeReader] Invalid byteSize: "
                "offset=%llu size=%u file=%zu\n",
                (unsigned long long)object.byteStart,
                object.byteSize,
                size);

            return false;
        }

        littleEndian = little;

        /*
            Reset hasil sebelumnya.
        */
        info = TextureInfo();

        info.pathID =
            object.pathID;

        info.objectOffset =
            object.byteStart;

        info.objectSize =
            object.byteSize;

        /*
            IMPORTANT:

            Jangan membaca dari:

                data

            karena itu adalah awal SerializedFile.

            Texture2D object berada di:

                data + object.byteStart
        */
        const uint8_t* objectData =
            data + object.byteStart;

        size_t objectSize =
            object.byteSize;

        Reader r(
            objectData,
            objectSize,
            littleEndian);

        /*
            Debug object.
        */
        std::printf(
            "\n[TextureTreeReader] Object\n"
            "  pathID     = %lld\n"
            "  byteStart  = %llu (0x%llX)\n"
            "  byteSize   = %u\n"
            "  endian     = %s\n",
            (long long)object.pathID,
            (unsigned long long)object.byteStart,
            (unsigned long long)object.byteStart,
            object.byteSize,
            littleEndian ? "little" : "big");

        /*
            Dump beberapa byte pertama object.
        */
        size_t dumpSize =
            std::min<size_t>(
                32,
                objectSize);

        std::printf(
            "  First bytes:");

        for (size_t i = 0;
             i < dumpSize;
             ++i)
        {
            if ((i % 16) == 0)
                std::printf("\n    ");

            std::printf(
                "%02X ",
                objectData[i]);
        }

        std::printf("\n");

        /*
            Parse berdasarkan TypeTree Texture2D.
        */
        bool result =
            ReadNode(
                serializedType.typeTree,
                r,
                "");

        std::printf(
            "  Parse result = %s\n"
            "  Consumed     = %zu / %zu bytes\n",
            result ? "OK" : "FAIL",
            r.pos,
            r.size);

        if (!result)
            return false;

        /*
            Validasi field utama.
        */
        std::printf(
            "  TextureInfo:\n"
            "    name            = %s\n"
            "    width           = %d\n"
            "    height          = %d\n"
            "    textureFormat   = %d (%s)\n"
            "    mipCount        = %d\n"
            "    completeSize    = %u\n"
            "    imageOffset     = %llu\n"
            "    imageSize       = %d\n"
            "    streamOffset    = %llu\n"
            "    streamSize      = %u\n"
            "    streamPath      = %s\n",
            info.name.c_str(),
            info.width,
            info.height,
            info.textureFormat,
            TextureFormatName(
                info.textureFormat),
            info.mipCount,
            info.completeImageSize,
            (unsigned long long)
                info.imageDataOffset,
            info.imageDataSize,
            (unsigned long long)
                info.streamOffset,
            info.streamSize,
            info.streamPath.c_str());

        return true;
    }

private:

    static bool IsPrimitive(
        const std::string& type)
    {
        return
            type == "bool" ||
            type == "char" ||
            type == "SInt8" ||
            type == "UInt8" ||
            type == "unsigned char" ||
            type == "short" ||
            type == "SInt16" ||
            type == "UInt16" ||
            type == "unsigned short" ||
            type == "int" ||
            type == "SInt32" ||
            type == "UInt32" ||
            type == "unsigned int" ||
            type == "long long" ||
            type == "SInt64" ||
            type == "UInt64" ||
            type == "unsigned long long" ||
            type == "float" ||
            type == "double";
    }

    static bool PathEnds(
        const std::string& path,
        const char* suffix)
    {
        std::string s(suffix);

        if (path.size() < s.size())
            return false;

        return path.compare(
            path.size() - s.size(),
            s.size(),
            s) == 0;
    }

    void CaptureInt(
        const TypeTreeNode& node,
        const std::string& path,
        int64_t value)
    {
        if (node.name == "m_Width")
        {
            info.width =
                (int32_t)value;
        }
        else if (node.name == "m_Height")
        {
            info.height =
                (int32_t)value;
        }
        else if (
            node.name == "m_CompleteImageSize")
        {
            if (value >= 0)
            {
                info.completeImageSize =
                    (uint32_t)value;
            }
        }
        else if (
            node.name == "m_TextureFormat")
        {
            info.textureFormat =
                (int32_t)value;
        }
        else if (
            node.name == "m_MipCount")
        {
            info.mipCount =
                (int32_t)value;
        }
        else if (
            node.name == "offset" &&
            path.find(
                "m_StreamData/offset") !=
                std::string::npos)
        {
            if (value >= 0)
            {
                info.streamOffset =
                    (uint64_t)value;
            }
        }
        else if (
            node.name == "size" &&
            path.find(
                "m_StreamData/size") !=
                std::string::npos)
        {
            if (value >= 0)
            {
                info.streamSize =
                    (uint32_t)value;
            }
        }
    }

    void CaptureString(
        const TypeTreeNode& node,
        const std::string& path,
        const std::string& value)
    {
        if (node.name == "m_Name")
        {
            info.name =
                value;
        }
        else if (
            node.name == "path" &&
            path.find(
                "m_StreamData/path") !=
                std::string::npos)
        {
            info.streamPath =
                value;
        }
    }

    bool ReadPrimitive(
        const TypeTreeNode& node,
        Reader& r,
        const std::string& path)
    {
        const std::string& type =
            node.type;

        /*
            bool
        */
        if (type == "bool")
        {
            bool value =
                r.ReadU8() != 0;

            if (node.name ==
                "m_IsReadable")
            {
                info.isReadable =
                    value;
            }

            if (node.AlignAfter())
                r.Align4();

            return true;
        }

        /*
            8-bit
        */
        if (type == "char" ||
            type == "UInt8" ||
            type == "unsigned char" ||
            type == "SInt8")
        {
            int64_t value =
                type == "SInt8"
                    ? (int64_t)r.ReadI8()
                    : (int64_t)r.ReadU8();

            CaptureInt(
                node,
                path,
                value);

            if (node.AlignAfter())
                r.Align4();

            return true;
        }

        /*
            16-bit
        */
        if (type == "short" ||
            type == "SInt16" ||
            type == "UInt16" ||
            type == "unsigned short")
        {
            int64_t value =
                type == "SInt16" ||
                type == "short"
                    ? (int64_t)r.ReadI16()
                    : (int64_t)r.ReadU16();

            CaptureInt(
                node,
                path,
                value);

            if (node.AlignAfter())
                r.Align4();

            return true;
        }

        /*
            32-bit
        */
        if (type == "int" ||
            type == "SInt32" ||
            type == "UInt32" ||
            type == "unsigned int")
        {
            int64_t value =
                type == "int" ||
                type == "SInt32"
                    ? (int64_t)r.ReadI32()
                    : (int64_t)r.ReadU32();

            CaptureInt(
                node,
                path,
                value);

            if (node.AlignAfter())
                r.Align4();

            return true;
        }

        /*
            64-bit
        */
        if (type == "long long" ||
            type == "SInt64" ||
            type == "UInt64" ||
            type == "unsigned long long")
        {
            int64_t value;

            if (type == "long long" ||
                type == "SInt64")
            {
                value =
                    r.ReadI64();
            }
            else
            {
                value =
                    (int64_t)r.ReadU64();
            }

            CaptureInt(
                node,
                path,
                value);

            if (node.AlignAfter())
                r.Align4();

            return true;
        }

        /*
            float
        */
        if (type == "float")
        {
            r.ReadFloat();

            if (node.AlignAfter())
                r.Align4();

            return true;
        }

        /*
            double
        */
        if (type == "double")
        {
            r.ReadU64();

            if (node.AlignAfter())
                r.Align4();

            return true;
        }

        return false;
    }

    bool ReadString(
        const TypeTreeNode& node,
        Reader& r,
        const std::string& path)
    {
        /*
            Unity string:

                uint32 length
                char[length]
                Align4
        */

        if (r.Remaining() < 4)
            return false;

        uint32_t length =
            r.ReadU32();

        if ((uint64_t)length >
            (uint64_t)r.Remaining())
        {
            return false;
        }

        std::string value;

        if (length != 0)
        {
            value.assign(
                reinterpret_cast<
                    const char*>(
                        r.data + r.pos),
                length);

            if (!r.Skip(length))
                return false;
        }

        r.Align4();

        CaptureString(
            node,
            path,
            value);

        return true;
    }

    bool ReadArray(
        const TypeTreeNode& node,
        Reader& r,
        const std::string& path)
    {
        /*
            Unity array TypeTree:

                Array
                  size
                  data
        */

        if (node.children.size() < 2)
            return false;

        if (r.Remaining() < 4)
            return false;

        int32_t count =
            r.ReadI32();

        if (count < 0 ||
            count > 100000000)
        {
            return false;
        }

        const TypeTreeNode& element =
            node.children[1];

        /*
            image data is a byte array.

            Instead of parsing each byte through
            ReadNode(), capture the entire block.
        */
        if (path.find("image data") !=
            std::string::npos)
        {
            if (element.type == "UInt8" ||
                element.type == "char" ||
                element.type == "unsigned char")
            {
                size_t bytes =
                    (size_t)count;

                if (bytes >
                    r.Remaining())
                {
                    return false;
                }

                /*
                    IMPORTANT:

                    This offset is relative to
                    the Texture2D object.

                    GetTextureDataPtr() later adds:

                        objectOffset +
                        imageDataOffset
                */
                info.imageDataOffset =
                    (uint64_t)r.pos;

                info.imageDataSize =
                    (int32_t)bytes;

                if (!r.Skip(bytes))
                    return false;

                if (node.AlignAfter())
                    r.Align4();

                return true;
            }
        }

        /*
            Normal array.
        */
        for (int32_t i = 0;
             i < count;
             ++i)
        {
            if (!ReadNode(
                    element,
                    r,
                    path + "/[]"))
            {
                return false;
            }
        }

        if (node.AlignAfter())
            r.Align4();

        return true;
    }

    bool ReadNode(
        const TypeTreeNode& node,
        Reader& r,
        const std::string& parentPath)
    {
        std::string path =
            parentPath;

        if (!node.name.empty())
        {
            if (!path.empty())
                path += "/";

            path += node.name;
        }

        /*
            Array
        */
        if (node.IsArray())
        {
            return ReadArray(
                node,
                r,
                path);
        }

        /*
            string
        */
        if (node.type == "string")
        {
            return ReadString(
                node,
                r,
                path);
        }

        /*
            Primitive
        */
        if (IsPrimitive(node.type))
        {
            return ReadPrimitive(
                node,
                r,
                path);
        }

        /*
            Struct / class / object.

            Recursively read children.
        */
        for (const auto& child :
             node.children)
        {
            if (!ReadNode(
                    child,
                    r,
                    path))
            {
                return false;
            }
        }

        if (node.AlignAfter())
            r.Align4();

        return true;
    }
};


// ============================================================
// Scanner Implementation
// ============================================================

struct UnityTextureScanner::Impl
{
    UnityFSBundle bundle;

    std::vector<uint8_t> serializedData;

    std::vector<TextureInfo> textures;

    std::string inputPath;
};


UnityTextureScanner::UnityTextureScanner()
{
    m_Impl =
        new Impl();
}

UnityTextureScanner::~UnityTextureScanner()
{
    delete m_Impl;
    m_Impl = nullptr;
}

void UnityTextureScanner::Clear()
{
    m_Impl->bundle =
        UnityFSBundle();

    m_Impl->serializedData.clear();
    m_Impl->textures.clear();
    m_Impl->inputPath.clear();
}

const std::vector<TextureInfo>&
UnityTextureScanner::GetTextures() const
{
    return m_Impl->textures;
}

const TextureInfo*
UnityTextureScanner::GetTexture(
    int index) const
{
    if (index < 0 ||
        index >=
            (int)m_Impl->textures.size())
    {
        return nullptr;
    }

    return &m_Impl->textures[index];
}

const std::string&
UnityTextureScanner::GetInputPath() const
{
    return m_Impl->inputPath;
}

static void DumpTypeTree(
    const TypeTreeNode& node,
    int depth = 0)
{
    for (int i = 0; i < depth; ++i)
        std::printf("  ");

    std::printf(
        "- type=\"%s\" "
        "name=\"%s\" "
        "level=%u "
        "flags=0x%02X "
        "byteSize=%d "
        "index=%d "
        "meta=0x%08X "
        "children=%zu\n",
        node.type.c_str(),
        node.name.c_str(),
        node.level,
        node.typeFlags,
        node.byteSize,
        node.index,
        node.metaFlags,
        node.children.size());

    for (const auto& child :
         node.children)
    {
        DumpTypeTree(child, depth + 1);
    }
}

bool UnityTextureScanner::Scan(
    const std::string& input)
{
    Clear();

    m_Impl->inputPath = input;

    std::printf(
        "\n========================================\n"
        "[UnityTextureScanner] SCAN\n"
        "========================================\n");

    std::printf(
        "[UnityTextureScanner] Input: %s\n",
        input.c_str());

    // ------------------------------------------------------------
    // Load UnityFS file
    // ------------------------------------------------------------

    if (!LoadFile(
            input,
            m_Impl->bundle.fileData))
    {
        std::printf(
            "[UnityTextureScanner] "
            "Cannot open: %s\n",
            input.c_str());

        return false;
    }

    std::printf(
        "[UnityTextureScanner] File loaded: %zu bytes\n",
        m_Impl->bundle.fileData.size());

    // ------------------------------------------------------------
    // Parse UnityFS header
    // ------------------------------------------------------------

    if (!m_Impl->bundle.ParseHeader())
    {
        std::printf(
            "[UnityTextureScanner] "
            "Invalid UnityFS.\n");

        return false;
    }

    std::printf(
        "[UnityTextureScanner] UnityFS header OK\n");

    std::printf(
        "  Signature      : %s\n",
        m_Impl->bundle.header.signature.c_str());

    std::printf(
        "  Format version : %u\n",
        m_Impl->bundle.header.formatVersion);

    std::printf(
        "  Player version : %s\n",
        m_Impl->bundle.header.playerVersion.c_str());

    std::printf(
        "  Engine version : %s\n",
        m_Impl->bundle.header.engineVersion.c_str());

    std::printf(
        "  Bundle size    : %llu\n",
        (unsigned long long)
            m_Impl->bundle.header.bundleSize);

    std::printf(
        "  BlocksInfo C   : %u\n",
        m_Impl->bundle.header.compressedBlocksInfoSize);

    std::printf(
        "  BlocksInfo U   : %u\n",
        m_Impl->bundle.header.uncompressedBlocksInfoSize);

    std::printf(
        "  Flags          : 0x%08X\n",
        m_Impl->bundle.header.flags);

    // ------------------------------------------------------------
    // Parse BlocksInfo
    // ------------------------------------------------------------

    if (!m_Impl->bundle.ParseBlocksInfo())
    {
        std::printf(
            "[UnityTextureScanner] "
            "BlocksInfo failed.\n");

        return false;
    }

    std::printf(
        "[UnityTextureScanner] BlocksInfo OK\n");

    std::printf(
        "  Blocks    : %zu\n",
        m_Impl->bundle.blocks.size());

    std::printf(
        "  Directory : %zu\n",
        m_Impl->bundle.directory.size());

    // ------------------------------------------------------------
    // Print directory
    // ------------------------------------------------------------

    std::printf(
        "\n----------------------------------------\n"
        "UnityFS Directory\n"
        "----------------------------------------\n");

    for (size_t i = 0;
         i < m_Impl->bundle.directory.size();
         ++i)
    {
        const UnityFSDirectoryEntry& entry =
            m_Impl->bundle.directory[i];

        std::printf(
            "[%zu] offset=%llu size=%llu flags=0x%08X\n"
            "    %s\n",
            i,
            (unsigned long long)entry.offset,
            (unsigned long long)entry.size,
            entry.flags,
            entry.path.c_str());
    }

    // ------------------------------------------------------------
    // Decompress content
    // ------------------------------------------------------------

    if (!m_Impl->bundle.DecompressContent())
    {
        std::printf(
            "[UnityTextureScanner] "
            "Content decompression failed.\n");

        return false;
    }

    std::printf(
        "[UnityTextureScanner] Content decompressed: %zu bytes\n",
        m_Impl->bundle.contentData.size());

    // ------------------------------------------------------------
    // Find SerializedFile
    // ------------------------------------------------------------

    const UnityFSDirectoryEntry*
        serializedEntry = nullptr;

    std::printf(
        "\n----------------------------------------\n"
        "SerializedFile Candidates\n"
        "----------------------------------------\n");

    for (const auto& entry :
         m_Impl->bundle.directory)
    {
        if (entry.size == 0)
            continue;

        const std::string base =
            BaseName(entry.path);

        if (EndsWith(
                base,
                ".resS"))
        {
            continue;
        }

        std::printf(
            "[Candidate]\n"
            "  Path   : %s\n"
            "  Offset : %llu\n"
            "  Size   : %llu\n"
            "  Flags  : 0x%08X\n",
            entry.path.c_str(),
            (unsigned long long)entry.offset,
            (unsigned long long)entry.size,
            entry.flags);

        /*
            Untuk sementara pilih candidate pertama.

            Jika ternyata Parse() gagal, output di atas akan
            menunjukkan semua candidate yang tersedia.
        */
        if (!serializedEntry)
        {
            serializedEntry = &entry;
        }
    }

    if (!serializedEntry)
    {
        std::printf(
            "[UnityTextureScanner] "
            "SerializedFile not found.\n");

        return false;
    }

    std::printf(
        "\n[UnityTextureScanner] "
        "Selected SerializedFile:\n");

    std::printf(
        "  Path   : %s\n",
        serializedEntry->path.c_str());

    std::printf(
        "  Offset : %llu\n",
        (unsigned long long)
            serializedEntry->offset);

    std::printf(
        "  Size   : %llu\n",
        (unsigned long long)
            serializedEntry->size);

    // ------------------------------------------------------------
    // Get SerializedFile data
    // ------------------------------------------------------------

    const uint8_t* serializedPtr =
        nullptr;

    size_t serializedSize = 0;

    if (!m_Impl->bundle.GetEntryData(
            *serializedEntry,
            serializedPtr,
            serializedSize))
    {
        std::printf(
            "[UnityTextureScanner] "
            "GetEntryData failed.\n");

        return false;
    }

    if (!serializedPtr ||
        serializedSize == 0)
    {
        std::printf(
            "[UnityTextureScanner] "
            "SerializedFile data empty.\n");

        return false;
    }

    // ------------------------------------------------------------
    // Copy SerializedFile
    // ------------------------------------------------------------

    m_Impl->serializedData.assign(
        serializedPtr,
        serializedPtr + serializedSize);

    std::printf(
        "\n----------------------------------------\n"
        "SerializedFile DEBUG\n"
        "----------------------------------------\n");

    std::printf(
        "Serialized ptr  : %p\n",
        (const void*)serializedPtr);

    std::printf(
        "Serialized size : %zu bytes\n",
        serializedSize);

    // ------------------------------------------------------------
    // Dump first 48 bytes
    // ------------------------------------------------------------

    if (serializedSize >= 48)
    {
        std::printf(
            "First 48 bytes:\n");

        for (size_t i = 0;
             i < 48;
             ++i)
        {
            std::printf(
                "%02X ",
                m_Impl->serializedData[i]);

            if ((i + 1) % 16 == 0)
                std::printf("\n");
        }
    }
    else
    {
        std::printf(
            "SerializedFile smaller than 48 bytes.\n");

        std::printf(
            "Bytes:\n");

        for (size_t i = 0;
             i < serializedSize;
             ++i)
        {
            std::printf(
                "%02X ",
                m_Impl->serializedData[i]);

            if ((i + 1) % 16 == 0)
                std::printf("\n");
        }

        std::printf("\n");

        return false;
    }

    // ------------------------------------------------------------
    // SerializedFile parser
    // ------------------------------------------------------------

    SerializedFileParser parser;

    std::printf(
        "\n[UnityTextureScanner] "
        "Parsing SerializedFile...\n");

    if (!parser.Parse(
            m_Impl->serializedData.data(),
            m_Impl->serializedData.size()))
    {
        std::printf(
            "[UnityTextureScanner] "
            "SerializedFile parse failed.\n");

        return false;
    }

    std::printf(
        "[UnityTextureScanner] "
        "SerializedFile parse OK.\n");

    std::printf(
        "  Version       : %u\n",
        parser.version);

    std::printf(
        "  Metadata size : %u\n",
        parser.metadataSize);

    std::printf(
        "  File size     : %llu\n",
        (unsigned long long)
            parser.fileSize);

    std::printf(
        "  Data offset   : %llu\n",
        (unsigned long long)
            parser.dataOffset);

    std::printf(
        "  Little endian : %s\n",
        parser.littleEndian
            ? "true"
            : "false");

    std::printf(
        "  Types         : %zu\n",
        parser.types.size());

    std::printf(
        "  Objects       : %zu\n",
        parser.objects.size());

    // ------------------------------------------------------------
    // Scan Texture2D
    // ------------------------------------------------------------

    m_Impl->textures.clear();

    int textureIndex = 0;

    for (const auto& object :
         parser.objects)
    {
        /*
            Unity class ID:
                Texture2D = 28
        */
        if (object.classID != 28)
            continue;

        if (object.typeID < 0 ||
            object.typeID >=
                (int)parser.types.size())
        {
            std::printf(
                "[UnityTextureScanner] "
                "Texture2D invalid typeID=%d\n",
                object.typeID);

            continue;
        }

        const SerializedType&
            type =
                parser.types[object.typeID];

        if (type.classID != 28)
            continue;

        // --------------------------------------------------------
        // Validate object range
        // --------------------------------------------------------

        if (object.byteStart >
            m_Impl->serializedData.size())
        {
            std::printf(
                "[UnityTextureScanner] "
                "Texture2D object offset invalid: "
                "%llu\n",
                (unsigned long long)
                    object.byteStart);

            continue;
        }

        const size_t objectStart =
            (size_t)object.byteStart;

        const size_t objectSize =
            (size_t)object.byteSize;

        if (objectSize >
            m_Impl->serializedData.size() -
            objectStart)
        {
            std::printf(
                "[UnityTextureScanner] "
                "Texture2D object size invalid: "
                "offset=%zu size=%zu\n",
                objectStart,
                objectSize);

            continue;
        }

        // --------------------------------------------------------
        // Read Texture2D TypeTree
        // --------------------------------------------------------
        
        std::printf(
    "\n========================================\n"
    "[Texture2D TypeTree]\n"
    "========================================\n");

DumpTypeTree(type.typeTree);

std::printf(
    "========================================\n\n");
        
        TextureTreeReader reader;

        if (!reader.Parse(
        type,
        object,
        m_Impl->serializedData.data(),
        m_Impl->serializedData.size(),
        parser.littleEndian))
        {
            std::printf(
                "[UnityTextureScanner] "
                "TextureTreeReader failed "
                "object=%llu\n",
                (unsigned long long)
                    object.pathID);

            continue;
        }

        TextureInfo info =
            reader.info;

        info.index =
            textureIndex++;

        // --------------------------------------------------------
        // StreamData
        // --------------------------------------------------------

        if (!info.streamPath.empty() &&
            info.streamSize > 0)
        {
            info.isStreamed = true;

            const UnityFSDirectoryEntry*
                resource =
                    m_Impl->bundle
                        .FindResourceForPath(
                            info.streamPath);

            if (resource)
            {
                const uint64_t absolute =
                    resource->offset +
                    info.streamOffset;

                if (absolute <=
                    m_Impl->bundle.contentData.size())
                {
                    if (info.streamSize <=
                        m_Impl->bundle.contentData.size() -
                        (size_t)absolute)
                    {
                        info.resolvedResourceOffset =
                            absolute;

                        info.resourceResolved =
                            true;
                    }
                }
            }

            if (!info.resourceResolved)
            {
                std::printf(
                    "[UnityTextureScanner] "
                    "Stream resource unresolved:\n"
                    "  Texture : %s\n"
                    "  Path    : %s\n"
                    "  Offset  : %llu\n"
                    "  Size    : %u\n",
                    info.name.c_str(),
                    info.streamPath.c_str(),
                    (unsigned long long)
                        info.streamOffset,
                    info.streamSize);
            }
        }
        else
        {
            info.isStreamed = false;
        }

        // --------------------------------------------------------
        // Add texture
        // --------------------------------------------------------

        m_Impl->textures.push_back(
            std::move(info));
    }

    // ------------------------------------------------------------
    // Print texture results
    // ------------------------------------------------------------

    std::printf(
        "\n========================================\n"
        "[UnityTextureScanner] RESULT\n"
        "========================================\n");

    std::printf(
        "Texture2D count = %zu\n",
        m_Impl->textures.size());

    for (size_t i = 0;
         i < m_Impl->textures.size();
         ++i)
    {
        const TextureInfo&
            tex =
                m_Impl->textures[i];

        std::printf(
            "\n[%zu]\n"
            "  Name          : %s\n"
            "  Path ID       : %lld\n"
            "  Object offset : %llu\n"
            "  Object size   : %u\n"
            "  Width         : %d\n"
            "  Height        : %d\n"
            "  Format        : %d\n"
            "  Mip count     : %d\n"
            "  Image size    : %d\n"
            "  Image offset  : %llu\n"
            "  Streamed      : %s\n"
            "  Stream path   : %s\n"
            "  Stream offset : %llu\n"
            "  Stream size   : %u\n"
            "  Resource OK   : %s\n",
            i,
            tex.name.c_str(),
            (long long)tex.pathID,
            (unsigned long long)
                tex.objectOffset,
            tex.objectSize,
            tex.width,
            tex.height,
            tex.textureFormat,
            tex.mipCount,
            tex.imageDataSize,
            (unsigned long long)
                tex.imageDataOffset,
            tex.isStreamed
                ? "true"
                : "false",
            tex.streamPath.c_str(),
            (unsigned long long)
                tex.streamOffset,
            tex.streamSize,
            tex.resourceResolved
                ? "true"
                : "false");
    }

    std::printf(
        "\n========================================\n"
        "[UnityTextureScanner] SCAN DONE\n"
        "========================================\n");

    return true;
}

bool UnityTextureScanner::ScanSilent(
    const std::string& input)
{
    /*
        Parser internal saat ini hanya menggunakan
        printf pada error dan hasil akhir.
        Untuk menjaga source tetap sederhana,
        ScanSilent memakai parser yang sama.
    */
    return Scan(input);
}

bool UnityTextureScanner::GetTextureDataPtr(
    int index,
    const uint8_t*& data,
    size_t& size) const
{
    data = nullptr;
    size = 0;

    const TextureInfo* texture =
        GetTexture(index);

    if (!texture)
        return false;

    /*
        Streamed texture.
    */
    if (texture->isStreamed)
    {
        if (!texture->resourceResolved)
            return false;

        uint64_t offset =
            texture->resolvedResourceOffset;

        uint64_t bytes =
            texture->streamSize;

        if (offset >
            m_Impl->bundle.contentData.size())
        {
            return false;
        }

        if (bytes >
            m_Impl->bundle.contentData.size() -
            (size_t)offset)
        {
            return false;
        }

        data =
            m_Impl->bundle.contentData.data() +
            (size_t)offset;

        size =
            (size_t)bytes;

        return true;
    }

    /*
        Embedded texture.

        objectOffset = absolute offset
        inside SerializedFile.

        imageDataOffset = offset inside
        Texture2D object.
    */
    uint64_t absolute =
        texture->objectOffset +
        texture->imageDataOffset;

    uint64_t bytes =
        texture->imageDataSize;

    if (bytes == 0)
        return false;

    if (absolute >
        m_Impl->serializedData.size())
    {
        return false;
    }

    if (bytes >
        m_Impl->serializedData.size() -
        (size_t)absolute)
    {
        return false;
    }

    data =
        m_Impl->serializedData.data() +
        (size_t)absolute;

    size =
        (size_t)bytes;

    return true;
}

bool UnityTextureScanner::GetTextureData(
    int index,
    std::vector<uint8_t>& output) const
{
    output.clear();

    const uint8_t* data =
        nullptr;

    size_t size = 0;

    if (!GetTextureDataPtr(
            index,
            data,
            size))
    {
        return false;
    }

    output.assign(
        data,
        data + size);

    return true;
}
