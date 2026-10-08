#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <algorithm>

#include <lz4.h>
#include <lzma.h>
#include <zlib.h>

/*
    ============================================================
    MLBB UNITY TEXTURE SCANNER
    ============================================================

    Target:
        UnityFS AssetBundle
        Unity 2019.x
        SerializedFile >= 19
        Texture2D classID = 28

    Current purpose:
        UnityFS
          -> SerializedFile
          -> TypeTree
          -> ObjectTable
          -> Texture2D
          -> m_StreamData

    This version DOES NOT decode ASTC/ETC yet.
    It only locates Texture2D metadata and resource bytes.

    Build:

    clang++ -std=c++17 -O2 unity_texture_scan.cpp \
        -o unity_texture_scan \
        -llz4 -llzma -lz
*/


// ============================================================
// Helpers
// ============================================================

static uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static uint64_t Align4(uint64_t value)
{
    return AlignUp(value, 4);
}

static uint64_t Align8(uint64_t value)
{
    return AlignUp(value, 8);
}

static uint64_t Align16(uint64_t value)
{
    return AlignUp(value, 16);
}

static std::string BaseName(const std::string& path)
{
    size_t p1 = path.find_last_of('/');
    size_t p2 = path.find_last_of('\\');

    size_t p = std::string::npos;

    if (p1 != std::string::npos && p2 != std::string::npos)
        p = std::max(p1, p2);
    else if (p1 != std::string::npos)
        p = p1;
    else
        p = p2;

    if (p == std::string::npos)
        return path;

    return path.substr(p + 1);
}

static bool EndsWith(
    const std::string& value,
    const std::string& suffix)
{
    if (suffix.size() > value.size())
        return false;

    return value.compare(
        value.size() - suffix.size(),
        suffix.size(),
        suffix) == 0;
}


// ============================================================
// Binary Reader
// ============================================================

class Reader
{
public:
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;

    bool littleEndian = false;

    Reader() = default;

    Reader(
        const uint8_t* p,
        size_t s,
        bool little = false)
        : data(p),
          size(s),
          pos(0),
          littleEndian(little)
    {
    }

    bool CanRead(size_t n) const
    {
        return pos <= size && n <= size - pos;
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

    bool Align(size_t alignment)
    {
        size_t p = (size_t)AlignUp(pos, alignment);

        if (p > size)
            return false;

        pos = p;
        return true;
    }

    uint8_t ReadU8()
    {
        if (!CanRead(1))
            return 0;

        return data[pos++];
    }

    int8_t ReadI8()
    {
        return (int8_t)ReadU8();
    }

    uint16_t ReadU16()
    {
        if (!CanRead(2))
            return 0;

        uint16_t v;

        if (littleEndian)
        {
            v =
                ((uint16_t)data[pos]) |
                ((uint16_t)data[pos + 1] << 8);
        }
        else
        {
            v =
                ((uint16_t)data[pos] << 8) |
                ((uint16_t)data[pos + 1]);
        }

        pos += 2;
        return v;
    }

    int16_t ReadI16()
    {
        return (int16_t)ReadU16();
    }

    uint32_t ReadU32()
    {
        if (!CanRead(4))
            return 0;

        uint32_t v;

        if (littleEndian)
        {
            v =
                ((uint32_t)data[pos]) |
                ((uint32_t)data[pos + 1] << 8) |
                ((uint32_t)data[pos + 2] << 16) |
                ((uint32_t)data[pos + 3] << 24);
        }
        else
        {
            v =
                ((uint32_t)data[pos] << 24) |
                ((uint32_t)data[pos + 1] << 16) |
                ((uint32_t)data[pos + 2] << 8) |
                ((uint32_t)data[pos + 3]);
        }

        pos += 4;
        return v;
    }

    int32_t ReadI32()
    {
        return (int32_t)ReadU32();
    }

    uint64_t ReadU64()
    {
        if (!CanRead(8))
            return 0;

        uint64_t v = 0;

        if (littleEndian)
        {
            for (int i = 0; i < 8; ++i)
                v |= ((uint64_t)data[pos + i]) << (i * 8);
        }
        else
        {
            for (int i = 0; i < 8; ++i)
                v |= ((uint64_t)data[pos + i]) << ((7 - i) * 8);
        }

        pos += 8;
        return v;
    }

    int64_t ReadI64()
    {
        return (int64_t)ReadU64();
    }

    float ReadFloat()
    {
        uint32_t raw = ReadU32();

        float f = 0.0f;
        std::memcpy(&f, &raw, sizeof(f));

        return f;
    }

    std::string ReadCString()
    {
        std::string result;

        while (CanRead(1))
        {
            char c = (char)data[pos++];

            if (c == '\0')
                break;

            result.push_back(c);
        }

        return result;
    }

    std::string ReadAlignedString()
    {
        uint32_t length = ReadU32();

        if (length > Remaining())
        {
            pos = size;
            return {};
        }

        std::string result;

        if (length)
        {
            result.assign(
                (const char*)(data + pos),
                (size_t)length);

            pos += length;
        }

        Align4();

        return result;
    }

    void Align4()
    {
        Align(4);
    }

    void Align8()
    {
        Align(8);
    }
};


// ============================================================
// Common TypeTree Strings
// ============================================================
//
// Unity stores many TypeTree strings using:
//     value | 0x80000000
//
// These are the common strings used by Unity/AssetStudio.
// Local strings use offsets without the high bit.
//

static const std::map<uint32_t, std::string> kCommonStrings =
{
    {0,    "AABB"},
    {5,    "AnimationClip"},
    {19,   "AnimationCurve"},
    {34,   "AnimationState"},
    {49,   "Array"},
    {55,   "Base"},
    {60,   "BitField"},
    {69,   "bitset"},
    {76,   "bool"},
    {81,   "char"},
    {86,   "ColorRGBA"},
    {96,   "Component"},
    {106,  "data"},
    {111,  "deque"},
    {117,  "double"},
    {124,  "dynamic_array"},
    {138,  "FastPropertyName"},
    {155,  "first"},
    {161,  "float"},
    {167,  "Font"},
    {172,  "GameObject"},
    {183,  "Generic Mono"},
    {196,  "GradientNEW"},
    {208,  "GUID"},
    {213,  "GUIStyle"},
    {222,  "int"},
    {226,  "list"},
    {231,  "long long"},
    {241,  "map"},
    {245,  "Matrix4x4f"},
    {256,  "MdFour"},
    {263,  "MonoBehaviour"},
    {277,  "MonoScript"},
    {288,  "m_ByteSize"},
    {299,  "m_Curve"},
    {307,  "m_EditorClassIdentifier"},
    {331,  "m_EditorHideFlags"},
    {349,  "m_Enabled"},
    {359,  "m_ExtensionPtr"},
    {374,  "m_GameObject"},
    {387,  "m_Index"},
    {395,  "m_IsArray"},
    {405,  "m_IsStatic"},
    {416,  "m_MetaFlag"},
    {427,  "m_Name"},
    {434,  "m_ObjectHideFlags"},
    {452,  "m_PrefabInternal"},
    {469,  "m_PrefabParentObject"},
    {490,  "m_Script"},
    {499,  "m_StaticEditorFlags"},
    {519,  "m_Type"},
    {526,  "m_Version"},
    {536,  "Object"},
    {543,  "pair"},
    {548,  "PPtr<Component>"},
    {564,  "PPtr<GameObject>"},
    {581,  "PPtr<Material>"},
    {596,  "PPtr<MonoBehaviour>"},
    {616,  "PPtr<MonoScript>"},
    {633,  "PPtr<Object>"},
    {646,  "PPtr<Prefab>"},
    {659,  "PPtr<Sprite>"},
    {672,  "PPtr<TextAsset>"},
    {688,  "PPtr<Texture>"},
    {702,  "PPtr<Texture2D>"},
    {718,  "PPtr<Transform>"},
    {734,  "Prefab"},
    {741,  "Quaternionf"},
    {753,  "Rectf"},
    {759,  "RectInt"},
    {767,  "RectOffset"},
    {778,  "second"},
    {785,  "set"},
    {789,  "short"},
    {795,  "size"},
    {800,  "SInt16"},
    {807,  "SInt32"},
    {814,  "SInt64"},
    {821,  "SInt8"},
    {827,  "staticvector"},
    {840,  "string"},
    {847,  "TextAsset"},
    {857,  "TextMesh"},
    {866,  "Texture"},
    {874,  "Texture2D"},
    {884,  "Transform"},
    {894,  "TypelessData"},
    {907,  "UInt16"},
    {914,  "UInt32"},
    {921, "UInt64"},
    {928, "UInt8"},
    {934, "unsigned int"},
    {947, "unsigned long long"},
    {966, "unsigned short"},
    {981, "vector"},
    {988, "Vector2f"},
    {997, "Vector3f"},
    {1006, "Vector4f"},
    {1015, "m_ScriptingClassIdentifier"},
    {1042, "Gradient"},
    {1051, "Type*"},
    {1057, "int2_storage"},
    {1070, "int3_storage"},
    {1083, "BoundsInt"},
    {1093, "m_CorrespondingSourceObject"},
    {1121, "m_PrefabInstance"},
    {1138, "m_PrefabAsset"},
    {1152, "FileSize"},
    {1161, "Hash128"}
};

static std::string ResolveCommonString(
    const std::vector<uint8_t>& stringBuffer,
    uint32_t value)
{
    // High bit means CommonString.
    if (value & 0x80000000U)
    {
        uint32_t offset =
            value & 0x7FFFFFFFU;

        auto it = kCommonStrings.find(offset);

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
        char c = (char)stringBuffer[p++];

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
// Serialized Type
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


// ============================================================
// Serialized Object
// ============================================================

struct SerializedObject
{
    int64_t pathID = 0;

    uint64_t byteStart = 0;
    uint32_t byteSize = 0;

    int32_t typeID = -1;
    int32_t classID = -1;
};


// ============================================================
// Texture Information
// ============================================================

struct TextureInfo
{
    int index = -1;
    int64_t pathID = 0;

    uint64_t objectOffset = 0;
    uint32_t objectSize = 0;

    std::string name;

    int32_t width = 0;
    int32_t height = 0;

    uint32_t completeImageSize = 0;

    int32_t textureFormat = -1;
    int32_t mipCount = 0;

    int32_t imageDataSize = 0;

    // Offset image data di dalam Texture2D object.
    uint64_t imageDataOffset = 0;

    bool isReadable = false;
    bool isStreamed = false;

    uint64_t streamOffset = 0;
    uint32_t streamSize = 0;

    std::string streamPath;

    uint64_t resolvedResourceOffset = 0;
    bool resourceResolved = false;
};


// ============================================================
// Unity TextureFormat
// ============================================================

static const char* TextureFormatName(int format)
{
    switch (format)
    {
        case 0:  return "None";
        case 1:  return "Alpha8";
        case 2:  return "ARGB4444";
        case 3:  return "RGB24";
        case 4:  return "RGBA32";
        case 5:  return "ARGB32";
        case 6:  return "ARGBFloat";
        case 7:  return "RGB565";
        case 8:  return "BGR24";
        case 9:  return "R16";
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
// UnityFS Structures
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
// File Loading
// ============================================================

static bool LoadFile(
    const std::string& path,
    std::vector<uint8_t>& output)
{
    FILE* fp = std::fopen(path.c_str(), "rb");

    if (!fp)
    {
        std::printf(
            "[ERROR] Cannot open file:\n%s\n",
            path.c_str());

        return false;
    }

    std::fseek(fp, 0, SEEK_END);

    long fileSize = std::ftell(fp);

    if (fileSize <= 0)
    {
        std::fclose(fp);
        return false;
    }

    std::fseek(fp, 0, SEEK_SET);

    output.resize((size_t)fileSize);

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
// LZ4
// ============================================================

static bool DecompressLZ4(
    const uint8_t* src,
    size_t srcSize,
    uint8_t* dst,
    size_t dstSize)
{
    int result =
        LZ4_decompress_safe(
            (const char*)src,
            (char*)dst,
            (int)srcSize,
            (int)dstSize);

    return result >= 0 &&
           (size_t)result == dstSize;
}


// ============================================================
// ZLIB
// ============================================================

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

    return result == Z_OK &&
           outputSize == dstSize;
}


// ============================================================
// UnityFS BlockInfo Decompression
// ============================================================

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

        case 1:
        {
            /*
                LZMA is uncommon in this particular bundle.
                Unity's raw LZMA stream needs its filter
                properties. We keep the implementation here
                as a clear unsupported path rather than
                silently corrupting data.
            */

            std::printf(
                "[WARN] LZMA block encountered "
                "(%zu -> %zu), skipping.\n",
                srcSize,
                dstSize);

            return false;
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

        default:
        {
            std::printf(
                "[ERROR] Unsupported UnityFS "
                "compression = %u\n",
                compression);

            return false;
        }
    }
}


// ============================================================
// UnityFS Parser
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
        Reader r(
            fileData.data(),
            fileData.size(),
            false);

        header.signature =
            r.ReadCString();

        if (header.signature != "UnityFS")
        {
            std::printf(
                "[ERROR] Not UnityFS.\n");

            return false;
        }

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

        /*
            UnityFS v7 aligns BlocksInfo to 16 bytes
            unless the info-at-end flag is used.
        */

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
                Align16(header.headerEnd);
        }
        else
        {
            header.blocksInfoOffset =
                Align16(header.headerEnd);

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
            std::printf(
                "[ERROR] BlocksInfo outside file.\n");

            return false;
        }

        const uint8_t* src =
            fileData.data() +
            header.blocksInfoOffset;

        std::vector<uint8_t> decoded;

        decoded.resize(
            header.uncompressedBlocksInfoSize);

        /*
            UnityFS flags:
              0x00 = uncompressed
              0x01 = LZMA
              0x02 = LZ4
              0x03 = LZ4HC
        */

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
        else if (compression == 1)
        {
            std::printf(
                "[ERROR] BlocksInfo uses LZMA. "
                "This source currently expects LZ4/LZ4HC.\n");

            return false;
        }
        else
        {
            std::printf(
                "[ERROR] Unsupported BlocksInfo "
                "compression %u\n",
                compression);

            return false;
        }

        if (!ok)
        {
            std::printf(
                "[ERROR] BlocksInfo decompression failed.\n");

            return false;
        }

        Reader r(
            decoded.data(),
            decoded.size(),
            false);

        /*
            UnityFS BlocksInfo starts with 16-byte hash.
        */

        if (!r.Skip(16))
            return false;

        uint32_t blockCount =
            r.ReadU32();

        blocks.clear();

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

        directory.clear();

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
            totalSize += block.uncompressedSize;

        if (totalSize == 0)
            return false;

        contentData.clear();

        contentData.resize(
            (size_t)totalSize);

        size_t outputPos = 0;

        for (size_t i = 0;
             i < blocks.size();
             ++i)
        {
            const auto& block =
                blocks[i];

            if (compressedPos +
                block.compressedSize >
                fileData.size())
            {
                std::printf(
                    "[ERROR] Block %zu outside file.\n",
                    i);

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

            bool ok =
                DecompressUnityBlock(
                    src,
                    block.compressedSize,
                    dst,
                    block.uncompressedSize,
                    block.flags);

            if (!ok)
            {
                std::printf(
                    "[ERROR] Failed block %zu.\n",
                    i);

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
    FindEntry(const std::string& path) const
    {
        for (const auto& entry : directory)
        {
            if (entry.path == path)
                return &entry;
        }

        return nullptr;
    }

    const UnityFSDirectoryEntry*
    FindResourceForPath(
        const std::string& path) const
    {
        std::string wanted =
            BaseName(path);

        for (const auto& entry : directory)
        {
            std::string base =
                BaseName(entry.path);

            if (base == wanted)
                return &entry;

            if (EndsWith(base, wanted))
                return &entry;
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

    size_t headerSize = 0;
    size_t metadataOffset = 0;

    std::string unityVersion;

    int32_t targetPlatform = 0;

    bool enableTypeTree = false;

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

        /*
            SerializedFile header itself is stored big-endian
            in the traditional layout.

            For Unity 2019 / SerializedFile version 22,
            layout is:

              0x00 legacy
              0x08 version
              0x10 metadataSize
              0x18 fileSize
              0x20 dataOffset
              0x28 endian
              0x29 reserved
        */

        uint32_t possibleVersion =
            ReadBE32(8);

        version =
            possibleVersion;

        if (version < 1 ||
            version > 50)
        {
            /*
                Some implementations preserve the
                older interpretation.
            */

            version =
                ReadLE32(8);

            if (version < 1 ||
                version > 50)
            {
                std::printf(
                    "[ERROR] Invalid SerializedFile "
                    "version: %u\n",
                    version);

                return false;
            }
        }

        if (version >= 22)
        {
            return ParseModern();
        }

        return ParseLegacy();
    }

private:

    uint32_t ReadBE32(size_t offset) const
    {
        if (offset + 4 > size)
            return 0;

        return
            ((uint32_t)data[offset] << 24) |
            ((uint32_t)data[offset + 1] << 16) |
            ((uint32_t)data[offset + 2] << 8) |
            ((uint32_t)data[offset + 3]);
    }

    uint64_t ReadBE64(size_t offset) const
    {
        if (offset + 8 > size)
            return 0;

        uint64_t v = 0;

        for (int i = 0; i < 8; ++i)
        {
            v =
                (v << 8) |
                data[offset + i];
        }

        return v;
    }

    uint32_t ReadLE32(size_t offset) const
    {
        if (offset + 4 > size)
            return 0;

        return
            ((uint32_t)data[offset]) |
            ((uint32_t)data[offset + 1] << 8) |
            ((uint32_t)data[offset + 2] << 16) |
            ((uint32_t)data[offset + 3] << 24);
    }

    uint64_t ReadLE64(size_t offset) const
    {
        if (offset + 8 > size)
            return 0;

        uint64_t v = 0;

        for (int i = 0; i < 8; ++i)
        {
            v |=
                ((uint64_t)data[offset + i])
                << (i * 8);
        }

        return v;
    }

    bool ParseModern()
    {
        if (size < 48)
            return false;

        /*
            The modern Unity header is:

            0x00 - legacy bytes
            0x08 - version
            0x10 - metadata size
            0x18 - file size
            0x20 - data offset
            0x28 - endian
        */

        metadataSize =
            ReadBE64(16);

        fileSize =
            ReadBE64(24);

        dataOffset =
            ReadBE64(32);

        uint8_t endian =
            data[40];

        littleEndian =
            endian == 0;

        headerSize = 48;
        metadataOffset = 48;

        if (metadataOffset >
            size)
        {
            return false;
        }

        Reader r(
            data,
            size,
            littleEndian);

        if (!r.Seek(metadataOffset))
            return false;

        return ParseMetadata(r);
    }

    bool ParseLegacy()
    {
        if (size < 20)
            return false;

        /*
            Legacy 20-byte SerializedFile header.
        */

        Reader be(
            data,
            size,
            false);

        metadataSize =
            be.ReadU32();

        fileSize =
            be.ReadU32();

        version =
            be.ReadU32();

        dataOffset =
            be.ReadU32();

        uint8_t endian =
            be.ReadU8();

        be.Skip(3);

        littleEndian =
            endian == 0;

        headerSize = 20;

        metadataOffset = 20;

        Reader r(
            data,
            size,
            littleEndian);

        if (!r.Seek(metadataOffset))
            return false;

        return ParseMetadata(r);
    }

    bool ParseMetadata(Reader& r)
    {
        if (version >= 7)
        {
            unityVersion =
                r.ReadCString();
        }

        if (version >= 8)
        {
            targetPlatform =
                r.ReadI32();
        }

        if (version >= 13)
        {
            enableTypeTree =
                r.ReadU8() != 0;
        }

        std::printf(
            "  Serialized version    : %u\n",
            version);

        std::printf(
            "  Unity version         : %s\n",
            unityVersion.c_str());

        std::printf(
            "  Target platform       : %d\n",
            targetPlatform);

        std::printf(
            "  Endianness            : %s\n",
            littleEndian
                ? "Little"
                : "Big");

        std::printf(
            "  Metadata size         : %llu\n",
            (unsigned long long)metadataSize);

        std::printf(
            "  Data offset           : 0x%llX\n",
            (unsigned long long)dataOffset);

        std::printf(
            "  TypeTree              : %s\n",
            enableTypeTree
                ? "YES"
                : "NO");

        if (!enableTypeTree)
        {
            std::printf(
                "[ERROR] Texture2D parser "
                "requires TypeTree.\n");

            return false;
        }

        int32_t typeCount =
            r.ReadI32();

        if (typeCount < 0 ||
            typeCount > 100000)
        {
            std::printf(
                "[ERROR] Invalid type count: %d\n",
                typeCount);

            return false;
        }

        types.clear();

        std::printf(
            "  Serialized types       : %d\n",
            typeCount);

        for (int32_t i = 0;
             i < typeCount;
             ++i)
        {
            SerializedType type;

            if (!ReadSerializedType(
                    r,
                    false,
                    type))
            {
                std::printf(
                    "[ERROR] Failed type %d\n",
                    i);

                return false;
            }

            types.push_back(
                std::move(type));
        }

        /*
            Versions 7..13 have bigIDEnabled.
        */

        int32_t bigIDEnabled = 0;

        if (version >= 7 &&
            version < 14)
        {
            bigIDEnabled =
                r.ReadI32();
        }

        int32_t objectCount =
            r.ReadI32();

        if (objectCount < 0 ||
            objectCount > 10000000)
        {
            std::printf(
                "[ERROR] Invalid object count: %d\n",
                objectCount);

            return false;
        }

        std::printf(
            "  Serialized objects    : %d\n",
            objectCount);

        objects.clear();
        objects.reserve(objectCount);

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
                /*
                    Object path IDs are aligned.
                */
                r.Align4();

                obj.pathID =
                    r.ReadI64();
            }

            if (version >= 22)
            {
                obj.byteStart =
                    r.ReadU64();
            }
            else
            {
                obj.byteStart =
                    r.ReadU32();
            }

            obj.byteStart +=
                dataOffset;

            obj.byteSize =
                r.ReadU32();

            obj.typeID =
                r.ReadI32();

            if (version < 16)
            {
                /*
                    Old class ID stored explicitly.
                */
                obj.classID =
                    (int32_t)r.ReadU16();
            }
            else
            {
                if (obj.typeID >= 0 &&
                    obj.typeID <
                    (int32_t)types.size())
                {
                    obj.classID =
                        types[obj.typeID].classID;
                }
            }

            /*
                Versions 11..16 contain script type index.
            */
            if (version >= 11 &&
                version < 17)
            {
                r.ReadI16();
            }

            /*
                Versions 15/16 contain stripped flag.
            */
            if (version == 15 ||
                version == 16)
            {
                r.ReadU8();
            }

            objects.push_back(obj);
        }

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
        {
            type.stripped =
                r.ReadU8() != 0;
        }

        if (version >= 17)
        {
            type.scriptTypeIndex =
                r.ReadI16();
        }

        if (version >= 13)
        {
            bool hasScriptID =
                false;

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

                if (!r.CanRead(16))
                    return false;

                for (int i = 0; i < 16; ++i)
                    type.scriptID[i] =
                        r.ReadU8();
            }

            type.oldTypeHash.resize(16);

            if (!r.CanRead(16))
                return false;

            for (int i = 0; i < 16; ++i)
                type.oldTypeHash[i] =
                    r.ReadU8();
        }

        if (enableTypeTree)
        {
            if (!ReadTypeTreeBlob(
                    r,
                    type.typeTree))
            {
                return false;
            }
        }

        /*
            Unity 2019 / serialized version 21+
            stores type dependencies.
        */
        if (version >= 21)
        {
            if (isRefType)
            {
                /*
                    RefType has className / namespace /
                    assemblyName.

                    Not needed for Texture2D.
                */
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

        /*
            Safety limits.
        */
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

        std::vector<RawNode> raw;

        raw.resize(nodeCount);

        for (uint32_t i = 0;
             i < nodeCount;
             ++i)
        {
            RawNode n;

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

            /*
                SerializedFile version 19+
                includes m_RefTypeHash.
            */
            if (version >= 19)
            {
                n.refTypeHash =
                    r.ReadU64();
            }
            else
            {
                n.refTypeHash = 0;
            }

            raw[i] = n;
        }

        std::vector<uint8_t> stringBuffer;

        stringBuffer.resize(
            stringBufferSize);

        for (uint32_t i = 0;
             i < stringBufferSize;
             ++i)
        {
            stringBuffer[i] =
                r.ReadU8();
        }

        std::vector<TypeTreeNode> flat;

        flat.resize(nodeCount);

        for (uint32_t i = 0;
             i < nodeCount;
             ++i)
        {
            const RawNode& n =
                raw[i];

            TypeTreeNode node;

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
                    stringBuffer,
                    n.typeOffset);

            node.name =
                ResolveCommonString(
                    stringBuffer,
                    n.nameOffset);

            flat[i] =
                std::move(node);
        }

        /*
            Convert flat level-based tree
            into actual child vectors.
        */

        if (flat.empty())
            return false;

        root =
            std::move(flat[0]);

        std::vector<TypeTreeNode*> stack;

        stack.push_back(&root);

        for (size_t i = 1;
             i < flat.size();
             ++i)
        {
            TypeTreeNode* parent = nullptr;

            while (!stack.empty() &&
                   stack.back()->level >=
                   flat[i].level)
            {
                stack.pop_back();
            }

            if (!stack.empty())
                parent = stack.back();

            if (!parent)
                continue;

            parent->children.push_back(
                std::move(flat[i]));

            stack.push_back(
                &parent->children.back());
        }

        return true;
    }
};


// ============================================================
// Texture TypeTree Reader
// ============================================================

class TextureTreeReader
{
public:

    const std::vector<uint8_t>* objectData =
        nullptr;

    bool littleEndian = true;

    TextureInfo info;

    std::string resourcePath;

    uint64_t objectBase = 0;

    bool Parse(
        const SerializedType& serializedType,
        const SerializedObject& object,
        const uint8_t* data,
        size_t size,
        bool little)
    {
        if (!data || size == 0)
            return false;

        objectData =
            new std::vector<uint8_t>(
                data,
                data + size);

        littleEndian =
            little;

        info.pathID =
            object.pathID;

        info.objectOffset =
            object.byteStart;

        info.objectSize =
            object.byteSize;

        Reader r(
            data,
            size,
            littleEndian);

        return ReadNode(
            serializedType.typeTree,
            r,
            "");
    }

private:

    /*
        We intentionally avoid depending on
        m_Name etc. being CommonString.
        Texture-specific names are normally in
        the local TypeTree string buffer.
    */

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

    static size_t PrimitiveSize(
        const std::string& type)
    {
        if (type == "bool" ||
            type == "char" ||
            type == "SInt8" ||
            type == "UInt8" ||
            type == "unsigned char")
            return 1;

        if (type == "short" ||
            type == "SInt16" ||
            type == "UInt16" ||
            type == "unsigned short")
            return 2;

        if (type == "int" ||
            type == "SInt32" ||
            type == "UInt32" ||
            type == "unsigned int" ||
            type == "float")
            return 4;

        if (type == "long long" ||
            type == "SInt64" ||
            type == "UInt64" ||
            type == "unsigned long long" ||
            type == "double")
            return 8;

        return 0;
    }

    static bool IsPPtr(
        const std::string& type)
    {
        return
            type.size() >= 5 &&
            type.compare(
                0,
                5,
                "PPtr<") == 0;
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
        else if (node.name ==
                 "m_CompleteImageSize")
        {
            info.completeImageSize =
                (uint32_t)value;
        }
        else if (node.name ==
                 "m_TextureFormat")
        {
            info.textureFormat =
                (int32_t)value;
        }
        else if (node.name ==
                 "m_MipCount")
        {
            info.mipCount =
                (int32_t)value;
        }
        else if (node.name ==
                 "image data" &&
                 path.find("image data") !=
                 std::string::npos)
        {
            info.imageDataSize =
                (int32_t)value;
        }
        else if (node.name == "size" &&
                 PathEnds(path,
                          "image data/size"))
        {
            info.imageDataSize =
                (int32_t)value;
        }
        else if (node.name == "offset" &&
                 path.find(
                     "m_StreamData/offset") !=
                 std::string::npos)
        {
            if (value >= 0)
                info.streamOffset =
                    (uint64_t)value;
        }
        else if (node.name == "size" &&
                 path.find(
                     "m_StreamData/size") !=
                 std::string::npos)
        {
            if (value >= 0)
                info.streamSize =
                    (uint32_t)value;
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
        else if (node.name == "path" &&
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

        if (type == "char" ||
            type == "UInt8" ||
            type == "unsigned char" ||
            type == "SInt8")
        {
            int64_t value =
                (type == "SInt8")
                    ? r.ReadI8()
                    : r.ReadU8();

            CaptureInt(
                node,
                path,
                value);

            if (node.AlignAfter())
                r.Align4();

            return true;
        }

        if (type == "short" ||
            type == "SInt16" ||
            type == "UInt16" ||
            type == "unsigned short")
        {
            int64_t value =
                (type == "SInt16" ||
                 type == "short")
                    ? r.ReadI16()
                    : r.ReadU16();

            CaptureInt(
                node,
                path,
                value);

            if (node.AlignAfter())
                r.Align4();

            return true;
        }

        if (type == "int" ||
            type == "SInt32" ||
            type == "UInt32" ||
            type == "unsigned int")
        {
            int64_t value =
                (type == "int" ||
                 type == "SInt32")
                    ? r.ReadI32()
                    : r.ReadU32();

            CaptureInt(
                node,
                path,
                value);

            if (node.AlignAfter())
                r.Align4();

            return true;
        }

        if (type == "long long" ||
            type == "SInt64" ||
            type == "UInt64" ||
            type == "unsigned long long")
        {
            int64_t value =
                (type == "long long" ||
                 type == "SInt64")
                    ? r.ReadI64()
                    : (int64_t)r.ReadU64();

            CaptureInt(
                node,
                path,
                value);

            if (node.AlignAfter())
                r.Align4();

            return true;
        }

        if (type == "float")
        {
            r.ReadFloat();

            if (node.AlignAfter())
                r.Align4();

            return true;
        }

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
        uint32_t length =
            r.ReadU32();

        if (length > r.Remaining())
            return false;

        std::string value;

        if (length)
        {
            value.assign(
                (const char*)r.data + r.pos,
                length);

            r.Skip(length);
        }

        /*
            Unity strings are aligned.
        */
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
        if (node.children.size() < 2)
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
            image data can be huge. We don't need
            to walk every byte for scanner mode.
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

        if (bytes > r.Remaining())
            return false;

        /*
         * Simpan posisi awal actual texture data.
         */
        info.imageDataOffset =
            (uint64_t)r.pos;

        info.imageDataSize =
            (int32_t)bytes;

        r.Skip(bytes);

        if (node.AlignAfter())
            r.Align4();

        return true;
    }
}

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
        if (!r.CanRead(1))
            return false;

        std::string path =
            parentPath;

        if (!node.name.empty())
        {
            if (!path.empty())
                path += "/";

            path += node.name;
        }

        /*
            Arrays have priority over primitive
            handling.
        */
        if (node.IsArray())
        {
            return ReadArray(
                node,
                r,
                path);
        }

        /*
            Unity serialized strings have an
            internal length + byte array.
        */
        if (node.type == "string")
        {
            return ReadString(
                node,
                r,
                path);
        }

        /*
            Primitive.
        */
        if (IsPrimitive(node.type))
        {
            return ReadPrimitive(
                node,
                r,
                path);
        }

        /*
            Compound structure.
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
// Scanner
// ============================================================

class UnityTextureScanner
{
public:

    UnityFSBundle bundle;

    std::vector<TextureInfo> textures;

    bool Scan(
        const std::string& input)
    {
        std::printf(
            "========================================\n");
        std::printf(
            "MLBB UNITY TEXTURE SCANNER\n");
        std::printf(
            "========================================\n");

        std::printf(
            "Input:\n%s\n\n",
            input.c_str());

        if (!LoadFile(
                input,
                bundle.fileData))
        {
            return false;
        }

        std::printf(
            "Loaded file: %zu bytes\n",
            bundle.fileData.size());

        // ----------------------------------------------------
        // UnityFS Header
        // ----------------------------------------------------

        if (!bundle.ParseHeader())
            return false;

        std::printf(
            "Magic: UnityFS.\n\n");

        std::printf(
            "========================================\n");
        std::printf(
            "UNITYFS HEADER\n");
        std::printf(
            "========================================\n");

        std::printf(
            "Format version         : %u\n",
            bundle.header.formatVersion);

        std::printf(
            "Unity player version   : %s\n",
            bundle.header.playerVersion.c_str());

        std::printf(
            "Unity engine version   : %s\n",
            bundle.header.engineVersion.c_str());

        std::printf(
            "Bundle size (header)   : %llu\n",
            (unsigned long long)
                bundle.header.bundleSize);

        std::printf(
            "Actual file size       : %zu\n",
            bundle.fileData.size());

        std::printf(
            "BlocksInfo compressed  : %u\n",
            bundle.header.compressedBlocksInfoSize);

        std::printf(
            "BlocksInfo uncompressed: %u\n",
            bundle.header.uncompressedBlocksInfoSize);

        std::printf(
            "Flags                  : 0x%X\n",
            bundle.header.flags);

        std::printf(
            "Header end             : 0x%zX\n",
            bundle.header.headerEnd);

        std::printf(
            "BlocksInfo offset      : 0x%zX\n",
            bundle.header.blocksInfoOffset);

        std::printf(
            "Data start             : 0x%zX\n",
            bundle.header.dataStart);

        // ----------------------------------------------------
        // BlocksInfo
        // ----------------------------------------------------

        if (!bundle.ParseBlocksInfo())
            return false;

        std::printf(
            "\nBlocksInfo decoded successfully.\n");

        std::printf(
            "Block count            : %zu\n",
            bundle.blocks.size());

        std::printf(
            "Directory count        : %zu\n",
            bundle.directory.size());

        std::printf(
            "\n");

        for (size_t i = 0;
             i < bundle.blocks.size();
             ++i)
        {
            const auto& b =
                bundle.blocks[i];

            std::printf(
                "Block %04zu | flags=0x%04X | "
                "%u -> %u\n",
                i,
                b.flags,
                b.compressedSize,
                b.uncompressedSize);
        }

        // ----------------------------------------------------
        // Decompress content
        // ----------------------------------------------------

        if (!bundle.DecompressContent())
            return false;

        std::printf(
            "\nContent size           : %zu\n",
            bundle.contentData.size());

        // ----------------------------------------------------
        // Directory
        // ----------------------------------------------------

        std::printf(
            "\n========================================\n");
        std::printf(
            "UNITY DIRECTORY\n");
        std::printf(
            "========================================\n");

        for (size_t i = 0;
             i < bundle.directory.size();
             ++i)
        {
            const auto& e =
                bundle.directory[i];

            std::printf(
                "[%03zu] %s\n",
                i,
                e.path.c_str());

            std::printf(
                "    offset=0x%llX "
                "size=%llu "
                "flags=0x%X\n",
                (unsigned long long)e.offset,
                (unsigned long long)e.size,
                e.flags);
        }

        // ----------------------------------------------------
        // Find SerializedFile
        // ----------------------------------------------------

        const UnityFSDirectoryEntry*
            serializedEntry = nullptr;

        for (const auto& entry :
             bundle.directory)
        {
            if (entry.size == 0)
                continue;

            std::string base =
                BaseName(entry.path);

            if (EndsWith(
                    base,
                    ".resS"))
            {
                continue;
            }

            /*
                First directory entry in this bundle
                is expected to be SerializedFile.
            */
            serializedEntry =
                &entry;

            break;
        }

        if (!serializedEntry)
        {
            std::printf(
                "\n[ERROR] SerializedFile entry "
                "not found.\n");

            return false;
        }

        const uint8_t* serializedData =
            nullptr;

        size_t serializedSize = 0;

        if (!bundle.GetEntryData(
                *serializedEntry,
                serializedData,
                serializedSize))
        {
            return false;
        }

        std::printf(
            "\n========================================\n");
        std::printf(
            "SERIALIZED FILE\n");
        std::printf(
            "========================================\n");

        std::printf(
            "Entry                  : %s\n",
            serializedEntry->path.c_str());

        std::printf(
            "Size                   : %zu\n",
            serializedSize);

        // ----------------------------------------------------
        // Parse SerializedFile
        // ----------------------------------------------------

        SerializedFileParser parser;

        if (!parser.Parse(
                serializedData,
                serializedSize))
        {
            std::printf(
                "[ERROR] SerializedFile parsing failed.\n");

            return false;
        }

        std::printf(
            "\n");

        // ----------------------------------------------------
        // Find Texture2D
        // ----------------------------------------------------

        textures.clear();

        int textureIndex = 0;

        for (const auto& object :
             parser.objects)
        {
            /*
                Texture2D classID = 28.
            */
            if (object.classID != 28)
                continue;

            if (object.typeID < 0 ||
                object.typeID >=
                    (int)parser.types.size())
            {
                continue;
            }

            const SerializedType&
                serializedType =
                    parser.types[object.typeID];

            if (serializedType.classID != 28)
                continue;

            if (object.byteStart >
                serializedSize)
            {
                continue;
            }

            if (object.byteSize >
                serializedSize -
                (size_t)object.byteStart)
            {
                continue;
            }

            const uint8_t* objectData =
                serializedData +
                (size_t)object.byteStart;

            size_t objectSize =
                object.byteSize;

            TextureTreeReader textureReader;

            if (!textureReader.Parse(
                    serializedType,
                    object,
                    objectData,
                    objectSize,
                    parser.littleEndian))
            {
                std::printf(
                    "\n[WARN] Failed Texture2D "
                    "object pathID=%lld\n",
                    (long long)object.pathID);

                continue;
            }

            TextureInfo info =
                textureReader.info;

            info.index =
                textureIndex++;

            textures.push_back(
                std::move(info));
        }

        // ----------------------------------------------------
        // Resolve .resS
        // ----------------------------------------------------

        for (auto& texture :
             textures)
        {
            if (texture.streamPath.empty())
                continue;

            const UnityFSDirectoryEntry*
                resource =
                    bundle.FindResourceForPath(
                        texture.streamPath);

            if (!resource)
                continue;

            uint64_t resourceAbsolute =
                resource->offset +
                texture.streamOffset;

            if (resourceAbsolute >
                bundle.contentData.size())
            {
                continue;
            }

            if (texture.streamSize >
                bundle.contentData.size() -
                (size_t)resourceAbsolute)
            {
                continue;
            }

            texture.resolvedResourceOffset =
                resourceAbsolute;

            texture.resourceResolved =
                true;
        }

        // ----------------------------------------------------
        // Output
        // ----------------------------------------------------

        std::printf(
            "\n========================================\n");
        std::printf(
            "TEXTURE2D RESULTS\n");
        std::printf(
            "========================================\n");

        if (textures.empty())
        {
            std::printf(
                "No Texture2D found.\n");
        }
        else
        {
            for (const auto& texture :
                 textures)
            {
                PrintTexture(
                    texture);
            }
        }

        std::printf(
            "\n========================================\n");
        std::printf(
            "SCAN COMPLETE\n");
        std::printf(
            "========================================\n");

        std::printf(
            "Texture2D count: %zu\n",
            textures.size());

        return true;
    }

private:

    static void PrintTexture(
        const TextureInfo& t)
    {
        std::printf(
            "\n[%03d]\n",
            t.index);

        std::printf(
            "  PathID          : %lld\n",
            (long long)t.pathID);

        std::printf(
            "  Object offset   : 0x%llX\n",
            (unsigned long long)t.objectOffset);

        std::printf(
            "  Object size     : %u\n",
            t.objectSize);

        std::printf(
            "  Name            : %s\n",
            t.name.empty()
                ? "<empty>"
                : t.name.c_str());

        std::printf(
            "  Width           : %d\n",
            t.width);

        std::printf(
            "  Height          : %d\n",
            t.height);

        std::printf(
            "  Complete size   : %u\n",
            t.completeImageSize);

        std::printf(
            "  TextureFormat   : %d (%s)\n",
            t.textureFormat,
            TextureFormatName(
                t.textureFormat));

        std::printf(
            "  MipCount        : %d\n",
            t.mipCount);

        std::printf(
            "  IsReadable      : %s\n",
            t.isReadable
                ? "YES"
                : "NO");

        std::printf(
            "  Image data size : %d\n",
            t.imageDataSize);

        if (!t.streamPath.empty())
        {
            std::printf(
                "  StreamData      : YES\n");

            std::printf(
                "    offset        : 0x%llX\n",
                (unsigned long long)
                    t.streamOffset);

            std::printf(
                "    size          : %u\n",
                t.streamSize);

            std::printf(
                "    path          : %s\n",
                t.streamPath.c_str());

            if (t.resourceResolved)
            {
                std::printf(
                    "    resource      : RESOLVED\n");

                std::printf(
                    "    absolute      : 0x%llX\n",
                    (unsigned long long)
                        t.resolvedResourceOffset);
            }
            else
            {
                std::printf(
                    "    resource      : NOT RESOLVED\n");
            }
        }
        else
        {
            std::printf(
                "  StreamData      : NO\n");
        }
    }
};


// ============================================================
// Main
// ============================================================

int main(int argc, char** argv)
{
    std::string input =
        "/storage/emulated/0/@MLBB/"
        "Atlas_Hero_Head.unity3d";

    if (argc >= 2)
        input = argv[1];

    UnityTextureScanner scanner;

    if (!scanner.Scan(input))
    {
        std::printf(
            "\n[FAILED]\n");

        return 1;
    }

    return 0;
}