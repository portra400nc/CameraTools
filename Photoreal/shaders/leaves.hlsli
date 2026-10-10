// The leaves pass's arithmetic, shared by shaders/leaves_ps.hlsl and core/plan.cpp, which compiles it as C++ so the
// native tests check the shader's own formulas. Keep it to what both languages read alike.

// The meters of foliage above a leaf at one atlas tap: from the caster nearest the sun, whose depth the atlas stores, down
// to the leaf at depth receiver, both larger toward the sun. 0 where nothing lies above the leaf.
float leaf_thickness(float stored, float receiver, float meters_per_depth) { return max(stored - receiver, 0) * meters_per_depth; }

// The share of sun light that comes through thickness meters of foliage: it falls by e every depth meters.
float leaf_transmission(float thickness, float depth) { return exp(-thickness / depth); }
