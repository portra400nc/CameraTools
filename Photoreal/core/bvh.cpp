#include "bvh.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>
#include <stdexcept>

// No fused multiply-adds, so a build on the Mac (arm64) gives the bytes and hash of the Deck's (x86-64 without FMA).
#pragma STDC FP_CONTRACT OFF

namespace photoreal
{
    namespace
    {
        constexpr uint32_t kMaxBins = 64;

        struct Box
        {
            float lo[3] = { INFINITY, INFINITY, INFINITY };
            float hi[3] = { -INFINITY, -INFINITY, -INFINITY };

            void add(const float p[3])
            {
                for (int k = 0; k < 3; k++)
                {
                    lo[k] = std::min(lo[k], p[k]);
                    hi[k] = std::max(hi[k], p[k]);
                }
            }
            void add(const Box &b)
            {
                if (b.lo[0] > b.hi[0])
                    return;
                add(b.lo);
                add(b.hi);
            }
            float area() const
            {
                if (lo[0] > hi[0])
                    return 0;
                const float dx = hi[0] - lo[0], dy = hi[1] - lo[1], dz = hi[2] - lo[2];
                return 2 * (dx * dy + dy * dz + dz * dx);
            }
        };

        struct Split
        {
            bool leaf = true;
            uint32_t middle = 0;    // the right child's first triangle in the order
            Box left, right;
        };

        // The build's shared state. Tasks work on disjoint ranges of order, so they need no lock, and the result is the
        // same however many there are.
        struct Builder
        {
            const BvhOptions &options;
            std::vector<Box> boxes;
            std::vector<std::array<float, 3>> centroids;
            std::vector<uint32_t> order;
            uint32_t parallel_depth = 0;

            Box bounds(uint32_t first, uint32_t n) const
            {
                Box b;
                for (uint32_t i = first; i < first + n; i++)
                    b.add(boxes[order[i]]);
                return b;
            }

            int bin_of(float c, float lo, float scale) const
            {
                return std::clamp(static_cast<int>((c - lo) * scale), 0, static_cast<int>(std::clamp<uint32_t>(options.bins, 2, kMaxBins)) - 1);
            }

            Split split(uint32_t first, uint32_t n)
            {
                Split s;
                if (n <= std::min(options.max_leaf, kMaxLeaf))
                    return s;
                Box centers;
                for (uint32_t i = first; i < first + n; i++)
                    centers.add(centroids[order[i]].data());
                const float parent = bounds(first, n).area();
                const uint32_t bins = std::clamp<uint32_t>(options.bins, 2, kMaxBins);
                float best = INFINITY;
                int best_axis = -1;
                int best_bin = 0;
                std::array<Box, kMaxBins> bin_boxes;
                std::array<uint32_t, kMaxBins> bin_counts;
                std::array<float, kMaxBins> left_area;
                std::array<uint32_t, kMaxBins> left_count;
                for (int axis = 0; axis < 3; axis++)
                {
                    const float lo = centers.lo[axis], hi = centers.hi[axis];
                    if (!(hi - lo > 1e-6f))
                        continue;
                    std::fill(bin_boxes.begin(), bin_boxes.begin() + bins, Box {});
                    std::fill(bin_counts.begin(), bin_counts.begin() + bins, 0u);
                    const float scale = static_cast<float>(bins) * 0.9999f / (hi - lo);
                    for (uint32_t i = first; i < first + n; i++)
                    {
                        const int b = bin_of(centroids[order[i]][axis], lo, scale);
                        bin_boxes[b].add(boxes[order[i]]);
                        bin_counts[b]++;
                    }
                    Box acc;
                    uint32_t count = 0;
                    for (uint32_t b = 0; b + 1 < bins; b++)
                    {
                        acc.add(bin_boxes[b]);
                        count += bin_counts[b];
                        left_area[b] = acc.area();
                        left_count[b] = count;
                    }
                    acc = Box {};
                    count = 0;
                    for (uint32_t b = bins - 1; b > 0; b--)
                    {
                        acc.add(bin_boxes[b]);
                        count += bin_counts[b];
                        const float cost = left_area[b - 1] * left_count[b - 1] + acc.area() * count;
                        if (left_count[b - 1] != 0 && count != 0 && cost < best)
                        {
                            best = cost;
                            best_axis = axis;
                            best_bin = static_cast<int>(b);
                        }
                    }
                }
                // A leaf costs n intersections; a split costs one traversal plus the children's intersections weighted by
                // the chance a ray through the parent enters each.
                const bool leaf_cheaper = best_axis < 0 || !(parent > 0) || 1 + best / parent >= static_cast<float>(n);
                if (leaf_cheaper && n <= kMaxLeaf)
                    return s;
                s.leaf = false;
                if (best_axis < 0)
                {
                    // Every centroid in one point, or every split leaves a side empty: halve the range as it is.
                    s.middle = first + n / 2;
                }
                else
                {
                    const float lo = centers.lo[best_axis], scale = static_cast<float>(bins) * 0.9999f / (centers.hi[best_axis] - lo);
                    const auto middle = std::partition(order.begin() + first, order.begin() + first + n,
                        [&](uint32_t t) { return bin_of(centroids[t][best_axis], lo, scale) < best_bin; });
                    s.middle = static_cast<uint32_t>(middle - order.begin());
                }
                s.left = bounds(first, s.middle - first);
                s.right = bounds(s.middle, first + n - s.middle);
                return s;
            }

            static uint32_t leaf_link(uint32_t first, uint32_t n) { return kLeafBit | first << 4 | (n - 1); }

            static void set_box(BvhNode &node, int child, const Box &b)
            {
                std::memcpy(child == 0 ? node.lo0 : node.lo1, b.lo, sizeof b.lo);
                std::memcpy(child == 0 ? node.hi0 : node.hi1, b.hi, sizeof b.hi);
            }

            // Appends the node of [first, first + n), whose split is s, and its subtree, depth first, links counted from
            // the start of out.
            void build_into(std::vector<BvhNode> &out, uint32_t first, uint32_t n, const Split &s)
            {
                const size_t index = out.size();
                out.push_back({});
                set_box(out[index], 0, s.left);
                set_box(out[index], 1, s.right);
                const uint32_t ranges[2][2] = { { first, s.middle - first }, { s.middle, first + n - s.middle } };
                for (int c = 0; c < 2; c++)
                {
                    const Split child = split(ranges[c][0], ranges[c][1]);
                    if (child.leaf)
                    {
                        out[index].link[c] = leaf_link(ranges[c][0], ranges[c][1]);
                        continue;
                    }
                    out[index].link[c] = static_cast<uint32_t>(out.size());
                    build_into(out, ranges[c][0], ranges[c][1], child);
                }
            }

            // The same as build_into, the first parallel_depth levels with the left child on its own thread, each subtree
            // in its own vector and joined after, so the numbering is build_into's.
            std::vector<BvhNode> subtree(uint32_t first, uint32_t n, const Split &s, uint32_t depth)
            {
                std::vector<BvhNode> out;
                if (depth >= parallel_depth)
                {
                    build_into(out, first, n, s);
                    return out;
                }
                const uint32_t ranges[2][2] = { { first, s.middle - first }, { s.middle, first + n - s.middle } };
                const Split left = split(ranges[0][0], ranges[0][1]);
                std::future<std::vector<BvhNode>> pending;
                if (!left.leaf)
                    pending = std::async(std::launch::async, [this, &ranges, &left, depth] { return subtree(ranges[0][0], ranges[0][1], left, depth + 1); });
                const Split right = split(ranges[1][0], ranges[1][1]);
                std::vector<BvhNode> right_nodes = right.leaf ? std::vector<BvhNode> {} : subtree(ranges[1][0], ranges[1][1], right, depth + 1);
                std::vector<BvhNode> left_nodes = pending.valid() ? pending.get() : std::vector<BvhNode> {};

                out.resize(1 + left_nodes.size() + right_nodes.size());
                BvhNode &node = out[0];
                set_box(node, 0, s.left);
                set_box(node, 1, s.right);
                node.link[0] = left.leaf ? leaf_link(ranges[0][0], ranges[0][1]) : 1;
                node.link[1] = right.leaf ? leaf_link(ranges[1][0], ranges[1][1]) : static_cast<uint32_t>(1 + left_nodes.size());
                const auto place = [&out](const std::vector<BvhNode> &nodes, uint32_t at) {
                    for (size_t i = 0; i < nodes.size(); i++)
                    {
                        BvhNode moved = nodes[i];
                        for (uint32_t &link : moved.link)
                            if ((link & kLeafBit) == 0)
                                link += at;
                        out[at + i] = moved;
                    }
                };
                place(left_nodes, 1);
                place(right_nodes, static_cast<uint32_t>(1 + left_nodes.size()));
                return out;
            }
        };

        uint64_t fnv1a(uint64_t h, const void *data, size_t size)
        {
            const auto *bytes = static_cast<const uint8_t *>(data);
            for (size_t i = 0; i < size; i++)
                h = (h ^ bytes[i]) * 0x100000001b3ull;
            return h;
        }
    }

    Bvh build_bvh(const std::vector<TraceTriangle> &triangles, const BvhOptions &options)
    {
        Bvh bvh;
        const size_t count = triangles.size();
        if (count == 0)
            return bvh;
        if (count > kMaxBvhTriangles)
            throw std::length_error("too many triangles for a leaf link");
        Builder b { options, std::vector<Box>(count), std::vector<std::array<float, 3>>(count), std::vector<uint32_t>(count), 0 };
        // The first log2(tasks) + 2 levels hand their left child to a thread of its own: more subtrees than threads, so
        // uneven splits still keep every thread busy.
        if (options.tasks > 1)
            while ((2u << b.parallel_depth) <= options.tasks)
                b.parallel_depth++;
        b.parallel_depth += options.tasks > 1 ? 2 : 0;
        for (uint32_t i = 0; i < count; i++)
        {
            for (const Vec3f &c : triangles[i].corner)
                b.boxes[i].add(c.data());
            for (int k = 0; k < 3; k++)
                b.centroids[i][k] = (b.boxes[i].lo[k] + b.boxes[i].hi[k]) * 0.5f;
            b.order[i] = i;
        }
        const uint32_t n = static_cast<uint32_t>(count);
        const Split root = b.split(0, n);
        if (root.leaf)
        {
            // At most max_leaf triangles: one node with a single leaf child.
            bvh.nodes.resize(1);
            Builder::set_box(bvh.nodes[0], 0, b.bounds(0, n));
            Builder::set_box(bvh.nodes[0], 1, Box {});
            bvh.nodes[0].link[0] = Builder::leaf_link(0, n);
            bvh.nodes[0].link[1] = kEmptyLink;
        }
        else
            bvh.nodes = b.subtree(0, n, root, 0);

        bvh.triangles.resize(count);
        bvh.source = b.order;
        for (uint32_t i = 0; i < count; i++)
        {
            const TraceTriangle &t = triangles[b.order[i]];
            BvhTriangle &out = bvh.triangles[i];
            out = {};
            for (int k = 0; k < 3; k++)
            {
                out.v0[k] = t.corner[0][k];
                out.e1[k] = t.corner[1][k] - t.corner[0][k];
                out.e2[k] = t.corner[2][k] - t.corner[0][k];
            }
            out.info = t.info;
        }
        Box all;
        all.add(b.bounds(0, n));
        const float root_area = all.area();
        for (const BvhNode &node : bvh.nodes)
            for (int c = 0; c < 2; c++)
            {
                if (node.link[c] == kEmptyLink)
                    continue;
                const bool leaf = (node.link[c] & kLeafBit) != 0;
                bvh.leaves += leaf;
                Box child;
                child.add(c == 0 ? node.lo0 : node.lo1);
                child.add(c == 0 ? node.hi0 : node.hi1);
                if (root_area > 0)
                    bvh.sah += double(child.area()) / root_area * (leaf ? (node.link[c] & 15u) + 1 : 1);
            }
        bvh.hash = fnv1a(fnv1a(0xcbf29ce484222325ull, bvh.nodes.data(), bvh.nodes.size() * sizeof(BvhNode)), bvh.triangles.data(),
            bvh.triangles.size() * sizeof(BvhTriangle));
        return bvh;
    }
}
