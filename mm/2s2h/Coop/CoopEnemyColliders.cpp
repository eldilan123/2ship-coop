#include "CoopEnemyColliders.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

extern "C" {
#include "functions.h"
}

namespace Coop::Enemies {

namespace {

using namespace Wire;

// s16 values per element: JntSph (center, radius), Cylinder (radius, height, yShift, pos), Tris (3 vertices),
// Quad (4 vertices), Sphere (center, radius). Positions are relative to the actor's world position.
constexpr uint8_t kGeometryCount[COLSHAPE_MAX] = { 4, 6, 9, 12, 4 };
constexpr size_t kMaxTrisElements = 16;

std::unordered_map<const Actor*, std::vector<Collider*>> sAcByActor;

int16_t Rel(float world, float base) {
    return static_cast<int16_t>(std::clamp(std::lround(world - base), -32768L, 32767L));
}

float Abs(int16_t rel, float base) {
    return base + static_cast<float>(rel);
}

int16_t AbsS16(int16_t rel, float base) {
    return static_cast<int16_t>(std::clamp(std::lround(base + rel), -32768L, 32767L));
}

size_t ShapeSize(uint8_t shape) {
    switch (shape) {
        case COLSHAPE_JNTSPH:
            return sizeof(ColliderJntSph);
        case COLSHAPE_CYLINDER:
            return sizeof(ColliderCylinder);
        case COLSHAPE_TRIS:
            return sizeof(ColliderTris);
        case COLSHAPE_QUAD:
            return sizeof(ColliderQuad);
        case COLSHAPE_SPHERE:
            return sizeof(ColliderSphere);
        default:
            return 0;
    }
}

void PutVertex(AcElement& e, size_t at, const Vec3f& v, const Vec3f& base) {
    e.geometry[at] = Rel(v.x, base.x);
    e.geometry[at + 1] = Rel(v.y, base.y);
    e.geometry[at + 2] = Rel(v.z, base.z);
}

Vec3f GetVertex(const AcElement& e, size_t at, const Vec3f& base) {
    return { Abs(e.geometry[at], base.x), Abs(e.geometry[at + 1], base.y), Abs(e.geometry[at + 2], base.z) };
}

void PutSphere(AcElement& e, const Sphere16& s, const Vec3f& base) {
    e.geometry[0] = Rel(s.center.x, base.x);
    e.geometry[1] = Rel(s.center.y, base.y);
    e.geometry[2] = Rel(s.center.z, base.z);
    e.geometry[3] = s.radius;
}

void GetSphere(const AcElement& e, Sphere16& s, const Vec3f& base) {
    s.center = { AbsS16(e.geometry[0], base.x), AbsS16(e.geometry[1], base.y), AbsS16(e.geometry[2], base.z) };
    s.radius = e.geometry[3];
}

// The host's view of one registered collider.
AcCollider Describe(const Actor* actor, Collider* col) {
    AcCollider c;
    c.offset = static_cast<uint16_t>(reinterpret_cast<const uint8_t*>(col) - reinterpret_cast<const uint8_t*>(actor));
    c.shape = col->shape;
    c.acFlags = col->acFlags & ~(AC_HIT | AC_BOUNCED);
    const Vec3f& base = actor->world.pos;
    switch (col->shape) {
        case COLSHAPE_JNTSPH: {
            auto* j = reinterpret_cast<ColliderJntSph*>(col);
            for (s32 i = 0; i < j->count && i < static_cast<s32>(kMaxAcElements); i++) {
                AcElement e;
                e.flags = j->elements[i].base.acElemFlags & kAcElemKeepFlags;
                PutSphere(e, j->elements[i].dim.worldSphere, base);
                c.elements.push_back(e);
            }
            break;
        }
        case COLSHAPE_CYLINDER: {
            auto* cyl = reinterpret_cast<ColliderCylinder*>(col);
            AcElement e;
            e.flags = cyl->elem.acElemFlags & kAcElemKeepFlags;
            e.geometry[0] = cyl->dim.radius;
            e.geometry[1] = cyl->dim.height;
            e.geometry[2] = cyl->dim.yShift;
            e.geometry[3] = Rel(cyl->dim.pos.x, base.x);
            e.geometry[4] = Rel(cyl->dim.pos.y, base.y);
            e.geometry[5] = Rel(cyl->dim.pos.z, base.z);
            c.elements.push_back(e);
            break;
        }
        case COLSHAPE_TRIS: {
            auto* tris = reinterpret_cast<ColliderTris*>(col);
            for (s32 i = 0; i < tris->count && i < static_cast<s32>(kMaxTrisElements); i++) {
                AcElement e;
                e.flags = tris->elements[i].base.acElemFlags & kAcElemKeepFlags;
                for (size_t v = 0; v < 3; v++) {
                    PutVertex(e, v * 3, tris->elements[i].dim.vtx[v], base);
                }
                c.elements.push_back(e);
            }
            break;
        }
        case COLSHAPE_QUAD: {
            auto* quad = reinterpret_cast<ColliderQuad*>(col);
            AcElement e;
            e.flags = quad->elem.acElemFlags & kAcElemKeepFlags;
            for (size_t v = 0; v < 4; v++) {
                PutVertex(e, v * 3, quad->dim.quad[v], base);
            }
            c.elements.push_back(e);
            break;
        }
        case COLSHAPE_SPHERE: {
            auto* sph = reinterpret_cast<ColliderSphere*>(col);
            AcElement e;
            e.flags = sph->elem.acElemFlags & kAcElemKeepFlags;
            PutSphere(e, sph->dim.worldSphere, base);
            c.elements.push_back(e);
            break;
        }
    }
    return c;
}

} // namespace

void CaptureAcRegistrations(PlayState* play) {
    sAcByActor.clear();
    const CollisionCheckContext& ctx = play->colChkCtx;
    const s32 count = std::clamp<s32>(ctx.colACCount, 0, static_cast<s32>(ARRAY_COUNT(ctx.colAC)));
    for (s32 i = 0; i < count; i++) {
        Collider* col = ctx.colAC[i];
        if (col == nullptr || col->actor == nullptr) {
            continue;
        }
        std::vector<Collider*>& list = sAcByActor[col->actor];
        if (list.size() < kMaxAcColliders && std::find(list.begin(), list.end(), col) == list.end()) {
            list.push_back(col);
        }
    }
}

void EncodeAcColliders(const Actor* actor, Writer& w) {
    auto it = sAcByActor.find(actor);
    const size_t instanceSize = actor->overlayEntry != nullptr && actor->overlayEntry->profile != nullptr
                                    ? actor->overlayEntry->profile->instanceSize
                                    : 0;
    std::vector<AcCollider> colliders;
    if (it != sAcByActor.end()) {
        for (Collider* col : it->second) {
            const uintptr_t offset = reinterpret_cast<uintptr_t>(col) - reinterpret_cast<uintptr_t>(actor);
            if (col->shape < COLSHAPE_MAX && offset + ShapeSize(col->shape) <= instanceSize && offset <= 0xFFFF) {
                colliders.push_back(Describe(actor, col));
            }
        }
    }
    w.Put(static_cast<uint8_t>(colliders.size()));
    for (const AcCollider& c : colliders) {
        w.Put(c.offset);
        w.Put(c.shape);
        w.Put(c.acFlags);
        w.Put(static_cast<uint8_t>(c.elements.size()));
        for (const AcElement& e : c.elements) {
            w.Put(e.flags);
            w.Bytes(e.geometry.data(), sizeof(int16_t) * kGeometryCount[c.shape]);
        }
    }
}

bool DecodeAcColliders(Reader& r, AcColliders& out, const char*& why) {
    out.clear();
    const uint8_t count = r.Get<uint8_t>();
    if (count > kMaxAcColliders) {
        why = "too many colliders";
        return false;
    }
    for (uint8_t i = 0; i < count; i++) {
        AcCollider c;
        c.offset = r.Get<uint16_t>();
        c.shape = r.Get<uint8_t>();
        c.acFlags = r.Get<uint8_t>() & ~(AC_HIT | AC_BOUNCED);
        const uint8_t elements = r.Get<uint8_t>();
        if (!r.ok) {
            break;
        }
        const bool single = c.shape == COLSHAPE_CYLINDER || c.shape == COLSHAPE_QUAD || c.shape == COLSHAPE_SPHERE;
        const size_t maxElements = c.shape == COLSHAPE_TRIS ? kMaxTrisElements : kMaxAcElements;
        if (c.shape >= COLSHAPE_MAX || (single && elements != 1) || elements > maxElements) {
            why = "bad collider shape or element count";
            return false;
        }
        for (uint8_t e = 0; e < elements; e++) {
            AcElement element;
            element.flags = r.Get<uint8_t>();
            r.Bytes(element.geometry.data(), sizeof(int16_t) * kGeometryCount[c.shape]);
            if (element.flags & ~kAcElemKeepFlags) {
                why = "bad collider element flags";
                return false;
            }
            c.elements.push_back(element);
        }
        out.push_back(std::move(c));
    }
    if (!r.ok) {
        why = "truncated colliders";
        return false;
    }
    return true;
}

Collider* PuppetCollider(Actor* actor, uint16_t offset, uint8_t shape, size_t elementCount) {
    if (actor->overlayEntry == nullptr || actor->overlayEntry->profile == nullptr || shape >= COLSHAPE_MAX ||
        static_cast<size_t>(offset) + ShapeSize(shape) > actor->overlayEntry->profile->instanceSize) {
        return nullptr;
    }
    Collider* col = reinterpret_cast<Collider*>(reinterpret_cast<uint8_t*>(actor) + offset);
    if (col->actor != actor || col->shape != shape) {
        return nullptr;
    }
    if (shape == COLSHAPE_JNTSPH) {
        auto* j = reinterpret_cast<ColliderJntSph*>(col);
        if (j->elements == nullptr || static_cast<size_t>(std::min<s32>(j->count, kMaxAcElements)) != elementCount) {
            return nullptr;
        }
    } else if (shape == COLSHAPE_TRIS) {
        auto* tris = reinterpret_cast<ColliderTris*>(col);
        if (tris->elements == nullptr ||
            static_cast<size_t>(std::min<s32>(tris->count, kMaxTrisElements)) != elementCount) {
            return nullptr;
        }
    }
    return col;
}

void RegisterPuppetColliders(PlayState* play, Actor* actor, const AcColliders& colliders, const Vec3f& base,
                             std::vector<Collider*>& registered, int& mismatches) {
    for (const AcCollider& c : colliders) {
        Collider* col = PuppetCollider(actor, c.offset, c.shape, c.elements.size());
        if (col == nullptr) {
            mismatches++;
            continue;
        }
        auto setFlags = [](ColliderElement& elem, uint8_t flags) {
            elem.acElemFlags = (elem.acElemFlags & ~kAcElemKeepFlags) | flags;
        };
        switch (c.shape) {
            case COLSHAPE_JNTSPH: {
                auto* j = reinterpret_cast<ColliderJntSph*>(col);
                for (size_t i = 0; i < c.elements.size(); i++) {
                    setFlags(j->elements[i].base, c.elements[i].flags);
                    GetSphere(c.elements[i], j->elements[i].dim.worldSphere, base);
                }
                break;
            }
            case COLSHAPE_CYLINDER: {
                auto* cyl = reinterpret_cast<ColliderCylinder*>(col);
                const AcElement& e = c.elements[0];
                setFlags(cyl->elem, e.flags);
                cyl->dim.radius = e.geometry[0];
                cyl->dim.height = e.geometry[1];
                cyl->dim.yShift = e.geometry[2];
                cyl->dim.pos = { AbsS16(e.geometry[3], base.x), AbsS16(e.geometry[4], base.y),
                                 AbsS16(e.geometry[5], base.z) };
                break;
            }
            case COLSHAPE_TRIS: {
                auto* tris = reinterpret_cast<ColliderTris*>(col);
                for (size_t i = 0; i < c.elements.size(); i++) {
                    setFlags(tris->elements[i].base, c.elements[i].flags);
                    Vec3f a = GetVertex(c.elements[i], 0, base);
                    Vec3f b = GetVertex(c.elements[i], 3, base);
                    Vec3f d = GetVertex(c.elements[i], 6, base);
                    Collider_SetTrisVertices(tris, static_cast<s32>(i), &a, &b, &d);
                }
                break;
            }
            case COLSHAPE_QUAD: {
                auto* quad = reinterpret_cast<ColliderQuad*>(col);
                setFlags(quad->elem, c.elements[0].flags);
                Vec3f v[4];
                for (size_t i = 0; i < 4; i++) {
                    v[i] = GetVertex(c.elements[0], i * 3, base);
                }
                Collider_SetQuadVertices(quad, &v[0], &v[1], &v[2], &v[3]);
                break;
            }
            case COLSHAPE_SPHERE: {
                auto* sph = reinterpret_cast<ColliderSphere*>(col);
                setFlags(sph->elem, c.elements[0].flags);
                GetSphere(c.elements[0], sph->dim.worldSphere, base);
                break;
            }
        }
        col->acFlags = c.acFlags;
        if (CollisionCheck_SetAC(play, &play->colChkCtx, col) >= 0) {
            registered.push_back(col);
        }
    }
}

} // namespace Coop::Enemies
