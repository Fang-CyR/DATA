#pragma once

#include <GLES2/gl2.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "UnityTextureScanner.h"


struct UnityGLTexture
{
    GLuint id = 0;

    int width = 0;
    int height = 0;

    int textureFormat = -1;

    bool valid = false;

    size_t scannerIndex =
        static_cast<size_t>(-1);

    std::string name;
};


class UnityTextureRender
{
public:

    UnityTextureRender() = default;

    ~UnityTextureRender();


    /*
     * ============================================================
     * BATTLE CACHE
     * ============================================================
     */

    void BeginBattle();

    void EndBattle();

    bool IsBattleActive() const
    {
        return m_BattleActive;
    }


    /*
     * ============================================================
     * TEXTURE LOAD
     * ============================================================
     */

    bool LoadHeroById(
        const UnityTextureScanner& scanner,
        int heroId
    );

    /*
     * Tetap dipertahankan untuk kompatibilitas.
     *
     * Untuk sistem battle cache baru,
     * gunakan LoadHeroById().
     */
    bool Load(
        const UnityTextureScanner& scanner,
        size_t index
    );


    /*
     * ============================================================
     * OLD CACHE API
     * ============================================================
     *
     * Tetap tersedia jika masih dipakai bagian lain.
     */
    void KeepOnlyHeroes(
        const std::vector<int>& activeHeroIds
    );

    void KeepOnlyHeroes(
        int hero1,
        int hero2,
        int hero3,
        int hero4,
        int hero5
    );


    void ReleaseHero(int heroId);

    void Release();

    void Clear();


    /*
     * ============================================================
     * GET TEXTURE
     * ============================================================
     */

    ImTextureID GetHeroTexture(
        int heroId
    ) const;

    GLuint GetHeroTextureId(
        int heroId
    ) const;

    bool HasHeroTexture(
        int heroId
    ) const;


    /*
     * ============================================================
     * IMGUI
     * ============================================================
     */

    void Image(
        ImVec2 size
    );


    /*
     * ============================================================
     * FIND
     * ============================================================
     */

    int FindByName(
        const UnityTextureScanner& scanner,
        const char* name
    ) const;


    static std::string MakeHeroName(
        int heroId
    );


    /*
     * ============================================================
     * INFO
     * ============================================================
     */

    size_t GetCacheSize() const
    {
        return m_Textures.size();
    }


    int GetCurrentHeroId() const
    {
        return m_CurrentHeroId;
    }


    const UnityGLTexture*
    GetCurrentTexture() const;


private:

    bool UploadTexture(
        const TextureInfo& info,
        const uint8_t* data,
        size_t dataSize,
        UnityGLTexture& output
    );


    bool UploadRaw(
        const TextureInfo& info,
        const uint8_t* data,
        size_t dataSize,
        UnityGLTexture& output
    );


    bool UploadCompressed(
        const TextureInfo& info,
        const uint8_t* data,
        size_t dataSize,
        UnityGLTexture& output
    );


    bool SupportsETC1() const;

    bool SupportsETC2() const;

    bool SupportsASTC() const;


    void SetupTextureParameters();

    void ReleaseTexture(
        UnityGLTexture& texture
    );


    void PrintGLError(
        const char* where
    );


private:

    /*
     * Key = HeroID
     *
     * Contoh:
     *
     * 75  -> HeroHead075
     * 81  -> HeroHead081
     * 92  -> HeroHead092
     */
    std::unordered_map<
        int,
        UnityGLTexture
    > m_Textures;


    int m_CurrentHeroId = -1;


    /*
     * Menandakan apakah sedang berada di battle.
     */
    bool m_BattleActive = false;


    /*
     * Maksimal texture hero yang boleh berada
     * di cache dalam satu battle.
     */
    static constexpr size_t MAX_BATTLE_TEXTURES = 5;
};