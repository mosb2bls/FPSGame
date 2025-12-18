#include "Core.h"
#include "Window.h"
#include "Timer.h"
#include "Maths.h"
#include "Shaders.h"
#include "Mesh.h"
#include "PSO.h"
#include "SkyDome.h"
#include "HeightmapTerrain.h"
#include "Gun.h"
#include "HybridGrassField.h"
#include "Rocks.h"
#include "AssetManager.h"
#include "Animation.h"
#include "modelState.h"
#include "RandomGenerator.h"  // Include the vegetation generator
#include <algorithm>
#include <Windows.h>

#define WIDTH  1920
#define HEIGHT 1080

static float deg2rad(float d) { return d * 3.1415926535f / 180.0f; }
static float clampf(float v, float a, float b) { return std::max(a, std::min(b, v)); }

// ============================================================================
// HELPER: Convert VegetationItems to GrassInstances
// ============================================================================
std::vector<GrassInstance> convertToGrassInstances(
    const std::vector<VegetationItem>& items,
    int numGroups,
    int numTypesPerGroup)
{
    std::vector<GrassInstance> instances;
    instances.reserve(items.size());

    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> randPhase(0.0f, 6.28318f);

    for (const auto& item : items)
    {
        GrassInstance inst;
        inst.position = item.position;
        inst.rotationY = item.rotationY;
        inst.scale = item.scale;
        inst.windPhase = randPhase(rng);  // Random wind phase

        // Map typeIndex to group and type within group
        // Simple mapping: distribute types across groups
        if (numGroups > 0 && numTypesPerGroup > 0)
        {
            inst.groupIndex = item.typeIndex % numGroups;
            inst.typeIndex = (item.typeIndex / numGroups) % numTypesPerGroup;
        }
        else
        {
            inst.groupIndex = 0;
            inst.typeIndex = 0;
        }

        instances.push_back(inst);
    }

    return instances;
}

// ============================================================================
// HELPER: Convert VegetationItems to RockInstances
// ============================================================================
std::vector<RockInstance> convertToRockInstances(const std::vector<VegetationItem>& items)
{
    std::vector<RockInstance> instances;
    instances.reserve(items.size());

    for (const auto& item : items)
    {
        RockInstance inst;
        inst.position = item.position;
        inst.rotationY = item.rotationY;
        inst.scale = item.scale;
        inst.typeIndex = item.typeIndex;
        inst.distanceToCamera = 0.0f;
        inst.lodLevel = 2;  // Start at lowest LOD
        instances.push_back(inst);
    }

    return instances;
}

// ============================================================================
// MAIN
// ============================================================================
int WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR lpCmdLine, int nCmdShow)
{
    Window window;
    window.create(WIDTH, HEIGHT, "the game");

    Core core;
    core.init(window.hwnd, WIDTH, HEIGHT);

    Shaders shaders;
    PSOManager psos;

    // ====================================================================
    // LOAD GRASS & ROCK ASSETS FROM CONFIG FILE
    // ====================================================================
    AssetManager assets;
    if (!assets.loadFromConfig(&core, "assets.cfg"))
    {
        std::string msg = "Failed to load assets.cfg";
        OutputDebugStringA(msg.c_str());
        MessageBoxA(nullptr, msg.c_str(), "Asset Load Error", MB_OK);
    }

    // ====================================================================
    // INITIALIZE SKY
    // ====================================================================
    SkyDome sky;
    sky.init(&core, &psos, &shaders, 5000.0f);

    // ====================================================================
    // INITIALIZE TERRAIN
    // ====================================================================
    HeightmapTerrain terrain;

    // Store terrain size for vegetation generator
    float terrainSizeX = 300.0f;
    float terrainSizeZ = 300.0f;

    bool terrainOK = terrain.init(
        &core, &psos, &shaders,
        "Assets/Heightmap/map2.png",
        512, 512,
        terrainSizeX, terrainSizeZ,
        40.0f, 0.0f,
        HeightmapTerrain::Format::PNG16
    );

    if (!terrainOK)
    {
        std::string msg = "Map not open";
        OutputDebugStringA(msg.c_str());
        MessageBoxA(nullptr, msg.c_str(), "Terrain Load Error", MB_OK);
        return 0;
    }

    // ====================================================================
    // ====================================================================
    //                 VEGETATION GENERATION SYSTEM
    // ====================================================================
    // ====================================================================

    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "   VEGETATION GENERATION SYSTEM\n";
    std::cout << "========================================\n\n";

    // ------------------------------------------------------------------------
    // STEP 1: Create the vegetation generator
    // ------------------------------------------------------------------------
    VegetationGenerator vegGen;

    // ------------------------------------------------------------------------
    // STEP 2: Configure the generation
    // ------------------------------------------------------------------------
    // 
    // OPTION A: Use a preset
    // -----------------------
    // VegetationConfig vegConfig = VegetationPresets::Meadow();  // Lush grass
    // VegetationConfig vegConfig = VegetationPresets::Rocky();   // Many rocks
    // VegetationConfig vegConfig = VegetationPresets::Forest();  // Mixed
    // VegetationConfig vegConfig = VegetationPresets::Desert();  // Sparse

    // OPTION B: Custom configuration
    // ------------------------------
    VegetationConfig vegConfig;

    // General distribution
    vegConfig.density = 1.0f;              // Items per square meter
   
    vegConfig.minPointSpacing = 1.5f;       // Minimum meters between points

    // Rock vs Grass balance
    vegConfig.rockProbability = 0.12f;      // 12% rocks, 88% grass
    vegConfig.noiseInfluence = 0.5f;        // Strong biome effect
    vegConfig.noiseScale = 0.018f;          // Medium-sized biomes

    // Grass settings
    vegConfig.grassMinScale = 0.7f;
    vegConfig.grassMaxScale = 1.4f;
    vegConfig.grassRadius = 0.2f;           // Small collision radius

    vegConfig.grassCluster.probability = 0.7f;   // 70% spawn as clusters
    vegConfig.grassCluster.minItems = 6;
    vegConfig.grassCluster.maxItems = 18;
    vegConfig.grassCluster.radius = 4.0f;
    vegConfig.grassCluster.falloff = 1.2f;       // Gradual falloff

    // Rock settings
    vegConfig.rockMinScale = 0.4f;
    vegConfig.rockMaxScale = 2.8f;
    vegConfig.rockRadius = 1.2f;            // Larger collision radius

    vegConfig.rockCluster.probability = 0.45f;   // 45% spawn as clusters
    vegConfig.rockCluster.minItems = 2;
    vegConfig.rockCluster.maxItems = 7;
    vegConfig.rockCluster.radius = 6.0f;
    vegConfig.rockCluster.falloff = 2.5f;        // Sharp falloff

    // Terrain constraints
    vegConfig.maxSlope = 40.0f;             // No vegetation on steep cliffs

    // ------------------------------------------------------------------------
    // STEP 3: Generate vegetation!
    // ------------------------------------------------------------------------
    // Use seed = 0 for random each time, or fixed seed for reproducible results
    unsigned int seed = 42;  // Fixed seed for consistent results

    std::cout << "[VegetationGenerator] Configuration:\n";
    std::cout << "  Total points: " << vegConfig.density << "\n";
    std::cout << "  Rock probability: " << (vegConfig.rockProbability * 100) << "%\n";
    std::cout << "  Grass cluster prob: " << (vegConfig.grassCluster.probability * 100) << "%\n";
    std::cout << "  Rock cluster prob: " << (vegConfig.rockCluster.probability * 100) << "%\n";
    std::cout << "  Seed: " << seed << "\n\n";

    vegGen.generate(&terrain, vegConfig, terrainSizeX, terrainSizeZ, seed);

    // ------------------------------------------------------------------------
    // STEP 4: Get the generated items
    // ------------------------------------------------------------------------
    const auto& generatedRocks = vegGen.getRockItems();
    const auto& generatedGrass = vegGen.getGrassItems();

    std::cout << "\n[Game] Vegetation generation complete!\n";
    std::cout << "  Rocks generated: " << generatedRocks.size() << "\n";
    std::cout << "  Grass generated: " << generatedGrass.size() << "\n\n";

    // ====================================================================
    // INITIALIZE ROCKS (Using VegetationGenerator output)
    // ====================================================================
    Rocks rocks;
    bool hasRocks = false;

    auto& rockSets = assets.getRockSets();
    if (!rockSets.empty() && !generatedRocks.empty())
    {
        auto& rockSet = rockSets[0];

        // Convert VegetationItems to RockInstances
        std::vector<RockInstance> rockInstances = convertToRockInstances(generatedRocks);

        // Initialize with pre-generated instances
        rocks.terrainSizeX = terrainSizeX;
        rocks.terrainSizeZ = terrainSizeZ;
        rocks.initWithInstances(&core, &psos, &shaders, &terrain,
            rockSet.modelPaths,
            rockSet.texturePaths,
            rockInstances,
            100.0f,     // View distance
            32.0f       // Chunk size
        );

        rocks.rockColor = Vec4(0.75f, 0.72f, 0.68f, 1.0f);  // Warm gray
        rocks.lodDistanceHigh = 25.0f;
        rocks.lodDistanceMedium = 60.0f;

        hasRocks = true;
        std::cout << "[Game] Rocks initialized: " << rockInstances.size() << " instances\n";
    }
    else
    {
        std::cout << "[Game] No rocks to initialize\n";
    }

    // ====================================================================
    // INITIALIZE GRASS (Using VegetationGenerator output)
    // ====================================================================
    HybridGrassField grassField;
    bool hasGrass = false;

    auto grassConfigs = assets.getGrassGroupConfigs();
    if (!grassConfigs.empty() && !generatedGrass.empty())
    {
        // Count groups and types for mapping
        int numGroups = (int)grassConfigs.size();
        int avgTypesPerGroup = 0;
        for (const auto& group : grassConfigs)
        {
            avgTypesPerGroup += (int)group.types.size();
        }
        avgTypesPerGroup = numGroups > 0 ? avgTypesPerGroup / numGroups : 1;

        // Convert VegetationItems to GrassInstances
        std::vector<GrassInstance> grassInstances = convertToGrassInstances(
            generatedGrass, numGroups, avgTypesPerGroup);

        // Initialize with pre-generated instances
        grassField.terrainSizeX = terrainSizeX;
        grassField.terrainSizeZ = terrainSizeZ;
        grassField.initWithInstances(&core, &psos, &shaders, &terrain,
            grassConfigs,
            grassInstances,
            50.0f,      // View distance
            16.0f       // Chunk size
        );

        // Customize colors
        grassField.colorTop = Vec4(0.55f, 0.95f, 0.45f, 1.0f);     // Bright green tips
        grassField.colorBottom = Vec4(0.25f, 0.55f, 0.22f, 1.0f);  // Darker base

        // Customize wind
        grassField.windDirection = Vec2(1.0f, 0.3f);
        grassField.windStrength = 1.3f;

        hasGrass = true;
        std::cout << "[Game] Grass initialized: " << grassInstances.size() << " instances\n";
    }
    else
    {
        std::cout << "[Game] No grass to initialize\n";
    }

    std::cout << "\n========================================\n";
    std::cout << "   VEGETATION SETUP COMPLETE\n";
    std::cout << "========================================\n\n";

    // ====================================================================
    // INITIALIZE GUN
    // ====================================================================
    Gun gunModel;
    gunModel.load(&core,
        "Assets/Models/AutomaticCarbine.gem",
        "Assets/Models/Textures/gun.png",
        &psos, &shaders);

    AnimationInstance gunAnim;
    gunAnim.init(&gunModel.animation, 0);

    // ====================================================================
    // FPS CAMERA STATE
    // ====================================================================
    Vec3 camPos(0.0f, 1.7f, -3.0f);
    const float eyeHeight = 1.7f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    const float moveSpeed = 4.0f;
    const float mouseSens = 0.0025f;
    const float pitchLimit = 1.45f;

    ShowCursor(FALSE);
    window.useMouseClip = true;

    modelState modelState;
    modelState.idleClip = "04 idle";
    modelState.walkClip = "07 walk";
    modelState.fireClip = "08 fire";
    modelState.reloadClip = "17 reload";
    modelState.shotsPerSecond = 12.0f;
    modelState.fireAnimRate = 3.0f;

    auto getCenterScreen = [&]() {
        RECT rc{};
        GetClientRect(window.hwnd, &rc);
        POINT c{ (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
        ClientToScreen(window.hwnd, &c);
        return c;
        };

    POINT center = getCenterScreen();
    SetCursorPos(center.x, center.y);

    // Viewmodel placement
    float gunX = 0.08f;
    float gunY = 0.0f;
    float gunZ = 0.0f;
    Vec3 gunScale(0.01f, 0.01f, 0.01f);
    const float PI = 3.141592654f;
    float modelRotX = 0.0f;
    float modelRotY = +PI * 1.01f;
    float modelRotZ = 0.0f;

    Timer timer;

    std::cout << "========================================\n";
    std::cout << "   GAME RUNNING - Press ESC to exit\n";
    std::cout << "========================================\n\n";

    // ====================================================================
    // MAIN GAME LOOP
    // ====================================================================
    while (1)
    {
        core.beginFrame();

        float dt = timer.dt();
        dt = std::min(dt, 0.05f);

        window.checkInput();
        if (window.keys[VK_ESCAPE]) break;

        center = getCenterScreen();

        // Mouse look
        POINT cur{};
        GetCursorPos(&cur);
        float dx = float(cur.x - center.x);
        float dy = float(cur.y - center.y);
        SetCursorPos(center.x, center.y);

        yaw += dx * mouseSens;
        pitch -= dy * mouseSens;
        pitch = clampf(pitch, -pitchLimit, +pitchLimit);

        // Forward vector
        Vec3 forward(
            sinf(yaw) * cosf(pitch),
            sinf(pitch),
            cosf(yaw) * cosf(pitch)
        );
        forward = forward.normalize();

        // Movement (flat, no vertical)
        Vec3 forwardFlat(forward.x, 0.0f, forward.z);
        if (forwardFlat.length() > 0.0001f)
            forwardFlat = forwardFlat.normalize();

        Vec3 worldUp(0, 1, 0);
        Vec3 rightFlat = Cross(worldUp, forwardFlat).normalize();

        // Sample terrain height
        float groundY = terrain.sampleHeightWorld(camPos.x, camPos.z);

        // Update systems
        if (hasGrass)
            grassField.update(dt);

        if (hasRocks)
            rocks.update(camPos);

        // Camera movement
        if (window.keys['W']) camPos = camPos + forwardFlat * (moveSpeed * dt);
        if (window.keys['S']) camPos = camPos - forwardFlat * (moveSpeed * dt);
        if (window.keys['A']) camPos = camPos - rightFlat * (moveSpeed * dt);
        if (window.keys['D']) camPos = camPos + rightFlat * (moveSpeed * dt);

        // Camera follows terrain height
        camPos.y = groundY + eyeHeight + 5.0f;

        // World matrices
        float aspect = (float)WIDTH / (float)HEIGHT;
        Matrix pWorld = Matrix::perspective(0.01f, 10000.0f, aspect, 60.0f);
        Matrix vWorld = Matrix::lookAt(camPos, camPos + forward, worldUp);
        Matrix vpWorld = vWorld * pWorld;

        core.beginRenderPass();

        // Draw sky
        sky.draw(&core, &psos, &shaders, vpWorld, camPos);

        // Draw terrain
        Matrix terrainW;
        terrain.draw(&core, &psos, &shaders, vpWorld, terrainW);

        // Draw rocks
        if (hasRocks)
            rocks.draw(&core, &psos, &shaders, vpWorld, camPos);

        // Draw grass
        if (hasGrass)
            grassField.draw(&core, &psos, &shaders, vpWorld, camPos);

        // Gun animation update
        modelState.update(window, gunAnim, dt);
        modelState.getGunOffset(gunX, gunY, gunZ, modelRotY);

        // Draw gun (viewmodel)
        Matrix pGun = Matrix::perspective(0.001f, 1000.0f, aspect, 60.0f);
        Matrix vpGun = pGun;
        Matrix S = Matrix::scaling(gunScale);
        Matrix R = Matrix::rotateZ(modelRotZ) * Matrix::rotateY(modelRotY) * Matrix::rotateX(modelRotX);
        Matrix T = Matrix::translation(Vec3(gunX, gunY, gunZ));
        Matrix Wgun = S * R * T;
        gunModel.draw(&core, &psos, &shaders, &gunAnim, vpGun, Wgun);

        core.finishFrame();
    }

    core.flushGraphicsQueue();
    return 0;
}