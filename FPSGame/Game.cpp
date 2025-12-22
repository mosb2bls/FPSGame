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
#include "Fog.h"
#include "LakeBottom.h"
#include "Lake.h"

#include "RandomGenerator.h"  // Include the vegetation generator
#include <algorithm>
#include <Windows.h>

#define _CRT_SECURE_NO_WARNINGS

#define WIDTH  1920
#define HEIGHT 1080

static float deg2rad(float d) { return d * 3.1415926535f / 180.0f; }
static float clampf(float v, float a, float b) { return std::max(a, std::min(b, v)); }

// ============================================================================
// === LAKE === Global lake pointer for collision and grass filtering
// ============================================================================
Lake* g_lake = nullptr;

// ============================================================================
// HELPER: Convert VegetationItems to GrassInstances
// === MODIFIED === Now filters out grass inside the lake
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

    int skippedCount = 0;

    for (const auto& item : items)
    {
        // === LAKE === Skip grass that's inside the lake
        if (g_lake != nullptr)
        {
            // Add a small margin around the lake (2 meters) to keep grass away from shore
            float margin = 2.0f;
            float dx = item.position.x - g_lake->config.center.x;
            float dz = item.position.z - g_lake->config.center.z;
            float distSq = dx * dx + dz * dz;
            float radiusWithMargin = g_lake->config.radius + margin;

            if (distSq < radiusWithMargin * radiusWithMargin)
            {
                skippedCount++;
                continue;  // Skip this grass instance
            }
        }

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

    if (skippedCount > 0)
    {
        std::cout << "[Grass] Skipped " << skippedCount << " grass instances inside lake area\n";
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

    const float spawnExclusionRadius = 5.0f;  // No rocks within 5m of spawn
    int skippedCount = 0;

    for (const auto& item : items)
    {
        // Skip rocks near spawn point (0, 0)
        float distSq = item.position.x * item.position.x + item.position.z * item.position.z;
        if (distSq < spawnExclusionRadius * spawnExclusionRadius)
        {
            skippedCount++;
            continue;
        }

        RockInstance inst;
        inst.position = item.position;
        inst.rotationY = item.rotationY;
        inst.scale = item.scale;
        inst.typeIndex = item.typeIndex;
        inst.distanceToCamera = 0.0f;
        inst.lodLevel = 2;  // Start at lowest LOD
        instances.push_back(inst);
    }

    if (skippedCount > 0)
    {
        std::cout << "[Rocks] Skipped " << skippedCount << " rocks near spawn point\n";
    }

    return instances;
}

// ============================================================================
// === LAKE === Scene render callback for reflection pass
// ============================================================================
struct SceneRenderData
{
    Core* core;
    PSOManager* psos;
    Shaders* shaders;
    SkyDome* sky;
    HeightmapTerrain* terrain;
    Rocks* rocks;
    HybridGrassField* grass;
    Vec3 cameraPos;
    bool hasRocks;
    bool hasGrass;
};

void RenderSceneForReflection(void* userData, const Matrix& view, const Matrix& proj)
{
    SceneRenderData* data = (SceneRenderData*)userData;

    // Calculate VP matrix
    Matrix viewCopy = view;
    Matrix projCopy = proj;
    Matrix vp = viewCopy * projCopy;
    Matrix terrainW;  // Identity

    // Restore Core's state for rendering
    data->core->setDefaultDescriptorHeaps();
    data->core->getCommandList()->SetGraphicsRootSignature(data->core->rootSignature);

    // Render sky (reflected)
    data->sky->draw(data->core, data->psos, data->shaders, vp, data->cameraPos);

    // Render terrain (reflected)
    data->terrain->draw(data->core, data->psos, data->shaders, vp, terrainW);

    // Render rocks (reflected) - optional, can skip for performance
    if (data->hasRocks)
        data->rocks->draw(data->core, data->psos, data->shaders, vp, data->cameraPos);
}

// ============================================================================
// === COLLISION === Check collision with rocks
// ============================================================================
bool checkRockCollision(const Vec3& position, const std::vector<RockInstance>& rockInstances, float playerRadius)
{
    for (const auto& rock : rockInstances)
    {
        // Calculate collision radius based on rock scale
        float rockRadius = rock.scale * 1.5f;  // Adjust multiplier as needed

        float dx = position.x - rock.position.x;
        float dz = position.z - rock.position.z;
        float distSq = dx * dx + dz * dz;

        float minDist = playerRadius + rockRadius;

        if (distSq < minDist * minDist)
        {
            return true;  // Collision detected
        }
    }
    return false;
}

// ============================================================================
// === COLLISION === Check collision with lake (water)
// ============================================================================
bool checkLakeCollision(const Vec3& position, const Lake& lake, float playerRadius)
{
    float dx = position.x - lake.config.center.x;
    float dz = position.z - lake.config.center.z;
    float distSq = dx * dx + dz * dz;

    // Allow player to get close to edge but not into water
    float waterEdge = lake.config.radius - playerRadius - 0.5f;  // 0.5m safety margin

    if (distSq < waterEdge * waterEdge)
    {
        return true;  // Would be in water
    }
    return false;
}


// ============================================================================
// MAIN
// ============================================================================
int WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR lpCmdLine, int nCmdShow)
{

    AllocConsole();
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);


    Window window;
    window.create(WIDTH, HEIGHT, "the game");

    Core core;
    core.init(window.hwnd, WIDTH, HEIGHT);

    Shaders shaders;
    PSOManager psos;

    // Initialize Lake
    Lake lake;
    g_lake = &lake;  // Set global pointer for grass filtering

    LakeBottom lakeBottom;

    // ====================================================================
    // === FOG === Initialize Volumetric Fog System
    // ====================================================================
    VolumetricFog fog;
    fog.init(&core, WIDTH, HEIGHT);

    // Configure fog appearance
    fog.config.density = 0.02f;              // Fog density (0.01 - 0.05 typical)
    fog.config.heightFalloff = 0.06f;        // How quickly fog thins with height
    fog.config.groundLevel = 0.0f;           // Y level of thickest fog
    fog.config.maxHeight = 60.0f;            // Fog disappears above this height

    fog.config.fogColor = Vec3(0.65f, 0.75f, 0.88f);   // Bluish fog
    fog.config.sunColor = Vec3(1.0f, 0.95f, 0.85f);   // Warm sunlight
    fog.config.ambientColor = Vec3(0.4f, 0.5f, 0.6f); // Ambient fog tint

    fog.config.sunDirection = Vec3(0.4f, 0.7f, -0.5f); // Direction TO sun
    fog.config.scattering = 0.6f;            // Light scattering intensity
    fog.config.mieG = 0.75f;                 // Forward scattering bias

    fog.config.raymarchSteps = 24;           // Quality (16-48, higher = better but slower)
    fog.config.maxDistance = 150.0f;         // Maximum fog distance

    fog.config.windSpeed = 0.4f;             // Fog drift speed
    fog.config.windDirection = Vec2(1.0f, 0.2f);  // Wind direction

    fog.enabled = true;  // Set to false to disable fog

    std::cout << "[Game] Fog system initialized\n";

    fog.config.groundLevel = -5.0f;
    // ====================================================================


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
    // === LAKE === Initialize Lake System (after terrain, BEFORE vegetation)
    // ====================================================================
    // === FIX #2: Position lake inside the world ===
    // Terrain is 300x300, so center is at (150, 150)
    // Place lake somewhere interesting - adjust these values as needed
    lake.config.center = Vec3(30.0f, 0.0f, 40.0f);    // Near spawn point (0,0)
    lake.config.radius = 25.0f;

    // Set water level ABOVE terrain so it covers the ground
    float lakeCenterHeight = terrain.sampleHeightWorld(lake.config.center.x, lake.config.center.z);
    lake.config.waterLevel = lakeCenterHeight + 0.1f;  // Above terrain to cover it

    std::cout << "[Lake] Water level: " << lake.config.waterLevel << "\n";

    // === FIX #1: Change water color to BLUE ===
    lake.config.shallowColor = Vec3(0.0f, 0.2f, 0.5f);    // Deep blue shallow
    lake.config.deepColor = Vec3(0.0f, 0.05f, 0.15f);     // Very dark blue deep
    lake.config.transparency = 0.85f;                      // More opaque
    lake.config.reflectionStrength = 0.6f;
    lake.config.fresnelBias = 0.02f;

    // Wave settings - calmer waves for a lake
    lake.config.waveSpeed = 0.6f;
    lake.config.waveScale = 0.3f;          // Smaller waves

    // Reflection settings
    lake.config.reflectionStrength = 0.8f;
    lake.config.reflectionDistortion = 0.02f;

    // Sun settings (match fog/scene lighting)
    lake.config.sunDirection = Vec3(0.4f, 0.7f, -0.5f);
    lake.config.sunColor = Vec3(1.0f, 0.95f, 0.8f);
    lake.config.specularPower = 256.0f;
    lake.config.specularIntensity = 1.5f;

    // Mesh quality
    lake.config.radialSegments = 64;
    lake.config.ringSegments = 32;

    // Initialize the lake
    lake.init(&core, &shaders, &psos, WIDTH, HEIGHT);

    std::cout << "[Game] Lake initialized at (" << lake.config.center.x << ", "
        << lake.config.waterLevel << ", " << lake.config.center.z
        << ") with radius " << lake.config.radius << "\n";
    // ====================================================================


	// lake bottom initialization
    lakeBottom.init(&core, &shaders, &psos,
        "Assets/Lake/ground.jpg",      // Your texture path
        lake.config.center,               // Same center as lake
        lake.config.radius,               // Same radius
        lake.config.waterLevel,           // Water surface level
        8.0f);
    

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
    std::vector<RockInstance> rockInstances;  // Keep for collision detection

    auto& rockSets = assets.getRockSets();
    if (!rockSets.empty() && !generatedRocks.empty())
    {
        auto& rockSet = rockSets[0];

        // Convert VegetationItems to RockInstances
        rockInstances = convertToRockInstances(generatedRocks);

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
    // === FIX #4: Grass filtering happens in convertToGrassInstances ===
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
        // === FIX #4: This now filters out grass inside the lake ===
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
        grassField.windStrength = 0.0f;

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
    Vec3 camPos(0.0f, 1.7f, 0.0f);
    const float eyeHeight = 1.7f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    const float moveSpeed = 4.0f;
    const float mouseSens = 0.0025f;
    const float pitchLimit = 1.45f;

    // === FIX #3: Player collision radius ===
    const float playerRadius = 0.5f;  // Player collision radius in meters

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
    float totalTime = 0.0f;

    // ====================================================================
    // === LAKE === Prepare scene render data for reflection callback
    // ====================================================================
    SceneRenderData sceneData;
    sceneData.core = &core;
    sceneData.psos = &psos;
    sceneData.shaders = &shaders;
    sceneData.sky = &sky;
    sceneData.terrain = &terrain;
    sceneData.rocks = &rocks;
    sceneData.grass = &grassField;
    sceneData.hasRocks = hasRocks;
    sceneData.hasGrass = hasGrass;
    // ====================================================================

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

        //fog
        // === FOG === Toggle and adjust fog with F-keys
        static bool togglePressed = false;

        if (window.keys['T'] && !togglePressed) {  // Use 'T' for Toggle
            fog.enabled = !fog.enabled;
        }
        togglePressed = window.keys['T'];

        if (window.keys['G'] && !togglePressed) {  // 'G' = more density
            fog.config.density = std::min(fog.config.density + 0.005f, 0.1f);
        }

        if (window.keys['H'] && !togglePressed) {  // 'H' = less density
            fog.config.density = std::max(fog.config.density - 0.005f, 0.001f);
        }
        // ====================================================================

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

        // ====================================================================
        // === FIX #3: Camera movement with collision detection ===
        // ====================================================================
        Vec3 newPos = camPos;

        // Calculate desired movement
        if (window.keys['W']) newPos = newPos + forwardFlat * (moveSpeed * dt);
        if (window.keys['S']) newPos = newPos - forwardFlat * (moveSpeed * dt);
        if (window.keys['A']) newPos = newPos - rightFlat * (moveSpeed * dt);
        if (window.keys['D']) newPos = newPos + rightFlat * (moveSpeed * dt);

        // Check collisions before applying movement
        bool canMove = true;

        // Check lake collision
        if (checkLakeCollision(newPos, lake, playerRadius))
        {
            canMove = false;
        }

        // Check rock collision
        if (canMove && hasRocks && checkRockCollision(newPos, rockInstances, playerRadius))
        {
            canMove = false;
        }

        float halfX = terrainSizeX * 0.5f;
        float halfZ = terrainSizeZ * 0.5f;
        if (newPos.x < -halfX + 1.0f) newPos.x = -halfX + 1.0f;
        if (newPos.x > halfX - 1.0f) newPos.x = halfX - 1.0f;
        if (newPos.z < -halfZ + 1.0f) newPos.z = -halfZ + 1.0f;
        if (newPos.z > halfZ - 1.0f) newPos.z = halfZ - 1.0f;

        // Apply movement only if no collision
        if (canMove)
        {
            camPos = newPos;
        }
        else
        {
            // Try sliding along obstacles (separate X and Z movement)
            Vec3 newPosX = camPos;
            Vec3 newPosZ = camPos;

            if (window.keys['W'] || window.keys['S'])
            {
                Vec3 moveDir = window.keys['W'] ? forwardFlat : (forwardFlat * -1.0f);
                newPosX.x = camPos.x + moveDir.x * moveSpeed * dt;
                newPosZ.z = camPos.z + moveDir.z * moveSpeed * dt;
            }
            if (window.keys['A'] || window.keys['D'])
            {
                Vec3 moveDir = window.keys['D'] ? rightFlat : (rightFlat * -1.0f);
                newPosX.x = camPos.x + moveDir.x * moveSpeed * dt;
                newPosZ.z = camPos.z + moveDir.z * moveSpeed * dt;
            }

            // Try X movement only
            bool canMoveX = true;
            if (checkLakeCollision(newPosX, lake, playerRadius)) canMoveX = false;
            if (canMoveX && hasRocks && checkRockCollision(newPosX, rockInstances, playerRadius)) canMoveX = false;
            if (newPosX.x < 1.0f || newPosX.x > terrainSizeX - 1.0f) canMoveX = false;

            // Try Z movement only
            bool canMoveZ = true;
            if (checkLakeCollision(newPosZ, lake, playerRadius)) canMoveZ = false;
            if (canMoveZ && hasRocks && checkRockCollision(newPosZ, rockInstances, playerRadius)) canMoveZ = false;
            if (newPosZ.z < 1.0f || newPosZ.z > terrainSizeZ - 1.0f) canMoveZ = false;

            if (canMoveX) camPos.x = newPosX.x;
            if (canMoveZ) camPos.z = newPosZ.z;
        }
        // ====================================================================

        // Camera follows terrain height
        camPos.y = groundY + eyeHeight + 5.0f;

        // World matrices
        float aspect = (float)WIDTH / (float)HEIGHT;
        Matrix pWorld = Matrix::perspective(0.01f, 10000.0f, aspect, 60.0f);
        Matrix vWorld = Matrix::lookAt(camPos, camPos + forward, worldUp);
        Matrix vpWorld = vWorld * pWorld;

        core.beginRenderPass();

        // ====================================================================
        // === LAKE === Update scene data with current camera position
        // ====================================================================
        sceneData.cameraPos = camPos;
        sceneData.hasRocks = hasRocks;
        sceneData.hasGrass = hasGrass;

        // === LAKE === Render reflection pass FIRST (before fog capture)
        lake.beginReflectionPass(vWorld, pWorld, camPos, RenderSceneForReflection, &sceneData);

        // === LAKE === Restore back buffer after reflection
        core.setBackBufferRenderTarget();
        core.setDefaultDescriptorHeaps();
        core.getCommandList()->SetGraphicsRootSignature(core.rootSignature);

        // Reset viewport to full screen
        D3D12_VIEWPORT vp = { 0, 0, (float)WIDTH, (float)HEIGHT, 0, 1 };
        D3D12_RECT scissor = { 0, 0, (LONG)WIDTH, (LONG)HEIGHT };
        core.getCommandList()->RSSetViewports(1, &vp);
        core.getCommandList()->RSSetScissorRects(1, &scissor);
        // ====================================================================

        // ====================================================================
        // === FOG === Begin scene capture (renders to fog's internal buffer)
        // ====================================================================
        if (fog.enabled)
        {
            fog.beginSceneCapture();
        }
        // ====================================================================

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


        lakeBottom.draw(&core, &psos, &shaders, vpWorld);


        // ====================================================================
        // === LAKE === Render lake surface (AFTER terrain, BEFORE fog composite)
        // ====================================================================
        lake.render(&core, &psos, &shaders, vpWorld, camPos, totalTime);
        // ====================================================================

        // ====================================================================
        // === FOG === End scene and apply fog (composites to back buffer)
        // ====================================================================
        totalTime += dt;
        if (fog.enabled)
        {
            fog.endSceneAndApplyFog(vWorld, pWorld, camPos, totalTime);

            // Restore state after fog
            core.setBackBufferRenderTarget();
            core.setDefaultDescriptorHeaps();
            core.getCommandList()->SetGraphicsRootSignature(core.rootSignature);
        }
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

    g_lake = nullptr;  // Clear global pointer
    core.flushGraphicsQueue();
    return 0;
}