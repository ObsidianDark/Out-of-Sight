#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <algorithm>
#include <cmath>
#include <vector>
#include <unordered_map>

using namespace geode::prelude;

struct ObjEntry {
    GameObject* obj;
    float halfW, halfH; // cached bounding-box half-extents, computed once at build time
    bool lodReduced = false; // current LOD tier for this object
};

static inline int64_t cellKey(int cx, int cy) {
    return (static_cast<int64_t>(cx) << 32) ^ static_cast<uint32_t>(cy);
}

class $modify(CullingGameLayer, GJBaseGameLayer) {
public:
    struct Fields {
        std::unordered_map<int64_t, std::vector<ObjEntry>> grid;
        std::vector<int64_t> prevKeys;
        std::vector<int64_t> activeKeys; // cells currently intersecting the camera, refreshed on camera move
        CCRect cameraBounds;             // cached so the per-tick LOD pass doesn't need cameraMoved
        bool built = false;
        float builtCellSize = -1.f;      // cell size the grid was actually built with -- detects setting changes
        int builtObjectCount = -1;       // m_objects->count() at last build -- detects newly streamed-in objects
        CCPoint lastCameraPos = CCPoint(NAN, NAN);
        float lastScale = -1.f;
        CCDrawNode* debugNode = nullptr; // lazily created, lives on m_objectLayer
    };

    // Reads the tunable cell size from settings each call rather than baking
    // it in as a constexpr. Smaller = more precise culling but more buckets
    // to check per frame; larger = fewer buckets but more objects per bucket.
    float getCellSize() {
        float size = static_cast<float>(Mod::get()->getSettingValue<double>("cell-size"));
        // Guard against zero/negative values from a misconfigured setting --
        // dividing positions by this later would otherwise be undefined/UB-adjacent.
        if (size <= 0.f) size = 300.f;
        return size;
    }

    void buildGrid() {
        auto& grid = m_fields->grid;
        grid.clear();
        if (!m_objects) return;

        bool ignoreTriggers = Mod::get()->getSettingValue<bool>("ignore-triggers");
        float cellSize = getCellSize();

        for (int i = 0; i < m_objects->count(); ++i) {
            auto obj = static_cast<GameObject*>(m_objects->objectAtIndex(i));
            if (!obj) continue;
            if (ignoreTriggers && obj->m_isTrigger) continue; // never touch these

            CCRect r = obj->boundingBox();
            float halfW = r.size.width * 0.5f;
            float halfH = r.size.height * 0.5f;
            CCPoint p = obj->getPosition();

            int cx = static_cast<int>(std::floor(p.x / cellSize));
            int cy = static_cast<int>(std::floor(p.y / cellSize));
            grid[cellKey(cx, cy)].push_back({ obj, halfW, halfH, false });
        }
        m_fields->prevKeys.clear();
        m_fields->activeKeys.clear();
        m_fields->built = true;
        m_fields->builtCellSize = cellSize;       // remember what size this grid was keyed with
        m_fields->builtObjectCount = m_objects->count(); // remember how many objects existed at this build
    }

    // Draws the current culling rectangle, in m_objectLayer's local coordinate
    // space (same space as GameObject positions), so no extra transform is
    // needed to align it visually with what's actually being culled.
    void updateDebugDraw(float left, float bottom, float right, float top, bool enabled) {
        if (!enabled) {
            if (m_fields->debugNode) m_fields->debugNode->setVisible(false);
            return;
        }
        if (!m_objectLayer) return;

        if (!m_fields->debugNode) {
            auto node = CCDrawNode::create();
            node->setZOrder(10000); // draw on top
            m_objectLayer->addChild(node);
            m_fields->debugNode = node;
        }

        auto node = m_fields->debugNode;
        node->setVisible(true);
        node->clear();

        CCPoint verts[4] = {
            ccp(left, bottom),
            ccp(right, bottom),
            ccp(right, top),
            ccp(left, top)
        };
        // transparent fill, 2px solid red border
        node->drawPolygon(verts, 4, ccc4f(0.f, 0.f, 0.f, 0.f), 2.f, ccc4f(1.f, 0.f, 0.f, 1.f));
    }

    // Hides decorative detail (currently: glow sprite) and dips opacity
    // without touching the base sprite's scale/position -- so collision
    // (derived from the base hitbox) and aspect ratio are completely
    // unaffected. NOTE: confirm m_glowSprite is the correct member on
    // GameObject for your Geode bindings version.
    void applyLod(ObjEntry & entry, bool reduced) {
        if (entry.lodReduced == reduced) return;
        entry.lodReduced = reduced;

        if (entry.obj->m_glowSprite) {
            entry.obj->m_glowSprite->setVisible(!reduced);
        }
        entry.obj->setOpacity(reduced ? 160 : 255);
    }

    // Runs every tick (independent of camera movement) over whatever's
    // currently in view, and checks each object's OWN scale -- not how big
    // it looks on screen due to camera zoom. An object shrunk via a scale
    // trigger/small-group scaling reads as scale < 1.0 here regardless of
    // where the camera is, which is the "smaller than usual" signal we want.
    void updateLod() {
        bool lodEnabled = Mod::get()->getSettingValue<bool>("enable-lod");
        if (!lodEnabled || !m_fields->built) return;

        float lodThreshold = static_cast<float>(Mod::get()->getSettingValue<double>("lod-threshold")); // fraction of normal scale, e.g. 0.5
        float lodHysteresis = static_cast<float>(Mod::get()->getSettingValue<double>("lod-hysteresis")); // e.g. 0.15 = 15%
        float offThreshold = lodThreshold * (1.f + lodHysteresis);
        float onThreshold = lodThreshold * (1.f - lodHysteresis);

        auto& grid = m_fields->grid;
        for (int64_t key : m_fields->activeKeys) {
            auto it = grid.find(key);
            if (it == grid.end()) continue;

            for (auto& entry : it->second) {
                if (!entry.obj->isVisible()) {
                    if (entry.lodReduced) applyLod(entry, false);
                    continue;
                }

                // Object's own scale, not the camera's. Non-uniform scaling
                // (rare, but some objects allow separate X/Y scale) uses the
                // larger axis so a stretched-thin object doesn't dodge LOD.
                float objScale = std::max(std::fabs(entry.obj->getScaleX()), std::fabs(entry.obj->getScaleY()));

                if (!entry.lodReduced && objScale < offThreshold) {
                    applyLod(entry, true);
                }
                else if (entry.lodReduced && objScale > onThreshold) {
                    applyLod(entry, false);
                }
            }
        }
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (!isLastTick) return;

        bool enabled = Mod::get()->getSettingValue<bool>("enable-culling");
        bool debugCulling = Mod::get()->getSettingValue<bool>("debug-culling");
        if (!enabled) {
            updateDebugDraw(0, 0, 0, 0, false); // hide overlay if culling is off
            return;
        }
        if (!m_objectLayer || !m_objects) return;

        // Rebuild if never built, the cell-size setting changed, OR the game
        // has streamed in more objects since the last build (GD loads level
        // sections progressively, so m_objects grows over time -- anything
        // added after our last build was never bucketed and would otherwise
        // sit at default visibility forever, which is why background objects
        // farther into the level weren't getting culled).
        float cellSize = getCellSize();
        int objectCount = m_objects->count();
        if (!m_fields->built || m_fields->builtCellSize != cellSize || m_fields->builtObjectCount != objectCount) {
            buildGrid();
        }

        // Cheap heuristic for the "camera hasn't moved" early-out below. This
        // is only used to skip the expensive grid scan/visibility pass -- the
        // actual bounds math no longer depends on these two values alone.
        CCPoint rawPos = m_objectLayer->getPosition();
        float rawScale = m_objectLayer->getScale();

        bool cameraMoved = !(rawPos.equals(m_fields->lastCameraPos) && rawScale == m_fields->lastScale);
        if (!cameraMoved && !debugCulling) {
            updateLod(); // object scale can change every tick, independent of the camera
            return;
        }
        m_fields->lastCameraPos = rawPos;
        m_fields->lastScale = rawScale;

        float margin = static_cast<float>(Mod::get()->getSettingValue<double>("culling-margin"));
        CCSize winSize = CCDirector::sharedDirector()->getWinSize();

        // Convert the two screen corners into m_objectLayer's local space via
        // the node's full ancestor transform chain, rather than hand-computing
        // bounds from m_objectLayer's own position/scale. The manual approach
        // silently assumed no other transform sits between the screen and
        // m_objectLayer (e.g. aspect-ratio adaptation applied on parent layers
        // for non-16:9 resolutions), which made the computed rect smaller than
        // what's actually visible -- objects outside the (wrong) rect were
        // still on-screen and never got culled.
        CCPoint corner0 = m_objectLayer->convertToNodeSpace(CCPointZero);
        CCPoint corner1 = m_objectLayer->convertToNodeSpace(ccp(winSize.width, winSize.height));

        float left = std::min(corner0.x, corner1.x) - margin;
        float right = std::max(corner0.x, corner1.x) + margin;
        float bottom = std::min(corner0.y, corner1.y) - margin;
        float top = std::max(corner0.y, corner1.y) + margin;

        float w = std::max(right - left, 0.f);
        float h = std::max(top - bottom, 0.f);
        CCRect cameraBounds(left, bottom, w, h);
        m_fields->cameraBounds = cameraBounds;

        updateDebugDraw(left, bottom, right, top, debugCulling);

        // If only the debug overlay needed updating (camera didn't actually move),
        // skip the grid scan/hide-show work below.
        if (!cameraMoved) {
            updateLod();
            return;
        }

        int cxMin = static_cast<int>(std::floor(left / cellSize));
        int cxMax = static_cast<int>(std::floor(right / cellSize));
        int cyMin = static_cast<int>(std::floor(bottom / cellSize));
        int cyMax = static_cast<int>(std::floor(top / cellSize));

        auto& grid = m_fields->grid;

        std::vector<int64_t> currKeys;
        for (int cx = cxMin; cx <= cxMax; ++cx) {
            for (int cy = cyMin; cy <= cyMax; ++cy) {
                int64_t key = cellKey(cx, cy);
                if (grid.find(key) != grid.end()) currKeys.push_back(key);
            }
        }
        m_fields->activeKeys = currKeys; // used by updateLod() every tick from here on

        // Union of this frame's active cells and last frame's -- this is what lets
        // objects that just left the window get one final hide check, same idea
        // as the previous prevLo/prevHi approach, just generalized to 2D.
        std::vector<int64_t> unionKeys = currKeys;
        unionKeys.insert(unionKeys.end(), m_fields->prevKeys.begin(), m_fields->prevKeys.end());
        std::sort(unionKeys.begin(), unionKeys.end());
        unionKeys.erase(std::unique(unionKeys.begin(), unionKeys.end()), unionKeys.end());

        for (int64_t key : unionKeys) {
            for (auto& entry : grid[key]) {
                CCPoint p = entry.obj->getPosition();
                CCRect objRect(p.x - entry.halfW, p.y - entry.halfH, entry.halfW * 2.f, entry.halfH * 2.f);
                bool inBounds = cameraBounds.intersectsRect(objRect);
                if (entry.obj->isVisible() != inBounds) {
                    entry.obj->setVisible(inBounds);
                }
                // Glow is rendered through a separate batch node in GD, not
                // as a true child of the object -- setVisible() on the base
                // object alone doesn't hide it, so it needs its own toggle
                // here, mirroring whatever the base object's cull state is.
                if (entry.obj->m_glowSprite && entry.obj->m_glowSprite->isVisible() != inBounds) {
                    entry.obj->m_glowSprite->setVisible(inBounds);
                }
            }
        }

        m_fields->prevKeys = std::move(currKeys);

        updateLod();
    }
};
