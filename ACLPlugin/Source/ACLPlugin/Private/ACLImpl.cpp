////////////////////////////////////////////////////////////////////////////////
// The MIT License (MIT)
//
// Copyright (c) 2018 Nicholas Frechette
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
////////////////////////////////////////////////////////////////////////////////

#include "ACLImpl.h"

#if WITH_EDITOR
acl::RotationFormat8 GetRotationFormat(ACLRotationFormat Format)
{
	switch (Format)
	{
	default:
	case ACLRotationFormat::ACLRF_Quat_128:			return acl::RotationFormat8::Quat_128;
	case ACLRotationFormat::ACLRF_QuatDropW_96:		return acl::RotationFormat8::QuatDropW_96;
	case ACLRotationFormat::ACLRF_QuatDropW_Variable:	return acl::RotationFormat8::QuatDropW_Variable;
	}
}

acl::VectorFormat8 GetVectorFormat(ACLVectorFormat Format)
{
	switch (Format)
	{
	default:
	case ACLVectorFormat::ACLVF_Vector3_96:			return acl::VectorFormat8::Vector3_96;
	case ACLVectorFormat::ACLVF_Vector3_Variable:		return acl::VectorFormat8::Vector3_Variable;
	}
}

TUniquePtr<acl::RigidSkeleton> BuildACLSkeleton(ACLAllocator& AllocatorImpl, const UAnimSequence& AnimSeq, const TArray<FBoneData>& BoneData, float DefaultVirtualVertexDistance, float SafeVirtualVertexDistance)
{
	using namespace acl;

	const int32 NumBones = BoneData.Num();

	TArray<RigidBone> ACLSkeletonBones;
	ACLSkeletonBones.Empty(NumBones);
	ACLSkeletonBones.AddDefaulted(NumBones);

	for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
	{
		const FBoneData& UE4Bone = BoneData[BoneIndex];
		RigidBone& ACLBone = ACLSkeletonBones[BoneIndex];
		ACLBone.name = String(AllocatorImpl, TCHAR_TO_ANSI(*UE4Bone.Name.ToString()));
		ACLBone.bind_transform = transform_cast(transform_set(QuatCast(UE4Bone.Orientation), VectorCast(UE4Bone.Position), vector_set(1.0f)));

		// We use a higher virtual vertex distance when bones have a socket attached or are keyed end effectors (IK, hand, camera, etc)
		ACLBone.vertex_distance = (UE4Bone.bHasSocket || UE4Bone.bKeyEndEffector) ? SafeVirtualVertexDistance : DefaultVirtualVertexDistance;

		const int32 ParentBoneIndex = UE4Bone.GetParent();
		ACLBone.parent_index = ParentBoneIndex >= 0 ? safe_static_cast<uint16_t>(ParentBoneIndex) : acl::k_invalid_bone_index;
	}

	return MakeUnique<RigidSkeleton>(AllocatorImpl, ACLSkeletonBones.GetData(), NumBones);
}

static int32 FindAnimationTrackIndex(const UAnimSequence& AnimSeq, int32 BoneIndex)
{
	const TArray<FTrackToSkeletonMap>& TrackToSkelMap = AnimSeq.GetRawTrackToSkeletonMapTable();
	if (BoneIndex != INDEX_NONE)
	{
		for (int32 TrackIndex = 0; TrackIndex < TrackToSkelMap.Num(); ++TrackIndex)
		{
			const FTrackToSkeletonMap& TrackToSkeleton = TrackToSkelMap[TrackIndex];
			if (TrackToSkeleton.BoneTreeIndex == BoneIndex)
				return TrackIndex;
		}
	}

	return INDEX_NONE;
}

TUniquePtr<acl::AnimationClip> BuildACLClip(ACLAllocator& AllocatorImpl, const UAnimSequence* AnimSeq, const acl::RigidSkeleton& ACLSkeleton, int32 RefFrameIndex, bool IsAdditive)
{
	using namespace acl;

	// Additive animations have 0,0,0 scale as the default since we add it
	const FVector UE4DefaultScale(IsAdditive ? 0.0f : 1.0f);
	const Vector4_64 ACLDefaultScale = vector_set(IsAdditive ? 0.0 : 1.0);

	if (AnimSeq != nullptr)
	{
		const TArray<FRawAnimSequenceTrack>& RawTracks = AnimSeq->GetRawAnimationData();
		const uint32 NumSamples = RefFrameIndex >= 0 ? 1 : AnimSeq->NumFrames;
		const uint32 SampleRate = RefFrameIndex >= 0 ? 30 : FMath::TruncToInt(((AnimSeq->NumFrames - 1) / AnimSeq->SequenceLength) + 0.5f);
		const uint32 FirstSampleIndex = RefFrameIndex >= 0 ? FMath::Min(RefFrameIndex, AnimSeq->NumFrames - 1) : 0;
		const String ClipName(AllocatorImpl, TCHAR_TO_ANSI(*AnimSeq->GetPathName()));

		TUniquePtr<AnimationClip> ACLClip = MakeUnique<AnimationClip>(AllocatorImpl, ACLSkeleton, NumSamples, SampleRate, ClipName);

		AnimatedBone* ACLBones = ACLClip->get_bones();
		const uint16 NumBones = ACLSkeleton.get_num_bones();
		for (uint16 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
		{
			const int32 TrackIndex = FindAnimationTrackIndex(*AnimSeq, BoneIndex);

			AnimatedBone& ACLBone = ACLBones[BoneIndex];

			// We output bone data in UE4 track order. If a track isn't present, we will use the bind pose and strip it from the
			// compressed stream.
			ACLBone.output_index = TrackIndex >= 0 ? TrackIndex : -1;

			if (TrackIndex >= 0)
			{
				// We have a track for this bone, use it
				const FRawAnimSequenceTrack& RawTrack = RawTracks[TrackIndex];

				for (uint32 SampleIndex = FirstSampleIndex; SampleIndex < NumSamples; ++SampleIndex)
				{
					const FQuat& RotationSample = RawTrack.RotKeys.Num() == 1 ? RawTrack.RotKeys[0] : RawTrack.RotKeys[SampleIndex];
					ACLBone.rotation_track.set_sample(SampleIndex, quat_cast(QuatCast(RotationSample)));

					const FVector& TranslationSample = RawTrack.PosKeys.Num() == 1 ? RawTrack.PosKeys[0] : RawTrack.PosKeys[SampleIndex];
					ACLBone.translation_track.set_sample(SampleIndex, vector_cast(VectorCast(TranslationSample)));

					const FVector& ScaleSample = RawTrack.ScaleKeys.Num() == 0 ? UE4DefaultScale : (RawTrack.ScaleKeys.Num() == 1 ? RawTrack.ScaleKeys[0] : RawTrack.ScaleKeys[SampleIndex]);
					ACLBone.scale_track.set_sample(SampleIndex, vector_cast(VectorCast(ScaleSample)));
				}
			}
			else
			{
				// No track data for this bone, it must be new. Use the bind pose instead
				const RigidBone& ACLRigidBone = ACLSkeleton.get_bone(BoneIndex);

				for (uint32 SampleIndex = FirstSampleIndex; SampleIndex < NumSamples; ++SampleIndex)
				{
					ACLBone.rotation_track.set_sample(SampleIndex, ACLRigidBone.bind_transform.rotation);
					ACLBone.translation_track.set_sample(SampleIndex, ACLRigidBone.bind_transform.translation);
					ACLBone.scale_track.set_sample(SampleIndex, ACLDefaultScale);
				}
			}
		}

		return ACLClip;
	}
	else
	{
		// No animation sequence provided, use the bind pose instead
		check(!IsAdditive);

		const uint16 NumBones = ACLSkeleton.get_num_bones();
		const uint32 NumSamples = 1;
		const uint32 SampleRate = 30;
		const String ClipName(AllocatorImpl, "Bind Pose");

		TUniquePtr<AnimationClip> ACLClip = MakeUnique<AnimationClip>(AllocatorImpl, ACLSkeleton, NumSamples, SampleRate, ClipName);

		AnimatedBone* ACLBones = ACLClip->get_bones();
		for (uint16 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
		{
			// Get the bind transform and make sure it has no scale
			const RigidBone& skel_bone = ACLSkeleton.get_bone(BoneIndex);
			const Transform_64 bind_transform = transform_set(skel_bone.bind_transform.rotation, skel_bone.bind_transform.translation, ACLDefaultScale);

			ACLBones[BoneIndex].rotation_track.set_sample(0, bind_transform.rotation);
			ACLBones[BoneIndex].translation_track.set_sample(0, bind_transform.translation);
			ACLBones[BoneIndex].scale_track.set_sample(0, bind_transform.scale);
		}

		return ACLClip;
	}
}
#endif	// WITH_EDITOR
