#pragma once

#include <string>
#include <vector>
#include <map>

#include "Maths.h"

struct Bone
{
	std::string name;
	Matrix offset;
	int parentIndex;
};

struct Skeleton
{
	std::vector<Bone> bones;
	Matrix globalInverse;
	int findBone(std::string name)
	{
		for (int i = 0; i < bones.size(); i++)
		{
			if (bones[i].name == name)
			{
				return i;
			}
		}
		return -1;
	}
};

struct AnimationFrame
{
	std::vector<Vec3> positions;
	std::vector<Quaternion> rotations;
	std::vector<Vec3> scales;
};

struct AnimationSequence // This holds rescaled times
{
	std::vector<AnimationFrame> frames;
	float ticksPerSecond;
	Vec3 interpolate(Vec3 p1, Vec3 p2, float t)
	{
		return ((p1 * (1.0f - t)) + (p2 * t));
	}
	Quaternion interpolate(Quaternion q1, Quaternion q2, float t)
	{
		return Quaternion::slerp(q1, q2, t);
	}
	float duration()
	{
		return ((float)frames.size() / ticksPerSecond);
	}
	void calcFrame(float t, int& frame, float& interpolationFact)
	{
		interpolationFact = t * ticksPerSecond;
		frame = (int)floorf(interpolationFact);
		interpolationFact = interpolationFact - (float)frame;
		frame = std::min(frame, (int)(frames.size() - 1));
	}
	bool running(float t)
	{
		if ((int)floorf(t * ticksPerSecond) < frames.size())
		{
			return true;
		}
		return false;
	}
	int nextFrame(int frame)
	{
		return std::min(frame + 1, (int)(frames.size() - 1));
	}
	Matrix interpolateBoneToGlobal(Matrix* matrices, int baseFrame, float interpolationFact, Skeleton* skeleton, int boneIndex)
	{
		Matrix scale = Matrix::scaling(interpolate(frames[baseFrame].scales[boneIndex], frames[nextFrame(baseFrame)].scales[boneIndex], interpolationFact));
		Matrix rotation = interpolate(frames[baseFrame].rotations[boneIndex], frames[nextFrame(baseFrame)].rotations[boneIndex], interpolationFact).toMatrix();
		Matrix translation = Matrix::translation(interpolate(frames[baseFrame].positions[boneIndex], frames[nextFrame(baseFrame)].positions[boneIndex], interpolationFact));
		Matrix local = scale * rotation * translation;
		if (skeleton->bones[boneIndex].parentIndex > -1)
		{
			Matrix global = local * matrices[skeleton->bones[boneIndex].parentIndex];
			return global;
		}
		return local;
	}
};

class Animation
{
public:
	std::map<std::string, AnimationSequence> animations;
	Skeleton skeleton;
	int bonesSize()
	{
		return skeleton.bones.size();
	}
	void calcFrame(std::string name, float t, int& frame, float& interpolationFact)
	{
		animations[name].calcFrame(t, frame, interpolationFact);
	}
	Matrix interpolateBoneToGlobal(std::string name, Matrix* matrices, int baseFrame, float interpolationFact, int boneIndex)
	{
		return animations[name].interpolateBoneToGlobal(matrices, baseFrame, interpolationFact, &skeleton, boneIndex);
	}
	void calcTransforms(Matrix* matrices, Matrix coordTransform)
	{
		for (int i = 0; i < bonesSize(); i++)
		{
			matrices[i] = skeleton.bones[i].offset * matrices[i] * skeleton.globalInverse * coordTransform;
		}
	}
	bool hasAnimation(std::string name)
	{
		if (animations.find(name) == animations.end())
		{
			return false;
		}
		return true;
	}
};

class AnimationInstance
{
public:
	Animation* animation;
	std::string usingAnimation;
	float t;
	Matrix matrices[256]; // This is defined as 256 to match the maximum number in the shader
	Matrix matricesPose[256]; // This is to store transforms needed for finding bone positions
	Matrix coordTransform;
	void init(Animation* _animation, int fromYZX)
	{
		animation = _animation;
		if (fromYZX == 1)
		{
			memset(coordTransform.a, 0, 16 * sizeof(float));
			coordTransform.a[0][0] = 1.0f;
			coordTransform.a[2][1] = 1.0f;
			coordTransform.a[1][2] = -1.0f;
			coordTransform.a[3][3] = 1.0f;
		}
	}
	void update(std::string name, float dt)
	{
		if (name == usingAnimation)
		{
			t += dt;
		} else
		{
			usingAnimation = name;
			t = 0;
		}
		if (animationFinished() == true)
		{
			return;
		}
		int frame = 0;
		float interpolationFact = 0;
		animation->calcFrame(name, t, frame, interpolationFact);
		for (int i = 0; i < animation->bonesSize(); i++)
		{
			matrices[i] = animation->interpolateBoneToGlobal(name, matrices, frame, interpolationFact, i);
		}
		animation->calcTransforms(matrices, coordTransform);
	}
	void resetAnimationTime()
	{
		t = 0;
	}
	bool animationFinished()
	{
		if (t > animation->animations[usingAnimation].duration())
		{
			return true;
		}
		return false;
	}
	Matrix findWorldMatrix(std::string boneName)
	{
		int boneID = animation->skeleton.findBone(boneName);
		std::vector<int> boneChain;
		int ID = boneID;
		while (ID != -1)
		{
			boneChain.push_back(ID);
			ID = animation->skeleton.bones[ID].parentIndex;
		}
		int frame = 0;
		float interpolationFact = 0;
		animation->calcFrame(usingAnimation, t, frame, interpolationFact);
		for (int i = boneChain.size() - 1; i > -1; i = i - 1)
		{
			matricesPose[boneChain[i]] = animation->interpolateBoneToGlobal(usingAnimation, matricesPose, frame, interpolationFact, boneChain[i]);
		}
		return (matricesPose[boneID] * coordTransform);
	}
};