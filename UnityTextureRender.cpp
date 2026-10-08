#include "UnityTextureRender.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>


/*
 * ================================================================
 * ETC / ASTC CONSTANTS
 * ================================================================
 */

#ifndef GL_ETC1_RGB8_OES
#define GL_ETC1_RGB8_OES 0x8D64
#endif


#ifndef GL_COMPRESSED_RGB8_ETC2
#define GL_COMPRESSED_RGB8_ETC2 0x9274
#endif


#ifndef GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2
#define GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2 0x9276
#endif


#ifndef GL_COMPRESSED_RGBA8_ETC2_EAC
#define GL_COMPRESSED_RGBA8_ETC2_EAC 0x9278
#endif


#ifndef GL_COMPRESSED_RGBA_ASTC_4x4_KHR
#define GL_COMPRESSED_RGBA_ASTC_4x4_KHR 0x93B0
#endif


#ifndef GL_COMPRESSED_RGBA_ASTC_5x5_KHR
#define GL_COMPRESSED_RGBA_ASTC_5x5_KHR 0x93B2
#endif


#ifndef GL_COMPRESSED_RGBA_ASTC_6x6_KHR
#define GL_COMPRESSED_RGBA_ASTC_6x6_KHR 0x93B4
#endif


#ifndef GL_COMPRESSED_RGBA_ASTC_8x8_KHR
#define GL_COMPRESSED_RGBA_ASTC_8x8_KHR 0x93B7
#endif


#ifndef GL_COMPRESSED_RGBA_ASTC_10x10_KHR
#define GL_COMPRESSED_RGBA_ASTC_10x10_KHR 0x93BB
#endif


#ifndef GL_COMPRESSED_RGBA_ASTC_12x12_KHR
#define GL_COMPRESSED_RGBA_ASTC_12x12_KHR 0x93BD
#endif


/*
 * ================================================================
 * DESTRUCTOR
 * ================================================================
 *
 * Jangan otomatis Release() di destructor.
 *
 * Graphics/OpenGL context bisa saja sudah dihancurkan ketika
 * object ini dihancurkan.
 */
UnityTextureRender::~UnityTextureRender()
{
}


/*
 * ================================================================
 * MAKE HERO NAME
 * ================================================================
 */

std::string
UnityTextureRender::MakeHeroName(
    int heroId
)
{
    char buffer[64];

    std::snprintf(
        buffer,
        sizeof(buffer),
        "HeroHead%03d",
        heroId
    );

    return std::string(buffer);
}


/*
 * ================================================================
 * FIND TEXTURE BY NAME
 * ================================================================
 */

int
UnityTextureRender::FindByName(
    const UnityTextureScanner& scanner,
    const char* name
) const
{
    if (!name)
        return -1;

    const auto& textures =
        scanner.GetTextures();

    for (size_t i = 0;
         i < textures.size();
         ++i)
    {
        if (textures[i].name == name)
        {
            return static_cast<int>(i);
        }
    }

    return -1;
}


/*
 * ================================================================
 * BEGIN BATTLE
 * ================================================================
 */

void
UnityTextureRender::BeginBattle()
{
    /*
     * Sudah berada di battle.
     *
     * Jangan clear cache setiap frame.
     */
    if (m_BattleActive)
        return;


    /*
     * Bersihkan sisa cache dari battle sebelumnya.
     *
     * Pada saat fungsi ini dipanggil, OpenGL context masih aktif
     * karena dipanggil dari DrawEsp() di dalam frame.
     */
    Release();


    m_BattleActive = true;


    printf(
        "[UnityGL] Battle cache START\n"
    );
}


/*
 * ================================================================
 * END BATTLE
 * ================================================================
 */

void
UnityTextureRender::EndBattle()
{
    /*
     * Tidak sedang battle.
     */
    if (!m_BattleActive)
        return;


    /*
     * Hapus seluruh texture hero dari battle sebelumnya.
     *
     * glDeleteTextures() dilakukan ketika OpenGL context masih
     * aktif.
     */
    Release();


    m_BattleActive = false;


    printf(
        "[UnityGL] Battle cache END\n"
    );
}


/*
 * ================================================================
 * LOAD HERO BY ID
 * ================================================================
 */

bool
UnityTextureRender::LoadHeroById(
    const UnityTextureScanner& scanner,
    int heroId
)
{
    if (heroId <= 0)
        return false;


    /*
     * ============================================================
     * CACHE HIT
     * ============================================================
     *
     * Kalau texture sudah ada, jangan upload lagi.
     */
    auto cached =
        m_Textures.find(heroId);

    if (cached != m_Textures.end())
    {
        if (cached->second.valid &&
            cached->second.id != 0)
        {
            m_CurrentHeroId = heroId;

            return true;
        }
    }


    /*
     * ============================================================
     * MAX 5 TEXTURES
     * ============================================================
     *
     * Cache battle tidak boleh lebih dari 5 texture.
     *
     * Jangan otomatis menghapus texture lama di sini karena
     * hal tersebut bisa menyebabkan:
     *
     * upload -> delete -> upload -> delete
     *
     * apabila dictionary entity sementara berubah.
     */
    if (m_Textures.size() >= MAX_BATTLE_TEXTURES)
    {
        printf(
            "[UnityGL] Cache full: %zu/%zu\n",
            m_Textures.size(),
            MAX_BATTLE_TEXTURES
        );

        return false;
    }


    /*
     * ============================================================
     * HERO NAME
     * ============================================================
     */

    std::string heroName =
        MakeHeroName(heroId);


    /*
     * ============================================================
     * FIND SCANNER INDEX
     * ============================================================
     */

    int index =
        FindByName(
            scanner,
            heroName.c_str()
        );

    if (index < 0)
    {
        printf(
            "[UnityGL] Hero not found: %s\n",
            heroName.c_str()
        );

        return false;
    }


    /*
     * ============================================================
     * TEXTURE INFO
     * ============================================================
     */

    const TextureInfo* info =
        scanner.GetTexture(index);

    if (!info)
    {
        printf(
            "[UnityGL] Invalid TextureInfo: %s\n",
            heroName.c_str()
        );

        return false;
    }


    /*
     * ============================================================
     * GET RAW DATA POINTER
     * ============================================================
     */

    size_t dataSize = 0;

    const uint8_t* data = nullptr;


    if (!scanner.GetTextureDataPtr(
            index,
            data,
            dataSize))
    {
        printf(
            "[UnityGL] GetTextureDataPtr failed: %s\n",
            heroName.c_str()
        );

        return false;
    }


    if (!data || dataSize == 0)
    {
        printf(
            "[UnityGL] Empty texture data: %s\n",
            heroName.c_str()
        );

        return false;
    }


    /*
     * ============================================================
     * UPLOAD
     * ============================================================
     */

    UnityGLTexture texture;


    printf(
        "[UnityGL] Loading HeroID=%d %s %dx%d format=%d\n",
        heroId,
        heroName.c_str(),
        info->width,
        info->height,
        info->textureFormat
    );


    if (!UploadTexture(
            *info,
            data,
            dataSize,
            texture))
    {
        printf(
            "[UnityGL] Upload failed: %s\n",
            heroName.c_str()
        );

        ReleaseTexture(texture);

        return false;
    }


    /*
     * Simpan informasi.
     */
    texture.scannerIndex =
        static_cast<size_t>(index);

    texture.name =
        heroName;


    /*
     * Masukkan ke cache berdasarkan HeroID.
     */
    m_Textures[heroId] =
        std::move(texture);


    m_CurrentHeroId =
        heroId;


    printf(
        "[UnityGL] OK HeroID=%d -> GL=%u cache=%zu/%zu\n",
        heroId,
        m_Textures[heroId].id,
        m_Textures.size(),
        MAX_BATTLE_TEXTURES
    );


    return true;
}


/*
 * ================================================================
 * GENERIC LOAD
 * ================================================================
 *
 * Dipertahankan agar API lama tetap ada.
 *
 * Untuk battle cache gunakan LoadHeroById().
 */
bool
UnityTextureRender::Load(
    const UnityTextureScanner& scanner,
    size_t index
)
{
    const auto& textures =
        scanner.GetTextures();


    if (index >= textures.size())
        return false;


    const TextureInfo& info =
        textures[index];


    /*
     * Kalau nama texture adalah HeroHeadXXX,
     * coba gunakan HeroID sebagai key.
     */
    int heroId = -1;


    if (info.name.rfind(
            "HeroHead",
            0) == 0)
    {
        const char* p =
            info.name.c_str() + 8;

        char* end = nullptr;

        long value =
            std::strtol(
                p,
                &end,
                10
            );

        if (end != p &&
            value > 0 &&
            value <= 9999)
        {
            heroId =
                static_cast<int>(value);
        }
    }


    /*
     * Untuk HeroHeadXXX gunakan cache HeroID.
     */
    if (heroId > 0)
    {
        return LoadHeroById(
            scanner,
            heroId
        );
    }


    /*
     * Texture non-HeroHead tidak digunakan
     * oleh battle hero cache.
     */
    return false;
}


/*
 * ================================================================
 * UPLOAD TEXTURE
 * ================================================================
 */

bool
UnityTextureRender::UploadTexture(
    const TextureInfo& info,
    const uint8_t* data,
    size_t dataSize,
    UnityGLTexture& output
)
{
    if (!data ||
        dataSize == 0)
    {
        return false;
    }


    if (info.width <= 0 ||
        info.height <= 0)
    {
        return false;
    }


    /*
     * ============================================================
     * RAW
     * ============================================================
     */

    switch (info.textureFormat)
    {
        case 2:   // RGB24
        case 3:   // RGBA32
        case 4:   // ARGB32
        case 5:   // ARGB32
        case 14:  // BGRA32
        case 59:  // R8
        {
            return UploadRaw(
                info,
                data,
                dataSize,
                output
            );
        }


        /*
         * ========================================================
         * ETC / ASTC
         * ========================================================
         */

        case 34:  // ETC1 RGB
        case 41:  // ETC2 RGB
        case 42:  // ETC2 RGB punchthrough alpha1
        case 43:  // ETC2 RGBA8
        case 44:  // ASTC 4x4
        case 45:  // ASTC 5x5
        case 46:  // ASTC 6x6
        case 47:  // ASTC 8x8
        case 48:  // ASTC 10x10
        case 49:  // ASTC 12x12
        case 50:  // ASTC 4x4
        case 51:  // ASTC 5x5
        case 52:  // ASTC 6x6
        case 53:  // ASTC 8x8
        case 54:  // ASTC 10x10
        case 55:  // ASTC 12x12
        {
            return UploadCompressed(
                info,
                data,
                dataSize,
                output
            );
        }
    }


    printf(
        "[UnityGL] Unsupported format=%d\n",
        info.textureFormat
    );


    return false;
}


/*
 * ================================================================
 * UPLOAD RAW
 * ================================================================
 */

bool
UnityTextureRender::UploadRaw(
    const TextureInfo& info,
    const uint8_t* data,
    size_t dataSize,
    UnityGLTexture& output
)
{
    const int width =
        info.width;

    const int height =
        info.height;


    GLenum format =
        GL_RGBA;


    GLenum internalFormat =
        GL_RGBA;


    size_t requiredSize = 0;


    std::vector<uint8_t> converted;


    const uint8_t* uploadData =
        data;


    /*
     * ============================================================
     * RGB24
     * ============================================================
     */

    if (info.textureFormat == 2)
    {
        requiredSize =
            static_cast<size_t>(width) *
            static_cast<size_t>(height) *
            3;


        if (dataSize < requiredSize)
            return false;


        format =
            GL_RGB;

        internalFormat =
            GL_RGB;
    }


    /*
     * ============================================================
     * RGBA32
     * ============================================================
     */

    else if (info.textureFormat == 3)
    {
        requiredSize =
            static_cast<size_t>(width) *
            static_cast<size_t>(height) *
            4;


        if (dataSize < requiredSize)
            return false;


        format =
            GL_RGBA;

        internalFormat =
            GL_RGBA;
    }


    /*
     * ============================================================
     * ARGB32
     *
     * Unity:
     *
     * A R G B
     *
     * OpenGL:
     *
     * R G B A
     * ============================================================
     */

    else if (
        info.textureFormat == 4 ||
        info.textureFormat == 5)
    {
        requiredSize =
            static_cast<size_t>(width) *
            static_cast<size_t>(height) *
            4;


        if (dataSize < requiredSize)
            return false;


        converted.resize(
            requiredSize
        );


        for (size_t i = 0;
             i < requiredSize;
             i += 4)
        {
            converted[i + 0] =
                data[i + 1];

            converted[i + 1] =
                data[i + 2];

            converted[i + 2] =
                data[i + 3];

            converted[i + 3] =
                data[i + 0];
        }


        uploadData =
            converted.data();


        format =
            GL_RGBA;

        internalFormat =
            GL_RGBA;
    }


    /*
     * ============================================================
     * BGRA32
     * ============================================================
     */

    else if (info.textureFormat == 14)
    {
        requiredSize =
            static_cast<size_t>(width) *
            static_cast<size_t>(height) *
            4;


        if (dataSize < requiredSize)
            return false;


        converted.resize(
            requiredSize
        );


        for (size_t i = 0;
             i < requiredSize;
             i += 4)
        {
            converted[i + 0] =
                data[i + 2];

            converted[i + 1] =
                data[i + 1];

            converted[i + 2] =
                data[i + 0];

            converted[i + 3] =
                data[i + 3];
        }


        uploadData =
            converted.data();


        format =
            GL_RGBA;

        internalFormat =
            GL_RGBA;
    }


    /*
     * ============================================================
     * R8
     * ============================================================
     */

    else if (info.textureFormat == 59)
    {
        requiredSize =
            static_cast<size_t>(width) *
            static_cast<size_t>(height);


        if (dataSize < requiredSize)
            return false;


        format =
            GL_LUMINANCE;

        internalFormat =
            GL_LUMINANCE;
    }


    else
    {
        return false;
    }


    /*
     * ============================================================
     * CREATE GL TEXTURE
     * ============================================================
     */

    GLuint textureId = 0;


    glGenTextures(
        1,
        &textureId
    );


    if (textureId == 0)
    {
        PrintGLError(
            "glGenTextures"
        );

        return false;
    }


    glBindTexture(
        GL_TEXTURE_2D,
        textureId
    );


    SetupTextureParameters();


    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        internalFormat,
        width,
        height,
        0,
        format,
        GL_UNSIGNED_BYTE,
        uploadData
    );


    GLenum error =
        glGetError();


    glBindTexture(
        GL_TEXTURE_2D,
        0
    );


    if (error != GL_NO_ERROR)
    {
        printf(
            "[UnityGL] glTexImage2D error=0x%X\n",
            error
        );


        glDeleteTextures(
            1,
            &textureId
        );


        return false;
    }


    output.id =
        textureId;

    output.width =
        width;

    output.height =
        height;

    output.textureFormat =
        info.textureFormat;

    output.valid =
        true;


    return true;
}


/*
 * ================================================================
 * UPLOAD COMPRESSED
 * ================================================================
 */

bool
UnityTextureRender::UploadCompressed(
    const TextureInfo& info,
    const uint8_t* data,
    size_t dataSize,
    UnityGLTexture& output
)
{
    const int width =
        info.width;

    const int height =
        info.height;


    GLenum glFormat =
        0;


    int blockWidth =
        4;

    int blockHeight =
        4;

    int bytesPerBlock =
        8;


    /*
     * ============================================================
     * ETC1
     * ============================================================
     */

    if (info.textureFormat == 34)
    {
        if (!SupportsETC1())
        {
            printf(
                "[UnityGL] ETC1 unsupported\n"
            );

            return false;
        }


        glFormat =
            GL_ETC1_RGB8_OES;


        blockWidth =
            4;

        blockHeight =
            4;

        bytesPerBlock =
            8;
    }


    /*
     * ============================================================
     * ETC2 RGB
     * ============================================================
     */

    else if (info.textureFormat == 41)
    {
        if (!SupportsETC2())
        {
            printf(
                "[UnityGL] ETC2 unsupported\n"
            );

            return false;
        }


        glFormat =
            GL_COMPRESSED_RGB8_ETC2;


        blockWidth =
            4;

        blockHeight =
            4;

        bytesPerBlock =
            8;
    }


    /*
     * ============================================================
     * ETC2 PUNCHTHROUGH
     * ============================================================
     */

    else if (info.textureFormat == 42)
    {
        if (!SupportsETC2())
        {
            printf(
                "[UnityGL] ETC2 unsupported\n"
            );

            return false;
        }


        glFormat =
            GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2;


        blockWidth =
            4;

        blockHeight =
            4;

        bytesPerBlock =
            8;
    }


    /*
     * ============================================================
     * ETC2 RGBA8
     * ============================================================
     */

    else if (info.textureFormat == 43)
    {
        if (!SupportsETC2())
        {
            printf(
                "[UnityGL] ETC2 unsupported\n"
            );

            return false;
        }


        glFormat =
            GL_COMPRESSED_RGBA8_ETC2_EAC;


        blockWidth =
            4;

        blockHeight =
            4;

        bytesPerBlock =
            16;
    }


    /*
     * ============================================================
     * ASTC
     * ============================================================
     */

    else
    {
        if (!SupportsASTC())
        {
            printf(
                "[UnityGL] ASTC unsupported\n"
            );

            return false;
        }


        switch (info.textureFormat)
        {
            case 44:
            case 50:
                glFormat =
                    GL_COMPRESSED_RGBA_ASTC_4x4_KHR;

                blockWidth = 4;
                blockHeight = 4;
                break;


            case 45:
            case 51:
                glFormat =
                    GL_COMPRESSED_RGBA_ASTC_5x5_KHR;

                blockWidth = 5;
                blockHeight = 5;
                break;


            case 46:
            case 52:
                glFormat =
                    GL_COMPRESSED_RGBA_ASTC_6x6_KHR;

                blockWidth = 6;
                blockHeight = 6;
                break;


            case 47:
            case 53:
                glFormat =
                    GL_COMPRESSED_RGBA_ASTC_8x8_KHR;

                blockWidth = 8;
                blockHeight = 8;
                break;


            case 48:
            case 54:
                glFormat =
                    GL_COMPRESSED_RGBA_ASTC_10x10_KHR;

                blockWidth = 10;
                blockHeight = 10;
                break;


            case 49:
            case 55:
                glFormat =
                    GL_COMPRESSED_RGBA_ASTC_12x12_KHR;

                blockWidth = 12;
                blockHeight = 12;
                break;


            default:
                return false;
        }


        bytesPerBlock =
            16;
    }


    /*
     * ============================================================
     * CALCULATE BLOCKS
     * ============================================================
     */

    size_t blocksX =
        (static_cast<size_t>(width) +
         blockWidth - 1) /
        blockWidth;


    size_t blocksY =
        (static_cast<size_t>(height) +
         blockHeight - 1) /
        blockHeight;


    size_t expectedSize =
        blocksX *
        blocksY *
        static_cast<size_t>(
            bytesPerBlock
        );


    if (dataSize < expectedSize)
    {
        printf(
            "[UnityGL] Compressed data too small: "
            "got=%zu expected=%zu\n",
            dataSize,
            expectedSize
        );

        return false;
    }


    /*
     * ============================================================
     * CREATE GL TEXTURE
     * ============================================================
     */

    GLuint textureId = 0;


    glGenTextures(
        1,
        &textureId
    );


    if (textureId == 0)
    {
        PrintGLError(
            "glGenTextures"
        );

        return false;
    }


    glBindTexture(
        GL_TEXTURE_2D,
        textureId
    );


    SetupTextureParameters();


    /*
     * Hanya upload mip 0.
     */
    glCompressedTexImage2D(
        GL_TEXTURE_2D,
        0,
        glFormat,
        width,
        height,
        0,
        static_cast<GLsizei>(
            expectedSize
        ),
        data
    );


    GLenum error =
        glGetError();


    glBindTexture(
        GL_TEXTURE_2D,
        0
    );


    if (error != GL_NO_ERROR)
    {
        printf(
            "[UnityGL] "
            "glCompressedTexImage2D error=0x%X\n",
            error
        );


        glDeleteTextures(
            1,
            &textureId
        );


        return false;
    }


    output.id =
        textureId;

    output.width =
        width;

    output.height =
        height;

    output.textureFormat =
        info.textureFormat;

    output.valid =
        true;


    return true;
}


/*
 * ================================================================
 * GL TEXTURE PARAMETERS
 * ================================================================
 */

void
UnityTextureRender::SetupTextureParameters()
{
    glTexParameteri(
        GL_TEXTURE_2D,
        GL_TEXTURE_MIN_FILTER,
        GL_LINEAR
    );


    glTexParameteri(
        GL_TEXTURE_2D,
        GL_TEXTURE_MAG_FILTER,
        GL_LINEAR
    );


    glTexParameteri(
        GL_TEXTURE_2D,
        GL_TEXTURE_WRAP_S,
        GL_CLAMP_TO_EDGE
    );


    glTexParameteri(
        GL_TEXTURE_2D,
        GL_TEXTURE_WRAP_T,
        GL_CLAMP_TO_EDGE
    );
}


/*
 * ================================================================
 * ETC1 SUPPORT
 * ================================================================
 */

bool
UnityTextureRender::SupportsETC1() const
{
    const char* version =
        reinterpret_cast<const char*>(
            glGetString(GL_VERSION)
        );


    if (version &&
        std::strstr(
            version,
            "OpenGL ES 3"
        ))
    {
        return true;
    }


    const char* extensions =
        reinterpret_cast<const char*>(
            glGetString(GL_EXTENSIONS)
        );


    if (!extensions)
        return false;


    return
        std::strstr(
            extensions,
            "GL_OES_compressed_ETC1_RGB8_texture"
        ) != nullptr;
}


/*
 * ================================================================
 * ETC2 SUPPORT
 * ================================================================
 */

bool
UnityTextureRender::SupportsETC2() const
{
    const char* version =
        reinterpret_cast<const char*>(
            glGetString(GL_VERSION)
        );


    if (version &&
        std::strstr(
            version,
            "OpenGL ES 3"
        ))
    {
        return true;
    }


    const char* extensions =
        reinterpret_cast<const char*>(
            glGetString(GL_EXTENSIONS)
        );


    if (!extensions)
        return false;


    return
        std::strstr(
            extensions,
            "GL_OES_compressed_ETC2_RGB8_texture"
        ) != nullptr;
}


/*
 * ================================================================
 * ASTC SUPPORT
 * ================================================================
 */

bool
UnityTextureRender::SupportsASTC() const
{
    const char* extensions =
        reinterpret_cast<const char*>(
            glGetString(GL_EXTENSIONS)
        );


    if (!extensions)
        return false;


    return
        std::strstr(
            extensions,
            "GL_KHR_texture_compression_astc_ldr"
        ) != nullptr;
}


/*
 * ================================================================
 * GET HERO TEXTURE ID
 * ================================================================
 */

GLuint
UnityTextureRender::GetHeroTextureId(
    int heroId
) const
{
    auto it =
        m_Textures.find(heroId);


    if (it == m_Textures.end())
        return 0;


    if (!it->second.valid)
        return 0;


    if (it->second.id == 0)
        return 0;


    return it->second.id;
}


/*
 * ================================================================
 * GET HERO TEXTURE
 * ================================================================
 */

ImTextureID
UnityTextureRender::GetHeroTexture(
    int heroId
) const
{
    GLuint id =
        GetHeroTextureId(heroId);


    if (id == 0)
        return (ImTextureID)0;


    /*
     * ImGui versi kamu menggunakan ImTextureID
     * yang kompatibel dengan integer.
     */
    return (ImTextureID)(intptr_t)id;
}


/*
 * ================================================================
 * HAS HERO TEXTURE
 * ================================================================
 */

bool
UnityTextureRender::HasHeroTexture(
    int heroId
) const
{
    return
        GetHeroTextureId(heroId) != 0;
}


/*
 * ================================================================
 * CURRENT TEXTURE
 * ================================================================
 */

const UnityGLTexture*
UnityTextureRender::GetCurrentTexture() const
{
    if (m_CurrentHeroId <= 0)
        return nullptr;


    auto it =
        m_Textures.find(
            m_CurrentHeroId
        );


    if (it == m_Textures.end())
        return nullptr;


    return &it->second;
}


/*
 * ================================================================
 * RELEASE ONE TEXTURE
 * ================================================================
 */

void
UnityTextureRender::ReleaseTexture(
    UnityGLTexture& texture
)
{
    if (texture.id != 0)
    {
        glDeleteTextures(
            1,
            &texture.id
        );
    }


    texture.id = 0;

    texture.valid = false;

    texture.width = 0;

    texture.height = 0;

    texture.textureFormat = -1;

    texture.scannerIndex =
        static_cast<size_t>(-1);

    texture.name.clear();
}


/*
 * ================================================================
 * RELEASE HERO
 * ================================================================
 */

void
UnityTextureRender::ReleaseHero(
    int heroId
)
{
    auto it =
        m_Textures.find(heroId);


    if (it == m_Textures.end())
        return;


    ReleaseTexture(
        it->second
    );


    m_Textures.erase(it);


    if (m_CurrentHeroId == heroId)
    {
        m_CurrentHeroId = -1;
    }
}


/*
 * ================================================================
 * RELEASE ALL
 * ================================================================
 */

void
UnityTextureRender::Release()
{
    for (auto& pair :
         m_Textures)
    {
        ReleaseTexture(
            pair.second
        );
    }


    m_Textures.clear();


    m_CurrentHeroId = -1;
}


/*
 * ================================================================
 * CLEAR
 * ================================================================
 */

void
UnityTextureRender::Clear()
{
    Release();
}


/*
 * ================================================================
 * OLD KEEP ONLY HEROES API
 * ================================================================
 *
 * Masih tersedia untuk kompatibilitas.
 *
 * Untuk sistem baru, DrawEsp() TIDAK memanggil ini setiap frame.
 */
void
UnityTextureRender::KeepOnlyHeroes(
    const std::vector<int>& activeHeroIds
)
{
    for (auto it =
             m_Textures.begin();
         it != m_Textures.end();)
    {
        bool keep = false;


        for (int id :
             activeHeroIds)
        {
            if (id == it->first)
            {
                keep = true;
                break;
            }
        }


        if (!keep)
        {
            ReleaseTexture(
                it->second
            );

            if (m_CurrentHeroId ==
                it->first)
            {
                m_CurrentHeroId = -1;
            }


            it =
                m_Textures.erase(it);
        }
        else
        {
            ++it;
        }
    }
}


/*
 * ================================================================
 * KEEP ONLY FIVE HEROES
 * ================================================================
 */

void
UnityTextureRender::KeepOnlyHeroes(
    int hero1,
    int hero2,
    int hero3,
    int hero4,
    int hero5
)
{
    std::vector<int> ids;

    ids.reserve(5);


    if (hero1 > 0)
        ids.push_back(hero1);

    if (hero2 > 0)
        ids.push_back(hero2);

    if (hero3 > 0)
        ids.push_back(hero3);

    if (hero4 > 0)
        ids.push_back(hero4);

    if (hero5 > 0)
        ids.push_back(hero5);


    KeepOnlyHeroes(ids);
}


/*
 * ================================================================
 * IMGUI IMAGE
 * ================================================================
 */

void
UnityTextureRender::Image(
    ImVec2 size
)
{
    if (m_CurrentHeroId <= 0)
        return;


    ImTextureID texture =
        GetHeroTexture(
            m_CurrentHeroId
        );


    if (texture == (ImTextureID)0)
        return;


    /*
     * UV tetap dibalik vertikal karena texture Unity/OpenGL
     * yang sekarang sudah terbukti benar dengan mapping ini.
     */
    ImGui::Image(
        texture,
        size,
        ImVec2(
            0.0f,
            1.0f
        ),
        ImVec2(
            1.0f,
            0.0f
        )
    );
}


/*
 * ================================================================
 * GL ERROR
 * ================================================================
 */

void
UnityTextureRender::PrintGLError(
    const char* where
)
{
    GLenum error =
        glGetError();


    if (error != GL_NO_ERROR)
    {
        printf(
            "[UnityGL] GL error at %s: 0x%X\n",
            where ? where : "?",
            error
        );
    }
}