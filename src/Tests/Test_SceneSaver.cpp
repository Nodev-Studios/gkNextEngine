#include <catch2/catch_all.hpp>

#include "TestCommon.hpp"
#include "Engine/Assets/Core/Node.hpp"
#include "Engine/Assets/Core/Scene.hpp"
#include "Engine/Assets/Data/Material.hpp"
#include "Engine/Runtime/Components/EnvironmentComponent.hpp"
#include "Engine/Runtime/Components/LightComponent.hpp"
#include "Engine/Runtime/Components/SceneReferenceComponent.hpp"
#include "Engine/Utilities/FileHelper.hpp"
#include "Modules/SceneExport/FSceneSaver.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

// A scene saved from the editor has to come back as the same scene: its area lights still lights,
// its scene references still references, and nothing a reference brought in baked into the host.

namespace
{
    constexpr const char* kSceneSaverTestDir = "assets/test_scene_saver";

    // A 2x2 RGBA PNG. A referenced asset with a real texture is what used to leak into the host:
    // the saver exported every material in the scene, the proxies' included.
    constexpr std::array<uint8_t, 74> kTinyPng = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d,
        0x24, 0x00, 0x00, 0x00, 0x11, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x38, 0xa1, 0xa1, 0xf1,
        0x1f, 0x84, 0x19, 0x60, 0x0c, 0x00, 0x4a, 0x04, 0x08, 0x5d, 0x1c, 0x87, 0x47, 0x44, 0x00, 0x00,
        0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };

    /// Builds a .glb in memory: one binary buffer, the JSON written by the caller around the
    /// accessors and views this appends.
    class FGlbBuilder
    {
    public:
        nlohmann::json Json = nlohmann::json::object();

        FGlbBuilder()
        {
            Json["asset"] = {{"version", "2.0"}};
            Json["bufferViews"] = nlohmann::json::array();
            Json["accessors"] = nlohmann::json::array();
        }

        /// A unit quad in the XZ plane facing -Y, the way a ceiling light hangs. Returns the
        /// primitive, ready to take a material.
        nlohmann::json AddQuad()
        {
            const std::array<float, 12> positions = {
                -0.5f, 0.0f, -0.5f, 0.5f, 0.0f, -0.5f, 0.5f, 0.0f, 0.5f, -0.5f, 0.0f, 0.5f,
            };
            const std::array<float, 12> normals = {
                0.0f, -1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, -1.0f, 0.0f,
            };
            const std::array<float, 8> texCoords = {0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
            const std::array<uint16_t, 6> indices = {0, 2, 1, 0, 3, 2};

            const int position = AddAccessor(positions.data(), sizeof(positions), 5126, 4, "VEC3", 34962);
            Json["accessors"][position]["min"] = {-0.5, 0.0, -0.5};
            Json["accessors"][position]["max"] = {0.5, 0.0, 0.5};
            const int normal = AddAccessor(normals.data(), sizeof(normals), 5126, 4, "VEC3", 34962);
            const int texCoord = AddAccessor(texCoords.data(), sizeof(texCoords), 5126, 4, "VEC2", 34962);
            const int index = AddAccessor(indices.data(), sizeof(indices), 5123, 6, "SCALAR", 34963);
            return {
                {"attributes", {{"POSITION", position}, {"NORMAL", normal}, {"TEXCOORD_0", texCoord}}},
                {"indices", index},
                {"mode", 4},
            };
        }

        int AddPngImage()
        {
            const int view = AddView(kTinyPng.data(), kTinyPng.size(), 0);
            Json["images"].push_back({{"bufferView", view}, {"mimeType", "image/png"}});
            return static_cast<int>(Json["images"].size() - 1);
        }

        void Write(const std::filesystem::path& path)
        {
            Json["buffers"] = nlohmann::json::array({{{"byteLength", bin_.size()}}});
            std::string jsonText = Json.dump();
            jsonText.append((4 - jsonText.size() % 4) % 4, ' ');
            std::vector<uint8_t> bin = bin_;
            bin.resize((bin.size() + 3) & ~size_t(3), 0);

            std::filesystem::create_directories(path.parent_path());
            std::ofstream out(path, std::ios::binary);
            REQUIRE(out.good());
            const auto writeU32 = [&out](uint32_t value) { out.write(reinterpret_cast<const char*>(&value), 4); };
            writeU32(0x46546C67u);
            writeU32(2);
            writeU32(static_cast<uint32_t>(12 + 8 + jsonText.size() + 8 + bin.size()));
            writeU32(static_cast<uint32_t>(jsonText.size()));
            writeU32(0x4E4F534Au);
            out.write(jsonText.data(), static_cast<std::streamsize>(jsonText.size()));
            writeU32(static_cast<uint32_t>(bin.size()));
            writeU32(0x004E4942u);
            out.write(reinterpret_cast<const char*>(bin.data()), static_cast<std::streamsize>(bin.size()));
        }

    private:
        int AddView(const void* data, size_t size, int target)
        {
            bin_.resize((bin_.size() + 3) & ~size_t(3), 0);
            const size_t offset = bin_.size();
            bin_.resize(offset + size);
            std::memcpy(bin_.data() + offset, data, size);
            nlohmann::json view = {{"buffer", 0}, {"byteOffset", offset}, {"byteLength", size}};
            if (target != 0)
            {
                view["target"] = target;
            }
            Json["bufferViews"].push_back(view);
            return static_cast<int>(Json["bufferViews"].size() - 1);
        }

        int AddAccessor(const void* data, size_t size, int componentType, int count, const char* type, int target)
        {
            const int view = AddView(data, size, target);
            Json["accessors"].push_back(
                {{"bufferView", view}, {"componentType", componentType}, {"count", count}, {"type", type}});
            return static_cast<int>(Json["accessors"].size() - 1);
        }

        std::vector<uint8_t> bin_;
    };

    std::filesystem::path PlatformPath(const std::string& relativePath)
    {
        return Utilities::FileHelper::GetPlatformFilePath(relativePath.c_str());
    }

    void WriteLeafAsset(const std::string& relativePath)
    {
        FGlbBuilder glb;
        nlohmann::json primitive = glb.AddQuad();
        primitive["material"] = 0;
        const int image = glb.AddPngImage();
        glb.Json["textures"] = nlohmann::json::array({{{"source", image}}});
        glb.Json["materials"] = nlohmann::json::array({{
            {"name", "LeafTextured"},
            {"pbrMetallicRoughness", {{"baseColorTexture", {{"index", 0}}}, {"metallicFactor", 0.0}}},
        }});
        glb.Json["meshes"] = nlohmann::json::array({{{"name", "LeafQuad"}, {"primitives", {primitive}}}});
        glb.Json["nodes"] = nlohmann::json::array({{{"name", "LeafQuad"}, {"mesh", 0}}});
        glb.Json["scenes"] = nlohmann::json::array({{{"nodes", {0}}}});
        glb.Json["scene"] = 0;
        glb.Write(PlatformPath(relativePath));
    }

    void WriteHostScene(const std::string& relativePath, const std::string& leafPath)
    {
        FGlbBuilder glb;
        nlohmann::json lightPrimitive = glb.AddQuad();
        lightPrimitive["material"] = 0;
        nlohmann::json floorPrimitive = glb.AddQuad();
        floorPrimitive["material"] = 1;
        glb.Json["materials"] = nlohmann::json::array({
            // Explicit metallic/roughness: FSceneLoader applies its Metallic (metallic > 0.95) and
            // Lambertian (roughness > 0.95) rules after DiffuseLight, so glTF's defaults (1, 1)
            // would turn this emissive material into a metal before the saver ever sees it.
            {{"name", "HostLight"},
             {"emissiveFactor", {1.0, 0.9, 0.8}},
             {"pbrMetallicRoughness", {{"metallicFactor", 0.0}, {"roughnessFactor", 0.5}}}},
            {{"name", "HostFloor"},
             {"pbrMetallicRoughness", {{"baseColorFactor", {0.5, 0.5, 0.5, 1.0}}, {"metallicFactor", 0.0}}}},
        });
        glb.Json["meshes"] = nlohmann::json::array({
            {{"name", "LightQuad"}, {"primitives", {lightPrimitive}}},
            {{"name", "FloorQuad"}, {"primitives", {floorPrimitive}}},
        });
        glb.Json["nodes"] = nlohmann::json::array({
            {{"name", "Light"}, {"mesh", 0}, {"translation", {0.0, 3.0, 0.0}},
             {"extras", {{"arealight", true}, {"tag", "lamp"}}}},
            {{"name", "Floor"}, {"mesh", 1}, {"scale", {10.0, 1.0, 10.0}}},
            {{"name", "Prop"}, {"translation", {1.0, 0.0, 0.0}},
             {"extras", {{"gkSceneReference", {{"version", 1}, {"asset", leafPath}}}}}},
            {{"name", "Environment"}, {"extras", {{"gkEnvironment", {{"HasSun", false}, {"HasSky", false}}}}}},
        });
        glb.Json["scenes"] = nlohmann::json::array({{{"nodes", {0, 1, 2, 3}}}});
        glb.Json["scene"] = 0;
        glb.Write(PlatformPath(relativePath));
    }

    nlohmann::json ReadGlbJson(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        REQUIRE(in.good());
        std::array<uint32_t, 5> header{};
        in.read(reinterpret_cast<char*>(header.data()), sizeof(header));
        REQUIRE(header[0] == 0x46546C67u);
        REQUIRE(header[4] == 0x4E4F534Au);
        std::string jsonText(header[3], '\0');
        in.read(jsonText.data(), static_cast<std::streamsize>(jsonText.size()));
        return nlohmann::json::parse(jsonText);
    }

    void LoadSceneAndWait(NextEngine& engine, const std::string& path)
    {
        engine.RequestLoadScene({.filename = path});
        for (int frame = 0; frame < 600 && !engine.IsSceneReady(); ++frame)
        {
            engine.Tick(true);
        }
        REQUIRE(engine.IsSceneReady());
    }

    uint32_t LightCount(const Assets::Scene& scene, const std::string& nodeName)
    {
        const Assets::Node* node = scene.GetNode(nodeName);
        REQUIRE(node != nullptr);
        const auto* light = node->GetComponent<Runtime::LightComponent>();
        return light != nullptr ? light->GetLightCount() : 0;
    }

    Runtime::ESceneReferenceStatus ReferenceStatus(const Assets::Scene& scene, const std::string& nodeName)
    {
        const Assets::Node* node = scene.GetNode(nodeName);
        REQUIRE(node != nullptr);
        const auto* reference = node->GetComponent<Runtime::SceneReferenceComponent>();
        REQUIRE(reference != nullptr);
        return reference->GetStatus();
    }

    const Runtime::EnvironmentComponent* FindEnvironment(const Assets::Scene& scene)
    {
        for (const auto& node : scene.Nodes())
        {
            if (const auto* environment = node->GetComponent<Runtime::EnvironmentComponent>())
            {
                return environment;
            }
        }
        return nullptr;
    }
}

TEST_CASE_METHOD(EngineTestFixture, "Saved scenes keep area lights and leave scene reference materials out",
                 "[GPU][Integration][SceneExport][SceneReference]")
{
    const std::string leafPath = std::string(kSceneSaverTestDir) + "/textured_leaf.glb";
    const std::string hostPath = std::string(kSceneSaverTestDir) + "/host.glb";
    const std::string savedPath = std::string(kSceneSaverTestDir) + "/saved.glb";
    WriteLeafAsset(leafPath);
    WriteHostScene(hostPath, leafPath);

    // What the editor sets: without it the scene frees its CPU geometry after the GPU upload and
    // there is nothing left to save.
    struct FKeepCpuMeshData
    {
        bool previous = GOption->KeepCPUMeshData;
        FKeepCpuMeshData() { GOption->KeepCPUMeshData = true; }
        ~FKeepCpuMeshData() { GOption->KeepCPUMeshData = previous; }
    } keepCpuMeshData;

    LoadSceneAndWait(*engine_, hostPath);
    {
        const Assets::Scene& scene = engine_->GetScene();
        REQUIRE(LightCount(scene, "Light") == 1);
        REQUIRE(ReferenceStatus(scene, "Prop") == Runtime::ESceneReferenceStatus::Loaded);
        // The proxy's material is in the live scene; that is what the saver must not copy.
        const auto& materials = scene.Materials();
        REQUIRE(std::ranges::any_of(materials, [](const Assets::FMaterial& material)
        {
            return material.name_ == "LeafTextured";
        }));

        REQUIRE(Assets::FSceneSaver::SaveGLBScene(PlatformPath(savedPath).string(), scene));
    }

    const nlohmann::json saved = ReadGlbJson(PlatformPath(savedPath));
    std::set<std::string> materialNames;
    for (const auto& material : saved.value("materials", nlohmann::json::array()))
    {
        materialNames.insert(material.value("name", std::string()));
    }
    CHECK(materialNames == std::set<std::string>{"HostLight", "HostFloor"});
    CHECK(saved.value("images", nlohmann::json::array()).empty());
    CHECK(saved.value("textures", nlohmann::json::array()).empty());

    LoadSceneAndWait(*engine_, savedPath);
    const Assets::Scene& reloaded = engine_->GetScene();
    CHECK(LightCount(reloaded, "Light") == 1);
    CHECK(ReferenceStatus(reloaded, "Prop") == Runtime::ESceneReferenceStatus::Loaded);
    const Assets::Node* light = reloaded.GetNode("Light");
    REQUIRE(light != nullptr);
    CHECK(light->GetTag() == "lamp");
    const Runtime::EnvironmentComponent* environment = FindEnvironment(reloaded);
    REQUIRE(environment != nullptr);
    CHECK_FALSE(environment->HasSun);
    CHECK_FALSE(environment->HasSky);
}
