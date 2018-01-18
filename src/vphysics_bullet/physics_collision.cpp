// Copyright Valve Corporation, All rights reserved.
// Bullet integration by Triang3l, derivative work, in public domain if detached from Valve's work.

#include "physics_collision.h"
#include "physics_object.h"
#include "mathlib/polyhedron.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// TODO: Cleanup the bbox cache when shutting down.
// Not sure what to do with it in thread contexts though.

static CPhysicsCollision s_PhysCollision;
CPhysicsCollision *g_pPhysCollision = &s_PhysCollision;

/*****************************
 * VCollide import structures
 *****************************/

#ifdef _X360
#pragma bitfield_order(push, lsb_to_msb)
#endif

struct VCollide_IVP_U_Float_Point {
	DECLARE_BYTESWAP_DATADESC();
	float k[3];
	float hesse_val;
};

struct VCollide_IVP_Compact_Edge {
	DECLARE_BYTESWAP_DATADESC();
	BEGIN_BITFIELD(bf)
	unsigned int start_point_index : 16;
	signed int opposite_index : 15;
	unsigned int is_virtual : 1;
	END_BITFIELD()
};

struct VCollide_IVP_Compact_Triangle {
	DECLARE_BYTESWAP_DATADESC();
	BEGIN_BITFIELD(bf)
	unsigned int tri_index : 12;
	unsigned int pierce_index : 12;
	unsigned int material_index : 7;
	unsigned int is_virtual : 1;
	END_BITFIELD()
	VCollide_IVP_Compact_Edge c_three_edges[3];
};

struct VCollide_IVP_Compact_Ledge {
	DECLARE_BYTESWAP_DATADESC();
	int c_point_offset;
	union {
		int ledgetree_node_offset;
		int client_data;
	};
	BEGIN_BITFIELD(bf)
	unsigned int has_children_flag : 2;
	unsigned int is_compact_flag : 2;
	unsigned int dummy : 4;
	unsigned int size_div_16 : 24;
	END_BITFIELD()
	short n_triangles;
	short for_future_use;

	FORCEINLINE int get_n_points() const {
		return size_div_16 - n_triangles - 1;
	}
};

struct VCollide_IVP_Compact_Ledgetree_Node {
	DECLARE_BYTESWAP_DATADESC();
	int offset_right_node;
	int offset_compact_ledge;
	float center[3];
	float radius;
	unsigned char box_sizes[3];
	unsigned char free_0;
};

struct VCollide_IVP_Compact_Surface {
	DECLARE_BYTESWAP_DATADESC();
	float mass_center[3];
	float rotation_inertia[3];
	float upper_limit_radius;
	BEGIN_BITFIELD(bf)
	unsigned int max_factor_surface_deviation : 8;
	int byte_size : 24;
	END_BITFIELD()
	int offset_ledgetree_root;
	int dummy[3];
};

#ifdef _X360
#pragma bitfield_order(pop)
#endif

BEGIN_BYTESWAP_DATADESC(VCollide_IVP_U_Float_Point)
	DEFINE_ARRAY(k, FIELD_FLOAT, 3),
	DEFINE_FIELD(hesse_val, FIELD_FLOAT)
END_BYTESWAP_DATADESC()

BEGIN_BYTESWAP_DATADESC(VCollide_IVP_Compact_Edge)
	DEFINE_BITFIELD(bf, FIELD_INTEGER, 32)
END_BYTESWAP_DATADESC()

BEGIN_BYTESWAP_DATADESC(VCollide_IVP_Compact_Triangle)
	DEFINE_BITFIELD(bf, FIELD_INTEGER, 32),
	DEFINE_EMBEDDED_ARRAY(c_three_edges, 3)
END_BYTESWAP_DATADESC()

BEGIN_BYTESWAP_DATADESC(VCollide_IVP_Compact_Ledge)
	DEFINE_FIELD(c_point_offset, FIELD_INTEGER),
	DEFINE_FIELD(ledgetree_node_offset, FIELD_INTEGER),
	DEFINE_BITFIELD(bf, FIELD_INTEGER, 32),
	DEFINE_FIELD(n_triangles, FIELD_SHORT),
	DEFINE_FIELD(for_future_use, FIELD_SHORT)
END_BYTESWAP_DATADESC()

BEGIN_BYTESWAP_DATADESC(VCollide_IVP_Compact_Ledgetree_Node)
	DEFINE_FIELD(offset_right_node, FIELD_INTEGER),
	DEFINE_FIELD(offset_compact_ledge, FIELD_INTEGER),
	DEFINE_ARRAY(center, FIELD_FLOAT, 3),
	DEFINE_FIELD(radius, FIELD_FLOAT)
END_BYTESWAP_DATADESC()

BEGIN_BYTESWAP_DATADESC(VCollide_IVP_Compact_Surface)
	DEFINE_ARRAY(mass_center, FIELD_FLOAT, 3),
	DEFINE_ARRAY(rotation_inertia, FIELD_FLOAT, 3),
	DEFINE_FIELD(upper_limit_radius, FIELD_FLOAT),
	DEFINE_BITFIELD(bf, FIELD_INTEGER, 32),
	DEFINE_FIELD(offset_ledgetree_root, FIELD_INTEGER),
	DEFINE_ARRAY(dummy, FIELD_INTEGER, 3)
END_BYTESWAP_DATADESC()

#define VCOLLIDE_IVP_COMPACT_SURFACE_ID MAKEID('I', 'V', 'P', 'S')

/****************************************
 * Utility for convexes and collideables
 ****************************************/

btVector3 CPhysicsCollision::BoxInertia(const btVector3 &extents) {
	btVector3 l2 = extents * extents;
	return (1.0f / 12.0f) * btVector3(l2.y() + l2.z(), l2.x() + l2.z(), l2.x() + l2.y());
}

btVector3 CPhysicsCollision::OffsetInertia(
		const btVector3 &inertia, const btVector3 &origin, bool absolute) {
	btScalar o2 = origin.length2();
	btVector3 newInertia = inertia + btVector3(o2, o2, o2) - (origin * origin);
	return absolute ? newInertia.absolute() : newInertia;
}

/***********
 * Convexes
 ***********/

void CPhysCollide::Initialize() {
	btCollisionShape *shape = GetShape();
	shape->setUserPointer(this);
	shape->setUserIndex(0);
}

float CPhysicsCollision::ConvexVolume(CPhysConvex *pConvex) {
	return (float) pConvex->GetVolume() * (BULLET2HL_FACTOR * BULLET2HL_FACTOR * BULLET2HL_FACTOR);
}

float CPhysicsCollision::ConvexSurfaceArea(CPhysConvex *pConvex) {
	return (float) pConvex->GetSurfaceArea() * (BULLET2HL_FACTOR * BULLET2HL_FACTOR);
}

void CPhysicsCollision::SetConvexGameData(CPhysConvex *pConvex, unsigned int gameData) {
	pConvex->GetShape()->setUserIndex((int) gameData);
}

void CPhysicsCollision::ConvexFree(CPhysConvex *pConvex) {
	if (pConvex->GetOwner() == CPhysConvex::OWNER_GAME) {
		delete pConvex;
	}
}

/***************
 * Convex hulls
 ***************/

void CPhysConvex_Hull::Initialize() {
	CPhysConvex::Initialize();
	m_Volume = -1.0;
	m_Shape.setMargin(VPHYSICS_CONVEX_DISTANCE_MARGIN);
}

CPhysConvex_Hull::CPhysConvex_Hull(const btVector3 *points, int pointCount,
		const unsigned int *indices, int triangleCount) :
		m_Shape(&points[0][0], pointCount) {
	Initialize();
	int indexCount = triangleCount * 3;
	m_TriangleIndices.resizeNoInitialize(indexCount);
	memcpy(&m_TriangleIndices[0], indices, indexCount * sizeof(indices[0]));
}

CPhysConvex_Hull::CPhysConvex_Hull(const VCollide_IVP_Compact_Ledge *ledge, CByteswap &byteswap,
		const btVector3 *ledgePoints, int ledgePointCount) :
		m_Shape(&ledgePoints[0][0], ledgePointCount) {
	Initialize();
	m_Shape.setUserIndex(ledge->client_data);
	VCollide_IVP_Compact_Ledge swappedLedge;
	byteswap.SwapBufferToTargetEndian(&swappedLedge, const_cast<VCollide_IVP_Compact_Ledge *>(ledge));
	const VCollide_IVP_Compact_Triangle *triangles =
			reinterpret_cast<const VCollide_IVP_Compact_Triangle *>(ledge + 1);
	int triangleCount = swappedLedge.n_triangles;
	m_TriangleIndices.resizeNoInitialize(triangleCount * 3);
	unsigned int *indices = &m_TriangleIndices[0];
	for (int triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex) {
		VCollide_IVP_Compact_Triangle swappedTriangle;
		byteswap.SwapBufferToTargetEndian(&swappedTriangle,
				const_cast<VCollide_IVP_Compact_Triangle *>(&triangles[triangleIndex]));
		int indexIndex = triangleIndex * 3;
		indices[indexIndex] = swappedTriangle.c_three_edges[0].start_point_index;
		indices[indexIndex + 1] = swappedTriangle.c_three_edges[1].start_point_index;
		indices[indexIndex + 2] = swappedTriangle.c_three_edges[2].start_point_index;
		if (swappedTriangle.material_index > 0) {
			if (m_TriangleMaterials.size() == 0) {
				m_TriangleMaterials.resizeNoInitialize(triangleCount);
				memset(&m_TriangleMaterials[0], 0, triangleCount * sizeof(m_TriangleMaterials[0]));
			}
			m_TriangleMaterials[triangleIndex] = swappedTriangle.material_index;
		}
	}
	if (m_TriangleMaterials.size() != 0) {
		CalculateTrianglePlanes();
	}
}

CPhysConvex_Hull *CPhysConvex_Hull::CreateFromBulletPoints(
		HullLibrary &hullLibrary, const btVector3 *points, int pointCount) {
	if (pointCount < 3) {
		return nullptr;
	}
	HullResult hull;
	HullError hullError = hullLibrary.CreateConvexHull(
			HullDesc(QF_TRIANGLES, pointCount, points), hull);
	if (hullError != QE_OK || hull.mNumFaces == 0) {
		AssertMsg(false, "Convex hull creation failed");
		return nullptr;
	}
	return new CPhysConvex_Hull(&hull.m_OutputVertices[0], hull.mNumOutputVertices,
			&hull.m_Indices[0], hull.mNumFaces);
}

CPhysConvex_Hull *CPhysConvex_Hull::CreateFromIVPCompactLedge(
		const VCollide_IVP_Compact_Ledge *ledge, CByteswap &byteswap) {
	VCollide_IVP_Compact_Ledge swappedLedge;
	byteswap.SwapBufferToTargetEndian(&swappedLedge, const_cast<VCollide_IVP_Compact_Ledge *>(ledge));
	const VCollide_IVP_U_Float_Point *ivpPoints = reinterpret_cast<const VCollide_IVP_U_Float_Point *>(
			reinterpret_cast<const byte *>(ledge) + swappedLedge.c_point_offset);
	btAlignedObjectArray<btVector3> pointArray;
	int pointCount = swappedLedge.get_n_points();
	if (pointCount < 3 || swappedLedge.n_triangles <= 0) {
		return nullptr;
	}
	pointArray.resizeNoInitialize(pointCount);
	btVector3 *points = &pointArray[0];
	for (int pointIndex = 0; pointIndex < pointCount; ++pointIndex) {
		VCollide_IVP_U_Float_Point swappedPoint;
		byteswap.SwapBufferToTargetEndian(&swappedPoint, const_cast<VCollide_IVP_U_Float_Point *>(&ivpPoints[pointIndex]));
		points[pointIndex].setValue(swappedPoint.k[0], -swappedPoint.k[1], -swappedPoint.k[2]);
	}
	return new CPhysConvex_Hull(ledge, byteswap, points, pointCount);
}

void CPhysConvex_Hull::CalculateVolumeProperties() {
	if (m_Volume >= 0.0f) {
		return;
	}
	// Based on btConvexTriangleMeshShape::calculatePrincipalAxisTransform, but without rotation.
	const btVector3 *points = &m_Shape.getPoints[0][0];
	const unsigned int *indices = &m_TriangleIndices[0];
	const btVector3 &ref = points[indices[0]];
	int indexCount = m_TriangleIndices.size();
	btScalar sixVolume = 0.0f;
	btVector3 massCenterSum(0.0f, 0.0f, 0.0f);
	for (int indexIndex = 3; indexIndex < indexCount; indexIndex += 3) {
		const btVector3 &p0 = points[indices[indexIndex]];
		const btVector3 &p1 = points[indices[indexIndex + 1]];
		const btVector3 &p2 = points[indices[indexIndex + 2]];
		btScalar tetrahedronSixVolume = btFabs((p0 - ref).triple(p1 - ref, p2 - ref));
		sixVolume += tetrahedronSixVolume;
		massCenterSum += (0.25f * tetrahedronSixVolume) * (p0 + p1 + p2 + ref);
	}
	m_Volume = (1.0f / 6.0f) * sixVolume;
	if (m_Volume > 0.0f) {
		m_MassCenter = massCenterSum / sixVolume;
		m_Inertia.setZero();
		for (int indexIndex = 0; indexIndex < indexCount; indexIndex += 3) {
			btVector3 a = points[indices[indexIndex]] - m_MassCenter;
			btVector3 b = points[indices[indexIndex + 1]] - m_MassCenter;
			btVector3 c = points[indices[indexIndex + 2]] - m_MassCenter;
			btVector3 i = btFabs(a.triple(b, c)) * (0.1f / 6.0f) *
					(a * a + b * b + c * c + a * b + a * c + b * c);
			m_Inertia[0] += i[1] + i[2];
			m_Inertia[1] += i[2] + i[0];
			m_Inertia[2] += i[0] + i[1];
		}
		m_Inertia = (m_Inertia / m_Volume).absolute();
	} else {
		// Use a box approximation.
		btVector3 aabbMin, aabbMax;
		m_Shape.getAabb(btTransform::getIdentity(), aabbMin, aabbMax);
		m_MassCenter = (aabbMin + aabbMax) * 0.5f;
		m_Inertia = CPhysicsCollision::OffsetInertia(
				CPhysicsCollision::BoxInertia(aabbMax - aabbMin), m_MassCenter);
	}
}

btScalar CPhysConvex_Hull::GetVolume() const {
	const_cast<CPhysConvex_Hull *>(this)->CalculateVolumeProperties();
	return m_Volume;
}

btScalar CPhysConvex_Hull::GetSurfaceArea() const {
	const btVector3 *points = m_Shape.getPoints();
	const unsigned int *indices = &m_TriangleIndices[0];
	int indexCount = m_TriangleIndices.size();
	btScalar area = 0.0f;
	for (int indexIndex = 0; indexIndex < indexCount; indexIndex += 3) {
		const btVector3 &p0 = points[indices[indexIndex]];
		const btVector3 &p1 = points[indices[indexIndex + 1]];
		const btVector3 &p2 = points[indices[indexIndex + 2]];
		area += (p1 - p0).cross(p2 - p0).length();
	}
	return 0.5f * area;
}

btVector3 CPhysConvex_Hull::GetMassCenter() const {
	const_cast<CPhysConvex_Hull *>(this)->CalculateVolumeProperties();
	return m_MassCenter;
}

btVector3 CPhysConvex_Hull::GetInertia() const {
	const_cast<CPhysConvex_Hull *>(this)->CalculateVolumeProperties();
	return m_Inertia;
}

// This is a hack, gives per-plane surface index, not per-triangle,
// as Bullet doesn't do per-triangle collision detection for convexes.
// However, per-triangle materials are used only by world brushes,
// which can't have coplanar triangles with different materials.
// It also assumes the contact point is very close to the surface.
int CPhysConvex_Hull::GetTriangleMaterialIndex(const btVector3 &point) const {
	int triangleCount = m_TriangleMaterials.size();
	if (triangleCount == 0) {
		return 0;
	}
	const btVector4 *planes = &m_TrianglePlanes[0];
	int closestTriangle = 0;
	btScalar closestTriangleDistance = btFabs(planes[0].dot(point) - planes[0].getW());
	for (int triangleIndex = 1; triangleCount < triangleIndex; ++triangleIndex) {
		const btVector4 &plane = planes[triangleIndex];
		btScalar distance = btFabs(plane.dot(point) - plane.getW());
		if (distance < closestTriangleDistance) {
			closestTriangleDistance = distance;
			closestTriangle = triangleIndex;
		}
	}
	return m_TriangleMaterials[closestTriangle];
}

void CPhysConvex_Hull::CalculateTrianglePlanes() {
	if (m_TrianglePlanes.size() != 0) {
		return;
	}
	int triangleCount = m_TriangleIndices.size() / 3;
	m_TrianglePlanes.resizeNoInitialize(triangleCount);
	btVector4 *planes = &m_TrianglePlanes[0];
	const btVector3 *points = &m_Shape.getPoints[0][0];
	const unsigned int *indices = &m_TriangleIndices[0];
	for (int triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex) {
		int indexIndex = triangleIndex * 3;
		const btVector3 &v1 = points[indices[indexIndex]];
		btVector3 normal = points[indices[indexIndex + 1]].cross(points[indices[indexIndex + 2]]);
		normal.normalize();
		planes[triangleIndex].setValue(normal.getX(), normal.getY(), normal.getZ(), v1.dot(normal));
	}
}

CPhysConvex *CPhysicsCollision::ConvexFromVerts(Vector **pVerts, int vertCount) {
	btAlignedObjectArray<btVector3> pointArray;
	pointArray.resizeNoInitialize(vertCount);
	btVector3 *points = &pointArray[0];
	for (int vertIndex = 0; vertIndex < vertCount; ++vertIndex) {
		ConvertPositionToBullet(*pVerts[vertIndex], points[vertIndex]);
	}
	return CPhysConvex_Hull::CreateFromBulletPoints(m_HullLibrary, points, vertCount);
}

CPhysConvex *CPhysicsCollision::ConvexFromConvexPolyhedron(const CPolyhedron &ConvexPolyhedron) {
	const Vector *verts = ConvexPolyhedron.pVertices;
	int vertCount = ConvexPolyhedron.iVertexCount;
	btAlignedObjectArray<btVector3> pointArray;
	pointArray.resizeNoInitialize(vertCount);
	btVector3 *points = &pointArray[0];
	for (int vertIndex = 0; vertIndex < vertCount; ++vertIndex) {
		ConvertPositionToBullet(verts[vertIndex], points[vertIndex]);
	}
	return CPhysConvex_Hull::CreateFromBulletPoints(m_HullLibrary, points, vertCount);
}

/***********************************************
 * Bounding boxes (both convex and collideable)
 ***********************************************/

CPhysConvex_Box::CPhysConvex_Box(const btVector3 &halfExtents, const btVector3 &origin) :
		m_Shape(halfExtents), m_Origin(origin) {
	Initialize();
	m_Shape.setMargin(VPHYSICS_CONVEX_DISTANCE_MARGIN);
}

btScalar CPhysConvex_Box::GetVolume() const {
	const btVector3 &halfExtents = m_Shape.getHalfExtentsWithoutMargin();
	return 8.0f * halfExtents.getX() * halfExtents.getY() * halfExtents.getZ();
}

btScalar CPhysConvex_Box::GetSurfaceArea() const {
	const btVector3 &halfExtents = m_Shape.getHalfExtentsWithoutMargin();
	return 8.0f * halfExtents.getX() * halfExtents.getY() +
			4.0 * halfExtents.getZ() * (halfExtents.getX() + halfExtents.getY());
}

btVector3 CPhysConvex_Box::GetInertia() const {
	return CPhysicsCollision::BoxInertia(2.0f * m_Shape.getHalfExtentsWithoutMargin());
}

CPhysCollide_Compound *CPhysicsCollision::CreateBBox(const Vector &mins, const Vector &maxs) {
	if (mins == maxs) {
		return nullptr;
	}

	btVector3 bulletMins, bulletMaxs;
	ConvertPositionToBullet(mins, bulletMins);
	ConvertPositionToBullet(maxs, bulletMaxs);
	btVector3 halfExtents = (bulletMaxs - bulletMins).absolute() * 0.5f;
	btVector3 origin = (bulletMins + bulletMaxs) * 0.5f;

	const btScalar threshold = HL2BULLET(0.1f);

	for (int bboxIndex = m_BBoxCache.Count() - 1; bboxIndex >= 0; --bboxIndex) {
		CPhysCollide_Compound *cacheCompound = m_BBoxCache[bboxIndex];
		const CPhysConvex_Box *cacheBox = static_cast<const CPhysConvex_Box *>(
				reinterpret_cast<const CPhysConvex *>(
						cacheCompound->GetCompoundShape()->getChildShape(0)->getUserPointer()));
		const btVector3 &cacheHalfExtents = cacheBox->GetBoxShape()->getHalfExtentsWithoutMargin();
		const btVector3 &cacheOrigin = cacheBox->GetOriginInCompound();
		for (int component = 0; component < 3; ++component) {
			if (btFabs(cacheHalfExtents[component] - halfExtents[component]) > threshold ||
					btFabs(cacheOrigin[component] - origin[component]) > threshold) {
				cacheCompound = nullptr;
				break;
			}
			if (cacheCompound != nullptr) {
				return cacheCompound;
			}
		}
	}

	CPhysConvex *box = new CPhysConvex_Box(halfExtents, origin);
	box->SetOwner(CPhysConvex::OWNER_INTERNAL);
	CPhysCollide_Compound *compound = new CPhysCollide_Compound(&box, 1);
	compound->SetOwner(CPhysCollide::OWNER_INTERNAL);
	m_BBoxCache.AddToTail(compound);
	return compound;
}

CPhysConvex *CPhysicsCollision::BBoxToConvex(const Vector &mins, const Vector &maxs) {
	CPhysCollide_Compound *compound = CreateBBox(mins, maxs);
	if (compound == nullptr) {
		return nullptr;
	}
	return reinterpret_cast<CPhysConvex *>(
			compound->GetCompoundShape()->getChildShape(0)->getUserPointer());
}

CPhysCollide *CPhysicsCollision::BBoxToCollide(const Vector &mins, const Vector &maxs) {
	return CreateBBox(mins, maxs);
}

/***************
 * Collideables
 ***************/

void CPhysCollide::RemoveObjectReference(IPhysicsObject *object) {
	if (m_ObjectReferenceList == object) {
		m_ObjectReferenceList = static_cast<CPhysicsObject *>(object)->GetNextCollideObject();
		if (m_ObjectReferenceList == object) {
			m_ObjectReferenceList = nullptr;
		}
	}
}

float CPhysicsCollision::CollideVolume(CPhysCollide *pCollide) {
	return (float) pCollide->GetVolume() * (BULLET2HL_FACTOR * BULLET2HL_FACTOR * BULLET2HL_FACTOR);
}

float CPhysicsCollision::CollideSurfaceArea(CPhysCollide *pCollide) {
	return (float) pCollide->GetSurfaceArea() * (BULLET2HL_FACTOR * BULLET2HL_FACTOR);
}

void CPhysicsCollision::CollideGetMassCenter(CPhysCollide *pCollide, Vector *pOutMassCenter) {
	ConvertPositionToHL(pCollide->GetMassCenter(), *pOutMassCenter);
}

void CPhysicsCollision::CollideSetMassCenter(CPhysCollide *pCollide, const Vector &massCenter) {
	btVector3 bulletMassCenter;
	ConvertPositionToBullet(massCenter, bulletMassCenter);
	pCollide->SetMassCenter(bulletMassCenter);
}

void CPhysCollide::NotifyObjectsOfMassCenterChange(const btVector3 &oldMassCenter) {
	IPhysicsObject *firstObject = GetObjectReferenceList();
	if (firstObject != nullptr) {
		CPhysicsObject *object = static_cast<CPhysicsObject *>(firstObject);
		do {
			object->NotifyMassCenterChanged(oldMassCenter);
			object = object->GetNextCollideObject();
		} while (object != firstObject);
	}
}

int CPhysicsCollision::CollideIndex(const CPhysCollide *pCollide) {
	return pCollide->GetShape()->getUserIndex();
}

void CPhysicsCollision::SetCollideIndex(CPhysCollide *pCollide, int index) {
	pCollide->GetShape()->setUserIndex(index);
}

/******************
 * Compound shapes
 ******************/

CPhysCollide_Compound::CPhysCollide_Compound(CPhysConvex **pConvex, int convexCount) :
		m_Shape(convexCount > 1, convexCount) {
	Assert(convexCount > 0);

	Initialize();

	// Calculate volume and center of mass.
	m_Volume = 0.0f;
	m_MassCenter.setZero();
	for (int convexIndex = 0; convexIndex < convexCount; ++convexIndex) {
		const CPhysConvex *convex = pConvex[convexIndex];
		btScalar convexVolume = convex->GetVolume();
		m_Volume += convexVolume;
		m_MassCenter += (convex->GetOriginInCompound() + convex->GetMassCenter()) * convexVolume;
	}
	if (m_Volume > 0.0f) {
		m_MassCenter /= m_Volume;
	} else {
		btVector3 aabbMin(BT_LARGE_FLOAT, BT_LARGE_FLOAT, BT_LARGE_FLOAT);
		btVector3 aabbMax(-BT_LARGE_FLOAT, -BT_LARGE_FLOAT, -BT_LARGE_FLOAT);
		const btTransform &convexAabbTransform = btTransform::getIdentity();
		for (int convexIndex = 0; convexIndex < convexCount; ++convexIndex) {
			const CPhysConvex *convex = pConvex[convexIndex];
			btVector3 convexAabbMin, convexAabbMax;
			convex->GetShape()->getAabb(convexAabbTransform, convexAabbMin, convexAabbMax);
			const btVector3 &convexOrigin = convex->GetOriginInCompound();
			aabbMin.setMin(convexAabbMin + convexOrigin);
			aabbMax.setMax(convexAabbMax + convexOrigin);
		}
		m_MassCenter = (aabbMin + aabbMax) * 0.5f;
	}

	btTransform transform(btMatrix3x3::getIdentity());
	for (int convexIndex = 0; convexIndex < convexCount; ++convexIndex) {
		CPhysConvex *convex = pConvex[convexIndex];
		if (convex->GetOwner() == CPhysConvex::OWNER_GAME) {
			convex->SetOwner(CPhysConvex::OWNER_COMPOUND);
		}
		transform.setOrigin(convex->GetOriginInCompound() - m_MassCenter);
		m_Shape.addChildShape(transform, convex->GetShape());
	}

	CalculateInertia();
}

CPhysCollide_Compound::CPhysCollide_Compound(
		const VCollide_IVP_Compact_Ledgetree_Node *root, CByteswap &byteswap,
		const btVector3 &massCenter, const btVector3 &inertia) :
		m_Shape(root->offset_right_node != 0 /* Swap not required */),
		m_Volume(-1.0f), m_MassCenter(massCenter), m_Inertia(inertia) {
	AddIVPCompactLedgetreeNode(root, byteswap);
}

void CPhysCollide_Compound::AddIVPCompactLedgetreeNode(
		const VCollide_IVP_Compact_Ledgetree_Node *node, CByteswap &byteswap) {
	VCollide_IVP_Compact_Ledgetree_Node swappedNode;
	byteswap.SwapBufferToTargetEndian(&swappedNode, const_cast<VCollide_IVP_Compact_Ledgetree_Node *>(node));
	if (swappedNode.offset_right_node == 0) {
		CPhysConvex_Hull *convex = CPhysConvex_Hull::CreateFromIVPCompactLedge(
				reinterpret_cast<const VCollide_IVP_Compact_Ledge *>(
						reinterpret_cast<const byte *>(node) + swappedNode.offset_compact_ledge), byteswap);
		convex->SetOwner(CPhysConvex::OWNER_COMPOUND);
		m_Shape.addChildShape(btTransform(btMatrix3x3::getIdentity(),
				convex->GetOriginInCompound() - m_MassCenter), convex->GetShape());
	} else {
		AddIVPCompactLedgetreeNode(node + 1, byteswap);
		AddIVPCompactLedgetreeNode(reinterpret_cast<const VCollide_IVP_Compact_Ledgetree_Node *>(
						reinterpret_cast<const byte *>(node) + swappedNode.offset_right_node), byteswap);
	}
}

CPhysCollide *CPhysicsCollision::ConvertConvexToCollide(CPhysConvex **pConvex, int convexCount) {
	if (convexCount == 0 || pConvex == nullptr) {
		return nullptr;
	}
	return new CPhysCollide_Compound(pConvex, convexCount);
}

btScalar CPhysCollide_Compound::GetVolume() const {
	if (m_Volume < 0.0f) {
		btScalar &volume = const_cast<CPhysCollide_Compound *>(this)->m_Volume;
		volume = 0.0f;
		int childCount = m_Shape.getNumChildShapes();
		for (int childIndex = 0; childIndex < childCount; ++childIndex) {
			volume += reinterpret_cast<const CPhysConvex *>(
					m_Shape.getChildShape(childIndex)->getUserPointer())->GetVolume();
		}
	}
	return m_Volume;
}

btScalar CPhysCollide_Compound::GetSurfaceArea() const {
	btScalar area = 0.0f;
	int childCount = m_Shape.getNumChildShapes();
	for (int childIndex = 0; childIndex < childCount; ++childIndex) {
		area += reinterpret_cast<const CPhysConvex *>(
				m_Shape.getChildShape(childIndex)->getUserPointer())->GetSurfaceArea();
	}
	return area;
}

void CPhysCollide_Compound::SetMassCenter(const btVector3 &massCenter) {
	btVector3 oldMassCenter = m_MassCenter;
	m_MassCenter = massCenter;
	int childCount = m_Shape.getNumChildShapes();
	for (int childIndex = 0; childIndex < childCount; ++childIndex) {
		btTransform childTransform = m_Shape.getChildTransform(childIndex);
		childTransform.setOrigin(reinterpret_cast<const CPhysConvex *>(
				m_Shape.getChildShape(childIndex)->getUserPointer())->GetOriginInCompound() - massCenter);
		m_Shape.updateChildTransform(childIndex, childTransform, false);
	}
	m_Shape.recalculateLocalAabb();
	CalculateInertia();
	NotifyObjectsOfMassCenterChange(oldMassCenter);
}

void CPhysCollide_Compound::CalculateInertia() {
	btScalar volume = GetVolume(); // Not necessarily precalculated.
	if (volume >= 0) {
		m_Inertia.setZero();
		int childCount = m_Shape.getNumChildShapes();
		for (int childIndex = 0; childIndex < childCount; ++childIndex) {
			const CPhysConvex *convex = reinterpret_cast<const CPhysConvex *>(
					m_Shape.getChildShape(childIndex)->getUserPointer());
			m_Inertia += convex->GetVolume() * CPhysicsCollision::OffsetInertia(
					convex->GetInertia(), m_Shape.getChildTransform(childIndex).getOrigin(), false);
		}
		m_Inertia = (m_Inertia / volume).absolute();
	} else {
		btVector3 aabbMin, aabbMax;
		m_Shape.getAabb(btTransform::getIdentity(), aabbMin, aabbMax);
		m_Inertia = CPhysicsCollision::OffsetInertia(
				CPhysicsCollision::BoxInertia(aabbMax - aabbMin),
				(aabbMin + aabbMax) * 0.5f);
	}
}

/**********
 * Spheres
 **********/

btScalar CPhysCollide_Sphere::GetVolume() const {
	btScalar radius = m_Shape.getRadius();
	return ((4.0f / 3.0f) * SIMD_PI) * radius * radius * radius;
}

btScalar CPhysCollide_Sphere::GetSurfaceArea() const {
	btScalar radius = m_Shape.getRadius();
	return (4.0f * SIMD_PI) * radius * radius;
}

btVector3 CPhysCollide_Sphere::GetInertia() const {
	btScalar elem = m_Shape.getRadius();
	elem *= elem * 0.4;
	return btVector3(elem, elem, elem);
}

CPhysCollide_Sphere *CPhysicsCollision::CreateSphereCollide(btScalar radius) {
	return new CPhysCollide_Sphere(radius);
}
