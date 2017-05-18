// ======================================================================== //
// Copyright 2009-2017 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "bvh_intersector_stream.h"
#include "bvh_intersector_node.h"

#include "../geometry/intersector_iterators.h"
#include "../geometry/triangle_intersector.h"
#include "../geometry/trianglev_intersector.h"
#include "../geometry/trianglev_mb_intersector.h"
#include "../geometry/trianglei_intersector.h"
#include "../geometry/quadv_intersector.h"
#include "../geometry/quadi_intersector.h"
#include "../geometry/bezier1v_intersector.h"
#include "../geometry/bezier1i_intersector.h"
#include "../geometry/linei_intersector.h"
#include "../geometry/subdivpatch1eager_intersector.h"
#include "../geometry/subdivpatch1cached_intersector.h"
#include "../geometry/object_intersector.h"

#include "../common/scene.h"
#include <bitset>

// todo: parent ptr also for single stream, should improve culling.

#define MAX_RAYS 64

namespace embree
{
  namespace isa
  {
    /* enable traversal of either two small streams or one large stream */
#if !defined(__AVX512F__)
    static const size_t MAX_RAYS_PER_OCTANT = 8*sizeof(unsigned int);
#else
    static const size_t MAX_RAYS_PER_OCTANT = 8*sizeof(size_t);
#endif
    static_assert(MAX_RAYS_PER_OCTANT <= MAX_INTERNAL_STREAM_SIZE, "maximal internal stream size exceeded");

    // =====================================================================================================
    // =====================================================================================================
    // =====================================================================================================

    template<int K>
    __forceinline size_t AOStoSOA(RayK<K>* rayK, Ray** inputRays, const size_t numTotalRays)
    {
      const size_t numPackets = (numTotalRays+K-1)/K; //todo : OPTIMIZE
      for (size_t i = 0; i < numPackets; i++)
        new (&rayK[i]) RayK<K>(zero,zero,zero,neg_inf);

      Vec3fa min_dir = pos_inf;
      Vec3fa max_dir = neg_inf;

      for (size_t i = 0; i < numTotalRays; i++) {
        const Vec3fa& org = inputRays[i]->org;
        const Vec3fa& dir = inputRays[i]->dir;
        min_dir = min(min_dir, dir);
        max_dir = max(max_dir, dir);
        const float tnear = max(0.0f, inputRays[i]->tnear);
        const float tfar  = inputRays[i]->tfar;
        const size_t packetID = i / K;
        const size_t slotID   = i % K;
        rayK[packetID].dir.x[slotID]  = dir.x;
        rayK[packetID].dir.y[slotID]  = dir.y;
        rayK[packetID].dir.z[slotID]  = dir.z;
        rayK[packetID].org.x[slotID]  = org.x;
        rayK[packetID].org.y[slotID]  = org.y;
        rayK[packetID].org.z[slotID]  = org.z;
        rayK[packetID].tnear[slotID]  = tnear;
        rayK[packetID].tfar[slotID]   = tfar;
        rayK[packetID].mask[slotID]   = inputRays[i]->mask;
        rayK[packetID].instID[slotID] = inputRays[i]->instID;
      }
      const size_t sign_min_dir = movemask(vfloat4(min_dir) < 0.0f);
      const size_t sign_max_dir = movemask(vfloat4(max_dir) < 0.0f);
      return ((sign_min_dir^sign_max_dir) & 0x7);
    }

    template<int K, bool occlusion>
    __forceinline void SOAtoAOS(Ray** inputRays, RayK<K>* rayK, const size_t numTotalRays)
    {
      for (size_t i = 0; i < numTotalRays; i++)
      {
        const size_t packetID = i / K;
        const size_t slotID   = i % K;
        const RayK<K>& ray = rayK[packetID];
        if (likely((unsigned)ray.geomID[slotID] != RTC_INVALID_GEOMETRY_ID))
        {
          if (occlusion)
            inputRays[i]->geomID = ray.geomID[slotID];
          else
          {
            inputRays[i]->tfar   = ray.tfar[slotID];
            inputRays[i]->Ng.x   = ray.Ng.x[slotID];
            inputRays[i]->Ng.y   = ray.Ng.y[slotID];
            inputRays[i]->Ng.z   = ray.Ng.z[slotID];
            inputRays[i]->u      = ray.u[slotID];
            inputRays[i]->v      = ray.v[slotID];
            inputRays[i]->geomID = ray.geomID[slotID];
            inputRays[i]->primID = ray.primID[slotID];
            inputRays[i]->instID = ray.instID[slotID];
          }
        }
      }
    }

    // =====================================================================================================
    // =====================================================================================================
    // =====================================================================================================

    template<int N, int Nx, int K, int types, bool robust, typename PrimitiveIntersector>
    __forceinline void BVHNIntersectorStream<N, Nx, K, types, robust, PrimitiveIntersector>::intersectCoherentSOA(BVH* __restrict__ bvh, RayK<K>** inputRays, size_t numOctantRays, IntersectContext* context)
    {
      __aligned(64) StackItemMaskCoherent stack[stackSizeSingle];  //!< stack of nodes

      RayK<K>** __restrict__ inputPackets = (RayK<K>**)inputRays;
      assert(numOctantRays <= MAX_RAYS);

      __aligned(64) Packet packet[MAX_RAYS/K];
      __aligned(64) Frusta frusta;

      const size_t m_active = initPacketsAndFrusta(inputPackets, numOctantRays, packet, frusta);
      if (unlikely(m_active == 0)) return;

      stack[0].mask    = m_active;
      stack[0].parent  = 0;
      stack[0].child   = bvh->root;
      stack[0].childID = (unsigned int)-1;
      //stack[0].dist    = (unsigned int)-1;

      ///////////////////////////////////////////////////////////////////////////////////
      ///////////////////////////////////////////////////////////////////////////////////
      ///////////////////////////////////////////////////////////////////////////////////

      const NearFarPreCompute pc(frusta.min_rdir);

      StackItemMaskCoherent* stackPtr = stack + 1;

      while (1) pop:
      {
        if (unlikely(stackPtr == stack)) break;

        STAT3(normal.trav_stack_pop,1,1,1);
        stackPtr--;
        /*! pop next node */
        NodeRef cur = NodeRef(stackPtr->child);
        size_t m_trav_active = stackPtr->mask;
        assert(m_trav_active);

        /* non-root and leaf => full culling test for all rays */
        if (unlikely(stackPtr->parent != 0 && cur.isLeaf()))
        {
          NodeRef parent = NodeRef(stackPtr->parent);
          const AlignedNode* __restrict__ const node = parent.alignedNode();
          const size_t b = stackPtr->childID;
          char *ptr = (char*)&node->lower_x + b*sizeof(float);
          assert(cur == node->child(b));

          const vfloat<K> minX = vfloat<K>(*(const float*)((const char*)ptr + pc.nearX));
          const vfloat<K> minY = vfloat<K>(*(const float*)((const char*)ptr + pc.nearY));
          const vfloat<K> minZ = vfloat<K>(*(const float*)((const char*)ptr + pc.nearZ));
          const vfloat<K> maxX = vfloat<K>(*(const float*)((const char*)ptr + pc.farX));
          const vfloat<K> maxY = vfloat<K>(*(const float*)((const char*)ptr + pc.farY));
          const vfloat<K> maxZ = vfloat<K>(*(const float*)((const char*)ptr + pc.farZ));

          m_trav_active = intersectAlignedNodePacket(packet, minX, minY, minZ, maxX, maxY, maxZ, m_trav_active);
          if (m_trav_active == 0) goto pop;
        }

        while (1)
        {
          if (unlikely(cur.isLeaf())) break;
          const AlignedNode* __restrict__ const node = cur.alignedNode();

          __aligned(64) size_t maskK[N];
          for (size_t i = 0; i < N; i++) maskK[i] = m_trav_active;
          vfloat<Nx> dist;
          const size_t m_node_hit = traverseCoherentStream(m_trav_active, packet, node, pc, frusta, maskK, dist);
          if (unlikely(m_node_hit == 0)) goto pop;

          BVHNNodeTraverserStreamHitCoherent<N, Nx, types>::traverseClosestHit(cur, m_trav_active, vbool<Nx>((int)m_node_hit), dist, (size_t*)maskK, stackPtr);
          assert(m_trav_active);
        }

        /*! this is a leaf node */
        assert(cur != BVH::emptyNode);
        STAT3(normal.trav_leaves, 1, 1, 1);
        size_t num; Primitive* prim = (Primitive*)cur.leaf(num);

        size_t bits = m_trav_active;

        /*! intersect stream of rays with all primitives */
        size_t lazy_node = 0;
#if defined(__SSE4_2__)
        STAT_USER(1,(__popcnt(bits)+K-1)/K*4);
#endif
        do
        {
          size_t i = __bsf(bits) / K;
          const size_t m_isec = ((((size_t)1 << K)-1) << (i*K));
          assert(m_isec & bits);
          bits &= ~m_isec;

          vbool<K> m_valid = (inputPackets[i]->tnear <= inputPackets[i]->tfar);
          PrimitiveIntersector::intersectK(m_valid, *inputPackets[i], context, prim, num, lazy_node);
          Packet &p = packet[i];
          p.max_dist = min(p.max_dist, inputPackets[i]->tfar);
        } while(bits);

      } // traversal + intersection
    }

    template<int N, int Nx, int K, int types, bool robust, typename PrimitiveIntersector>
    __forceinline void BVHNIntersectorStream<N, Nx, K, types, robust, PrimitiveIntersector>::occludedCoherentSOA(BVH* __restrict__ bvh, RayK<K>** inputRays, size_t numOctantRays, IntersectContext* context)
    {
      __aligned(64) StackItemMaskCoherent stack[stackSizeSingle];  //!< stack of nodes

      RayK<K>** __restrict__ inputPackets = (RayK<K>**)inputRays;
      assert(numOctantRays <= MAX_RAYS);

      /* inactive rays should have been filtered out before */
      __aligned(64) Packet packet[MAX_RAYS/K];
      __aligned(64) Frusta frusta;

      size_t m_active = initPacketsAndFrusta(inputPackets, numOctantRays, packet, frusta);

      /* valid rays */
      if (unlikely(m_active == 0)) return;

      stack[0].mask    = m_active;
      stack[0].parent  = 0;
      stack[0].child   = bvh->root;
      stack[0].childID = (unsigned int)-1;
      //stack[0].dist    = (unsigned int)-1;

      ///////////////////////////////////////////////////////////////////////////////////
      ///////////////////////////////////////////////////////////////////////////////////
      ///////////////////////////////////////////////////////////////////////////////////

      const NearFarPreCompute pc(frusta.min_rdir);

      StackItemMaskCoherent* stackPtr = stack + 1;

      while (1) pop:
      {
        if (unlikely(stackPtr == stack)) break;

        STAT3(normal.trav_stack_pop,1,1,1);
        stackPtr--;
        /*! pop next node */
        NodeRef cur = NodeRef(stackPtr->child);
        size_t m_trav_active = stackPtr->mask & m_active;
        if (unlikely(!m_trav_active)) continue;

        assert(m_trav_active);

        /* non-root and leaf => full culling test for all rays */
        if (unlikely(stackPtr->parent != 0 && cur.isLeaf()))
        {
          NodeRef parent = NodeRef(stackPtr->parent);
          const AlignedNode* __restrict__ const node = parent.alignedNode();
          const size_t b   = stackPtr->childID;
          char *ptr = (char*)&node->lower_x + b*sizeof(float);
          assert(cur == node->child(b));

          const vfloat<K> minX = vfloat<K>(*(const float*)((const char*)ptr + pc.nearX));
          const vfloat<K> minY = vfloat<K>(*(const float*)((const char*)ptr + pc.nearY));
          const vfloat<K> minZ = vfloat<K>(*(const float*)((const char*)ptr + pc.nearZ));
          const vfloat<K> maxX = vfloat<K>(*(const float*)((const char*)ptr + pc.farX));
          const vfloat<K> maxY = vfloat<K>(*(const float*)((const char*)ptr + pc.farY));
          const vfloat<K> maxZ = vfloat<K>(*(const float*)((const char*)ptr + pc.farZ));

          m_trav_active = intersectAlignedNodePacket(packet, minX, minY, minZ, maxX, maxY, maxZ, m_trav_active);

          if (m_trav_active == 0) goto pop;
        }

        while (1)
        {
          if (unlikely(cur.isLeaf())) break;
          const AlignedNode* __restrict__ const node = cur.alignedNode();

          __aligned(64) size_t maskK[N];
          for (size_t i = 0; i < N; i++) maskK[i] = m_trav_active;

          vfloat<Nx> dist;
          const size_t m_node_hit = traverseCoherentStream(m_trav_active, packet, node, pc, frusta, maskK, dist);
          if (unlikely(m_node_hit == 0)) goto pop;

          BVHNNodeTraverserStreamHitCoherent<N, Nx, types>::traverseAnyHit(cur, m_trav_active, vbool<Nx>((int)m_node_hit), (size_t*)maskK, stackPtr);
          assert(m_trav_active);
        }

        /*! this is a leaf node */
        assert(cur != BVH::emptyNode);
        STAT3(normal.trav_leaves, 1, 1, 1);
        size_t num; Primitive* prim = (Primitive*)cur.leaf(num);

        size_t bits = m_trav_active & m_active;
        /*! intersect stream of rays with all primitives */
        size_t lazy_node = 0;
#if defined(__SSE4_2__)
        STAT_USER(1,(__popcnt(bits)+K-1)/K*4);
#endif
        while(bits)
        {
          size_t i = __bsf(bits) / K;
          const size_t m_isec = ((((size_t)1 << K)-1) << (i*K));
          assert(m_isec & bits);
          bits &= ~m_isec;

          vbool<K> m_valid = (inputPackets[i]->tnear <= inputPackets[i]->tfar);
          vbool<K> m_hit = PrimitiveIntersector::occludedK(m_valid, *inputPackets[i], context, prim, num, lazy_node);
          inputPackets[i]->geomID = select(m_hit, vint<K>(zero), inputPackets[i]->geomID);
          m_active &= ~((size_t)movemask(m_hit) << (i*K));
        }

      } // traversal + intersection
    }

    // =====================================================================================================
    // =====================================================================================================
    // =====================================================================================================

    template<int N, int Nx, int K, int types, bool robust, typename PrimitiveIntersector>
    void BVHNIntersectorStream<N, Nx, K, types, robust, PrimitiveIntersector>::intersectCoherent(BVH* __restrict__ bvh, Ray** inputRays, size_t numTotalRays, IntersectContext* context)
    {
      if (likely(context->flags == IntersectContext::INPUT_RAY_DATA_AOS))
      {
        /* AOS to SOA conversion */
        RayK<K> rayK[MAX_RAYS / K];
        RayK<K>* rayK_ptr[MAX_RAYS / K];
        for (size_t i = 0; i < MAX_RAYS / K; i++) rayK_ptr[i] = &rayK[i];
        AOStoSOA(rayK, inputRays, numTotalRays);
        /* stream tracer as fast path */
        BVHNIntersectorStream<N, Nx, K, types, robust, PrimitiveIntersector>::intersectCoherentSOA(bvh, (RayK<K>**)rayK_ptr, numTotalRays, context);
        /* SOA to AOS conversion */
        SOAtoAOS<K, false>(inputRays, rayK, numTotalRays);
      }
      else
      {
        assert(context->getInputSOAWidth() == K);
        /* stream tracer as fast path */
        BVHNIntersectorStream<N, Nx, K, types, robust, PrimitiveIntersector>::intersectCoherentSOA(bvh, (RayK<K>**)inputRays, numTotalRays, context);
      }
    }

    template<int N, int Nx, int K, int types, bool robust, typename PrimitiveIntersector>
    void BVHNIntersectorStream<N, Nx, K, types, robust, PrimitiveIntersector>::occludedCoherent(BVH* __restrict__ bvh, Ray **inputRays, size_t numTotalRays, IntersectContext* context)
    {
      if (likely(context->flags == IntersectContext::INPUT_RAY_DATA_AOS))
      {
        /* AOS to SOA conversion */
        RayK<K> rayK[MAX_RAYS / K];
        RayK<K>* rayK_ptr[MAX_RAYS / K];
        for (size_t i = 0; i < MAX_RAYS / K; i++) rayK_ptr[i] = &rayK[i];
        AOStoSOA(rayK, inputRays, numTotalRays);
        /* stream tracer as fast path */
        BVHNIntersectorStream<N, Nx, K, types, robust, PrimitiveIntersector>::occludedCoherentSOA(bvh, (RayK<K>**)rayK_ptr, numTotalRays, context);
        /* SOA to AOS conversion */
        SOAtoAOS<K, true>(inputRays, rayK, numTotalRays);
      }
      else
      {
        assert(context->getInputSOAWidth() == K);
        BVHNIntersectorStream<N, Nx, K, types, robust, PrimitiveIntersector>::occludedCoherentSOA(bvh, (RayK<K>**)inputRays, numTotalRays, context);
      }
    }

    // =====================================================================================================
    // =====================================================================================================
    // =====================================================================================================

    template<int N, int Nx, int K, int types, bool robust, typename PrimitiveIntersector>
    void BVHNIntersectorStream<N, Nx, K, types, robust, PrimitiveIntersector>::intersect(BVH* __restrict__ bvh, Ray** inputRays, size_t numTotalRays, IntersectContext* context)
    {
#if ENABLE_COHERENT_STREAM_PATH == 1
      if (unlikely(PrimitiveIntersector::validIntersectorK && !robust && isCoherent(context->user->flags)))
      {
        intersectCoherent(bvh, inputRays, numTotalRays, context);
        return;
      }
#endif
      assert(context->flags == IntersectContext::INPUT_RAY_DATA_AOS);

      __aligned(64) RayCtx ray_ctx[MAX_RAYS_PER_OCTANT];
      __aligned(64) Precalculations pre[MAX_RAYS_PER_OCTANT];
      __aligned(64) StackItemMask stack[stackSizeSingle];  //!< stack of nodes

      for (size_t r = 0; r < numTotalRays; r += MAX_RAYS_PER_OCTANT)
      {
        Ray** __restrict__ rays = inputRays + r;
        const size_t numOctantRays = (r + MAX_RAYS_PER_OCTANT >= numTotalRays) ? numTotalRays-r : MAX_RAYS_PER_OCTANT;

        /* inactive rays should have been filtered out before */
        size_t m_active = numOctantRays == 8*sizeof(size_t) ? (size_t)-1 : (((size_t)1 << numOctantRays))-1;

        if (m_active == 0) return;

        /* do per ray precalculations */
        for (size_t i = 0; i < numOctantRays; i++) {
          new (&ray_ctx[i]) RayCtx(rays[i]);
          new (&pre[i]) Precalculations(*rays[i], bvh);
        }

        const NearFarPreCompute pc(ray_ctx[0].rdir);

        /*
        size_t bits = m_active;
        for (; bits!=0; ) {
          const size_t i = __bscf(bits);
          intersect1(bvh, bvh->root, pre[i], *rays[i], ray_ctx[i], pc, context);
        }
        continue;
        */

        stack[0].ptr  = BVH::invalidNode;
        stack[0].mask = (size_t)-1;
        stack[0].dist = neg_inf;
        stack[1].ptr  = bvh->root;
        stack[1].mask = m_active;
        stack[1].dist = neg_inf;

        ///////////////////////////////////////////////////////////////////////////////////
        ///////////////////////////////////////////////////////////////////////////////////
        ///////////////////////////////////////////////////////////////////////////////////

        StackItemMask* stackPtr = stack + 2;

        while (1) pop:
        {
          /*! pop next node */
          STAT3(normal.trav_stack_pop,1,1,1);
          stackPtr--;

          NodeRef cur = NodeRef(stackPtr->ptr);
          if (unlikely(cur == BVH::invalidNode)) {
            break;
          }

          size_t m_trav_active = stackPtr->mask;

#if 1
          /* culling */
          size_t active_bits = m_trav_active;
          do
          {
            const size_t i = __bscf(active_bits);
            if (likely(*(float*)&stackPtr->dist <= ray_ctx[i].tfar()))
              goto trav;
            m_trav_active = active_bits;
          } while (active_bits);
          continue;
        trav:
#endif

#if 0
#if defined(__SSE4_2__)
          /* switch to single */
          /*
          if (__popcnt(m_trav_active) <= 2)
          {
            size_t bits = m_trav_active;
            for (; bits!=0; ) {
              const size_t i = __bscf(bits);
              intersect1(bvh, cur, pre[i], *rays[i], ray_ctx[i], pc, context);
              ray_ctx[i].update(rays[i]);
            }
            continue;
          }
          */

          if (__popcnt(m_trav_active) == 1)
          {
            const size_t i = __bsf(m_trav_active);
            intersect1(bvh, cur, pre[i], *rays[i], ray_ctx[i], pc, context);
            ray_ctx[i].update(rays[i]);
            continue;
          }
#endif
#endif

          assert(m_trav_active);

          const vfloat<Nx> inf(pos_inf);

          while (1)
          {
            if (unlikely(cur.isLeaf())) break;
            const AlignedNode* __restrict__ const node = cur.alignedNode();
            assert(m_trav_active);

#if defined(__AVX512F__)
            /* AVX512 path for up to 64 rays */
            vllong<Nxd> maskK(zero);
            vfloat<Nx> dist(inf);
            const vbool<Nx> vmask = traversalLoop<true>(m_trav_active,node,pc,ray_ctx,dist,maskK);
            if (unlikely(none(vmask))) goto pop;
            BVHNNodeTraverserStreamHit<N, Nx, types>::traverseClosestHit(cur, m_trav_active, vmask, dist, (size_t*)&maskK, stackPtr);
#else
            /* AVX path for up to 32 rays */
            vint<Nx> maskK(zero);
            vfloat<Nx> dist(inf);
            const vbool<Nx> vmask = traversalLoop<true>(m_trav_active,node,pc,ray_ctx,dist,maskK);
            if (unlikely(none(vmask))) goto pop;
            BVHNNodeTraverserStreamHit<N, Nx, types>::traverseClosestHit(cur, m_trav_active, vmask, dist, (unsigned int*)&maskK, stackPtr);
            assert(m_trav_active);
#endif
          }

          /* current ray stream is done? */
          if (unlikely(cur == BVH::invalidNode))
            break;

          /*! this is a leaf node */
          assert(cur != BVH::emptyNode);
          STAT3(normal.trav_leaves, 1, 1, 1);
          size_t num; Primitive* prim = (Primitive*)cur.leaf(num);

          size_t bits = m_trav_active;

          /*! intersect stream of rays with all primitives */
          size_t lazy_node = 0;
          size_t valid_isec MAYBE_UNUSED = PrimitiveIntersector::intersect(pre, bits, rays, context, prim, num, lazy_node);

          /* update tfar in ray context on successful hit */
          size_t isec_bits = valid_isec;
          while(isec_bits)
          {
            const size_t i = __bscf(isec_bits);
            ray_ctx[i].update(rays[i]);
          }
        } // traversal + intersection
      }
    }

    template<int N, int Nx, int K, int types, bool robust, typename PrimitiveIntersector>
    void BVHNIntersectorStream<N, Nx, K, types, robust, PrimitiveIntersector>::intersect1(const BVH* bvh,
                                                                                          NodeRef root,
                                                                                          Precalculations& pre,
                                                                                          Ray& ray,
                                                                                          RayCtx& ray_ctx,
                                                                                          const NearFarPreCompute& pc,
                                                                                          IntersectContext* context)
    {
      /*! stack state */
      StackItemT<NodeRef> stack[stackSizeSingle];  //!< stack of nodes
      StackItemT<NodeRef>* stackPtr = stack + 1;        //!< current stack pointer
      StackItemT<NodeRef>* stackEnd = stack + stackSizeSingle;
      stack[0].ptr = root;
      stack[0].dist = neg_inf;

      /*! load the ray into SIMD registers */
      size_t leafType = 0;

      /*
      TravRay<N,Nx> vray(ray.org,ray.dir);
      vfloat<Nx> ray_near = max(ray.tnear,0.0f);
      vfloat<Nx> ray_far  = max(ray.tfar ,0.0f);
      */

      TravRay<N,Nx> vray;
      vray.org_xyz = ray.org;
      vray.dir_xyz = ray.dir;
      vray.org = Vec3vf<N>(ray.org.x,ray.org.y,ray.org.z);
      vray.dir = Vec3vf<N>(ray.dir.x,ray.dir.y,ray.dir.z);
      vray.rdir = Vec3vf<N>(ray_ctx.rdir.x,ray_ctx.rdir.y,ray_ctx.rdir.z);
#if defined(__AVX2__)
      const Vec3fa ray_org_rdir = ray_ctx.org_rdir;
      vray.org_rdir = Vec3vf<N>(ray_org_rdir.x,ray_org_rdir.y,ray_org_rdir.z);
#endif
      vray.nearX = pc.nearX;
      vray.nearY = pc.nearY;
      vray.nearZ = pc.nearZ;
      vray.farX  = pc.farX;
      vray.farY  = pc.farY;
      vray.farZ  = pc.farZ;

      vfloat<Nx> ray_near = max(ray.tnear,0.0f);
      vfloat<Nx> ray_far  = max(ray_ctx.tfar(), 0.0f);

      /* pop loop */
      while (true) pop:
      {
        /*! pop next node */
        if (unlikely(stackPtr == stack)) break;
        stackPtr--;
        NodeRef cur = NodeRef(stackPtr->ptr);

        /*! if popped node is too far, pop next one */
#if defined(__AVX512ER__)
        /* much faster on KNL */
        if (unlikely(any(vfloat<Nx>(*(float*)&stackPtr->dist) > ray_far)))
          continue;
#else
        if (unlikely(*(float*)&stackPtr->dist > ray.tfar))
          continue;
#endif

        /* downtraversal loop */
        while (true)
        {
          /*! stop if we found a leaf node */
          if (unlikely(cur.isLeaf())) break;
          STAT3(normal.trav_nodes,1,1,1);

          /* intersect node */
          size_t mask = 0;
          vfloat<Nx> tNear;
          BVHNNodeIntersector1<N,Nx,types,robust>::intersect(cur,vray,ray_near,ray_far,ray.time,tNear,mask);

          /*! if no child is hit, pop next node */
          if (unlikely(mask == 0))
            goto pop;

          /* select next child and push other children */
          BVHNNodeTraverser1<N,Nx,types>::traverseClosestHit(cur,mask,tNear,stackPtr,stackEnd);
        }

        /*! this is a leaf node */
        assert(cur != BVH::emptyNode);
        STAT3(normal.trav_leaves, 1, 1, 1);
        size_t num; Primitive* prim = (Primitive*)cur.leaf(num);

        size_t lazy_node = 0;
        PrimitiveIntersector::intersect(pre,ray,context,leafType,prim,num,lazy_node);

        ray_far = ray.tfar;

        if (unlikely(lazy_node)) {
          stackPtr->ptr = lazy_node;
          stackPtr->dist = neg_inf;
          stackPtr++;
        }
      }
    }


    template<int N, int Nx, int K, int types, bool robust, typename PrimitiveIntersector>
    void BVHNIntersectorStream<N, Nx, K, types, robust, PrimitiveIntersector>::occluded(BVH* __restrict__ bvh, Ray **inputRays, size_t numTotalRays, IntersectContext* context)
    {
#if ENABLE_COHERENT_STREAM_PATH == 1
      if (unlikely(PrimitiveIntersector::validIntersectorK && !robust && isCoherent(context->user->flags)))
      {
        occludedCoherent(bvh, inputRays, numTotalRays, context);
        return;
      }
#endif
      assert(context->flags == IntersectContext::INPUT_RAY_DATA_AOS);

      __aligned(64) RayCtx ray_ctx[MAX_RAYS_PER_OCTANT];
      __aligned(64) Precalculations pre[MAX_RAYS_PER_OCTANT];
      __aligned(64) StackItemMask stack[stackSizeSingle];  //!< stack of nodes

      for (size_t r = 0; r < numTotalRays; r += MAX_RAYS_PER_OCTANT)
      {
        Ray** rays = inputRays + r;
        const size_t numOctantRays = (r + MAX_RAYS_PER_OCTANT >= numTotalRays) ? numTotalRays-r : MAX_RAYS_PER_OCTANT;
        size_t m_active = numOctantRays == 8*sizeof(size_t) ? (size_t)-1 : (((size_t)1 << numOctantRays))-1;
        if (unlikely(m_active == 0)) continue;

        /* do per ray precalculations */
        for (size_t i = 0; i < numOctantRays; i++) {
          new (&ray_ctx[i]) RayCtx(rays[i]);
          new (&pre[i]) Precalculations(*rays[i], bvh);
        }

        stack[0].ptr  = BVH::invalidNode;
        stack[0].mask = (size_t)-1;
        stack[1].ptr  = bvh->root;
        stack[1].mask = m_active;

        StackItemMask* stackPtr = stack + 2;

        const NearFarPreCompute pc(ray_ctx[0].rdir);

        while (1) pop:
        {
          /*! pop next node */
          STAT3(shadow.trav_stack_pop,1,1,1);
          stackPtr--;
          NodeRef cur = NodeRef(stackPtr->ptr);
          assert(stackPtr->mask);
          size_t m_trav_active = stackPtr->mask & m_active;
          if (unlikely(m_trav_active == 0 && cur != BVH::invalidNode)) continue;

          const vfloat<Nx> inf(pos_inf);

          while (1)
          {
            if (likely(cur.isLeaf())) break;
            assert(m_trav_active);

            const AlignedNode* __restrict__ const node = cur.alignedNode();

#if defined(__AVX512F__)
            /* AVX512 path for up to 64 rays */
            vllong<Nxd> maskK(zero);
            vfloat<Nx> dist(inf);
            const vbool<Nx> vmask = traversalLoop<false>(m_trav_active,node,pc,ray_ctx,dist,maskK);
            if (unlikely(none(vmask))) goto pop;
            BVHNNodeTraverserStreamHit<N, Nx, types>::traverseAnyHit(cur, m_trav_active, vmask, (size_t*)&maskK, stackPtr);
#else
            /* AVX path for up to 32 rays */
            vint<Nx> maskK(zero);
            vfloat<Nx> dist(inf);
            const vbool<Nx> vmask = traversalLoop<false>(m_trav_active,node,pc,ray_ctx,dist,maskK);
            if (unlikely(none(vmask))) goto pop;
            BVHNNodeTraverserStreamHit<N, Nx, types>::traverseAnyHit(cur, m_trav_active, vmask, (unsigned int*)&maskK, stackPtr);
#endif
          }

          /* current ray stream is done? */
          if (unlikely(cur == BVH::invalidNode))
            break;

          /*! this is a leaf node */
          assert(cur != BVH::emptyNode);
          STAT3(shadow.trav_leaves, 1, 1, 1);
          size_t num; Primitive* prim = (Primitive*)cur.leaf(num);

          size_t lazy_node = 0;
          size_t bits = m_trav_active & m_active;

          assert(bits);
          m_active = m_active & ~PrimitiveIntersector::occluded(pre, bits, rays, context, prim, num, lazy_node);
          if (unlikely(m_active == 0)) break;
        } // traversal + intersection
      }
    }

    ////////////////////////////////////////////////////////////////////////////////
    /// ArrayIntersectorKStream Definitions
    ////////////////////////////////////////////////////////////////////////////////

    typedef ArrayIntersectorKStream<VSIZEX,
                                    TriangleMIntersector1Moeller<SIMD_MODE(4) COMMA true >,
                                    TriangleMIntersectorKMoeller<4 COMMA VSIZEX COMMA VSIZEX COMMA true > > Triangle4IntersectorStreamMoeller;

    typedef ArrayIntersectorKStream<VSIZEX,
                                    TriangleMIntersector1Moeller<SIMD_MODE(4) COMMA false >,
                                    TriangleMIntersectorKMoeller<4 COMMA VSIZEX COMMA VSIZEX COMMA false > > Triangle4IntersectorStreamMoellerNoFilter;

    typedef ArrayIntersectorKStream<VSIZEX,
                                    TriangleMvIntersector1Pluecker<SIMD_MODE(4) COMMA true >,
                                    TriangleMvIntersectorKPluecker<4 COMMA VSIZEX COMMA VSIZEX COMMA true > > Triangle4vIntersectorStreamPluecker;

    typedef ArrayIntersectorKStream<VSIZEX,
                                    TriangleMiIntersector1Moeller<SIMD_MODE(4) COMMA true >,
                                    TriangleMiIntersectorKMoeller<4 COMMA VSIZEX COMMA VSIZEX COMMA true > > Triangle4iIntersectorStreamMoeller;

    typedef ArrayIntersectorKStream<VSIZEX,
                                    TriangleMiIntersector1Pluecker<SIMD_MODE(4) COMMA true >,
                                    TriangleMiIntersectorKPluecker<4 COMMA VSIZEX COMMA VSIZEX COMMA true > > Triangle4iIntersectorStreamPluecker;

    typedef ArrayIntersectorKStream<VSIZEX,
                                    QuadMvIntersector1Moeller<4 COMMA true >,
                                    QuadMvIntersectorKMoeller<4 COMMA VSIZEX COMMA true > > Quad4vIntersectorStreamMoeller;

    typedef ArrayIntersectorKStream<VSIZEX,
                                    QuadMvIntersector1Moeller<4 COMMA false >,
                                    QuadMvIntersectorKMoeller<4 COMMA VSIZEX COMMA false > > Quad4vIntersectorStreamMoellerNoFilter;

    typedef ArrayIntersectorKStream<VSIZEX,
                                    QuadMiIntersector1Moeller<4 COMMA true >,
                                    QuadMiIntersectorKMoeller<4 COMMA VSIZEX COMMA true > > Quad4iIntersectorStreamMoeller;

    typedef ArrayIntersectorKStream<VSIZEX,
                                    QuadMvIntersector1Pluecker<4 COMMA true >,
                                    QuadMvIntersectorKPluecker<4 COMMA VSIZEX COMMA true > > Quad4vIntersectorStreamPluecker;

    typedef ArrayIntersectorKStream<VSIZEX,
                                    QuadMiIntersector1Pluecker<4 COMMA true >,
                                    QuadMiIntersectorKPluecker<4 COMMA VSIZEX COMMA true > > Quad4iIntersectorStreamPluecker;

    typedef ArrayIntersectorKStream<VSIZEX,
                                    ObjectIntersector1<false>,
                                    ObjectIntersectorK<VSIZEX COMMA false > > ObjectIntersectorStream;
  }
}
