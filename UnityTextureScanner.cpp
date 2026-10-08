#include "UnityTextureScanner.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "lz4.h"
#include <zlib.h>


namespace
{

static size_t AlignUp(
    size_t value,
    size_t alignment
)
{
    if (alignment == 0)
        return value;

    const size_t remainder =
        value % alignment;

    if (remainder == 0)
        return value;

    return value +
           (alignment - remainder);
}


static size_t Align4Value(
    size_t value
)
{
    return AlignUp(
        value,
        4
    );
}


static size_t Align8(
    size_t value
)
{
    return AlignUp(
        value,
        8
    );
}


static size_t Align16(
    size_t value
)
{
    return AlignUp(
        value,
        16
    );
}


static std::string BaseName(
    const std::string& path
)
{
    const size_t p =
        path.find_last_of(
            "/\\"
        );

    if (p == std::string::npos)
        return path;

    return path.substr(
        p + 1
    );
}


static bool EndsWith(
    const std::string& value,
    const std::string& suffix
)
{
    if (value.size() < suffix.size())
        return false;

    return std::equal(
        suffix.rbegin(),
        suffix.rend(),
        value.rbegin()
    );
}


/*
 * ================================================================
 * READER
 * ================================================================
 */

class Reader
{
public:
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;

    bool littleEndian = true;

    Reader() = default;

    Reader(
        const uint8_t* p,
        size_t s,
        bool little = true
    )
        : data(p),
          size(s),
          pos(0),
          littleEndian(little)
    {
    }

    bool CanRead(
        size_t count
    ) const
    {
        return
            pos <= size &&
            count <= size - pos;
    }

    bool Seek(
        size_t newPos
    )
    {
        if (newPos > size)
            return false;

        pos = newPos;
        return true;
    }

    bool Skip(
        size_t count
    )
    {
        if (!CanRead(count))
            return false;

        pos += count;
        return true;
    }

    bool Align4()
    {
        const size_t p =
            Align4Value(pos);

        return Seek(p);
    }

    uint8_t ReadU8()
    {
        if (!CanRead(1))
            return 0;

        return data[pos++];
    }

    int8_t ReadI8()
    {
        return static_cast<int8_t>(
            ReadU8()
        );
    }

    uint16_t ReadU16()
    {
        if (!CanRead(2))
            return 0;

        uint16_t a = data[pos + 0];
        uint16_t b = data[pos + 1];

        pos += 2;

        if (littleEndian)
        {
            return
                a |
                static_cast<uint16_t>(
                    b << 8
                );
        }

        return
            static_cast<uint16_t>(
                (a << 8) | b
            );
    }

    int16_t ReadI16()
    {
        return static_cast<int16_t>(
            ReadU16()
        );
    }

    uint32_t ReadU32()
    {
        if (!CanRead(4))
            return 0;

        uint32_t a = data[pos + 0];
        uint32_t b = data[pos + 1];
        uint32_t c = data[pos + 2];
        uint32_t d = data[pos + 3];

        pos += 4;

        if (littleEndian)
        {
            return
                a |
                (b << 8) |
                (c << 16) |
                (d << 24);
        }

        return
            (a << 24) |
            (b << 16) |
            (c << 8) |
            d;
    }

    int32_t ReadI32()
    {
        return static_cast<int32_t>(
            ReadU32()
        );
    }

    uint64_t ReadU64()
    {
        if (!CanRead(8))
            return 0;

        uint64_t result = 0;

        if (littleEndian)
        {
            for (int i = 0; i < 8; ++i)
            {
                result |=
                    static_cast<uint64_t>(
                        data[pos + i]
                    ) << (i * 8);
            }
        }
        else
        {
            for (int i = 0; i < 8; ++i)
            {
                result =
                    (result << 8) |
                    data[pos + i];
            }
        }

        pos += 8;

        return result;
    }

    int64_t ReadI64()
    {
        return static_cast<int64_t>(
            ReadU64()
        );
    }

    float ReadFloat()
    {
        uint32_t value =
            ReadU32();

        float result = 0.0f;

        std::memcpy(
            &result,
            &value,
            sizeof(result)
        );

        return result;
    }

    double ReadDouble()
    {
        uint64_t value =
            ReadU64();

        double result = 0.0;

        std::memcpy(
            &result,
            &value,
            sizeof(result)
        );

        return result;
    }

    std::string ReadCString()
    {
        if (pos >= size)
            return {};

        const size_t start = pos;

        while (pos < size &&
               data[pos] != 0)
        {
            ++pos;
        }

        std::string result(
            reinterpret_cast<
                const char*
            >(data + start),
            pos - start
        );

        if (pos < size)
            ++pos;

        return result;
    }
};


/*
 * ================================================================
 * TYPE TREE
 * ================================================================
 */

struct TypeTreeNode
{
    int16_t version = 0;
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
        return
            (metaFlags & 0x4000) != 0 ||
            type == "Array";
    }

    bool AlignAfter() const
    {
        return
            (metaFlags & 0x4000) != 0x0;
    }
};


/*
 * ================================================================
 * COMMON TYPE TREE STRINGS
 *
 * Unity TypeTree stores some strings by index rather than directly
 * inside the local string buffer.
 * ================================================================
 */

static const char* ResolveCommonString(
    uint32_t index
)
{
    /*
     * Unity common-string table.
     *
     * Only strings used by Texture2D parsing are important here,
     * but the table contains the standard primitive/object names
     * needed by the TypeTree reader.
     */

    static const char* const strings[] =
    {
        "AABB",
        "AnimationClip",
        "AnimationCurve",
        "AnimationEvent",
        "Array",
        "Base",
        "BitField",
        "bitset",
        "bool",
        "char",
        "ColorRGBA",
        "Component",
        "data",
        "deque",
        "first",
        "float",
        "Font",
        "GameObject",
        "GenericMono",
        "Gradient",
        "GUID",
        "GUIStyle",
        "GUIStyleState",
        "int",
        "list",
        "long long",
        "map",
        "Matrix4x4",
        "MonoBehaviour",
        "MonoScript",
        "m_ByteSize",
        "m_Curve",
        "m_EditorClassIdentifier",
        "m_Enabled",
        "m_ExtensionPtr",
        "m_GameObject",
        "m_Name",
        "m_ObjectHideFlags",
        "m_PrefabInternal",
        "m_PrefabParentObject",
        "m_PrefabInstance",
        "m_PrefabAsset",
        "m_Script",
        "m_StaticEditorFlags",
        "m_Type",
        "m_UserData",
        "m_CorrespondingSourceObject",
        "m_PrefabInstance",
        "m_PrefabAsset",
        "Object",
        "pair",
        "PPtr<Component>",
        "PPtr<GameObject>",
        "PPtr<Material>",
        "PPtr<MonoBehaviour>",
        "PPtr<MonoScript>",
        "PPtr<Object>",
        "PPtr<Shader>",
        "PPtr<Sprite>",
        "PPtr<Texture>",
        "PPtr<Texture2D>",
        "PPtr<Transform>",
        "Quaternionf",
        "Rectf",
        "RectInt",
        "short",
        "size",
        "string",
        "TextAsset",
        "Texture",
        "Texture2D",
        "Transform",
        "TypelessData",
        "uint",
        "unsigned int",
        "unsigned long long",
        "unsigned short",
        "vector",
        "Vector2f",
        "Vector3f",
        "Vector4f",
        "m_Width",
        "m_Height",
        "m_CompleteImageSize",
        "m_TextureFormat",
        "m_MipCount",
        "m_IsReadable",
        "m_ImageCount",
        "m_TextureDimension",
        "m_TextureSettings",
        "m_FilterMode",
        "m_Aniso",
        "m_MipBias",
        "m_WrapU",
        "m_WrapV",
        "m_WrapW",
        "m_LightmapFormat",
        "m_ColorSpace",
        "m_StreamingMipmaps",
        "m_StreamingMipmapsPriority",
        "m_StreamingGroupID",
        "m_ImageDataSize",
        "image data",
        "m_StreamData",
        "offset",
        "size",
        "path",
        "m_Lightmap",
        "m_Width",
        "m_Height",
        "m_TextureFormat",
        "m_MipCount",
        "m_IsReadable",
        "m_Name"
    };

    constexpr uint32_t count =
        static_cast<uint32_t>(
            sizeof(strings) /
            sizeof(strings[0])
        );

    if (index >= count)
        return nullptr;

    return strings[index];
}


/*
 * ================================================================
 * SERIALIZED STRUCTURES
 * ================================================================
 */

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


/*
 * ================================================================
 * UNITYFS
 * ================================================================
 */

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


/*
 * ================================================================
 * FILE LOADER
 * ================================================================
 */

static bool LoadFile(
    const std::string& path,
    std::vector<uint8_t>& output
)
{
    output.clear();

    FILE* file =
        std::fopen(
            path.c_str(),
            "rb"
        );

    if (!file)
        return false;

    if (std::fseek(
            file,
            0,
            SEEK_END) != 0)
    {
        std::fclose(file);
        return false;
    }

    const long length =
        std::ftell(file);

    if (length <= 0)
    {
        std::fclose(file);
        return false;
    }

    if (std::fseek(
            file,
            0,
            SEEK_SET) != 0)
    {
        std::fclose(file);
        return false;
    }

    output.resize(
        static_cast<size_t>(
            length
        )
    );

    const size_t read =
        std::fread(
            output.data(),
            1,
            output.size(),
            file
        );

    std::fclose(file);

    if (read != output.size())
    {
        output.clear();
        return false;
    }

    return true;
}


/*
 * ================================================================
 * LZ4
 * ================================================================
 */

static bool DecompressLZ4(
    const uint8_t* input,
    size_t inputSize,
    uint8_t* output,
    size_t outputSize
)
{
    if (!input ||
        !output ||
        inputSize == 0 ||
        outputSize == 0)
    {
        return false;
    }

    if (inputSize >
        static_cast<size_t>(
            INT32_MAX
        ))
    {
        return false;
    }

    if (outputSize >
        static_cast<size_t>(
            INT32_MAX
        ))
    {
        return false;
    }

    const int result =
        LZ4_decompress_safe(
            reinterpret_cast<
                const char*
            >(input),
            reinterpret_cast<
                char*
            >(output),
            static_cast<int>(
                inputSize
            ),
            static_cast<int>(
                outputSize
            )
        );

    return result >= 0 &&
           static_cast<size_t>(
               result
           ) == outputSize;
}


/*
 * ================================================================
 * ZLIB
 * ================================================================
 */

static bool DecompressZlib(
    const uint8_t* input,
    size_t inputSize,
    uint8_t* output,
    size_t outputSize
)
{
    if (!input ||
        !output ||
        inputSize == 0 ||
        outputSize == 0)
    {
        return false;
    }

    uLongf destinationSize =
        static_cast<uLongf>(
            outputSize
        );

    const int result =
        ::uncompress(
            output,
            &destinationSize,
            input,
            static_cast<uLong>(
                inputSize
            )
        );

    return
        result == Z_OK &&
        destinationSize == outputSize;
}


/*
 * ================================================================
 * UNITYFS BLOCK DECOMPRESSION
 * ================================================================
 */

static bool DecompressUnityBlock(
    const uint8_t* input,
    size_t inputSize,
    uint8_t* output,
    size_t outputSize,
    uint16_t flags
)
{
    const uint16_t compression =
        flags & 0x3F;

    switch (compression)
    {
        case 0:
        {
            if (inputSize != outputSize)
                return false;

            std::memcpy(
                output,
                input,
                outputSize
            );

            return true;
        }

        case 2:
        case 3:
        {
            return DecompressLZ4(
                input,
                inputSize,
                output,
                outputSize
            );
        }

        case 4:
        {
            return DecompressZlib(
                input,
                inputSize,
                output,
                outputSize
            );
        }

        default:
            return false;
    }
}


/*
 * ================================================================
 * UNITYFS BUNDLE
 * ================================================================
 */

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
        if (fileData.size() < 16)
            return false;

        Reader r(
            fileData.data(),
            fileData.size(),
            false
        );

        header.signature =
            r.ReadCString();

        if (header.signature != "UnityFS")
            return false;

        header.formatVersion =
            r.ReadU32();

        if (header.formatVersion == 0 ||
            header.formatVersion > 100)
        {
            return false;
        }

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

        if (r.pos > fileData.size())
            return false;

        header.headerEnd =
            r.pos;

        const bool blocksAtEnd =
            (header.flags & 0x80) != 0;

        if (blocksAtEnd)
        {
            header.blocksInfoOffset =
                fileData.size() -
                header.compressedBlocksInfoSize;

            header.dataStart =
                header.headerEnd;
        }
        else
        {
            header.blocksInfoOffset =
                header.headerEnd;

            header.dataStart =
                Align16(
                    header.headerEnd +
                    header.compressedBlocksInfoSize
                );
        }

        if (header.blocksInfoOffset >
            fileData.size())
        {
            return false;
        }

        if (header.compressedBlocksInfoSize >
            fileData.size() -
            header.blocksInfoOffset)
        {
            return false;
        }

        return true;
    }


    bool ParseBlocksInfo()
    {
        if (header.compressedBlocksInfoSize == 0 ||
            header.uncompressedBlocksInfoSize == 0)
        {
            return false;
        }

        if (header.blocksInfoOffset >
            fileData.size())
        {
            return false;
        }

        if (header.compressedBlocksInfoSize >
            fileData.size() -
            header.blocksInfoOffset)
        {
            return false;
        }

        const uint8_t* compressed =
            fileData.data() +
            header.blocksInfoOffset;

        std::vector<uint8_t> info(
            header.uncompressedBlocksInfoSize
        );

        const uint16_t compression =
            static_cast<uint16_t>(
                header.flags & 0x3F
            );

        bool ok = false;

        if (compression == 0)
        {
            if (header.compressedBlocksInfoSize !=
                header.uncompressedBlocksInfoSize)
            {
                return false;
            }

            std::memcpy(
                info.data(),
                compressed,
                info.size()
            );

            ok = true;
        }
        else if (
            compression == 2 ||
            compression == 3)
        {
            ok =
                DecompressLZ4(
                    compressed,
                    header.compressedBlocksInfoSize,
                    info.data(),
                    info.size()
                );
        }
        else if (compression == 4)
        {
            ok =
                DecompressZlib(
                    compressed,
                    header.compressedBlocksInfoSize,
                    info.data(),
                    info.size()
                );
        }

        if (!ok)
            return false;

        Reader r(
            info.data(),
            info.size(),
            false
        );

        /*
         * UnityFS BlocksInfo begins with a 16-byte hash.
         */
        if (!r.Skip(16))
            return false;

        const uint32_t blockCount =
            r.ReadU32();

        if (blockCount == 0 ||
            blockCount > 1000000)
        {
            return false;
        }

        blocks.clear();

        blocks.reserve(
            blockCount
        );

        for (uint32_t i = 0;
             i < blockCount;
             ++i)
        {
            UnityFSBlock block;

            block.compressedSize =
                r.ReadU32();

            block.uncompressedSize =
                r.ReadU32();

            block.flags =
                r.ReadU16();

            if (!r.CanRead(0))
                return false;

            blocks.push_back(
                block
            );
        }

        const uint32_t directoryCount =
            r.ReadU32();

        if (directoryCount == 0 ||
            directoryCount > 1000000)
        {
            return false;
        }

        directory.clear();

        directory.reserve(
            directoryCount
        );

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
                std::move(entry)
            );
        }

        return true;
    }


    bool DecompressContent()
    {
        if (blocks.empty())
            return false;

        uint64_t totalSize = 0;

        for (const auto& block :
             blocks)
        {
            totalSize +=
                block.uncompressedSize;

            if (totalSize >
                static_cast<uint64_t>(
                    SIZE_MAX
                ))
            {
                return false;
            }
        }

        contentData.resize(
            static_cast<size_t>(
                totalSize
            )
        );

        size_t sourceOffset =
            header.dataStart;

        size_t destinationOffset = 0;

        for (const auto& block :
             blocks)
        {
            if (sourceOffset >
                fileData.size())
            {
                return false;
            }

            if (block.compressedSize >
                fileData.size() -
                sourceOffset)
            {
                return false;
            }

            if (destinationOffset >
                contentData.size())
            {
                return false;
            }

            if (block.uncompressedSize >
                contentData.size() -
                destinationOffset)
            {
                return false;
            }

            const uint8_t* input =
                fileData.data() +
                sourceOffset;

            uint8_t* output =
                contentData.data() +
                destinationOffset;

            if (!DecompressUnityBlock(
                    input,
                    block.compressedSize,
                    output,
                    block.uncompressedSize,
                    block.flags))
            {
                return false;
            }

            sourceOffset +=
                block.compressedSize;

            destinationOffset +=
                block.uncompressedSize;
        }

        return true;
    }


    const UnityFSDirectoryEntry*
    FindResourceForPath(
        const std::string& path
    ) const
    {
        const std::string wanted =
            BaseName(path);

        for (const auto& entry :
             directory)
        {
            const std::string name =
                BaseName(entry.path);

            if (name == wanted)
                return &entry;

            if (entry.path == path)
                return &entry;

            if (EndsWith(
                    entry.path,
                    path))
            {
                return &entry;
            }
        }

        return nullptr;
    }


    bool GetEntryData(
        const UnityFSDirectoryEntry& entry,
        const uint8_t*& data,
        size_t& size
    ) const
    {
        data = nullptr;
        size = 0;

        if (entry.offset >
            contentData.size())
        {
            return false;
        }

        if (entry.size >
            contentData.size() -
            static_cast<size_t>(
                entry.offset
            ))
        {
            return false;
        }

        data =
            contentData.data() +
            static_cast<size_t>(
                entry.offset
            );

        size =
            static_cast<size_t>(
                entry.size
            );

        return true;
    }
};


/*
 * ================================================================
 * SERIALIZED FILE PARSER
 * ================================================================
 */

class SerializedFileParser
{
public:
    const uint8_t* data = nullptr;

    size_t size = 0;

    uint32_t version = 0;

    uint32_t metadataSize = 0;

    uint64_t fileSize = 0;

    uint64_t dataOffset = 0;

    bool littleEndian = true;

    std::vector<
        SerializedType
    > types;

    std::vector<
        SerializedObject
    > objects;


    bool Parse(
        const uint8_t* input,
        size_t inputSize
    )
    {
        data = input;
        size = inputSize;

        if (!data ||
            size < 32)
        {
            return false;
        }

        uint32_t beVersion =
            ReadBE32(
                data + 8
            );

        uint32_t leVersion =
            ReadLE32(
                data + 8
            );

        if (beVersion >= 1 &&
            beVersion <= 50)
        {
            version = beVersion;
        }
        else if (
            leVersion >= 1 &&
            leVersion <= 50)
        {
            version = leVersion;
        }
        else
        {
            return false;
        }

        if (version >= 22)
            return ParseModern();

        return ParseLegacy();
    }


private:

    static uint32_t ReadBE32(
        const uint8_t* p
    )
    {
        return
            (static_cast<uint32_t>(p[0]) << 24) |
            (static_cast<uint32_t>(p[1]) << 16) |
            (static_cast<uint32_t>(p[2]) << 8) |
            static_cast<uint32_t>(p[3]);
    }


    static uint64_t ReadBE64(
        const uint8_t* p
    )
    {
        uint64_t result = 0;

        for (int i = 0; i < 8; ++i)
        {
            result =
                (result << 8) |
                p[i];
        }

        return result;
    }


    static uint32_t ReadLE32(
        const uint8_t* p
    )
    {
        return
            static_cast<uint32_t>(p[0]) |
            (static_cast<uint32_t>(p[1]) << 8) |
            (static_cast<uint32_t>(p[2]) << 16) |
            (static_cast<uint32_t>(p[3]) << 24);
    }


    bool ParseModern()
    {
        if (size < 48)
            return false;

        metadataSize =
            ReadBE32(
                data + 16
            );

        fileSize =
            ReadBE64(
                data + 24
            );

        dataOffset =
            ReadBE64(
                data + 32
            );

        const uint8_t endian =
            data[40];

        littleEndian =
            endian != 0;

        if (dataOffset >= size)
            return false;

        if (metadataSize >
            size - 48)
        {
            return false;
        }

        Reader r(
            data + 48,
            metadataSize,
            littleEndian
        );

        return ParseMetadata(r);
    }


    bool ParseLegacy()
    {
        if (size < 16)
            return false;

        uint32_t metadata =
            ReadBE32(
                data + 0
            );

        uint32_t file =
            ReadBE32(
                data + 4
            );

        uint32_t offset =
            ReadBE32(
                data + 8
            );

        if (metadata == 0 ||
            offset >= size)
        {
            return false;
        }

        metadataSize =
            metadata;

        fileSize =
            file;

        dataOffset =
            offset;

        littleEndian = false;

        size_t metadataStart = 16;

        if (metadataStart >
            size)
        {
            return false;
        }

        if (metadataSize >
            size - metadataStart)
        {
            return false;
        }

        Reader r(
            data + metadataStart,
            metadataSize,
            littleEndian
        );

        return ParseMetadata(r);
    }


    bool ParseMetadata(
        Reader& r
    )
    {
        if (version >= 7)
        {
            if (!r.ReadCString().empty())
            {
            }
        }

        if (version >= 8)
        {
            r.ReadI32();
        }

        bool enableTypeTree = true;

        if (version >= 13)
        {
            enableTypeTree =
                r.ReadU8() != 0;
        }

        if (!enableTypeTree)
            return false;

        const int32_t typeCount =
            r.ReadI32();

        if (typeCount <= 0 ||
            typeCount > 100000)
        {
            return false;
        }

        types.clear();

        types.reserve(
            static_cast<size_t>(
                typeCount
            )
        );

        for (int32_t i = 0;
             i < typeCount;
             ++i)
        {
            SerializedType type;

            if (!ReadSerializedType(
                    r,
                    type))
            {
                return false;
            }

            types.push_back(
                std::move(type)
            );
        }

        bool bigIDEnabled = false;

        if (version >= 7 &&
            version <= 13)
        {
            bigIDEnabled =
                r.ReadI32() != 0;
        }

        const int32_t objectCount =
            r.ReadI32();

        if (objectCount < 0 ||
            objectCount > 10000000)
        {
            return false;
        }

        objects.clear();

        objects.reserve(
            static_cast<size_t>(
                objectCount
            )
        );

        for (int32_t i = 0;
             i < objectCount;
             ++i)
        {
            SerializedObject object;

            if (version >= 14)
            {
                r.Align4();
            }

            if (bigIDEnabled)
            {
                object.pathID =
                    r.ReadI64();
            }
            else if (version < 14)
            {
                object.pathID =
                    r.ReadI32();
            }
            else
            {
                object.pathID =
                    r.ReadI64();
            }

            if (version >= 22)
            {
                object.byteStart =
                    r.ReadU64();
            }
            else
            {
                object.byteStart =
                    r.ReadU32();
            }

            object.byteStart +=
                dataOffset;

            object.byteSize =
                r.ReadU32();

            object.typeID =
                r.ReadI32();

            if (version < 16)
            {
                object.classID =
                    r.ReadI16();
            }
            else
            {
                if (object.typeID >= 0 &&
                    static_cast<size_t>(
                        object.typeID
                    ) < types.size())
                {
                    object.classID =
                        types[
                            object.typeID
                        ].classID;
                }
            }

            if (version < 11)
            {
                r.ReadI16();
            }

            if (version == 15 ||
                version == 16)
            {
                r.ReadI16();
            }

            if (version >= 17)
            {
                r.ReadI32();
            }

            objects.push_back(
                object
            );
        }

        return true;
    }


    bool ReadSerializedType(
        Reader& r,
        SerializedType& type
    )
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
            const bool hasScriptID =
                type.classID == 114;

            if (hasScriptID)
            {
                if (!r.CanRead(16))
                    return false;

                type.scriptID.resize(16);

                for (size_t i = 0;
                     i < 16;
                     ++i)
                {
                    type.scriptID[i] =
                        r.ReadU8();
                }
            }
        }

        if (version >= 12)
        {
            if (!r.CanRead(16))
                return false;

            type.oldTypeHash.resize(16);

            for (size_t i = 0;
                 i < 16;
                 ++i)
            {
                type.oldTypeHash[i] =
                    r.ReadU8();
            }
        }

        if (!ReadTypeTreeBlob(
                r,
                type.typeTree))
        {
            return false;
        }

        if (version >= 21)
        {
            const int32_t dependencyCount =
                r.ReadI32();

            if (dependencyCount < 0 ||
                dependencyCount > 100000)
            {
                return false;
            }

            type.dependencies.resize(
                static_cast<size_t>(
                    dependencyCount
                )
            );

            for (int32_t i = 0;
                 i < dependencyCount;
                 ++i)
            {
                type.dependencies[i] =
                    r.ReadI32();
            }
        }

        return true;
    }


    bool ReadTypeTreeBlob(
        Reader& r,
        TypeTreeNode& root
    )
    {
        const uint32_t nodeCount =
            r.ReadU32();

        const uint32_t stringBufferSize =
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
            int16_t version;
            uint8_t level;
            uint8_t typeFlags;

            uint32_t typeOffset;
            uint32_t nameOffset;

            int32_t byteSize;
            int32_t index;
            int32_t metaFlags;

            uint64_t refTypeHash;
        };

        std::vector<RawNode> nodes;

        nodes.resize(
            nodeCount
        );

        for (uint32_t i = 0;
             i < nodeCount;
             ++i)
        {
            RawNode node{};

            node.version =
                r.ReadI16();

            node.level =
                r.ReadU8();

            node.typeFlags =
                r.ReadU8();

            node.typeOffset =
                r.ReadU32();

            node.nameOffset =
                r.ReadU32();

            node.byteSize =
                r.ReadI32();

            node.index =
                r.ReadI32();

            node.metaFlags =
                r.ReadI32();

            node.refTypeHash =
                r.ReadU64();

            nodes[i] = node;
        }

        if (!r.CanRead(
                stringBufferSize))
        {
            return false;
        }

        const uint8_t* stringBuffer =
            r.data + r.pos;

        r.pos +=
            stringBufferSize;


        auto ResolveString =
            [&](uint32_t offset) -> std::string
        {
            /*
             * High bit means common string.
             */
            if (offset & 0x80000000u)
            {
                const uint32_t index =
                    offset &
                    0x7FFFFFFFu;

                const char* common =
                    ResolveCommonString(
                        index
                    );

                if (common)
                    return common;

                return {};
            }

            if (offset >= stringBufferSize)
                return {};

            const char* ptr =
                reinterpret_cast<
                    const char*
                >(
                    stringBuffer +
                    offset
                );

            const size_t remaining =
                stringBufferSize -
                offset;

            size_t length = 0;

            while (length < remaining &&
                   ptr[length] != '\0')
            {
                ++length;
            }

            return std::string(
                ptr,
                length
            );
        };


        std::vector<TypeTreeNode>
            flat;

        flat.resize(
            nodeCount
        );

        for (uint32_t i = 0;
             i < nodeCount;
             ++i)
        {
            flat[i].version =
                nodes[i].version;

            flat[i].level =
                nodes[i].level;

            flat[i].typeFlags =
                nodes[i].typeFlags;

            flat[i].typeStringOffset =
                nodes[i].typeOffset;

            flat[i].nameStringOffset =
                nodes[i].nameOffset;

            flat[i].byteSize =
                nodes[i].byteSize;

            flat[i].index =
                nodes[i].index;

            flat[i].metaFlags =
                nodes[i].metaFlags;

            flat[i].refTypeHash =
                nodes[i].refTypeHash;

            flat[i].type =
                ResolveString(
                    nodes[i].typeOffset
                );

            flat[i].name =
                ResolveString(
                    nodes[i].nameOffset
                );
        }


        /*
         * The first node is the root.
         */
        root =
            std::move(flat[0]);

        root.children.clear();


        /*
         * Rebuild hierarchy recursively.
         *
         * Unity TypeTree nodes are stored flat and ordered by
         * level.
         */
        std::function<
            size_t(
                TypeTreeNode&,
                size_t,
                uint8_t
            )
        > BuildChildren;

        BuildChildren =
            [&](TypeTreeNode& parent,
                size_t index,
                uint8_t parentLevel)
                -> size_t
        {
            while (index < flat.size())
            {
                const uint8_t level =
                    flat[index].level;

                if (level <= parentLevel)
                    break;

                if (level !=
                    static_cast<uint8_t>(
                        parentLevel + 1
                    ))
                {
                    ++index;
                    continue;
                }

                TypeTreeNode child =
                    flat[index];

                ++index;

                index =
                    BuildChildren(
                        child,
                        index,
                        level
                    );

                parent.children.push_back(
                    std::move(child)
                );
            }

            return index;
        };


        BuildChildren(
            root,
            1,
            root.level
        );

        return true;
    }
};


/*
 * ================================================================
 * TEXTURE TREE READER
 * ================================================================
 */

class TextureTreeReader
{
public:
    bool littleEndian = true;

    TextureInfo info;


    bool Parse(
        const SerializedType& type,
        const SerializedObject& object,
        const uint8_t* data,
        size_t dataSize,
        bool little
    )
    {
        littleEndian =
            little;

        info = TextureInfo{};

        info.pathID =
            object.pathID;

        info.objectOffset =
            object.byteStart;

        info.objectSize =
            object.byteSize;

        if (!data)
            return false;

        if (object.byteStart >
            dataSize)
        {
            return false;
        }

        if (object.byteSize >
            dataSize -
            static_cast<size_t>(
                object.byteStart
            ))
        {
            return false;
        }

        Reader r(
            data +
            static_cast<size_t>(
                object.byteStart
            ),
            object.byteSize,
            littleEndian
        );

        return ReadNode(
            r,
            type.typeTree,
            ""
        );
    }


private:

    static bool IsPrimitive(
        const std::string& type
    )
    {
        return
            type == "bool" ||
            type == "char" ||
            type == "short" ||
            type == "unsigned short" ||
            type == "int" ||
            type == "unsigned int" ||
            type == "long long" ||
            type == "unsigned long long" ||
            type == "float" ||
            type == "double" ||
            type == "UInt8" ||
            type == "SInt8" ||
            type == "UInt16" ||
            type == "SInt16" ||
            type == "UInt32" ||
            type == "SInt32" ||
            type == "UInt64" ||
            type == "SInt64";
    }


    static bool PathEnds(
        const std::string& path,
        const char* value
    )
    {
        if (!value)
            return false;

        const size_t length =
            std::strlen(value);

        if (path.size() < length)
            return false;

        return
            path.compare(
                path.size() - length,
                length,
                value
            ) == 0;
    }


    void CaptureInt(
        const std::string& name,
        int64_t value
    )
    {
        if (name == "m_Width")
        {
            info.width =
                static_cast<int32_t>(
                    value
                );
        }
        else if (name == "m_Height")
        {
            info.height =
                static_cast<int32_t>(
                    value
                );
        }
        else if (
            name ==
            "m_CompleteImageSize")
        {
            info.completeImageSize =
                static_cast<uint32_t>(
                    value
                );
        }
        else if (
            name ==
            "m_TextureFormat")
        {
            info.textureFormat =
                static_cast<int32_t>(
                    value
                );
        }
        else if (
            name == "m_MipCount")
        {
            info.mipCount =
                static_cast<int32_t>(
                    value
                );
        }
        else if (
            name == "m_IsReadable")
        {
            info.isReadable =
                value != 0;
        }
        else if (
            name == "offset" &&
            info.isStreamed)
        {
            info.streamOffset =
                static_cast<uint64_t>(
                    value
                );
        }
        else if (
            name == "size" &&
            info.isStreamed)
        {
            info.streamSize =
                static_cast<uint32_t>(
                    value
                );
        }
    }


    void CaptureString(
        const std::string& name,
        const std::string& value,
        const std::string& path
    )
    {
        if (name == "m_Name")
        {
            info.name =
                value;
        }
        else if (
            path.find(
                "m_StreamData"
            ) != std::string::npos &&
            name == "path")
        {
            info.streamPath =
                value;

            info.isStreamed =
                !value.empty();
        }
    }


    bool ReadPrimitive(
        Reader& r,
        const TypeTreeNode& node,
        const std::string& path
    )
    {
        const std::string& type =
            node.type;

        if (type == "bool")
        {
            const int64_t value =
                r.ReadU8();

            CaptureInt(
                node.name,
                value
            );
        }
        else if (
            type == "char" ||
            type == "SInt8")
        {
            const int64_t value =
                r.ReadI8();

            CaptureInt(
                node.name,
                value
            );
        }
        else if (
            type == "short" ||
            type == "SInt16")
        {
            const int64_t value =
                r.ReadI16();

            CaptureInt(
                node.name,
                value
            );
        }
        else if (
            type == "unsigned short" ||
            type == "UInt16")
        {
            const int64_t value =
                r.ReadU16();

            CaptureInt(
                node.name,
                value
            );
        }
        else if (
            type == "int" ||
            type == "SInt32")
        {
            const int64_t value =
                r.ReadI32();

            CaptureInt(
                node.name,
                value
            );
        }
        else if (
            type == "unsigned int" ||
            type == "UInt32")
        {
            const int64_t value =
                r.ReadU32();

            CaptureInt(
                node.name,
                value
            );
        }
        else if (
            type == "long long" ||
            type == "SInt64")
        {
            const int64_t value =
                r.ReadI64();

            CaptureInt(
                node.name,
                value
            );
        }
        else if (
            type == "unsigned long long" ||
            type == "UInt64")
        {
            const int64_t value =
                static_cast<int64_t>(
                    r.ReadU64()
                );

            CaptureInt(
                node.name,
                value
            );
        }
        else if (
            type == "float")
        {
            r.ReadFloat();
        }
        else if (
            type == "double")
        {
            r.ReadDouble();
        }
        else
        {
            return false;
        }

        if (node.AlignAfter())
            r.Align4();

        return true;
    }


    bool ReadString(
        Reader& r,
        const TypeTreeNode& node
    )
    {
        if (!r.CanRead(4))
            return false;

        const uint32_t length =
            r.ReadU32();

        if (length >
            r.Remaining())
        {
            return false;
        }

        std::string value;

        if (length > 0)
        {
            value.assign(
                reinterpret_cast<
                    const char*
                >(r.data + r.pos),
                length
            );
        }

        r.pos += length;

        if (!r.Align4())
            return false;

        CaptureString(
            node.name,
            value,
            node.name
        );

        return true;
    }


    bool ReadArray(
        Reader& r,
        const TypeTreeNode& node,
        const std::string& path
    )
    {
        if (!r.CanRead(4))
            return false;

        const int32_t count =
            r.ReadI32();

        if (count < 0 ||
            count > 100000000)
        {
            return false;
        }

        if (node.children.empty())
            return true;

        const TypeTreeNode& element =
            node.children[0];

        const std::string childPath =
            path + "/" + node.name;

        const bool imageData =
            childPath.find(
                "image data"
            ) != std::string::npos;

        const bool byteElement =
            element.type == "UInt8" ||
            element.type == "SInt8" ||
            element.type == "char" ||
            element.type == "unsigned char";


        if (imageData && byteElement)
        {
            const size_t bytes =
                static_cast<size_t>(
                    count
                );

            if (!r.CanRead(bytes))
                return false;

            info.imageDataOffset =
                r.pos;

            info.imageDataSize =
                count;

            r.pos += bytes;

            return r.Align4();
        }


        for (int32_t i = 0;
             i < count;
             ++i)
        {
            if (!ReadNode(
                    r,
                    element,
                    childPath))
            {
                return false;
            }
        }

        return true;
    }


    bool ReadNode(
        Reader& r,
        const TypeTreeNode& node,
        const std::string& parentPath
    )
    {
        if (r.pos > r.size)
            return false;

        const std::string path =
            parentPath.empty()
                ? node.name
                : parentPath +
                  "/" +
                  node.name;


        if (node.type == "string")
        {
            return ReadString(
                r,
                node
            );
        }


        if (node.IsArray())
        {
            return ReadArray(
                r,
                node,
                parentPath
            );
        }


        if (IsPrimitive(node.type))
        {
            return ReadPrimitive(
                r,
                node,
                path
            );
        }


        const bool streamData =
            node.name ==
            "m_StreamData";

        if (streamData)
            info.isStreamed = true;


        for (const auto& child :
             node.children)
        {
            if (!ReadNode(
                    r,
                    child,
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


/*
 * ================================================================
 * IMPLEMENTATION
 * ================================================================
 */

struct UnityTextureScanner::Impl
{
    UnityFSBundle bundle;

    std::vector<uint8_t>
        serializedData;

    std::vector<TextureInfo>
        textures;

    std::string inputPath;
};


/*
 * ================================================================
 * CONSTRUCTOR
 * ================================================================
 */

UnityTextureScanner::UnityTextureScanner()
    : m_Impl(
        new Impl()
    )
{
}


UnityTextureScanner::~UnityTextureScanner()
{
    delete m_Impl;

    m_Impl = nullptr;
}


/*
 * ================================================================
 * CLEAR
 * ================================================================
 */

void
UnityTextureScanner::Clear()
{
    if (!m_Impl)
        return;

    m_Impl->bundle =
        UnityFSBundle();

    m_Impl->serializedData.clear();

    m_Impl->textures.clear();

    m_Impl->inputPath.clear();
}


/*
 * ================================================================
 * SCAN
 * ================================================================
 */

bool
UnityTextureScanner::Scan(
    const std::string& input
)
{
    if (!m_Impl)
        return false;

    Clear();

    m_Impl->inputPath =
        input;


    /*
     * ------------------------------------------------------------
     * LOAD UNITYFS
     * ------------------------------------------------------------
     */

    if (!LoadFile(
            input,
            m_Impl->bundle.fileData))
    {
        return false;
    }


    /*
     * ------------------------------------------------------------
     * PARSE UNITYFS HEADER
     * ------------------------------------------------------------
     */

    if (!m_Impl->bundle.ParseHeader())
        return false;


    /*
     * ------------------------------------------------------------
     * BLOCKS INFO
     * ------------------------------------------------------------
     */

    if (!m_Impl->bundle.ParseBlocksInfo())
        return false;


    /*
     * ------------------------------------------------------------
     * DECOMPRESS CONTENT
     * ------------------------------------------------------------
     */

    if (!m_Impl->bundle.DecompressContent())
        return false;


    /*
     * ------------------------------------------------------------
     * FIND SERIALIZED FILE
     *
     * Prefer the main serialized file instead of .resS.
     * ------------------------------------------------------------
     */

    const UnityFSDirectoryEntry*
        selectedEntry = nullptr;


    for (const auto& entry :
         m_Impl->bundle.directory)
    {
        const std::string name =
            BaseName(
                entry.path
            );

        if (name.empty())
            continue;

        if (EndsWith(
                name,
                ".resS"))
        {
            continue;
        }

        if (EndsWith(
                name,
                ".resource"))
        {
            continue;
        }

        selectedEntry =
            &entry;

        break;
    }


    if (!selectedEntry)
        return false;


    /*
     * ------------------------------------------------------------
     * GET SERIALIZED DATA
     * ------------------------------------------------------------
     */

    const uint8_t* serializedPtr =
        nullptr;

    size_t serializedSize = 0;


    if (!m_Impl->bundle.GetEntryData(
            *selectedEntry,
            serializedPtr,
            serializedSize))
    {
        return false;
    }


    if (!serializedPtr ||
        serializedSize == 0)
    {
        return false;
    }


    /*
     * Keep serialized data alive because TextureInfo offsets
     * point into it.
     */
    m_Impl->serializedData.assign(
        serializedPtr,
        serializedPtr +
        serializedSize
    );


    /*
     * ------------------------------------------------------------
     * PARSE SERIALIZED FILE
     * ------------------------------------------------------------
     */

    SerializedFileParser parser;

    if (!parser.Parse(
            m_Impl->serializedData.data(),
            m_Impl->serializedData.size()))
    {
        return false;
    }


    /*
     * ------------------------------------------------------------
     * FIND TEXTURE2D
     * ------------------------------------------------------------
     */

    int textureIndex = 0;


    for (const auto& object :
         parser.objects)
    {
        /*
         * Texture2D class ID.
         */
        if (object.classID != 28)
            continue;


        if (object.typeID < 0 ||
            static_cast<size_t>(
                object.typeID
            ) >= parser.types.size())
        {
            continue;
        }


        const SerializedType& type =
            parser.types[
                static_cast<size_t>(
                    object.typeID
                )
            ];


        if (type.classID != 28)
            continue;


        TextureTreeReader reader;


        if (!reader.Parse(
                type,
                object,
                m_Impl->serializedData.data(),
                m_Impl->serializedData.size(),
                parser.littleEndian))
        {
            continue;
        }


        TextureInfo info =
            reader.info;


        /*
         * Basic validation.
         */
        if (info.width <= 0 ||
            info.height <= 0)
        {
            continue;
        }


        if (info.name.empty())
        {
            continue;
        }


        info.index =
            textureIndex++;


        /*
         * --------------------------------------------------------
         * RESOLVE STREAMED RESOURCE
         * --------------------------------------------------------
         */

        if (info.isStreamed &&
            !info.streamPath.empty())
        {
            const UnityFSDirectoryEntry*
                resource =
                    m_Impl->bundle
                        .FindResourceForPath(
                            info.streamPath
                        );


            if (resource)
            {
                uint64_t absoluteOffset =
                    resource->offset +
                    info.streamOffset;


                if (absoluteOffset <=
                    m_Impl->bundle.contentData.size())
                {
                    const uint64_t remaining =
                        static_cast<uint64_t>(
                            m_Impl->bundle.contentData.size()
                        ) -
                        absoluteOffset;


                    if (info.streamSize <=
                        remaining)
                    {
                        info.resolvedResourceOffset =
                            absoluteOffset;

                        info.resourceResolved =
                            true;
                    }
                }
            }
        }


        m_Impl->textures.push_back(
            std::move(info)
        );
    }


    return !m_Impl->textures.empty();
}


/*
 * ================================================================
 * GET TEXTURES
 * ================================================================
 */

const std::vector<TextureInfo>&
UnityTextureScanner::GetTextures() const
{
    static const std::vector<TextureInfo>
        empty;

    if (!m_Impl)
        return empty;

    return m_Impl->textures;
}


/*
 * ================================================================
 * GET TEXTURE
 * ================================================================
 */

const TextureInfo*
UnityTextureScanner::GetTexture(
    int index
) const
{
    if (!m_Impl)
        return nullptr;

    if (index < 0)
        return nullptr;

    const size_t i =
        static_cast<size_t>(
            index
        );

    if (i >=
        m_Impl->textures.size())
    {
        return nullptr;
    }

    return
        &m_Impl->textures[i];
}


/*
 * ================================================================
 * GET TEXTURE DATA POINTER
 * ================================================================
 */

bool
UnityTextureScanner::GetTextureDataPtr(
    int index,
    const uint8_t*& data,
    size_t& size
) const
{
    data = nullptr;
    size = 0;

    if (!m_Impl)
        return false;

    if (index < 0)
        return false;

    const size_t i =
        static_cast<size_t>(
            index
        );

    if (i >=
        m_Impl->textures.size())
    {
        return false;
    }

    const TextureInfo& info =
        m_Impl->textures[i];


    /*
     * ------------------------------------------------------------
     * STREAMED TEXTURE
     * ------------------------------------------------------------
     */

    if (info.isStreamed)
    {
        if (!info.resourceResolved)
            return false;

        const uint64_t offset =
            info.resolvedResourceOffset;

        const uint64_t total =
            m_Impl->bundle.contentData.size();

        if (offset > total)
            return false;

        if (info.streamSize >
            total - offset)
        {
            return false;
        }

        data =
            m_Impl->bundle.contentData.data() +
            static_cast<size_t>(
                offset
            );

        size =
            static_cast<size_t>(
                info.streamSize
            );

        return true;
    }


    /*
     * ------------------------------------------------------------
     * EMBEDDED TEXTURE
     * ------------------------------------------------------------
     */

    if (info.imageDataSize <= 0)
        return false;


    const uint64_t objectOffset =
        info.objectOffset;

    const uint64_t imageOffset =
        info.imageDataOffset;


    if (objectOffset >
        m_Impl->serializedData.size())
    {
        return false;
    }


    if (imageOffset >
        static_cast<uint64_t>(
            m_Impl->serializedData.size()
        ) -
        objectOffset)
    {
        return false;
    }


    const uint64_t absoluteOffset =
        objectOffset +
        imageOffset;


    if (absoluteOffset >
        m_Impl->serializedData.size())
    {
        return false;
    }


    const size_t available =
        m_Impl->serializedData.size() -
        static_cast<size_t>(
            absoluteOffset
        );


    if (static_cast<size_t>(
            info.imageDataSize
        ) > available)
    {
        return false;
    }


    data =
        m_Impl->serializedData.data() +
        static_cast<size_t>(
            absoluteOffset
        );

    size =
        static_cast<size_t>(
            info.imageDataSize
        );

    return true;
}


/*
 * ================================================================
 * INPUT PATH
 * ================================================================
 */

const std::string&
UnityTextureScanner::GetInputPath() const
{
    static const std::string empty;

    if (!m_Impl)
        return empty;

    return m_Impl->inputPath;
}