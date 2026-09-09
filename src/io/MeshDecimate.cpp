#include "MeshDecimate.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace materializr {
namespace {

using Vec3 = std::array<double, 3>;

// Symmetric 4x4 quadric stored as its 10 unique coefficients, in the order:
// [xx, xy, xz, xw, yy, yz, yw, zz, zw, ww].
using Quadric = std::array<double, 10>;

inline Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
}
inline double dot(const Vec3& a, const Vec3& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
inline double length(const Vec3& a) { return std::sqrt(dot(a, a)); }

// Accumulate the outer product of a (normalized) plane [a b c d] into Q, scaled.
void addPlane(Quadric& Q, double a, double b, double c, double d, double w) {
    Q[0]+=w*a*a; Q[1]+=w*a*b; Q[2]+=w*a*c; Q[3]+=w*a*d;
    Q[4]+=w*b*b; Q[5]+=w*b*c; Q[6]+=w*b*d;
    Q[7]+=w*c*c; Q[8]+=w*c*d;
    Q[9]+=w*d*d;
}

void addQ(Quadric& dst, const Quadric& src) {
    for (int i = 0; i < 10; ++i) dst[i] += src[i];
}

// v^T Q v for the homogeneous point (x, y, z, 1).
double vertexError(const Quadric& Q, const Vec3& p) {
    const double x = p[0], y = p[1], z = p[2];
    return Q[0]*x*x + 2*Q[1]*x*y + 2*Q[2]*x*z + 2*Q[3]*x
         + Q[4]*y*y + 2*Q[5]*y*z + 2*Q[6]*y
         + Q[7]*z*z + 2*Q[8]*z
         + Q[9];
}

inline double det3(double a,double b,double c,
                   double d,double e,double f,
                   double g,double h,double i) {
    return a*(e*i-f*h) - b*(d*i-f*g) + c*(d*h-e*g);
}

// Solve A x = b where A is the upper-left 3x3 of Q and b = -[xw, yw, zw].
// Returns false (caller falls back to endpoints) when A is near-singular.
bool optimalPoint(const Quadric& Q, Vec3& out) {
    const double a00=Q[0], a01=Q[1], a02=Q[2];
    const double a11=Q[4], a12=Q[5], a22=Q[7];
    const double b0=-Q[3], b1=-Q[6], b2=-Q[8];
    const double det = det3(a00,a01,a02, a01,a11,a12, a02,a12,a22);
    const double scale = std::fabs(a00)+std::fabs(a11)+std::fabs(a22)+1e-30;
    if (std::fabs(det) < 1e-10 * scale) return false;
    const double inv = 1.0 / det;
    out[0] = det3(b0,a01,a02, b1,a11,a12, b2,a12,a22) * inv;
    out[1] = det3(a00,b0,a02, a01,b1,a12, a02,b2,a22) * inv;
    out[2] = det3(a00,a01,b0, a01,a11,b1, a02,a12,b2) * inv;
    return true;
}

struct Collapse {
    double cost;
    int u, v;        // collapse v into u
    int versionU, versionV;
    Vec3 target;
};
struct CollapseGreater {
    bool operator()(const Collapse& a, const Collapse& b) const { return a.cost > b.cost; }
};

uint64_t edgeKey(int a, int b) {
    if (a > b) std::swap(a, b);
    return (static_cast<uint64_t>(static_cast<uint32_t>(a)) << 32) |
           static_cast<uint32_t>(b);
}

} // namespace

int decimateMesh(SimpleMesh& mesh, int targetTriangles) {
    const int nTris = static_cast<int>(mesh.tris.size());
    if (nTris <= targetTriangles || targetTriangles < 1) return nTris;

    const int nVerts = static_cast<int>(mesh.nodes.size());
    std::vector<Vec3> pos(nVerts);
    for (int i = 0; i < nVerts; ++i)
        pos[i] = {mesh.nodes[i].X(), mesh.nodes[i].Y(), mesh.nodes[i].Z()};

    std::vector<std::array<int, 3>> tris = mesh.tris;
    std::vector<char> triAlive(nTris, 1);
    std::vector<char> vAlive(nVerts, 1);
    std::vector<int> version(nVerts, 0);
    std::vector<Quadric> Q(nVerts);
    for (auto& q : Q) q.fill(0.0);
    std::vector<std::vector<int>> vtri(nVerts); // incident triangles per vertex (lazy: may hold dead)

    // Triangle plane → normalized (a,b,c,d); returns false for a degenerate facet.
    auto trianglePlane = [&](int t, double& a, double& b, double& c, double& d) -> bool {
        const auto& T = tris[t];
        const Vec3 n = cross(sub(pos[T[1]], pos[T[0]]), sub(pos[T[2]], pos[T[0]]));
        const double len = length(n);
        if (len < 1e-20) return false;
        a = n[0]/len; b = n[1]/len; c = n[2]/len;
        d = -(a*pos[T[0]][0] + b*pos[T[0]][1] + c*pos[T[0]][2]);
        return true;
    };

    // Accumulate per-triangle quadrics + adjacency, drop degenerate facets.
    int aliveTriCount = 0;
    std::unordered_map<uint64_t, int> edgeUseCount;
    for (int t = 0; t < nTris; ++t) {
        double a, b, c, d;
        if (!trianglePlane(t, a, b, c, d)) { triAlive[t] = 0; continue; }
        ++aliveTriCount;
        Quadric kp; kp.fill(0.0);
        addPlane(kp, a, b, c, d, 1.0);
        for (int k = 0; k < 3; ++k) {
            addQ(Q[tris[t][k]], kp);
            vtri[tris[t][k]].push_back(t);
        }
        for (int k = 0; k < 3; ++k)
            edgeUseCount[edgeKey(tris[t][k], tris[t][(k+1)%3])]++;
    }

    // Boundary preservation: an edge used by a single triangle is an open border.
    // Pin it by adding a heavily-weighted plane through the edge, perpendicular
    // to that triangle, to both endpoints' quadrics.
    for (int t = 0; t < nTris; ++t) {
        if (!triAlive[t]) continue;
        double na, nb, nc, nd;
        if (!trianglePlane(t, na, nb, nc, nd)) continue;
        const Vec3 tn = {na, nb, nc};
        for (int k = 0; k < 3; ++k) {
            const int u = tris[t][k], w = tris[t][(k+1)%3];
            if (edgeUseCount[edgeKey(u, w)] != 1) continue; // interior edge
            const Vec3 edir = sub(pos[w], pos[u]);
            Vec3 pn = cross(edir, tn);
            const double len = length(pn);
            if (len < 1e-20) continue;
            pn = {pn[0]/len, pn[1]/len, pn[2]/len};
            const double pd = -dot(pn, pos[u]);
            const double wgt = 1000.0; // dominate interior quadrics so borders hold
            addPlane(Q[u], pn[0], pn[1], pn[2], pd, wgt);
            addPlane(Q[w], pn[0], pn[1], pn[2], pd, wgt);
        }
    }

    // Cost + optimal target for collapsing edge (u, v).
    auto evaluate = [&](int u, int v, Vec3& target) -> double {
        Quadric q = Q[u];
        addQ(q, Q[v]);
        if (!optimalPoint(q, target)) {
            // Singular: pick the cheaper of u, v, midpoint.
            const Vec3 mid = {(pos[u][0]+pos[v][0])*0.5,
                              (pos[u][1]+pos[v][1])*0.5,
                              (pos[u][2]+pos[v][2])*0.5};
            const double eu = vertexError(q, pos[u]);
            const double ev = vertexError(q, pos[v]);
            const double em = vertexError(q, mid);
            if (eu <= ev && eu <= em) { target = pos[u]; return eu; }
            if (ev <= em)             { target = pos[v]; return ev; }
            target = mid; return em;
        }
        return vertexError(q, target);
    };

    std::priority_queue<Collapse, std::vector<Collapse>, CollapseGreater> heap;
    auto pushEdge = [&](int u, int v) {
        if (u == v || !vAlive[u] || !vAlive[v]) return;
        Vec3 target;
        const double cost = evaluate(u, v, target);
        heap.push({cost, u, v, version[u], version[v], target});
    };

    {
        std::unordered_set<uint64_t> seen;
        for (int t = 0; t < nTris; ++t) {
            if (!triAlive[t]) continue;
            for (int k = 0; k < 3; ++k) {
                int u = tris[t][k], v = tris[t][(k+1)%3];
                if (seen.insert(edgeKey(u, v)).second) pushEdge(u, v);
            }
        }
    }

    // Would collapsing v→u (moving the merged vertex to `target`) flip any of the
    // surrounding triangles? Reject the collapse if so - guards against folds.
    auto wouldFlip = [&](int u, int v, const Vec3& target) -> bool {
        for (int pass = 0; pass < 2; ++pass) {
            const int src = pass == 0 ? u : v;
            for (int t : vtri[src]) {
                if (!triAlive[t]) continue;
                const auto& T = tris[t];
                // Skip triangles that contain the edge itself (they vanish).
                bool hasU = (T[0]==u||T[1]==u||T[2]==u);
                bool hasV = (T[0]==v||T[1]==v||T[2]==v);
                if (hasU && hasV) continue;
                Vec3 p[3];
                for (int k = 0; k < 3; ++k) {
                    int idx = T[k];
                    p[k] = (idx == u || idx == v) ? target : pos[idx];
                }
                const Vec3 oldN = cross(sub(pos[T[1]], pos[T[0]]), sub(pos[T[2]], pos[T[0]]));
                const Vec3 newN = cross(sub(p[1], p[0]), sub(p[2], p[0]));
                if (length(newN) < 1e-20) return true; // collapsed to a sliver
                if (dot(oldN, newN) < 0.0) return true; // normal flipped
            }
            if (u == v) break;
        }
        return false;
    };

    while (aliveTriCount > targetTriangles && !heap.empty()) {
        const Collapse c = heap.top();
        heap.pop();
        if (!vAlive[c.u] || !vAlive[c.v]) continue;
        if (version[c.u] != c.versionU || version[c.v] != c.versionV) continue; // stale
        if (wouldFlip(c.u, c.v, c.target)) continue;

        // Commit: keep u at the optimal target, fold v into it.
        pos[c.u] = c.target;
        addQ(Q[c.u], Q[c.v]);

        for (int t : vtri[c.v]) {
            if (!triAlive[t]) continue;
            auto& T = tris[t];
            for (int k = 0; k < 3; ++k) if (T[k] == c.v) T[k] = c.u;
            if (T[0]==T[1] || T[1]==T[2] || T[0]==T[2]) {
                triAlive[t] = 0; // degenerate (was on the collapsed edge)
                --aliveTriCount;
            } else {
                vtri[c.u].push_back(t);
            }
        }
        vAlive[c.v] = 0;
        ++version[c.u];

        // Re-cost every edge now incident to u.
        std::unordered_set<int> neighbors;
        for (int t : vtri[c.u]) {
            if (!triAlive[t]) continue;
            for (int k = 0; k < 3; ++k)
                if (tris[t][k] != c.u) neighbors.insert(tris[t][k]);
        }
        for (int k : neighbors) pushEdge(c.u, k);
    }

    // Compact survivors into a fresh mesh.
    std::vector<int> remap(nVerts, -1);
    SimpleMesh out;
    out.nodes.reserve(nVerts);
    for (int t = 0; t < nTris; ++t) {
        if (!triAlive[t]) continue;
        std::array<int, 3> nt;
        for (int k = 0; k < 3; ++k) {
            const int oldIdx = tris[t][k];
            if (remap[oldIdx] == -1) {
                remap[oldIdx] = static_cast<int>(out.nodes.size());
                out.nodes.emplace_back(pos[oldIdx][0], pos[oldIdx][1], pos[oldIdx][2]);
            }
            nt[k] = remap[oldIdx];
        }
        out.tris.push_back(nt);
    }

    mesh = std::move(out);
    return static_cast<int>(mesh.tris.size());
}

} // namespace materializr
