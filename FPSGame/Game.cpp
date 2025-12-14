
#include "Core.h"
#include "Window.h"
#include "Timer.h"
#include "Maths.h"
#include "Shaders.h"
#include "Mesh.h"
#include "PSO.h"
#include "GEMLoader.h"
#include "Animation.h"
#include "modelState.h"
#include "SkyDome.h"
#include "HeightmapTerrain.h"
#include <algorithm>
#include <Windows.h>
#include "Gun.h"

// Properties -> Linker -> System -> Windows

#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")


class Plane
{
public:
	Mesh mesh;
	std::string shaderName;
	STATIC_VERTEX addVertex(Vec3 p, Vec3 n, float tu, float tv)
	{
		STATIC_VERTEX v;
		v.pos = p;
		v.normal = n;
		Frame frame;
		frame.fromVector(n);
		v.tangent = frame.u;
		v.tu = tu;
		v.tv = tv;
		return v;
	}
	void init(Core* core, PSOManager *psos, Shaders* shaders)
	{
		std::vector<STATIC_VERTEX> vertices;
		vertices.push_back(addVertex(Vec3(-1, 0, -1), Vec3(0, 1, 0), 0, 0));
		vertices.push_back(addVertex(Vec3(1, 0, -1), Vec3(0, 1, 0), 1, 0));
		vertices.push_back(addVertex(Vec3(-1, 0, 1), Vec3(0, 1, 0), 0, 1));
		vertices.push_back(addVertex(Vec3(1, 0, 1), Vec3(0, 1, 0), 1, 1));
		std::vector<unsigned int> indices;
		indices.push_back(0);
		indices.push_back(1);
		indices.push_back(2);
		indices.push_back(1);
		indices.push_back(3);
		indices.push_back(2);
		mesh.init(core, vertices, indices);
		shaders->load(core, "StaticModelUntextured", "Shaders/VS.txt", "Shaders/PSUntextured.txt");
		shaderName = "StaticModelUntextured";
		psos->createPSO(core, "StaticModelUntexturedPSO", shaders->find("StaticModelUntextured")->vs, shaders->find("StaticModelUntextured")->ps, VertexLayoutCache::getStaticLayout());
	}
	void draw(Core* core, PSOManager* psos, Shaders* shaders, Matrix &vp)
	{
		Matrix planeWorld;
		shaders->updateConstantVS("StaticModelUntextured", "staticMeshBuffer", "VP", &vp);
		shaders->updateConstantVS("StaticModelUntextured", "staticMeshBuffer", "W", &planeWorld);
		shaders->apply(core, shaderName);
		psos->bind(core, "StaticModelUntexturedPSO");
		mesh.draw(core);
	}
};

class StaticModel
{
public:
	std::vector<Mesh *> meshes;
	std::vector<std::string> textureFilenames;
	void load(Core* core, std::string filename, Shaders* shaders, PSOManager* psos)
	{
		GEMLoader::GEMModelLoader loader;
		std::vector<GEMLoader::GEMMesh> gemmeshes;
		loader.load(filename, gemmeshes);
		for (int i = 0; i < gemmeshes.size(); i++)
		{
			Mesh* mesh = new Mesh();
			std::vector<STATIC_VERTEX> vertices;
			for (int j = 0; j < gemmeshes[i].verticesStatic.size(); j++)
			{
				STATIC_VERTEX v;
				memcpy(&v, &gemmeshes[i].verticesStatic[j], sizeof(STATIC_VERTEX));
				vertices.push_back(v);
			}
			mesh->init(core, vertices, gemmeshes[i].indices);
			meshes.push_back(mesh);
		}
		shaders->load(core, "StaticModelUntextured", "Shaders/VS.txt", "Shaders/PSUntextured.txt");
		psos->createPSO(core, "StaticModelPSO", shaders->find("StaticModelUntextured")->vs, shaders->find("StaticModelUntextured")->ps, VertexLayoutCache::getStaticLayout());
	}
	void updateWorld(Shaders* shaders, Matrix& w)
	{
		shaders->updateConstantVS("StaticModelUntextured", "staticMeshBuffer", "W", &w);
	}
	void draw(Core* core, PSOManager* psos, Shaders* shaders, Matrix &vp)
	{
		shaders->updateConstantVS("StaticModelUntextured", "staticMeshBuffer", "VP", &vp);
		shaders->apply(core, "StaticModelUntextured");
		psos->bind(core, "StaticModelPSO");
		for (int i = 0; i < meshes.size(); i++)
		{
			meshes[i]->draw(core);
		}
	}
};

class AnimatedModel
{
public:
	std::vector<Mesh *> meshes;
	Animation animation;
	std::vector<std::string> textureFilenames;
	void load(Core* core, std::string filename, PSOManager* psos, Shaders* shaders)
	{
		GEMLoader::GEMModelLoader loader;
		std::vector<GEMLoader::GEMMesh> gemmeshes;
		GEMLoader::GEMAnimation gemanimation;
		loader.load(filename, gemmeshes, gemanimation);
		for (int i = 0; i < gemmeshes.size(); i++)
		{
			Mesh *mesh = new Mesh();
			std::vector<ANIMATED_VERTEX> vertices;
			for (int j = 0; j < gemmeshes[i].verticesAnimated.size(); j++)
			{
				ANIMATED_VERTEX v;
				memcpy(&v, &gemmeshes[i].verticesAnimated[j], sizeof(ANIMATED_VERTEX));
				vertices.push_back(v);
			}
			mesh->init(core, vertices, gemmeshes[i].indices);
			meshes.push_back(mesh);
		}
		shaders->load(core, "AnimatedUntextured", "Shaders/VSAnim.txt", "Shaders/PSUntextured.txt");
		psos->createPSO(core, "AnimatedModelPSO", shaders->find("AnimatedUntextured")->vs, shaders->find("AnimatedUntextured")->ps, VertexLayoutCache::getAnimatedLayout());
		memcpy(&animation.skeleton.globalInverse, &gemanimation.globalInverse, 16 * sizeof(float));
		for (int i = 0; i < gemanimation.bones.size(); i++)
		{
			Bone bone;
			bone.name = gemanimation.bones[i].name;
			memcpy(&bone.offset, &gemanimation.bones[i].offset, 16 * sizeof(float));
			bone.parentIndex = gemanimation.bones[i].parentIndex;
			animation.skeleton.bones.push_back(bone);
		}
		for (int i = 0; i < gemanimation.animations.size(); i++)
		{
			std::string name = gemanimation.animations[i].name;
			AnimationSequence aseq;
			aseq.ticksPerSecond = gemanimation.animations[i].ticksPerSecond;
			for (int j = 0; j < gemanimation.animations[i].frames.size(); j++)
			{
				AnimationFrame frame;
				for (int index = 0; index < gemanimation.animations[i].frames[j].positions.size(); index++)
				{
					Vec3 p;
					Quaternion q;
					Vec3 s;
					memcpy(&p, &gemanimation.animations[i].frames[j].positions[index], sizeof(Vec3));
					frame.positions.push_back(p);
					memcpy(&q, &gemanimation.animations[i].frames[j].rotations[index], sizeof(Quaternion));
					frame.rotations.push_back(q);
					memcpy(&s, &gemanimation.animations[i].frames[j].scales[index], sizeof(Vec3));
					frame.scales.push_back(s);
				}
				aseq.frames.push_back(frame);
			}
			animation.animations.insert({ name, aseq });
		}
	}
	void updateWorld(Shaders* shaders, Matrix& w)
	{
		shaders->updateConstantVS("AnimatedUntextured", "staticMeshBuffer", "W", &w);
	}
	void draw(Core* core, PSOManager* psos, Shaders* shaders, AnimationInstance* instance, Matrix& vp, Matrix& w)
	{
		psos->bind(core, "AnimatedModelPSO");
		shaders->updateConstantVS("AnimatedUntextured", "staticMeshBuffer", "W", &w);
		shaders->updateConstantVS("AnimatedUntextured", "staticMeshBuffer", "VP", &vp);
		shaders->updateConstantVS("AnimatedUntextured", "staticMeshBuffer", "bones", instance->matrices);
		shaders->apply(core, "AnimatedUntextured");
		for (int i = 0; i < meshes.size(); i++)
		{
			meshes[i]->draw(core);
		}
	}
};


#define WIDTH  1920
#define HEIGHT 1080

static float deg2rad(float d) { return d * 3.1415926535f / 180.0f; }


static float clampf(float v, float a, float b) { return std::max(a, std::min(b, v)); }

int WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR lpCmdLine, int nCmdShow)
{
	Window window;
	window.create(1920, 1080, "the game");

	Core core;
	core.init(window.hwnd, 1920, 1080);

	Shaders shaders;
	PSOManager psos;

	// --------------------------
	// 1) 初始化 SkyDome
	// --------------------------
	SkyDome sky;
	sky.init(&core, &psos, &shaders, 5000.0f);


	// --------------------------
	// 2) 初始化 HeightmapTerrain
	// --------------------------
	HeightmapTerrain terrain;
	
	bool terrainOK = terrain.init(
		&core, &psos, &shaders,
		"Assets/Heightmap/map2.png",
		512, 512,          // 高度图分辨率
		300.0f, 300.0f,    // 地形在世界中覆盖的 X/Z 尺寸（米/单位）
		40.0f,             // heightScale：最高高度（单位）
		0.0f,              // heightOffset：整体抬高（可选）
		HeightmapTerrain::Format::PNG16
	);
	if (!terrainOK)
	{
		std::string msg = "Map not open";
		OutputDebugStringA(msg.c_str());
		MessageBoxA(nullptr, msg.c_str(), "Shader File Error", MB_OK);

		return 0;
	}

	// --------------------------
	// 3) 你的 FPS 枪模型（照旧）
	// --------------------------
	Gun gunModel;
	gunModel.load(&core,
		"Assets/Models/AutomaticCarbine.gem",
		"Assets/Models/Textures/gun.png",  
		&psos,
		&shaders);

	AnimationInstance gunAnim;
	gunAnim.init(&gunModel.animation, 0);
	




	// --------------------------
	// 4) FPS camera state
	// --------------------------
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

	// --------------------------
	// 5) Viewmodel placement
	// --------------------------
	float gunX = 0.08f;
	float gunY = 0.0f;
	float gunZ = 0.0f;

	Vec3 gunScale(0.01f, 0.01f, 0.01f);

	const float PI = 3.141592654f;
	float modelRotX = 0.0f;
	float modelRotY = +PI * 1.01f;
	float modelRotZ = 0.0f;

	Timer timer;

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

		// Forward (用于视角)
		Vec3 forward(
			sinf(yaw) * cosf(pitch),
			sinf(pitch),
			cosf(yaw) * cosf(pitch)
		);
		forward = forward.normalize();

		// Movement：忽略pitch，避免上下漂
		Vec3 forwardFlat(forward.x, 0.0f, forward.z);
		if (forwardFlat.length() > 0.0001f) forwardFlat = forwardFlat.normalize();

		Vec3 worldUp(0, 1, 0);
		Vec3 rightFlat = Cross(worldUp, forwardFlat).normalize();

		float groundY = terrain.sampleHeightWorld(camPos.x, camPos.z);


		if (window.keys['W']) camPos = camPos + forwardFlat * (moveSpeed * dt);
		if (window.keys['S']) camPos = camPos - forwardFlat * (moveSpeed * dt);
		if (window.keys['A']) camPos = camPos - rightFlat * (moveSpeed * dt);
		if (window.keys['D']) camPos = camPos + rightFlat * (moveSpeed * dt);


		camPos.y = groundY + eyeHeight;
		//camPos.y = eyeHeight;
		// --------------------------
		// World VP
		// --------------------------
		float aspect = 1920.0f / 1080.0f;
		Matrix pWorld = Matrix::perspective(0.01f, 10000.0f, aspect, 60.0f);
		Matrix vWorld = Matrix::lookAt(camPos, camPos + forward, worldUp);
		Matrix vpWorld = vWorld * pWorld;

		core.beginRenderPass();


		// 先画天空（写入最远深度），再画地形和其它世界物体
		sky.draw(&core, &psos, &shaders, vpWorld, camPos);



		// 地形 world 矩阵：默认单位矩阵即可（Matrix默认是单位阵）
		Matrix terrainW;
		terrain.draw(&core, &psos, &shaders, vpWorld, terrainW);



		// --------------------------
		// Gun animation update
		// --------------------------
		modelState.update(window, gunAnim, dt);
		modelState.getGunOffset(gunX, gunY, gunZ, modelRotY);

		// --------------------------
		// Draw gun as viewmodel (VP = P)
		// --------------------------
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