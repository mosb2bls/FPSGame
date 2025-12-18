#pragma once

#include "Core.h"
#include "Mesh.h"
#include "Maths.h"
#include "HeightmapTerrain.h"
#include <vector>
#include <random>
#include <cmath>
#include <algorithm>
#include <unordered_set>
#include <functional>

// ============================================================================
// VEGETATION GENERATOR - FIXED VERSION
// ============================================================================
// Guarantees full map coverage using grid-based spawning with jitter
// ============================================================================

// ----------------------------------------------------------------------------
// CONFIGURATION STRUCTURES
// ----------------------------------------------------------------------------

enum class VegetationType
{
    Grass,
    Rock
};

struct VegetationItem
{
    Vec3 position;
    float rotationY;
    float scale;
    int typeIndex;          // Which grass/rock variant (0, 1, 2, etc.)
    VegetationType type;    // Grass or Rock
    float radius;           // Bounding radius for collision
};

struct ClusterConfig
{
    float probability = 0.3f;       // Chance to spawn a cluster (0-1)
    int minItems = 3;               // Minimum items in a cluster
    int maxItems = 8;               // Maximum items in a cluster
    float radius = 5.0f;            // Cluster spread radius
    float falloff = 2.0f;           // How quickly density falls off from center
};

struct VegetationConfig
{
    // General settings
    float density = 0.5f;                   // Items per square meter (approximately)
    float minPointSpacing = 2.0f;           // Minimum distance between spawn points

    // Type balance
    float rockProbability = 0.15f;          // Base chance for rock (vs grass)
    float noiseInfluence = 0.4f;            // How much noise affects rock/grass balance
    float noiseScale = 0.02f;               // Scale of noise (smaller = larger biomes)

    // Grass settings
    float grassMinScale = 0.8f;
    float grassMaxScale = 1.2f;
    float grassRadius = 0.3f;               // Collision radius for grass
    ClusterConfig grassCluster;

    // Rock settings
    float rockMinScale = 0.5f;
    float rockMaxScale = 2.0f;
    float rockRadius = 1.0f;                // Collision radius for rocks
    ClusterConfig rockCluster;

    // Terrain constraints
    float minSlope = 0.0f;                  // Minimum terrain slope (0 = flat)
    float maxSlope = 45.0f;                 // Maximum terrain slope in degrees
    float minHeight = -1000.0f;             // Minimum terrain height
    float maxHeight = 1000.0f;              // Maximum terrain height

    VegetationConfig()
    {
        // Default grass cluster: frequent, small clusters
        grassCluster.probability = 0.6f;
        grassCluster.minItems = 5;
        grassCluster.maxItems = 15;
        grassCluster.radius = 3.0f;
        grassCluster.falloff = 1.5f;

        // Default rock cluster: less frequent, varied sizes
        rockCluster.probability = 0.4f;
        rockCluster.minItems = 2;
        rockCluster.maxItems = 5;
        rockCluster.radius = 4.0f;
        rockCluster.falloff = 2.0f;
    }
};

// ----------------------------------------------------------------------------
// SPATIAL HASH GRID - For efficient overlap detection
// ----------------------------------------------------------------------------

class SpatialHashGrid
{
public:
    void init(float worldSizeX, float worldSizeZ, float cellSize)
    {
        this->cellSize = cellSize;
        this->offsetX = worldSizeX * 0.5f;
        this->offsetZ = worldSizeZ * 0.5f;
        this->gridWidth = (int)std::ceil(worldSizeX / cellSize) + 1;
        this->gridHeight = (int)std::ceil(worldSizeZ / cellSize) + 1;

        cells.clear();
        cells.resize(gridWidth * gridHeight);
    }

    void clear()
    {
        for (auto& cell : cells)
            cell.clear();
    }

    void insert(const VegetationItem& item)
    {
        int cellIdx = getCellIndex(item.position.x, item.position.z);
        if (cellIdx >= 0 && cellIdx < (int)cells.size())
            cells[cellIdx].push_back(item);
    }

    bool checkOverlap(float x, float z, float radius) const
    {
        // Check neighboring cells (3x3 grid around the point)
        int centerCellX = (int)((x + offsetX) / cellSize);
        int centerCellZ = (int)((z + offsetZ) / cellSize);

        for (int dz = -1; dz <= 1; dz++)
        {
            for (int dx = -1; dx <= 1; dx++)
            {
                int cellX = centerCellX + dx;
                int cellZ = centerCellZ + dz;

                if (cellX < 0 || cellX >= gridWidth || cellZ < 0 || cellZ >= gridHeight)
                    continue;

                int cellIdx = cellZ * gridWidth + cellX;

                for (const auto& item : cells[cellIdx])
                {
                    float distX = item.position.x - x;
                    float distZ = item.position.z - z;
                    float distSq = distX * distX + distZ * distZ;
                    float minDist = item.radius + radius;

                    if (distSq < minDist * minDist)
                        return true; // Overlap detected
                }
            }
        }

        return false;
    }

private:
    int getCellIndex(float x, float z) const
    {
        int cellX = (int)((x + offsetX) / cellSize);
        int cellZ = (int)((z + offsetZ) / cellSize);

        if (cellX < 0 || cellX >= gridWidth || cellZ < 0 || cellZ >= gridHeight)
            return -1;

        return cellZ * gridWidth + cellX;
    }

    float cellSize = 5.0f;
    float offsetX = 0.0f;
    float offsetZ = 0.0f;
    int gridWidth = 0;
    int gridHeight = 0;
    std::vector<std::vector<VegetationItem>> cells;
};

// ----------------------------------------------------------------------------
// NOISE GENERATOR - For natural biome distribution
// ----------------------------------------------------------------------------

class NoiseGenerator
{
public:
    NoiseGenerator(unsigned int seed = 12345)
    {
        // Initialize permutation table
        std::mt19937 rng(seed);
        for (int i = 0; i < 256; i++)
            perm[i] = i;

        std::shuffle(perm, perm + 256, rng);

        // Duplicate for overflow
        for (int i = 0; i < 256; i++)
            perm[256 + i] = perm[i];
    }

    // 2D Perlin noise, returns value in range [-1, 1]
    float noise2D(float x, float y) const
    {
        // Find unit grid cell
        int X = (int)std::floor(x) & 255;
        int Y = (int)std::floor(y) & 255;

        // Relative position in cell
        x -= std::floor(x);
        y -= std::floor(y);

        // Fade curves
        float u = fade(x);
        float v = fade(y);

        // Hash corners
        int A = perm[X] + Y;
        int AA = perm[A];
        int AB = perm[A + 1];
        int B = perm[X + 1] + Y;
        int BA = perm[B];
        int BB = perm[B + 1];

        // Blend
        return lerp(v,
            lerp(u, grad(perm[AA], x, y), grad(perm[BA], x - 1, y)),
            lerp(u, grad(perm[AB], x, y - 1), grad(perm[BB], x - 1, y - 1))
        );
    }

    // Fractal Brownian Motion - multiple octaves of noise
    float fbm(float x, float y, int octaves = 4, float persistence = 0.5f) const
    {
        float total = 0.0f;
        float amplitude = 1.0f;
        float frequency = 1.0f;
        float maxValue = 0.0f;

        for (int i = 0; i < octaves; i++)
        {
            total += noise2D(x * frequency, y * frequency) * amplitude;
            maxValue += amplitude;
            amplitude *= persistence;
            frequency *= 2.0f;
        }

        return total / maxValue;
    }

private:
    float fade(float t) const { return t * t * t * (t * (t * 6 - 15) + 10); }
    float lerp(float t, float a, float b) const { return a + t * (b - a); }

    float grad(int hash, float x, float y) const
    {
        int h = hash & 3;
        float u = h < 2 ? x : y;
        float v = h < 2 ? y : x;
        return ((h & 1) ? -u : u) + ((h & 2) ? -2.0f * v : 2.0f * v);
    }

    int perm[512];
};

// ----------------------------------------------------------------------------
// MAIN VEGETATION GENERATOR CLASS
// ----------------------------------------------------------------------------

class VegetationGenerator
{
public:
    // Generate all vegetation for the terrain
    void generate(
        HeightmapTerrain* terrain,
        const VegetationConfig& config,
        float terrainSizeX,
        float terrainSizeZ,
        unsigned int seed = 0)
    {
        std::cout << "\n[VegetationGenerator] Starting generation...\n";
        std::cout << "  Terrain size: " << terrainSizeX << " x " << terrainSizeZ << "\n";

        // Initialize
        if (seed == 0)
        {
            std::random_device rd;
            seed = rd();
        }

        rng.seed(seed);
        noise = NoiseGenerator(seed);

        this->terrain = terrain;
        this->config = config;
        this->terrainSizeX = terrainSizeX;
        this->terrainSizeZ = terrainSizeZ;

        // Clear previous data
        grassItems.clear();
        rockItems.clear();

        // Initialize spatial grid
        float cellSize = std::max(config.rockRadius, config.grassRadius) * 4.0f;
        spatialGrid.init(terrainSizeX, terrainSizeZ, cellSize);

        // FIXED: Use grid-based spawning for FULL MAP COVERAGE
        std::cout << "[VegetationGenerator] Generating spawn points (grid-based)...\n";
        std::vector<Vec2> spawnPoints = generateSpawnPointsGrid();
        std::cout << "  Generated " << spawnPoints.size() << " spawn points\n";

        // For each point, determine type and generate items
        std::cout << "[VegetationGenerator] Placing vegetation...\n";

        int clusterCount = 0;

        for (const auto& point : spawnPoints)
        {
            // Determine vegetation type based on noise + probability
            VegetationType type = determineType(point.x, point.y);

            // Check if this should be a cluster
            bool isCluster = shouldGenerateCluster(type);

            if (isCluster)
            {
                clusterCount++;
                generateCluster(point.x, point.y, type);
            }
            else
            {
                // Single item
                tryPlaceItem(point.x, point.y, type);
            }
        }

        std::cout << "[VegetationGenerator] Generation complete!\n";
        std::cout << "  Grass items: " << grassItems.size() << "\n";
        std::cout << "  Rock items: " << rockItems.size() << "\n";
        std::cout << "  Clusters generated: " << clusterCount << "\n";
    }

    // Get results
    const std::vector<VegetationItem>& getGrassItems() const { return grassItems; }
    const std::vector<VegetationItem>& getRockItems() const { return rockItems; }

    // Convert to RockInstance format
    std::vector<RockInstance> getRockInstances() const
    {
        std::vector<RockInstance> instances;
        instances.reserve(rockItems.size());

        for (const auto& item : rockItems)
        {
            RockInstance inst;
            inst.position = item.position;
            inst.rotationY = item.rotationY;
            inst.scale = item.scale;
            inst.typeIndex = item.typeIndex;
            inst.distanceToCamera = 0.0f;
            inst.lodLevel = 2;
            instances.push_back(inst);
        }

        return instances;
    }

private:
    // ========================================================================
    // FIXED: Grid-based spawn point generation for FULL MAP COVERAGE
    // ========================================================================
    std::vector<Vec2> generateSpawnPointsGrid()
    {
        std::vector<Vec2> points;

        float halfX = terrainSizeX * 0.5f;
        float halfZ = terrainSizeZ * 0.5f;

        // Calculate grid spacing based on density
        // density = items per square meter
        // spacing = sqrt(1/density) for roughly even distribution
        float spacing = config.minPointSpacing;
        if (config.density > 0)
        {
            spacing = std::max(spacing, 1.0f / std::sqrt(config.density));
        }

        // Calculate grid dimensions
        int gridCountX = (int)std::ceil(terrainSizeX / spacing);
        int gridCountZ = (int)std::ceil(terrainSizeZ / spacing);

        std::cout << "  Grid: " << gridCountX << " x " << gridCountZ
            << " (spacing: " << spacing << "m)\n";

        // Random distributions for jitter
        float jitterAmount = spacing * 0.4f;  // 40% jitter
        std::uniform_real_distribution<float> jitterX(-jitterAmount, jitterAmount);
        std::uniform_real_distribution<float> jitterZ(-jitterAmount, jitterAmount);
        std::uniform_real_distribution<float> skipChance(0.0f, 1.0f);

        // Generate points on grid with jitter
        for (int gz = 0; gz < gridCountZ; gz++)
        {
            for (int gx = 0; gx < gridCountX; gx++)
            {
                // Base position (grid cell center)
                float baseX = -halfX + (gx + 0.5f) * spacing;
                float baseZ = -halfZ + (gz + 0.5f) * spacing;

                // Add random jitter
                float x = baseX + jitterX(rng);
                float z = baseZ + jitterZ(rng);

                // Clamp to terrain bounds
                x = std::clamp(x, -halfX + 1.0f, halfX - 1.0f);
                z = std::clamp(z, -halfZ + 1.0f, halfZ - 1.0f);

                // Random skip for more natural look (10% chance to skip)
                if (skipChance(rng) < 0.1f)
                    continue;

                // Check terrain constraints
                if (!isValidTerrainLocation(x, z))
                    continue;

                points.push_back(Vec2(x, z));
            }
        }

        // Shuffle points for random processing order
        std::shuffle(points.begin(), points.end(), rng);

        return points;
    }

    // Check if terrain location is suitable for vegetation
    bool isValidTerrainLocation(float x, float z)
    {
        if (!terrain) return true;

        float height = terrain->sampleHeightWorld(x, z);

        // Height check
        if (height < config.minHeight || height > config.maxHeight)
            return false;

        // Slope check (sample nearby points to estimate slope)
        float delta = 0.5f;
        float h1 = terrain->sampleHeightWorld(x + delta, z);
        float h2 = terrain->sampleHeightWorld(x - delta, z);
        float h3 = terrain->sampleHeightWorld(x, z + delta);
        float h4 = terrain->sampleHeightWorld(x, z - delta);

        float slopeX = (h1 - h2) / (2.0f * delta);
        float slopeZ = (h3 - h4) / (2.0f * delta);
        float slopeAngle = std::atan(std::sqrt(slopeX * slopeX + slopeZ * slopeZ)) * 180.0f / 3.14159265f;

        if (slopeAngle < config.minSlope || slopeAngle > config.maxSlope)
            return false;

        return true;
    }

    // Determine if this point should have grass or rock
    VegetationType determineType(float x, float z)
    {
        // Base probability
        float rockChance = config.rockProbability;

        // Modify by noise (creates rocky/grassy biomes)
        float noiseValue = noise.fbm(x * config.noiseScale, z * config.noiseScale, 4, 0.5f);
        noiseValue = (noiseValue + 1.0f) * 0.5f; // Normalize to 0-1

        rockChance += (noiseValue - 0.5f) * config.noiseInfluence * 2.0f;
        rockChance = std::clamp(rockChance, 0.05f, 0.95f);

        std::uniform_real_distribution<float> rand01(0.0f, 1.0f);
        return (rand01(rng) < rockChance) ? VegetationType::Rock : VegetationType::Grass;
    }

    // Check if we should generate a cluster instead of single item
    bool shouldGenerateCluster(VegetationType type)
    {
        const ClusterConfig& clusterConfig = (type == VegetationType::Rock)
            ? config.rockCluster
            : config.grassCluster;

        std::uniform_real_distribution<float> rand01(0.0f, 1.0f);
        return rand01(rng) < clusterConfig.probability;
    }

    // Generate a cluster of items around a center point
    void generateCluster(float centerX, float centerZ, VegetationType type)
    {
        const ClusterConfig& clusterConfig = (type == VegetationType::Rock)
            ? config.rockCluster
            : config.grassCluster;

        std::uniform_int_distribution<int> randCount(clusterConfig.minItems, clusterConfig.maxItems);
        int itemCount = randCount(rng);

        std::uniform_real_distribution<float> randAngle(0.0f, 2.0f * 3.14159265f);
        std::uniform_real_distribution<float> rand01(0.0f, 1.0f);

        // Place center item first
        tryPlaceItem(centerX, centerZ, type);

        // Generate surrounding items with falloff
        for (int i = 1; i < itemCount; i++)
        {
            // Use exponential distribution for distance (more items near center)
            float t = rand01(rng);
            float distance = clusterConfig.radius * (1.0f - std::pow(1.0f - t, clusterConfig.falloff));

            // Add some jitter to avoid perfect circles
            float jitter = rand01(rng) * 0.3f + 0.85f; // 0.85 - 1.15
            distance *= jitter;

            float angle = randAngle(rng);
            float x = centerX + std::cos(angle) * distance;
            float z = centerZ + std::sin(angle) * distance;

            // Check bounds
            float halfX = terrainSizeX * 0.5f;
            float halfZ = terrainSizeZ * 0.5f;
            if (x < -halfX || x > halfX || z < -halfZ || z > halfZ)
                continue;

            // Try to place (might fail due to overlap)
            tryPlaceItem(x, z, type);
        }
    }

    // Try to place a single item, returns false if blocked by overlap
    bool tryPlaceItem(float x, float z, VegetationType type)
    {
        if (!terrain) return false;

        // Check terrain validity
        if (!isValidTerrainLocation(x, z))
            return false;

        // Get terrain height
        float y = terrain->sampleHeightWorld(x, z);

        // Determine radius and scale
        float radius, scale;
        int typeIndex;

        std::uniform_real_distribution<float> rand01(0.0f, 1.0f);
        std::uniform_real_distribution<float> randRotation(0.0f, 2.0f * 3.14159265f);

        if (type == VegetationType::Rock)
        {
            std::uniform_real_distribution<float> randScale(config.rockMinScale, config.rockMaxScale);
            scale = randScale(rng);
            radius = config.rockRadius * scale;

            // Random rock type (assuming 3 types, adjust as needed)
            std::uniform_int_distribution<int> randType(0, 2);
            typeIndex = randType(rng);
        }
        else
        {
            std::uniform_real_distribution<float> randScale(config.grassMinScale, config.grassMaxScale);
            scale = randScale(rng);
            radius = config.grassRadius * scale;

            // Random grass type
            std::uniform_int_distribution<int> randType(0, 8);
            typeIndex = randType(rng);
        }

        // Check for overlap with existing items
        if (spatialGrid.checkOverlap(x, z, radius))
            return false;

        // Create and store the item
        VegetationItem item;
        item.position = Vec3(x, y, z);
        item.rotationY = randRotation(rng);
        item.scale = scale;
        item.typeIndex = typeIndex;
        item.type = type;
        item.radius = radius;

        // Add to appropriate list and spatial grid
        if (type == VegetationType::Rock)
            rockItems.push_back(item);
        else
            grassItems.push_back(item);

        spatialGrid.insert(item);

        return true;
    }

    // Member variables
    HeightmapTerrain* terrain = nullptr;
    VegetationConfig config;
    float terrainSizeX = 0.0f;
    float terrainSizeZ = 0.0f;

    std::mt19937 rng;
    NoiseGenerator noise;
    SpatialHashGrid spatialGrid;

    std::vector<VegetationItem> grassItems;
    std::vector<VegetationItem> rockItems;
};

// ----------------------------------------------------------------------------
// BIOME PRESETS - Ready-to-use configurations
// ----------------------------------------------------------------------------

namespace VegetationPresets
{
    // Lush meadow with lots of grass, few rocks
    inline VegetationConfig Meadow()
    {
        VegetationConfig config;
        config.density = 2.0f;              // High density
        config.minPointSpacing = 1.0f;
        config.rockProbability = 0.05f;
        config.noiseInfluence = 0.1f;

        config.grassCluster.probability = 0.7f;
        config.grassCluster.minItems = 8;
        config.grassCluster.maxItems = 20;
        config.grassCluster.radius = 4.0f;

        config.rockCluster.probability = 0.2f;
        config.rockCluster.minItems = 1;
        config.rockCluster.maxItems = 3;

        return config;
    }

    // Rocky terrain with sparse grass
    inline VegetationConfig Rocky()
    {
        VegetationConfig config;
        config.density = 0.5f;
        config.minPointSpacing = 2.5f;
        config.rockProbability = 0.6f;
        config.noiseInfluence = 0.3f;

        config.grassCluster.probability = 0.3f;
        config.grassCluster.minItems = 3;
        config.grassCluster.maxItems = 8;

        config.rockCluster.probability = 0.5f;
        config.rockCluster.minItems = 3;
        config.rockCluster.maxItems = 8;
        config.rockCluster.radius = 6.0f;

        config.rockMinScale = 0.8f;
        config.rockMaxScale = 3.0f;

        return config;
    }

    // Mixed forest floor
    inline VegetationConfig Forest()
    {
        VegetationConfig config;
        config.density = 1.0f;
        config.minPointSpacing = 1.5f;
        config.rockProbability = 0.15f;
        config.noiseInfluence = 0.5f;
        config.noiseScale = 0.03f;

        config.grassCluster.probability = 0.5f;
        config.grassCluster.minItems = 5;
        config.grassCluster.maxItems = 12;

        config.rockCluster.probability = 0.4f;
        config.rockCluster.minItems = 2;
        config.rockCluster.maxItems = 5;

        return config;
    }

    // Desert with scattered rocks and sparse vegetation
    inline VegetationConfig Desert()
    {
        VegetationConfig config;
        config.density = 0.2f;
        config.minPointSpacing = 4.0f;
        config.rockProbability = 0.7f;
        config.noiseInfluence = 0.2f;

        config.grassCluster.probability = 0.2f;
        config.grassCluster.minItems = 2;
        config.grassCluster.maxItems = 5;
        config.grassCluster.radius = 2.0f;

        config.rockCluster.probability = 0.3f;
        config.rockCluster.minItems = 1;
        config.rockCluster.maxItems = 4;

        config.grassMinScale = 0.5f;
        config.grassMaxScale = 0.8f;

        return config;
    }

    // Dense vegetation - full coverage
    inline VegetationConfig Dense()
    {
        VegetationConfig config;
        config.density = 3.0f;              // Very high density
        config.minPointSpacing = 0.8f;      // Close spacing
        config.rockProbability = 0.1f;
        config.noiseInfluence = 0.3f;

        config.grassCluster.probability = 0.8f;
        config.grassCluster.minItems = 10;
        config.grassCluster.maxItems = 25;
        config.grassCluster.radius = 5.0f;

        config.rockCluster.probability = 0.3f;
        config.rockCluster.minItems = 2;
        config.rockCluster.maxItems = 4;

        return config;
    }
}