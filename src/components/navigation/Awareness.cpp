/*
 Copyright (C) 2014 Erik Ogenvik

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "Awareness.h"
#include "fastlz.h"

#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "DetourNavMeshBuilder.h"
#include "DetourTileCache.h"
#include "DetourTileCacheBuilder.h"
#include "DetourCommon.h"
#include "DetourObstacleAvoidance.h"

#include "domain/IHeightProvider.h"
#include "domain/EmberEntity.h"

#include "framework/LoggingInstance.h"

#include <Eris/View.h>
#include <Eris/Avatar.h>
#include <Eris/Entity.h>
#include <wfmath/wfmath.h>

#include <Atlas/Message/Element.h>

#include <sigc++/bind.h>

#include <cmath>
#include <vector>
#include <cstring>
#include <queue>

#define MAX_PATHPOLY      256 // max number of polygons in a path
#define MAX_PATHVERT      512 // most verts in a path
#define MAX_OBSTACLES_CIRCLES 4 // max number of circle obstacles to consider when doing avoidance
namespace Ember
{
namespace Navigation
{

// This value specifies how many layers (or "floors") each navmesh tile is expected to have.
static const int EXPECTED_LAYERS_PER_TILE = 1;

struct FastLZCompressor: public dtTileCacheCompressor
{
	virtual ~FastLZCompressor()
	{
	}

	virtual int maxCompressedSize(const int bufferSize)
	{
		return (int)(bufferSize * 1.05f);
	}

	virtual dtStatus compress(const unsigned char* buffer, const int bufferSize, unsigned char* compressed, const int /*maxCompressedSize*/, int* compressedSize)
	{
		*compressedSize = fastlz_compress((const void * const )buffer, bufferSize, compressed);
		return DT_SUCCESS;
	}

	virtual dtStatus decompress(const unsigned char* compressed, const int compressedSize, unsigned char* buffer, const int maxBufferSize, int* bufferSize)
	{
		*bufferSize = fastlz_decompress(compressed, compressedSize, buffer, maxBufferSize);
		return *bufferSize < 0 ? DT_FAILURE : DT_SUCCESS;
	}
};

struct LinearAllocator: public dtTileCacheAlloc
{
	unsigned char* buffer;
	int capacity;
	int top;
	int high;

	LinearAllocator(const int cap) :
			buffer(0), capacity(0), top(0), high(0)
	{
		resize(cap);
	}

	virtual ~LinearAllocator()
	{
		dtFree(buffer);
	}

	void resize(const int cap)
	{
		if (buffer)
			dtFree(buffer);
		buffer = (unsigned char*)dtAlloc(cap, DT_ALLOC_PERM);
		capacity = cap;
	}

	virtual void reset()
	{
		high = dtMax(high, top);
		top = 0;
	}

	virtual void* alloc(const int size)
	{
		if (!buffer)
			return 0;
		if (top + size > capacity)
			return 0;
		unsigned char* mem = &buffer[top];
		top += size;
		return mem;
	}

	virtual void free(void* /*ptr*/)
	{
		// Empty
	}
};

struct MeshProcess: public dtTileCacheMeshProcess
{

	inline MeshProcess()
	{
	}

	virtual ~MeshProcess()
	{
	}

	virtual void process(struct dtNavMeshCreateParams* params, unsigned char* polyAreas, unsigned short* polyFlags)
	{
		// Update poly flags from areas.
		for (int i = 0; i < params->polyCount; ++i) {
			if (polyAreas[i] == DT_TILECACHE_WALKABLE_AREA)
				polyAreas[i] = POLYAREA_GROUND;

			if (polyAreas[i] == POLYAREA_GROUND || polyAreas[i] == POLYAREA_GRASS || polyAreas[i] == POLYAREA_ROAD) {
				polyFlags[i] = POLYFLAGS_WALK;
			} else if (polyAreas[i] == POLYAREA_WATER) {
				polyFlags[i] = POLYFLAGS_SWIM;
			} else if (polyAreas[i] == POLYAREA_DOOR) {
				polyFlags[i] = POLYFLAGS_WALK | POLYFLAGS_DOOR;
			}
		}
	}
};

struct TileCacheData
{
	unsigned char* data;
	int dataSize;
};

static const int MAX_LAYERS = 32;

struct RasterizationContext
{
	RasterizationContext() :
			solid(0), triareas(0), lset(0), chf(0), ntiles(0)
	{
		memset(tiles, 0, sizeof(TileCacheData) * MAX_LAYERS);
	}

	~RasterizationContext()
	{
		rcFreeHeightField(solid);
		delete[] triareas;
		rcFreeHeightfieldLayerSet(lset);
		rcFreeCompactHeightfield(chf);
		for (int i = 0; i < MAX_LAYERS; ++i) {
			dtFree(tiles[i].data);
			tiles[i].data = 0;
		}
	}

	rcHeightfield* solid;
	unsigned char* triareas;
	rcHeightfieldLayerSet* lset;
	rcCompactHeightfield* chf;
	TileCacheData tiles[MAX_LAYERS];
	int ntiles;
};

struct InputGeometry
{
	std::vector<float> verts;
	std::vector<int> tris;
	std::vector<WFMath::RotBox<2>> entityAreas;
};

class AwarenessContext: public rcContext
{
protected:
	virtual void doLog(const rcLogCategory category, const char* msg, const int len)
	{
		if (category == RC_LOG_PROGRESS) {
			S_LOG_VERBOSE("Recast: " << msg);
		} else if (category == RC_LOG_WARNING) {
			S_LOG_WARNING("Recast: " << msg);
		} else {
			S_LOG_FAILURE("Recast: " << msg);
		}
	}

};

Awareness::Awareness(Eris::View& view, IHeightProvider& heightProvider) :
		mView(view), mHeightProvider(heightProvider), mAvatarEntity(view.getAvatar()->getEntity()), mCurrentLocation(mAvatarEntity->getLocation()), mAvatarRadius(0.4f), m_ctx(new AwarenessContext()), m_tileCache(nullptr), m_navMesh(nullptr), m_navQuery(dtAllocNavMeshQuery()), mFilter(nullptr)
{

	m_talloc = new LinearAllocator(128000);
	m_tcomp = new FastLZCompressor;
	m_tmproc = new MeshProcess;

	// Setup the default query filter
	mFilter = new dtQueryFilter();
	mFilter->setIncludeFlags(0xFFFF);    // Include all
	mFilter->setExcludeFlags(0);         // Exclude none
	// Area flags for polys to consider in search, and their cost
	mFilter->setAreaCost(POLYAREA_GROUND, 1.0f);

	//height of our agent, default to 2 meters unless there's a bbox
	float h = 2.0f;

	if (mAvatarEntity->hasBBox() && mAvatarEntity->getBBox().isValid()) {
		auto avatarBbox = mAvatarEntity->getBBox();
		//get the radius from the avatar entity
		WFMath::AxisBox<2> avatar2dBbox(WFMath::Point<2>(avatarBbox.lowCorner().x(), avatarBbox.lowCorner().y()), WFMath::Point<2>(avatarBbox.highCorner().x(), avatarBbox.highCorner().y()));
		mAvatarRadius = std::max(0.2f, avatar2dBbox.boundingSphere().radius()); //Don't make the radius smaller than 0.2 meters, to avoid too many cells

		//height of our agent
		h = avatarBbox.highCorner().z() - avatarBbox.lowCorner().z();
	}

	auto extent = view.getTopLevel()->getBBox();
	const WFMath::Point<3>& lower = extent.lowCorner();
	const WFMath::Point<3>& upper = extent.highCorner();

	//Recast uses y for the vertical axis
	m_cfg.bmin[0] = lower.x();
	m_cfg.bmin[2] = lower.y();
	m_cfg.bmin[1] = std::min(-500.f, lower.z());
	m_cfg.bmax[0] = upper.x();
	m_cfg.bmax[2] = upper.y();
	m_cfg.bmax[1] = std::max(500.f, upper.z());

	int gw = 0, gh = 0;
	float cellsize = mAvatarRadius / 2.0f; //Should be enough for outdoors; indoors we might want r / 3.0 instead
	rcCalcGridSize(m_cfg.bmin, m_cfg.bmax, cellsize, &gw, &gh);
	const int tilesize = 80; //This equals 16 meters
	const int tilewidth = (gw + tilesize - 1) / tilesize;
	const int tileheight = (gh + tilesize - 1) / tilesize;

	// Max tiles and max polys affect how the tile IDs are caculated.
	// There are 22 bits available for identifying a tile and a polygon.
	int tileBits = rcMin((int)dtIlog2(dtNextPow2(tilewidth * tileheight * EXPECTED_LAYERS_PER_TILE)), 14);
	if (tileBits > 14)
		tileBits = 14;
	int polyBits = 22 - tileBits;
	unsigned int maxTiles = 1 << tileBits;
	unsigned int maxPolysPerTile = 1 << polyBits;

	//For an explanation of these values see http://digestingduck.blogspot.se/2009/08/recast-settings-uncovered.html

	m_cfg.cs = cellsize;
	m_cfg.ch = m_cfg.cs / 2.0f; //Height of one voxel; should really only come into play when doing 3d traversal
//	m_cfg.ch = std::max(upper.z() - lower.z(), 100.0f); //For 2d traversal make the voxel size as large as possible
	m_cfg.walkableHeight = std::ceil(h / m_cfg.ch); //This is in voxels
	m_cfg.walkableClimb = 100; //TODO: implement proper system for limiting climbing; for now just use a large voxel number
	m_cfg.walkableRadius = std::ceil(mAvatarRadius / m_cfg.cs);
	m_cfg.walkableSlopeAngle = 70; //TODO: implement proper system for limiting climbing; for now just use 70 degrees

	m_cfg.maxEdgeLen = m_cfg.walkableRadius * 8.0f;
	m_cfg.maxSimplificationError = 1.3f;
	m_cfg.minRegionArea = (int)rcSqr(8);
	m_cfg.mergeRegionArea = (int)rcSqr(20);

	m_cfg.tileSize = tilesize;
	m_cfg.borderSize = m_cfg.walkableRadius + 3; // Reserve enough padding.
	m_cfg.width = m_cfg.tileSize + m_cfg.borderSize * 2;
	m_cfg.height = m_cfg.tileSize + m_cfg.borderSize * 2;
//	m_cfg.detailSampleDist = m_detailSampleDist < 0.9f ? 0 : m_cfg.cs * m_detailSampleDist;
//	m_cfg.detailSampleMaxError = m_cfg.m_cellHeight * m_detailSampleMaxError;

	// Tile cache params.
	dtTileCacheParams tcparams;
	memset(&tcparams, 0, sizeof(tcparams));
	rcVcopy(tcparams.orig, m_cfg.bmin);
	tcparams.cs = m_cfg.cs;
	tcparams.ch = m_cfg.ch;
	tcparams.width = (int)m_cfg.tileSize;
	tcparams.height = (int)m_cfg.tileSize;
	tcparams.walkableHeight = h;
	tcparams.walkableRadius = mAvatarRadius;
	tcparams.walkableClimb = m_cfg.walkableClimb;
//	tcparams.maxSimplificationError = m_edgeMaxError;
	tcparams.maxTiles = tilewidth * tileheight * EXPECTED_LAYERS_PER_TILE;
	tcparams.maxObstacles = 128;

	dtFreeTileCache(m_tileCache);

	dtStatus status;

	m_tileCache = dtAllocTileCache();
	if (!m_tileCache) {
		m_ctx->log(RC_LOG_ERROR, "buildTiledNavigation: Could not allocate tile cache.");
		return;
	}
	status = m_tileCache->init(&tcparams, m_talloc, m_tcomp, m_tmproc);
	if (dtStatusFailed(status)) {
		m_ctx->log(RC_LOG_ERROR, "buildTiledNavigation: Could not init tile cache.");
		return;
	}

	dtFreeNavMesh(m_navMesh);

	m_navMesh = dtAllocNavMesh();
	if (!m_navMesh) {
		m_ctx->log(RC_LOG_ERROR, "buildTiledNavigation: Could not allocate navmesh.");
		return;
	}

	dtNavMeshParams params;
	memset(&params, 0, sizeof(params));
	rcVcopy(params.orig, m_cfg.bmin);
	params.tileWidth = tilesize * cellsize;
	params.tileHeight = tilesize * cellsize;
	params.maxTiles = maxTiles;
	params.maxPolys = maxPolysPerTile;

	status = m_navMesh->init(&params);
	if (dtStatusFailed(status)) {
		m_ctx->log(RC_LOG_ERROR, "buildTiledNavigation: Could not init navmesh.");
		return;
	}

	status = m_navQuery->init(m_navMesh, 2048);
	if (dtStatusFailed(status)) {
		m_ctx->log(RC_LOG_ERROR, "buildTiledNavigation: Could not init Detour navmesh query");
		return;
	}

	mObstacleAvoidanceQuery = dtAllocObstacleAvoidanceQuery();
	mObstacleAvoidanceQuery->init(MAX_OBSTACLES_CIRCLES, 0);

	mObstacleAvoidanceParams = new dtObstacleAvoidanceParams;
	mObstacleAvoidanceParams->velBias = 0.4f;
	mObstacleAvoidanceParams->weightDesVel = 2.0f;
	mObstacleAvoidanceParams->weightCurVel = 0.75f;
	mObstacleAvoidanceParams->weightSide = 0.75f;
	mObstacleAvoidanceParams->weightToi = 2.5f;
	mObstacleAvoidanceParams->horizTime = 2.5f;
	mObstacleAvoidanceParams->gridSize = 33;
	mObstacleAvoidanceParams->adaptiveDivs = 7;
	mObstacleAvoidanceParams->adaptiveRings = 2;
	mObstacleAvoidanceParams->adaptiveDepth = 5;

	mAvatarEntity->LocationChanged.connect(sigc::mem_fun(*this, &Awareness::AvatarEntity_LocationChanged));

	view.EntitySeen.connect(sigc::mem_fun(*this, &Awareness::View_EntitySeen));
	view.EntityCreated.connect(sigc::mem_fun(*this, &Awareness::View_EntitySeen));

	std::function<bool(EmberEntity&)> attachListenersFunction = [&](EmberEntity& entity) {
		if (&entity != mAvatarEntity) {
			//Only observe entities that have a bounding box.
			if (entity.hasBBox() && entity.getBBox().isValid()) {
				auto result = mObservedEntities.emplace(&entity, EntityConnections());
				EntityConnections& connections = result.first->second;
				connections.locationChanged = entity.LocationChanged.connect(sigc::bind(sigc::mem_fun(*this, &Awareness::Entity_LocationChanged), &entity));
				entity.BeingDeleted.connect(sigc::bind(sigc::mem_fun(*this, &Awareness::Entity_BeingDeleted), &entity));

				//Only actively observe the entity if it has the same location as the avatar.
				if (entity.getLocation() == mCurrentLocation) {
					connections.isIgnored = false;
					if (entity.hasAttr("velocity")) {
						mMovingEntities.push_back(&entity);
						connections.isMoving = true;
					} else {
						connections.moved = entity.Moved.connect(sigc::bind(sigc::mem_fun(*this, &Awareness::Entity_Moved), &entity));
						connections.isMoving = false;
					}
				} else {
					connections.isIgnored = true;
				}
			}
		}
		return true;
	};

	EmberEntity* entity = static_cast<EmberEntity*>(mView.getTopLevel());
	entity->accept(attachListenersFunction);
	for (size_t i = 0; i < entity->numContained(); ++i) {
		buildEntityAreas(*entity->getContained(i), mEntityAreas);
	}
}

Awareness::~Awareness()
{
	delete mObstacleAvoidanceParams;
	dtFreeObstacleAvoidanceQuery(mObstacleAvoidanceQuery);

	dtFreeNavMesh(m_navMesh);
	dtFreeNavMeshQuery(m_navQuery);
	delete mFilter;

	dtFreeTileCache(m_tileCache);

	delete m_tmproc;
	delete m_tcomp;
	delete m_talloc;

	delete m_ctx;
}

void Awareness::View_EntitySeen(Eris::Entity* entity)
{
	if (entity->hasBBox() && entity->getBBox().isValid()) {
		//We always want to connect to the location changed signal
		auto result = mObservedEntities.emplace(entity, EntityConnections());
		EntityConnections& connections = result.first->second;
		connections.locationChanged = entity->LocationChanged.connect(sigc::bind(sigc::mem_fun(*this, &Awareness::Entity_LocationChanged), entity));
		connections.isMoving = false;
		entity->BeingDeleted.connect(sigc::bind(sigc::mem_fun(*this, &Awareness::Entity_BeingDeleted), entity));

//		//Check if the entity belongs to either the avatar or an entity which is "simple".
//		Eris::Entity* location = entity->getLocation();
//		while (location) {
//			if (location == mAvatarEntity) {
//				connections.isIgnored = true;
//				return;
//			}
//			if (location->hasAttr("simple")) {
//				auto simpleElement = location->valueOfAttr("simple");
//				if (simpleElement.isInt() && simpleElement.asInt() == 1) {
//					connections.isIgnored = true;
//					return;
//				}
//			}
//			location = location->getLocation();
//		}
		//Only actively observe the entity if it has the same location as the avatar.
		if (entity->getLocation() == mCurrentLocation) {
			connections.isIgnored = false;

			if (entity->hasAttr("velocity")) {
				connections.isMoving = true;
				mMovingEntities.push_back(entity);
			} else {

				std::map<Eris::Entity*, WFMath::RotBox<2>> areas;
				buildEntityAreas(*entity, areas);
				for (auto& entry : areas) {
					markTilesAsDirty(entry.second.boundingBox());
					mEntityAreas.insert(entry);
				}

				connections.moved = entity->Moved.connect(sigc::bind(sigc::mem_fun(*this, &Awareness::Entity_Moved), entity));
				connections.isMoving = false;
			}

		} else {
			connections.isIgnored = true;
		}

	}
}

void Awareness::Entity_Moved(Eris::Entity* entity)
{
	//If an entity which previously didn't move start moving we need to move it to the "movable entities" collection.
	if (entity->hasAttr("velocity")) {
		mMovingEntities.push_back(entity);
		auto I = mObservedEntities.find(entity);
		//No need to listen to more Moved events.
		I->second.moved.disconnect();
		I->second.isMoving = true;
		auto existingI = mEntityAreas.find(entity);
		if (existingI != mEntityAreas.end()) {
			//The entity already was registered; mark those tiles where the entity previously were as dirty.
			markTilesAsDirty(existingI->second.boundingBox());
			mEntityAreas.erase(entity);
		}
	} else {
		std::map<Eris::Entity*, WFMath::RotBox<2>> areas;

		buildEntityAreas(*entity, areas);

		for (auto& entry : areas) {
			markTilesAsDirty(entry.second.boundingBox());
			auto existingI = mEntityAreas.find(entry.first);
			if (existingI != mEntityAreas.end()) {
				//The entity already was registered; mark both those tiles where the entity previously were as well as the new tiles as dirty.
				markTilesAsDirty(existingI->second.boundingBox());
				existingI->second = entry.second;
			} else {
				mEntityAreas.insert(entry);
			}
		}

	}

}



bool Awareness::avoidObstacles(const WFMath::Point<2>& position, const WFMath::Vector<2>& desiredVelocity, WFMath::Vector<2>& newVelocity) const
{
	struct EntityCollisionEntry
	{
		float distance;
		Eris::Entity* entity;
		WFMath::Point<2> viewPosition;
		WFMath::Ball<2> viewRadius;
	};

	auto comp = []( EntityCollisionEntry& a, EntityCollisionEntry& b ) {return a.distance < b.distance;};
	std::priority_queue<EntityCollisionEntry, std::vector<EntityCollisionEntry>, decltype( comp )> nearestEntities(comp);

	WFMath::Ball<2> playerRadius(position, 5);

	for (auto entity : mMovingEntities) {

		if (entity->isVisible()) {

			//All of the entities have the same location as we have, so we don't need to resolve the position in the world.

			WFMath::Point<3> pos = entity->getPredictedPos();
			if (!pos.isValid()) {
				continue;
			}

			//TODO: calculate 2d bounding sphere instead of 3d
			WFMath::Point<2> entityView2dPos(pos.x(), pos.y());
			WFMath::Ball<2> entityViewRadius(entityView2dPos, entity->getBBox().boundingSphereSloppy().radius());

			if (WFMath::Intersect(playerRadius, entityViewRadius, false) || WFMath::Contains(playerRadius, entityViewRadius, false)) {
				nearestEntities.push(EntityCollisionEntry( { WFMath::Distance(position, entityView2dPos), entity, entityView2dPos, entityViewRadius }));
			}
		}

	}

	if (!nearestEntities.empty()) {
		mObstacleAvoidanceQuery->reset();
		int i = 0;
		while (!nearestEntities.empty() && i < MAX_OBSTACLES_CIRCLES) {
			const EntityCollisionEntry& entry = nearestEntities.top();
			auto entity = entry.entity;
			float pos[] { entry.viewPosition.x(), 0, entry.viewPosition.y() };
			float vel[] { entity->getPredictedVelocity().x(), 0, entity->getPredictedVelocity().y() };
			mObstacleAvoidanceQuery->addCircle(pos, entry.viewRadius.radius(), vel, vel);
			nearestEntities.pop();
			++i;
		}

		float pos[] { position.x(), 0, position.y() };
		float vel[] { desiredVelocity.x(), 0, desiredVelocity.y() };
		float dvel[] { desiredVelocity.x(), 0, desiredVelocity.y() };
		float nvel[] { 0, 0, 0 };
		float desiredSpeed = desiredVelocity.mag();

		auto samples = mObstacleAvoidanceQuery->sampleVelocityAdaptive(pos, mAvatarRadius, desiredSpeed, vel, dvel, nvel, mObstacleAvoidanceParams, nullptr);
		if (samples > 0) {
			if (!WFMath::Equal(vel[0], nvel[0]) || !WFMath::Equal(vel[2], nvel[2])) {
				newVelocity.x() = nvel[0];
				newVelocity.y() = nvel[2];
				newVelocity.setValid(true);
				return true;
			}
		}

	}
	return false;

}

void Awareness::Entity_BeingDeleted(Eris::Entity* entity)
{
	auto I = mObservedEntities.find(entity);
	if (I != mObservedEntities.end()) {
		if (!I->second.isIgnored) {
			if (I->second.isMoving) {
				mMovingEntities.remove(entity);
			} else {
				std::map<Eris::Entity*, WFMath::RotBox<2>> areas;

				buildEntityAreas(*entity, areas);

				for (auto& entry : areas) {
					markTilesAsDirty(entry.second.boundingBox());
				}
				mEntityAreas.erase(entity);
			}
			mObservedEntities.erase(entity);
		}
	}
}

void Awareness::AvatarEntity_LocationChanged(Eris::Entity*)
{
	//TODO: rebuild awareness as the location has changed
}

void Awareness::Entity_LocationChanged(Eris::Entity* oldLoc, Eris::Entity* entity)
{
	auto I = mObservedEntities.find(entity);
	if (I != mObservedEntities.end()) {
		EntityConnections& connections = I->second;

		if (entity->getLocation() == mCurrentLocation) {
			assert(connections.moved == false);
			connections.isIgnored = false;
			//Entity was ignored but shouldn't be anymore
			if (entity->hasAttr("velocity")) {
				connections.isMoving = true;
				mMovingEntities.push_back(entity);
			} else {

				//The position of the entity is invalid at this; the Moved signal will be emitted after the LocationChanged signal.

//				std::map<Eris::Entity*, WFMath::RotBox<2>> areas;
//				buildEntityAreas(*entity, areas);
//				for (auto& entry : areas) {
//					markTilesAsDirty(entry.second.boundingBox());
//					mEntityAreas.insert(entry);
//				}

				connections.moved = entity->Moved.connect(sigc::bind(sigc::mem_fun(*this, &Awareness::Entity_Moved), entity));
			}
		} else {
			//Only sever connections if the entity wasn't already ignored.
			if (!connections.isIgnored) {
				if (connections.isMoving) {
					assert(connections.moved == false);
					mMovingEntities.remove(entity);
				} else {
					assert(connections.moved == true);
					connections.moved.disconnect();

					auto areasI = mEntityAreas.find(entity);
					if (areasI != mEntityAreas.end()) {
						markTilesAsDirty(areasI->second.boundingBox());
						mEntityAreas.erase(areasI);
					}
				}
				connections.isIgnored = true;
			}
		}

//		//Check if the entity belongs to either the avatar or an entity which is "simple".
//		Eris::Entity* location = entity->getLocation();
//		while (location) {
//			if (location == mAvatarEntity) {
//				shouldBeIgnored = true;
//				break;
//			}
//			if (location->hasAttr("simple")) {
//				auto simpleElement = location->valueOfAttr("simple");
//				if (simpleElement.isInt() && simpleElement.asInt() == 1) {
//					shouldBeIgnored = true;
//					break;
//				}
//			}
//			location = location->getLocation();
//		}
//
//		if (shouldBeIgnored) {
//			//We should remove the entity from awareness.
//			if (!connections.isIgnored) {
//				if (connections.isMoving) {
//					mMovingEntities.remove(entity);
//				} else {
//					connections.moved.disconnect();
//					std::map<Eris::Entity*, WFMath::RotBox<2>> areas;
//
//					buildEntityAreas(*entity, areas);
//
//					for (auto& entry : areas) {
//						markTilesAsDirty(entry.second.boundingBox());
//					}
//					mEntityAreas.erase(entity);
//				}
//			}
//			connections.isIgnored = true;
//		} else if (connections.isIgnored) {
//			//Entity was ignored but shouldn't be anymore
//			if (entity->hasAttr("velocity")) {
//				connections.isMoving = true;
//				mMovingEntities.push_back(entity);
//			} else {
//
//				std::map<Eris::Entity*, WFMath::RotBox<2>> areas;
//				buildEntityAreas(*entity, areas);
//				for (auto& entry : areas) {
//					markTilesAsDirty(entry.second.boundingBox());
//					mEntityAreas.insert(entry);
//				}
//
//				connections.moved = entity->Moved.connect(sigc::bind(sigc::mem_fun(*this, &Awareness::Entity_Moved), entity));
//			}
//
//		}
	}
}

void Awareness::markTilesAsDirty(const WFMath::AxisBox<2>& area)
{
	int tileMinXIndex, tileMaxXIndex, tileMinYIndex, tileMaxYIndex;
	findAffectedTiles(area, tileMinXIndex, tileMaxXIndex, tileMinYIndex, tileMaxYIndex);
	markTilesAsDirty(tileMinXIndex, tileMaxXIndex, tileMinYIndex, tileMaxYIndex);
}

void Awareness::markTilesAsDirty(int tileMinXIndex, int tileMaxXIndex, int tileMinYIndex, int tileMaxYIndex)
{
	bool wereDirtyTiles = !mDirtyAwareTiles.empty();

	for (int tx = tileMinXIndex; tx <= tileMaxXIndex; ++tx) {
		for (int ty = tileMinYIndex; ty <= tileMaxYIndex; ++ty) {
			std::pair<int, int> index(tx, ty);
			if (mAwareTiles.find(index) != mAwareTiles.end()) {
				if (mDirtyAwareTiles.insert(index).second) {
					mDirtyAwareOrderedTiles.push_back(index);
				}
			} else {
				mDirtyUnwareTiles.insert(index);
			}
		}
	}
	if (!wereDirtyTiles && !mDirtyAwareTiles.empty()) {
		EventTileDirty();
	}
}

size_t Awareness::rebuildDirtyTile()
{
	if (!mDirtyAwareTiles.empty()) {
		const auto tileIndexI = mDirtyAwareOrderedTiles.begin();
		const auto& tileIndex = *tileIndexI;

		float tilesize = m_cfg.tileSize * m_cfg.cs;
		WFMath::AxisBox<2> adjustedArea(WFMath::Point<2>(m_cfg.bmin[0] + (tileIndex.first * tilesize), m_cfg.bmin[2] + (tileIndex.second * tilesize)), WFMath::Point<2>(m_cfg.bmin[0] + ((tileIndex.first + 1) * tilesize), m_cfg.bmin[2] + ((tileIndex.second + 1) * tilesize)));

		std::vector<WFMath::RotBox<2>> entityAreas;
		findEntityAreas(adjustedArea, entityAreas);

		rebuildTile(tileIndex.first, tileIndex.second, entityAreas);
		mDirtyAwareTiles.erase(tileIndex);
		mDirtyAwareOrderedTiles.erase(tileIndexI);
	}
	return mDirtyAwareTiles.size();
}

void Awareness::findAffectedTiles(const WFMath::AxisBox<2>& area, int& tileMinXIndex, int& tileMaxXIndex, int& tileMinYIndex, int& tileMaxYIndex) const
{
	float tilesize = m_cfg.tileSize * m_cfg.cs;
	WFMath::Point<2> lowCorner = area.lowCorner();
	WFMath::Point<2> highCorner = area.highCorner();

	if (lowCorner.x() < m_cfg.bmin[0]) {
		lowCorner.x() = m_cfg.bmin[0];
	}
	if (lowCorner.y() < m_cfg.bmin[2]) {
		lowCorner.y() = m_cfg.bmin[2];
	}
	if (lowCorner.x() > m_cfg.bmax[0]) {
		lowCorner.x() = m_cfg.bmax[0];
	}
	if (lowCorner.y() > m_cfg.bmax[2]) {
		lowCorner.y() = m_cfg.bmax[2];
	}

	if (highCorner.x() < m_cfg.bmin[0]) {
		highCorner.x() = m_cfg.bmin[0];
	}
	if (highCorner.y() < m_cfg.bmin[2]) {
		highCorner.y() = m_cfg.bmin[2];
	}
	if (highCorner.x() > m_cfg.bmax[0]) {
		highCorner.x() = m_cfg.bmax[0];
	}
	if (highCorner.y() > m_cfg.bmax[2]) {
		highCorner.y() = m_cfg.bmax[2];
	}

	tileMinXIndex = (lowCorner.x() - m_cfg.bmin[0]) / tilesize;
	tileMaxXIndex = (highCorner.x() - m_cfg.bmin[0]) / tilesize;
	tileMinYIndex = (lowCorner.y() - m_cfg.bmin[2]) / tilesize;
	tileMaxYIndex = (highCorner.y() - m_cfg.bmin[2]) / tilesize;
}

int Awareness::findPath(const WFMath::Point<3>& start, const WFMath::Point<3>& end, std::list<WFMath::Point<3>>& path)
{

	float pStartPos[] { start.x(), start.z(), start.y() };
	float pEndPos[] { end.x(), end.z(), end.y() };
	float extent[] { 2, 100, 2 }; //Look two meters in each direction

	dtStatus status;
	dtPolyRef StartPoly;
	float StartNearest[3];
	dtPolyRef EndPoly;
	float EndNearest[3];
	dtPolyRef PolyPath[MAX_PATHPOLY];
	int nPathCount = 0;
	float StraightPath[MAX_PATHVERT * 3];
	int nVertCount = 0;

// find the start polygon
	status = m_navQuery->findNearestPoly(pStartPos, extent, mFilter, &StartPoly, StartNearest);
	if ((status & DT_FAILURE) || (status & DT_STATUS_DETAIL_MASK))
		return -1; // couldn't find a polygon

// find the end polygon
	status = m_navQuery->findNearestPoly(pEndPos, extent, mFilter, &EndPoly, EndNearest);
	if ((status & DT_FAILURE) || (status & DT_STATUS_DETAIL_MASK))
		return -2; // couldn't find a polygon

	status = m_navQuery->findPath(StartPoly, EndPoly, StartNearest, EndNearest, mFilter, PolyPath, &nPathCount, MAX_PATHPOLY);
	if ((status & DT_FAILURE) || (status & DT_STATUS_DETAIL_MASK))
		return -3; // couldn't create a path
	if (nPathCount == 0)
		return -4; // couldn't find a path

	status = m_navQuery->findStraightPath(StartNearest, EndNearest, PolyPath, nPathCount, StraightPath, NULL, NULL, &nVertCount, MAX_PATHVERT);
	if ((status & DT_FAILURE) || (status & DT_STATUS_DETAIL_MASK))
		return -5; // couldn't create a path
	if (nVertCount == 0)
		return -6; // couldn't find a path

// At this point we have our path.
	for (int nVert = 0; nVert < nVertCount; nVert++) {
		path.push_back(WFMath::Point<3>(StraightPath[nVert * 3], StraightPath[(nVert * 3) + 2], StraightPath[(nVert * 3) + 1]));
	}

	return nVertCount;
}

void Awareness::setAwarenessArea(const WFMath::RotBox<2>& area, const WFMath::Segment<2>& focusLine)
{

	WFMath::AxisBox<2> axisbox = area.boundingBox();

//adjust area to fit with tiles

	float tilesize = m_cfg.tileSize * m_cfg.cs;
	WFMath::Point<2> lowCorner = axisbox.lowCorner();
	WFMath::Point<2> highCorner = axisbox.highCorner();

	if (lowCorner.x() < m_cfg.bmin[0]) {
		lowCorner.x() = m_cfg.bmin[0];
	}
	if (lowCorner.y() < m_cfg.bmin[2]) {
		lowCorner.y() = m_cfg.bmin[2];
	}
	if (lowCorner.x() > m_cfg.bmax[0]) {
		lowCorner.x() = m_cfg.bmax[0];
	}
	if (lowCorner.y() > m_cfg.bmax[2]) {
		lowCorner.y() = m_cfg.bmax[2];
	}

	if (highCorner.x() < m_cfg.bmin[0]) {
		highCorner.x() = m_cfg.bmin[0];
	}
	if (highCorner.y() < m_cfg.bmin[2]) {
		highCorner.y() = m_cfg.bmin[2];
	}
	if (highCorner.x() > m_cfg.bmax[0]) {
		highCorner.x() = m_cfg.bmax[0];
	}
	if (highCorner.y() > m_cfg.bmax[2]) {
		highCorner.y() = m_cfg.bmax[2];
	}

	int tileMinXIndex = (lowCorner.x() - m_cfg.bmin[0]) / tilesize;
	int tileMaxXIndex = (highCorner.x() - m_cfg.bmin[0]) / tilesize;
	int tileMinYIndex = (lowCorner.y() - m_cfg.bmin[2]) / tilesize;
	int tileMaxYIndex = (highCorner.y() - m_cfg.bmin[2]) / tilesize;

//Now mark tiles
	const float tcs = m_cfg.tileSize * m_cfg.cs;
	const float tileBorderSize = m_cfg.borderSize * m_cfg.cs;

	auto oldDirtyAwareTiles = mDirtyAwareTiles;
	mDirtyAwareTiles.clear();
	mDirtyAwareOrderedTiles.clear();
	mAwareTiles.clear();
	bool wereDirtyTiles = !mDirtyAwareTiles.empty();
	for (int tx = tileMinXIndex; tx <= tileMaxXIndex; ++tx) {
		for (int ty = tileMinYIndex; ty <= tileMaxYIndex; ++ty) {
			// Tile bounds.
			WFMath::AxisBox<2> tileBounds(WFMath::Point<2>((m_cfg.bmin[0] + tx * tcs) - tileBorderSize, (m_cfg.bmin[2] + ty * tcs) - tileBorderSize), WFMath::Point<2>((m_cfg.bmin[0] + (tx + 1) * tcs) + tileBorderSize, (m_cfg.bmin[2] + (ty + 1) * tcs) + tileBorderSize));
			if (WFMath::Intersect(area, tileBounds, false) || WFMath::Contains(area, tileBounds, false)) {

				std::pair<int, int> index(tx, ty);
				//If true we should insert in the front of the dirty tiles list.
				bool insertFront = false;
				//If true we should insert in the back of the dirty tiles list.
				bool insertBack = false;
				//If the tile was marked as dirty in the old aware tiles, retain it as such
				if (oldDirtyAwareTiles.find(index) != oldDirtyAwareTiles.end()) {
					if (focusLine.isValid() && WFMath::Intersect(focusLine, tileBounds, false)) {
						insertFront = true;
					} else {
						insertBack = true;
					}
				} else if (mDirtyUnwareTiles.find(index) != mDirtyUnwareTiles.end()) {
					//if the tile was marked as dirty in the unaware tiles we'll move it to the dirty aware collection.
					if (focusLine.isValid() && WFMath::Intersect(focusLine, tileBounds, false)) {
						insertFront = true;
					} else {
						insertBack = true;
					}
				} else {
					//The tile wasn't marked as dirty in any set, but it might be that it hasn't been processed before.
					auto tile = m_tileCache->getTileAt(tx, ty, 0);
					if (!tile) {
						if (focusLine.isValid() && WFMath::Intersect(focusLine, tileBounds, false)) {
							insertFront = true;
						} else {
							insertBack = true;
						}
					}
				}

				if (insertFront) {
					if (mDirtyAwareTiles.insert(index).second) {
						mDirtyAwareOrderedTiles.push_front(index);
					}
				} else if (insertBack) {
					if (mDirtyAwareTiles.insert(index).second) {
						mDirtyAwareOrderedTiles.push_back(index);
					}
				}

				mDirtyUnwareTiles.erase(index);

				mAwareTiles.insert(index);
			}
		}
	}

	if (!wereDirtyTiles && !mDirtyAwareTiles.empty()) {
		EventTileDirty();
	}
}

void Awareness::rebuildTile(int tx, int ty, const std::vector<WFMath::RotBox<2>>& entityAreas)
{
	TileCacheData tiles[MAX_LAYERS];
	memset(tiles, 0, sizeof(tiles));

	int ntiles = rasterizeTileLayers(entityAreas, tx, ty, m_cfg, tiles, MAX_LAYERS);

	for (int j = 0; j < ntiles; ++j) {
		TileCacheData* tile = &tiles[j];

		dtTileCacheLayerHeader* header = (dtTileCacheLayerHeader*)tile->data;
		dtTileRef tileRef = m_tileCache->getTileRef(m_tileCache->getTileAt(header->tx, header->ty, header->tlayer));
		if (tileRef) {
			m_tileCache->removeTile(tileRef, NULL, NULL);
		}
		dtStatus status = m_tileCache->addTile(tile->data, tile->dataSize, DT_COMPRESSEDTILE_FREE_DATA, 0);  // Add compressed tiles to tileCache
		if (dtStatusFailed(status)) {
			dtFree(tile->data);
			tile->data = 0;
			continue;
		}
	}

	m_tileCache->buildNavMeshTilesAt(tx, ty, m_navMesh);

	EventTileUpdated(tx, ty);

}

void Awareness::buildEntityAreas(Eris::Entity& entity, std::map<Eris::Entity*, WFMath::RotBox<2>>& entityAreas)
{
	if (&entity == mAvatarEntity) {
		return;
	}

	if (entity.hasAttr("velocity")) {
		return;
	}

//The entity is solid (i.e. can be collided with) if it has a bbox and the "solid" property isn't set to false (or 0 as it's an int).
	bool isSolid = entity.hasBBox();
//No need to check if the entity has no bbox.
	if (isSolid && entity.hasAttr("solid")) {
		auto solidElement = entity.valueOfAttr("solid");
		if (solidElement.isInt() && solidElement.asInt() == 0) {
			isSolid = false;
		}
	}

//If an entity is "simple" it means that we shouldn't consider any of its child entities. Like with a character with an inventory.
	bool isSimple = false;
	if (entity.hasAttr("simple")) {
		auto simpleElement = entity.valueOfAttr("simple");
		if (simpleElement.isInt() && simpleElement.asInt() == 1) {
			isSimple = true;
		}
	}

	if (isSolid) {
		//we now have to get the location of the entity in world space
		auto pos = entity.getViewPosition();
		auto orientation = entity.getViewOrientation();
		if (pos.isValid() && orientation.isValid()) {

			WFMath::Vector<3> xVec = WFMath::Vector<3>(1.0, 0.0, 0.0).rotate(orientation);
			double theta = atan2(xVec.y(), xVec.x()); // rotation about Z

			WFMath::RotMatrix<2> rm;
			rm.rotation(theta);

			auto bbox = entity.getBBox();

			WFMath::Point<2> highCorner(bbox.highCorner().x(), bbox.highCorner().y());
			WFMath::Point<2> lowCorner(bbox.lowCorner().x(), bbox.lowCorner().y());

			//Expand the box a little so that we can navigate around it without being stuck on it.
			//We'll the radius of the avatar.
			highCorner += WFMath::Vector<2>(mAvatarRadius, mAvatarRadius);
			lowCorner -= WFMath::Vector<2>(mAvatarRadius, mAvatarRadius);

			WFMath::RotBox<2> rotbox(WFMath::Point<2>::ZERO(), highCorner - lowCorner, WFMath::RotMatrix<2>().identity());
			rotbox.shift(WFMath::Vector<2>(lowCorner.x(), lowCorner.y()));
			rotbox.rotatePoint(rm, WFMath::Point<2>::ZERO());

			rotbox.shift(WFMath::Vector<2>(pos.x(), pos.y()));

			entityAreas.insert(std::make_pair(&entity, rotbox));
		}
	}

//	if (!isSimple) {
//		for (size_t i = 0; i < entity.numContained(); ++i) {
//			buildEntityAreas(*entity.getContained(i), entityAreas);
//		}
//	}
}

void Awareness::findEntityAreas(const WFMath::AxisBox<2>& extent, std::vector<WFMath::RotBox<2> >& areas)
{

	for (auto& entry : mEntityAreas) {
		auto& rotbox = entry.second;
		if (WFMath::Contains(extent, rotbox, false) || WFMath::Intersect(extent, rotbox, false)) {
			areas.push_back(rotbox);
		}
	}
}

int Awareness::rasterizeTileLayers(const std::vector<WFMath::RotBox<2>>& entityAreas, const int tx, const int ty, const rcConfig& cfg, TileCacheData* tiles, const int maxTiles)
{
	std::vector<float> vertsVector;
	std::vector<int> trisVector;

	FastLZCompressor comp;
	RasterizationContext rc;

// Tile bounds.
	const float tcs = cfg.tileSize * cfg.cs;

	rcConfig tcfg;
	memcpy(&tcfg, &cfg, sizeof(tcfg));

	tcfg.bmin[0] = cfg.bmin[0] + tx * tcs;
	tcfg.bmin[1] = cfg.bmin[1];
	tcfg.bmin[2] = cfg.bmin[2] + ty * tcs;
	tcfg.bmax[0] = cfg.bmin[0] + (tx + 1) * tcs;
	tcfg.bmax[1] = cfg.bmax[1];
	tcfg.bmax[2] = cfg.bmin[2] + (ty + 1) * tcs;
	tcfg.bmin[0] -= tcfg.borderSize * tcfg.cs;
	tcfg.bmin[2] -= tcfg.borderSize * tcfg.cs;
	tcfg.bmax[0] += tcfg.borderSize * tcfg.cs;
	tcfg.bmax[2] += tcfg.borderSize * tcfg.cs;

//First define all vertices. Get one extra vertex in each direction so that there's no cutoff at the tile's edges.
	int heightsXMin = std::floor(tcfg.bmin[0]) - 1;
	int heightsXMax = std::ceil(tcfg.bmax[0]) + 1;
	int heightsYMin = std::floor(tcfg.bmin[2]) - 1;
	int heightsYMax = std::ceil(tcfg.bmax[2]) + 1;
	int sizeX = heightsXMax - heightsXMin;
	int sizeY = heightsYMax - heightsYMin;

//Blit height values with 1 meter interval
	std::vector<float> heights(sizeX * sizeY);
	mHeightProvider.blitHeights(heightsXMin, heightsXMax, heightsYMin, heightsYMax, heights);

	float* heightData = heights.data();
	for (int y = heightsYMin; y < heightsYMax; ++y) {
		for (int x = heightsXMin; x < heightsXMax; ++x) {
			vertsVector.push_back(x);
			vertsVector.push_back(*heightData);
			vertsVector.push_back(y);
			heightData++;
		}
	}


//Then define the triangles
	for (int y = 0; y < (sizeY - 1); y++) {
		for (int x = 0; x < (sizeX - 1); x++) {
			size_t vertPtr = (y * sizeX) + x;
			//make a square, including the vertices to the right and below
			trisVector.push_back(vertPtr);
			trisVector.push_back(vertPtr + sizeX);
			trisVector.push_back(vertPtr + 1);

			trisVector.push_back(vertPtr + 1);
			trisVector.push_back(vertPtr + sizeX);
			trisVector.push_back(vertPtr + 1 + sizeX);
		}
	}

	float* verts = vertsVector.data();
	int* tris = trisVector.data();
	const int nverts = vertsVector.size() / 3;
	const int ntris = trisVector.size() / 3;

// Allocate voxel heightfield where we rasterize our input data to.
	rc.solid = rcAllocHeightfield();
	if (!rc.solid) {
		m_ctx->log(RC_LOG_ERROR, "buildNavigation: Out of memory 'solid'.");
		return 0;
	}
	if (!rcCreateHeightfield(m_ctx, *rc.solid, tcfg.width, tcfg.height, tcfg.bmin, tcfg.bmax, tcfg.cs, tcfg.ch)) {
		m_ctx->log(RC_LOG_ERROR, "buildNavigation: Could not create solid heightfield.");
		return 0;
	}

// Allocate array that can hold triangle flags.
	rc.triareas = new unsigned char[ntris];
	if (!rc.triareas) {
		m_ctx->log(RC_LOG_ERROR, "buildNavigation: Out of memory 'm_triareas' (%d).", ntris / 3);
		return 0;
	}

	memset(rc.triareas, 0, ntris * sizeof(unsigned char));
	rcMarkWalkableTriangles(m_ctx, tcfg.walkableSlopeAngle, verts, nverts, tris, ntris, rc.triareas);

	rcRasterizeTriangles(m_ctx, verts, nverts, tris, rc.triareas, ntris, *rc.solid, tcfg.walkableClimb);

// Once all geometry is rasterized, we do initial pass of filtering to
// remove unwanted overhangs caused by the conservative rasterization
// as well as filter spans where the character cannot possibly stand.

//NOTE: These are disabled for now since we currently only handle a simple 2d height map
//with bounding boxes snapped to the ground. If this changes these calls probably needs to be activated.
//	rcFilterLowHangingWalkableObstacles(m_ctx, tcfg.walkableClimb, *rc.solid);
//	rcFilterLedgeSpans(m_ctx, tcfg.walkableHeight, tcfg.walkableClimb, *rc.solid);
//	rcFilterWalkableLowHeightSpans(m_ctx, tcfg.walkableHeight, *rc.solid);

	rc.chf = rcAllocCompactHeightfield();
	if (!rc.chf) {
		m_ctx->log(RC_LOG_ERROR, "buildNavigation: Out of memory 'chf'.");
		return 0;
	}
	if (!rcBuildCompactHeightfield(m_ctx, tcfg.walkableHeight, tcfg.walkableClimb, *rc.solid, *rc.chf)) {
		m_ctx->log(RC_LOG_ERROR, "buildNavigation: Could not build compact data.");
		return 0;
	}

// Erode the walkable area by agent radius.
	if (!rcErodeWalkableArea(m_ctx, tcfg.walkableRadius, *rc.chf)) {
		m_ctx->log(RC_LOG_ERROR, "buildNavigation: Could not erode.");
		return 0;
	}

// Mark areas.
	for (auto& rotbox : entityAreas) {
		float verts[3 * 4];

		verts[0] = rotbox.getCorner(1).x();
		verts[1] = 0;
		verts[2] = rotbox.getCorner(1).y();

		verts[3] = rotbox.getCorner(3).x();
		verts[4] = 0;
		verts[5] = rotbox.getCorner(3).y();

		verts[6] = rotbox.getCorner(2).x();
		verts[7] = 0;
		verts[8] = rotbox.getCorner(2).y();

		verts[9] = rotbox.getCorner(0).x();
		verts[10] = 0;
		verts[11] = rotbox.getCorner(0).y();

		rcMarkConvexPolyArea(m_ctx, verts, 4, tcfg.bmin[1], tcfg.bmax[1], DT_TILECACHE_NULL_AREA, *rc.chf);
	}

	rc.lset = rcAllocHeightfieldLayerSet();
	if (!rc.lset) {
		m_ctx->log(RC_LOG_ERROR, "buildNavigation: Out of memory 'lset'.");
		return 0;
	}
	if (!rcBuildHeightfieldLayers(m_ctx, *rc.chf, tcfg.borderSize, tcfg.walkableHeight, *rc.lset)) {
		m_ctx->log(RC_LOG_ERROR, "buildNavigation: Could not build heighfield layers.");
		return 0;
	}

	rc.ntiles = 0;
	for (int i = 0; i < rcMin(rc.lset->nlayers, MAX_LAYERS); ++i) {
		TileCacheData* tile = &rc.tiles[rc.ntiles++];
		const rcHeightfieldLayer* layer = &rc.lset->layers[i];

		// Store header
		dtTileCacheLayerHeader header;
		header.magic = DT_TILECACHE_MAGIC;
		header.version = DT_TILECACHE_VERSION;

		// Tile layer location in the navmesh.
		header.tx = tx;
		header.ty = ty;
		header.tlayer = i;
		dtVcopy(header.bmin, layer->bmin);
		dtVcopy(header.bmax, layer->bmax);

		// Tile info.
		header.width = (unsigned char)layer->width;
		header.height = (unsigned char)layer->height;
		header.minx = (unsigned char)layer->minx;
		header.maxx = (unsigned char)layer->maxx;
		header.miny = (unsigned char)layer->miny;
		header.maxy = (unsigned char)layer->maxy;
		header.hmin = (unsigned short)layer->hmin;
		header.hmax = (unsigned short)layer->hmax;

		dtStatus status = dtBuildTileCacheLayer(&comp, &header, layer->heights, layer->areas, layer->cons, &tile->data, &tile->dataSize);
		if (dtStatusFailed(status)) {
			return 0;
		}
	}

// Transfer ownership of tile data from build context to the caller.
	int n = 0;
	for (int i = 0; i < rcMin(rc.ntiles, maxTiles); ++i) {
		tiles[n++] = rc.tiles[i];
		rc.tiles[i].data = 0;
		rc.tiles[i].dataSize = 0;
	}

	return n;
}

void Awareness::processTiles(const WFMath::AxisBox<2>& area, const std::function<void(unsigned int, dtTileCachePolyMesh&, float* origin, float cellsize, float cellheight, dtTileCacheLayer& layer)>& processor) const
{
	float bmin[] { area.lowCorner().x(), -100, area.lowCorner().y() };
	float bmax[] { area.highCorner().x(), 100, area.highCorner().y() };

	dtCompressedTileRef tilesRefs[256];
	int ntiles;
	dtStatus status = m_tileCache->queryTiles(bmin, bmax, tilesRefs, &ntiles, 256);
	if (status == DT_SUCCESS) {
		std::vector<const dtCompressedTile*> tiles(ntiles);
		for (int i = 0; i < ntiles; ++i) {
			tiles[i] = m_tileCache->getTileByRef(tilesRefs[i]);
		}
		processTiles(tiles, processor);
	}
}

void Awareness::processTile(const int tx, const int ty, const std::function<void(unsigned int, dtTileCachePolyMesh&, float* origin, float cellsize, float cellheight, dtTileCacheLayer& layer)>& processor) const
{
	dtCompressedTileRef tilesRefs[MAX_LAYERS];
	const int ntiles = m_tileCache->getTilesAt(tx, ty, tilesRefs, MAX_LAYERS);

	std::vector<const dtCompressedTile*> tiles(ntiles);
	for (int i = 0; i < ntiles; ++i) {
		tiles[i] = m_tileCache->getTileByRef(tilesRefs[i]);
	}

	processTiles(tiles, processor);
}

void Awareness::processAllTiles(const std::function<void(unsigned int, dtTileCachePolyMesh&, float* origin, float cellsize, float cellheight, dtTileCacheLayer& layer)>& processor) const
{
	int ntiles = m_tileCache->getTileCount();
	std::vector<const dtCompressedTile*> tiles(ntiles);
	for (int i = 0; i < ntiles; ++i) {
		tiles[i] = m_tileCache->getTile(i);
	}

	processTiles(tiles, processor);

}

void Awareness::processTiles(std::vector<const dtCompressedTile*> tiles, const std::function<void(unsigned int, dtTileCachePolyMesh&, float* origin, float cellsize, float cellheight, dtTileCacheLayer& layer)>& processor) const
{
	struct TileCacheBuildContext
	{
		inline TileCacheBuildContext(struct dtTileCacheAlloc* a) :
				layer(0), lcset(0), lmesh(0), alloc(a)
		{
		}
		inline ~TileCacheBuildContext()
		{
			purge();
		}
		void purge()
		{
			dtFreeTileCacheLayer(alloc, layer);
			layer = 0;
			dtFreeTileCacheContourSet(alloc, lcset);
			lcset = 0;
			dtFreeTileCachePolyMesh(alloc, lmesh);
			lmesh = 0;
		}
		struct dtTileCacheLayer* layer;
		struct dtTileCacheContourSet* lcset;
		struct dtTileCachePolyMesh* lmesh;
		struct dtTileCacheAlloc* alloc;
	};

	dtTileCacheAlloc* talloc = m_tileCache->getAlloc();
	dtTileCacheCompressor* tcomp = m_tileCache->getCompressor();
	const dtTileCacheParams* params = m_tileCache->getParams();

	for (const dtCompressedTile* tile : tiles) {

		talloc->reset();

		TileCacheBuildContext bc(talloc);
		const int walkableClimbVx = (int)(params->walkableClimb / params->ch);
		dtStatus status;

		// Decompress tile layer data.
		status = dtDecompressTileCacheLayer(talloc, tcomp, tile->data, tile->dataSize, &bc.layer);
		if (dtStatusFailed(status))
			return;

		// Build navmesh
		status = dtBuildTileCacheRegions(talloc, *bc.layer, walkableClimbVx);
		if (dtStatusFailed(status))
			return;

//TODO this part is replicated from navmesh tile building in DetourTileCache. Maybe that can be reused. Also is it really necessary to do an extra navmesh rebuild from compressed tile just to draw it? Can't I just draw it somewhere where the navmesh is rebuilt?
		bc.lcset = dtAllocTileCacheContourSet(talloc);
		if (!bc.lcset)
			return;
		status = dtBuildTileCacheContours(talloc, *bc.layer, walkableClimbVx, params->maxSimplificationError, *bc.lcset);
		if (dtStatusFailed(status))
			return;

		bc.lmesh = dtAllocTileCachePolyMesh(talloc);
		if (!bc.lmesh)
			return;
		status = dtBuildTileCachePolyMesh(talloc, *bc.lcset, *bc.lmesh);
		if (dtStatusFailed(status))
			return;

		processor(m_tileCache->getTileRef(tile), *bc.lmesh, tile->header->bmin, params->cs, params->ch, *bc.layer);

	}
}

}
}