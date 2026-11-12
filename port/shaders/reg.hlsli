// reg.hlsli: the register declaration every ICO shader uses.
//
// ICO_REG(r, s) is register(r, space s): the register space is the RHI bind
// group and the register number the RHI slot (common.hlsli's binding
// scheme).  The Direct3D 11 backend compiles the same sources with FXC
// (shader model 5.0), which has no register spaces: under ICO_D3D11 the
// macro drops the space.  The register numbers are distinct across the
// groups (b0; b1, b2, b3, t0, t4; t1, t2, s1; u0), so a flat namespace has
// no collision.
#ifndef ICO_REG_HLSLI
#define ICO_REG_HLSLI

#ifdef ICO_D3D11
#define ICO_REG(r, s) register(r)
#else
#define ICO_REG_SPACE(s) space##s
#define ICO_REG(r, s) register(r, ICO_REG_SPACE(s))
#endif


// VR fork: the stereo mode (port/rhi/rhi_vr.h, ICO_VR=1, SPIR-V only).
// Every texture is a two-layer array, one layer an eye, and multiview draws
// both eyes in one draw: a shader reads its own eye's layer by the view
// index (g_viewIndex, SPIR-V's ViewIndex built-in: 0 the left eye, 1 the
// right).  Without ICO_VR the macros are the plain Texture2D forms, so the
// sources stay one for every backend.
//   ICO_T2D(T)               the texture type
//   ICO_UV(uv)               a sample's coordinate (float2)
//   ICO_LD(c, lv)            Load's coordinate of int2 c at level lv
//   ICO_LDXY(x, y, lv)       the same from two ints
//   ICO_LD3(p)               the same from an int3 (xy, level)
//   ICO_DIMS(t, w, h)        GetDimensions(w, h)
//   ICO_DIMS_LV(t, lv, w, h, n) GetDimensions(lv, w, h, n)
//   ICO_T2DMS(T)             the multisampled texture type (MSAA)
//   ICO_MSLD(p)              a multisampled Load's coordinate of int2 p
//   ICO_MSDIMS(t, w, h, n)   its GetDimensions(w, h, samples)
#ifdef ICO_VR
[[vk::ext_extension("SPV_KHR_multiview")]] [[vk::ext_capability(/* MultiView */ 4439)]]
[[vk::ext_builtin_input(/* ViewIndex */ 4440)]] static const uint g_viewIndex;
#define ICO_T2D(T) Texture2DArray<T>
#define ICO_UV(uv) float3((uv), (float)g_viewIndex)
#define ICO_LD(c, lv) int4((c), (int)g_viewIndex, (lv))
#define ICO_LDXY(x, y, lv) int4((x), (y), (int)g_viewIndex, (lv))
#define ICO_LD3(p) int4((p).xy, (int)g_viewIndex, (p).z)
#define ICO_DIMS(t, w, h) { uint ico_dims_l_; (t).GetDimensions((w), (h), ico_dims_l_); }
#define ICO_DIMS_LV(t, lv, w, h, n) { uint ico_dims_l_; (t).GetDimensions((lv), (w), (h), ico_dims_l_, (n)); }
#define ICO_T2DMS(T) Texture2DMSArray<T>
#define ICO_MSLD(p) int3((p), (int)g_viewIndex)
#define ICO_MSDIMS(t, w, h, n) { uint ico_dims_l_; (t).GetDimensions((w), (h), ico_dims_l_, (n)); }
#else
#define ICO_T2D(T) Texture2D<T>
#define ICO_UV(uv) (uv)
#define ICO_LD(c, lv) int3((c), (lv))
#define ICO_LDXY(x, y, lv) int3((x), (y), (lv))
#define ICO_LD3(p) (p)
#define ICO_DIMS(t, w, h) (t).GetDimensions((w), (h))
#define ICO_DIMS_LV(t, lv, w, h, n) (t).GetDimensions((lv), (w), (h), (n))
#define ICO_T2DMS(T) Texture2DMS<T>
#define ICO_MSLD(p) (p)
#define ICO_MSDIMS(t, w, h, n) (t).GetDimensions((w), (h), (n))
#endif

#endif
