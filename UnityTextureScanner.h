#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

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

    /*
        Offset image data di dalam Texture2D object.
    */
    uint64_t imageDataOffset = 0;

    bool isReadable = false;
    bool isStreamed = false;

    uint64_t streamOffset = 0;
    uint32_t streamSize = 0;

    std::string streamPath;

    /*
        Offset absolut terhadap bundle.contentData.
    */
    uint64_t resolvedResourceOffset = 0;
    bool resourceResolved = false;
};

class UnityTextureScanner
{
public:
    UnityTextureScanner();
    ~UnityTextureScanner();

    /*
        Parse UnityFS + SerializedFile + Texture2D.
    */
    bool Scan(const std::string& input);

    /*
        Scan file tanpa output verbose.
    */
    bool ScanSilent(const std::string& input);

    /*
        Hapus hasil scan.
    */
    void Clear();

    /*
        Daftar Texture2D yang ditemukan.
    */
    const std::vector<TextureInfo>& GetTextures() const;

    /*
        Ambil metadata texture.
    */
    const TextureInfo* GetTexture(int index) const;

    /*
        Ambil raw bytes Texture2D.

        Untuk embedded:
            mengambil image data dari SerializedFile.

        Untuk streamed:
            mengambil bytes dari .resS.
    */
    bool GetTextureData(
        int index,
        std::vector<uint8_t>& output) const;

    /*
        Versi pointer tanpa copy.

        Pointer hanya valid selama scanner
        belum di-Clear / Scan ulang.
    */
    bool GetTextureDataPtr(
        int index,
        const uint8_t*& data,
        size_t& size) const;

    /*
        Path bundle terakhir.
    */
    const std::string& GetInputPath() const;

private:
    struct Impl;
    Impl* m_Impl;
};