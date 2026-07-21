#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/loader/SettingV3.hpp>
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

// --------------------------------------------------------------------------
// Cached settings. Settings are global (not per-GameLayer), so they live here
// rather than in Fields. listenForSettingChanges only fires on a *change* --
// it does not fire on startup -- so every value here must also be seeded once
// with getSettingValue in $on_mod(Loaded) below. Everything runs on the main
// thread (settings UI callbacks and processCommands both do), so plain
// fields are fine here; no atomics/locking needed.
// --------------------------------------------------------------------------
namespace {
    struct CullingSettings {
        bool enableCulling = true;
        bool debugCulling = false;
        bool ignoreTriggers = false;
        bool enableLod = false;
        float cellSize = 300.f;
        float margin = 0.f;
        float lodThreshold = 0.5f;
        float lodHysteresis = 0.15f;
    };

    CullingSettings g_settings;

    // Same UB-guard the original getCellSize() had, kept in one place now.
    float sanitizeCellSize(double raw) {
        float v = static_cast<float>(raw);
        return v <= 0.f ? 300.f : v;
    }
}

$on_mod(Loaded) {
    auto mod = Mod::get();

    // Seed initial values -- listenForSettingChanges will NOT call these on
    // startup, only on subsequent edits, so this step isn't optional.
    g_settings.enableCulling = mod->getSettingValue<bool>("enable-culling");
    g_settings.debugCulling = mod->getSettingValue<bool>("debug-culling");
    g_settings.ignoreTriggers = mod->getSettingValue<bool>("ignore-triggers");
    g_settings.enableLod = mod->getSettingValue<bool>("enable-lod");
    g_settings.cellSize = sanitizeCellSize(mod->getSettingValue<double>("cell-size"));
    g_settings.margin = static_cast<float>(mod->getSettingValue<double>("culling-margin"));
    g_settings.lodThreshold = static_cast<float>(mod->getSettingValue<double>("lod-threshold"));
    g_settings.lodHysteresis = static_cast<float>(mod->getSettingValue<double>("lod-hysteresis"));

    listenForSettingChanges<bool>("enable-culling", [](bool v) {
        g_settings.enableCulling = v;
        });
    listenForSettingChanges<bool>("debug-culling", [](bool v) {
        g_settings.debugCulling = v;
        });
    listenForSettingChanges<bool>("ignore-triggers", [](bool v) {
        g_settings.ignoreTriggers = v;
        });
    listenForSettingChanges<bool>("enable-lod", [](bool v) {
        g_settings.enableLod = v;
        });
    listenForSettingChanges<double>("cell-size", [](double v) {
        g_settings.cellSize = sanitizeCellSize(v);
        });
    listenForSettingChanges<double>("culling-margin", [](double v) {
        g_settings.margin = static_cast<float>(v);
        });
    listenForSettingChanges<double>("lod-threshold", [](double v) {
        g_settings.lodThreshold = static_cast<float>(v);
        });
    listenForSettingChanges<double>("lod-hysteresis", [](double v) {
        g_settings.lodHysteresis = static_cast<float>(v);
        });
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
        int builtObjectCount = -1;       // m_objects->count() at last build/append -- detects newly streamed-in objects
        CCPoint lastCameraPos = CCPoint(NAN, NAN);
        float lastScale = -1.f;
        CCDrawNode* debugNode = nullptr; // lazily created, lives on m_objectLayer
    };

    // Buckets a single object into the grid. Shared by buildGrid() (full
    // rebuild) and addObjectsToGrid() (incremental append) so the two paths
    // can't drift out of sync with each other.
    void bucketObject(GameObject * obj, float cellSize, bool ignoreTriggers) {
        if (!obj) return;
        if (ignoreTriggers && obj->m_isTrigger) return; // never touch these

        CCRect r = obj->boundingBox();
        float halfW = r.size.width * 0.5f;
        float halfH = r.size.height * 0.5f;
        CCPoint p = obj->getPosition();

        int cx = static_cast<int>(std::floor(p.x / cellSize));
        int cy = static_cast<int>(std::floor(p.y / cellSize));
        m_fields->grid[cellKey(cx, cy)].push_back({ obj, halfW, halfH, false });
    }

    // Full rebuild: clears everything and re-buckets every object. Only
    // needed on first run or when cell-size actually changes (rare -- a user
    // moving a slider), since that changes which bucket every object belongs
    // in.
    void buildGrid() {
        auto& grid = m_fields->grid;
        grid.clear();
        m_fields->prevKeys.clear();
        m_fields->activeKeys.clear();
        if (!m_objects) return;

        float cellSize = g_settings.cellSize;
        bool ignoreTriggers = g_settings.ignoreTriggers;

        for (int i = 0; i < m_objects->count(); ++i) {
            bucketObject(static_cast<GameObject*>(m_objects->objectAtIndex(i)), cellSize, ignoreTriggers);
        }

        m_fields->built = true;
        m_fields->builtCellSize = cellSize;
        m_fields->builtObjectCount = m_objects->count();
    }

    // Incremental append: GD streams objects into m_objects progressively as
    // the player approaches them, so objectCount climbs steadily through a
    // level. Existing objects are already correctly bucketed for the current
    // cell size, so there's no need to re-touch them -- just bucket the new
    // tail. This assumes m_objects only grows by append (GD's normal
    // streaming behavior) and that indices already seen never get reordered
    // or removed out from under us; if some other mod mutates m_objects
    // that way, this would silently miss/duplicate entries, hence the sanity
    // check below rather than assuming it blindly.
    void addObjectsToGrid(int fromIndex) {
        if (!m_objects) return;
        int count = m_objects->count();
        if (count <= fromIndex) return; // nothing new, or count went backwards unexpectedly

        float cellSize = g_settings.cellSize;
        bool ignoreTriggers = g_settings.ignoreTriggers;

        for (int i = fromIndex; i < count; ++i) {
            bucketObject(static_cast<GameObject*>(m_objects->objectAtIndex(i)), cellSize, ignoreTriggers);
        }
        m_fields->builtObjectCount = count;
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
        if (!g_settings.enableLod || !m_fields->built) return;

        float lodThreshold = g_settings.lodThreshold; // fraction of normal scale, e.g. 0.5
        float lodHysteresis = g_settings.lodHysteresis; // e.g. 0.15 = 15%
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

        if (!g_settings.enableCulling) {
            updateDebugDraw(0, 0, 0, 0, false); // hide overlay if culling is off
            return;
        }
        if (!m_objectLayer || !m_objects) return;

        // Rebuild only if never built or the cell-size setting actually
        // changed (changes every object's bucket). Otherwise, if GD has
        // streamed in more objects since the last build/append, just bucket
        // the new tail -- cheap, and avoids re-touching objects that are
        // already correctly bucketed.
        float cellSize = g_settings.cellSize;
        int objectCount = m_objects->count();
        if (!m_fields->built || m_fields->builtCellSize != cellSize) {
            buildGrid();
        }
        else if (objectCount > m_fields->builtObjectCount) {
            addObjectsToGrid(m_fields->builtObjectCount);
        }

        // Cheap heuristic for the "camera hasn't moved" early-out below. This
        // is only used to skip the expensive grid scan/visibility pass -- the
        // actual bounds math no longer depends on these two values alone.
        CCPoint rawPos = m_objectLayer->getPosition();
        float rawScale = m_objectLayer->getScale();

        bool debugCulling = g_settings.debugCulling;
        bool cameraMoved = !(rawPos.equals(m_fields->lastCameraPos) && rawScale == m_fields->lastScale);
        if (!cameraMoved && !debugCulling) {
            updateLod(); // object scale can change every tick, independent of the camera
            return;
        }
        m_fields->lastCameraPos = rawPos;
        m_fields->lastScale = rawScale;

        float margin = g_settings.margin;
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
