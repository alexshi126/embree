// ======================================================================== //
// Copyright 2009-2014 Intel Corporation                                    //
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

#include "bvh4_intersector4_hybrid.h"
#include "bvh4_intersector4_single.h"

#include "geometry/triangle4_intersector4_moeller.h"
#if defined (__AVX__)
#include "geometry/triangle8_intersector4_moeller.h"
#endif
#include "geometry/triangle4v_intersector4_pluecker.h"

#define SWITCH_THRESHOLD 3
#define SWITCH_DURING_DOWN_TRAVERSAL 1

namespace embree
{
  namespace isa
  {
    /* ray/box intersection */
    __forceinline sseb intersectBox(const Ray4& ray, const ssef& ray_tfar, const sse3f& rdir, const BVH4::NodeMB* node, const int i, ssef& dist) 
    {
      const ssef lower_x = ssef(node->lower_x[i]) + ray.time * ssef(node->lower_dx[i]);
      const ssef lower_y = ssef(node->lower_y[i]) + ray.time * ssef(node->lower_dy[i]);
      const ssef lower_z = ssef(node->lower_z[i]) + ray.time * ssef(node->lower_dz[i]);
      const ssef upper_x = ssef(node->upper_x[i]) + ray.time * ssef(node->upper_dx[i]);
      const ssef upper_y = ssef(node->upper_y[i]) + ray.time * ssef(node->upper_dy[i]);
      const ssef upper_z = ssef(node->upper_z[i]) + ray.time * ssef(node->upper_dz[i]);
      
      const ssef dminx = (lower_x - ray.org.x) * rdir.x;
      const ssef dminy = (lower_y - ray.org.y) * rdir.y;
      const ssef dminz = (lower_z - ray.org.z) * rdir.z;
      const ssef dmaxx = (upper_x - ray.org.x) * rdir.x;
      const ssef dmaxy = (upper_y - ray.org.y) * rdir.y;
      const ssef dmaxz = (upper_z - ray.org.z) * rdir.z;
      
      const ssef dlowerx = min(dminx,dmaxx);
      const ssef dlowery = min(dminy,dmaxy);
      const ssef dlowerz = min(dminz,dmaxz);
      
      const ssef dupperx = max(dminx,dmaxx);
      const ssef duppery = max(dminy,dmaxy);
      const ssef dupperz = max(dminz,dmaxz);
      
      const ssef near = max(dlowerx,dlowery,dlowerz,ray.tnear);
      const ssef far  = min(dupperx,duppery,dupperz,ray_tfar );
      dist = near;
      
      return near <= far;
    }

    template<int types, typename PrimitiveIntersector4>
    void BVH4Intersector4Hybrid<types,PrimitiveIntersector4>::intersect(sseb* valid_i, BVH4* bvh, Ray4& ray)
    {
      /* load ray */
      const sseb valid0 = *valid_i;
      sse3f ray_org = ray.org;
      sse3f ray_dir = ray.dir;
      ssef ray_tnear = ray.tnear, ray_tfar  = ray.tfar;
      const sse3f rdir = rcp_safe(ray_dir);
      const sse3f org(ray_org), org_rdir = org * rdir;
      ray_tnear = select(valid0,ray_tnear,ssef(pos_inf));
      ray_tfar  = select(valid0,ray_tfar ,ssef(neg_inf));
      const ssef inf = ssef(pos_inf);
      Precalculations pre(valid0,ray);

      /* compute near/far per ray */
      sse3i nearXYZ;
      nearXYZ.x = select(rdir.x >= 0.0f,ssei(0*(int)sizeof(ssef)),ssei(1*(int)sizeof(ssef)));
      nearXYZ.y = select(rdir.y >= 0.0f,ssei(2*(int)sizeof(ssef)),ssei(3*(int)sizeof(ssef)));
      nearXYZ.z = select(rdir.z >= 0.0f,ssei(4*(int)sizeof(ssef)),ssei(5*(int)sizeof(ssef)));

      /* allocate stack and push root node */
      ssef    stack_near[stackSizeChunk];
      NodeRef stack_node[stackSizeChunk];
      stack_node[0] = BVH4::invalidNode;
      stack_near[0] = inf;
      stack_node[1] = bvh->root;
      stack_near[1] = ray_tnear; 
      NodeRef* stackEnd = stack_node+stackSizeChunk;
      NodeRef* __restrict__ sptr_node = stack_node + 2;
      ssef*    __restrict__ sptr_near = stack_near + 2;
      
      while (1) pop:
      {
        /* pop next node from stack */
        assert(sptr_node > stack_node);
        sptr_node--;
        sptr_near--;
        NodeRef curNode = *sptr_node;
        if (unlikely(curNode == BVH4::invalidNode)) {
          assert(sptr_node == stack_node);
          break;
        }
        
        /* cull node if behind closest hit point */
        ssef curDist = *sptr_near;
        const sseb active = curDist < ray_tfar;
        if (unlikely(none(active)))
          continue;
        
        /* switch to single ray traversal */
#if !defined(__WIN32__) || defined(__X86_64__)
        size_t bits = movemask(active);
        if (unlikely(__popcnt(bits) <= SWITCH_THRESHOLD)) {
          for (size_t i=__bsf(bits); bits!=0; bits=__btc(bits,i), i=__bsf(bits)) {
            BVH4Intersector4Single<types,PrimitiveIntersector4>::intersect1(bvh, curNode, i, pre, ray, ray_org, ray_dir, rdir, ray_tnear, ray_tfar, nearXYZ);
          }
          ray_tfar = min(ray_tfar,ray.tfar);
          continue;
        }
#endif

        while (1)
        {
	  /* process normal nodes */
          if (likely((types & 0x1) && curNode.isNode()))
          {
	    const sseb valid_node = ray_tfar > curDist;
	    STAT3(normal.trav_nodes,1,popcnt(valid_node),4);
	    const Node* __restrict__ const node = curNode.node();
	    
	    /* pop of next node */
	    assert(sptr_node > stack_node);
	    sptr_node--;
	    sptr_near--;
	    curNode = *sptr_node; 
	    curDist = *sptr_near;
	    
#pragma unroll(4)
	    for (unsigned i=0; i<BVH4::N; i++)
	    {
	      const NodeRef child = node->children[i];
	      if (unlikely(child == BVH4::emptyNode)) break;
	      
#if defined(__AVX2__)
	      const ssef lclipMinX = msub(node->lower_x[i],rdir.x,org_rdir.x);
	      const ssef lclipMinY = msub(node->lower_y[i],rdir.y,org_rdir.y);
	      const ssef lclipMinZ = msub(node->lower_z[i],rdir.z,org_rdir.z);
	      const ssef lclipMaxX = msub(node->upper_x[i],rdir.x,org_rdir.x);
	      const ssef lclipMaxY = msub(node->upper_y[i],rdir.y,org_rdir.y);
	      const ssef lclipMaxZ = msub(node->upper_z[i],rdir.z,org_rdir.z);
	      const ssef lnearP = maxi(maxi(mini(lclipMinX, lclipMaxX), mini(lclipMinY, lclipMaxY)), mini(lclipMinZ, lclipMaxZ));
	      const ssef lfarP  = mini(mini(maxi(lclipMinX, lclipMaxX), maxi(lclipMinY, lclipMaxY)), maxi(lclipMinZ, lclipMaxZ));
	      const sseb lhit   = maxi(lnearP,ray_tnear) <= mini(lfarP,ray_tfar);      
#else
	      const ssef lclipMinX = (node->lower_x[i] - org.x) * rdir.x;
	      const ssef lclipMinY = (node->lower_y[i] - org.y) * rdir.y;
	      const ssef lclipMinZ = (node->lower_z[i] - org.z) * rdir.z;
	      const ssef lclipMaxX = (node->upper_x[i] - org.x) * rdir.x;
	      const ssef lclipMaxY = (node->upper_y[i] - org.y) * rdir.y;
	      const ssef lclipMaxZ = (node->upper_z[i] - org.z) * rdir.z;
	      const ssef lnearP = max(max(min(lclipMinX, lclipMaxX), min(lclipMinY, lclipMaxY)), min(lclipMinZ, lclipMaxZ));
	      const ssef lfarP  = min(min(max(lclipMinX, lclipMaxX), max(lclipMinY, lclipMaxY)), max(lclipMinZ, lclipMaxZ));
	      const sseb lhit   = max(lnearP,ray_tnear) <= min(lfarP,ray_tfar);      
#endif
	      
	      /* if we hit the child we choose to continue with that child if it 
		 is closer than the current next child, or we push it onto the stack */
	      if (likely(any(lhit)))
	      {
		assert(sptr_node < stackEnd);
		assert(child != BVH4::emptyNode);
		const ssef childDist = select(lhit,lnearP,inf);
		sptr_node++;
		sptr_near++;
		
		/* push cur node onto stack and continue with hit child */
		if (any(childDist < curDist))
		{
		  *(sptr_node-1) = curNode;
		  *(sptr_near-1) = curDist; 
		  curDist = childDist;
		  curNode = child;
		}
		
		/* push hit child onto stack */
		else {
		  *(sptr_node-1) = child;
		  *(sptr_near-1) = childDist; 
		}
	      }     
	    }
#if SWITCH_DURING_DOWN_TRAVERSAL == 1
          // seems to be the best place for testing utilization
          if (unlikely(popcnt(ray_tfar > curDist) <= SWITCH_THRESHOLD))
            {
              *sptr_node++ = curNode;
              *sptr_near++ = curDist;
              goto pop;
            }
#endif
	  }
	  
	  /* process motion blur nodes */
          else if (likely((types & 0x10) && curNode.isNodeMB()))
	  {
	    const sseb valid_node = ray_tfar > curDist;
	    STAT3(normal.trav_nodes,1,popcnt(valid_node),4);
	    const BVH4::NodeMB* __restrict__ const node = curNode.nodeMB();
          
	    /* pop of next node */
	    assert(sptr_node > stack_node);
	    sptr_node--;
	    sptr_near--;
	    curNode = *sptr_node; 
	    curDist = *sptr_near;
	    
#pragma unroll(4)
	    for (unsigned i=0; i<BVH4::N; i++)
	    {
	      const NodeRef child = node->child(i);
	      if (unlikely(child == BVH4::emptyNode)) break;

	      ssef lnearP;
	      const sseb lhit = intersectBox(ray,ray_tfar,rdir,node,i,lnearP);
	      
	      /* if we hit the child we choose to continue with that child if it 
		 is closer than the current next child, or we push it onto the stack */
	      if (likely(any(lhit)))
	      {
		assert(sptr_node < stackEnd);
		assert(child != BVH4::emptyNode);
		const ssef childDist = select(lhit,lnearP,inf);
		sptr_node++;
		sptr_near++;
		
		/* push cur node onto stack and continue with hit child */
		if (any(childDist < curDist))
		{
		  *(sptr_node-1) = curNode;
		  *(sptr_near-1) = curDist; 
		  curDist = childDist;
		  curNode = child;
		}
		
		/* push hit child onto stack */
		else {
		  *(sptr_node-1) = child;
		  *(sptr_near-1) = childDist; 
		}
	      }	      
	    }
#if SWITCH_DURING_DOWN_TRAVERSAL == 1
          // seems to be the best place for testing utilization
          if (unlikely(popcnt(ray_tfar > curDist) <= SWITCH_THRESHOLD))
            {
              *sptr_node++ = curNode;
              *sptr_near++ = curDist;
              goto pop;
            }
#endif
	  }
	  else 
	    break;
	}
        
        /* return if stack is empty */
        if (unlikely(curNode == BVH4::invalidNode)) {
          assert(sptr_node == stack_node);
          break;
        }
        
        /* intersect leaf */
        const sseb valid_leaf = ray_tfar > curDist;

        STAT3(normal.trav_leaves,1,popcnt(valid_leaf),4);
        size_t items; const Primitive* prim = (Primitive*) curNode.leaf(items);
        PrimitiveIntersector4::intersect(valid_leaf,pre,ray,prim,items,bvh->geometry);
        ray_tfar = select(valid_leaf,ray.tfar,ray_tfar);
      }
      AVX_ZERO_UPPER();
    }

    
    template<int types, typename PrimitiveIntersector4>
    void BVH4Intersector4Hybrid<types,PrimitiveIntersector4>::occluded(sseb* valid_i, BVH4* bvh, Ray4& ray)
    {
      /* load ray */
      const sseb valid = *valid_i;
      sseb terminated = !valid;
      sse3f ray_org = ray.org, ray_dir = ray.dir;
      ssef ray_tnear = ray.tnear, ray_tfar  = ray.tfar;
      const sse3f rdir = rcp_safe(ray_dir);
      const sse3f org(ray_org), org_rdir = org * rdir;
      ray_tnear = select(valid,ray_tnear,ssef(pos_inf));
      ray_tfar  = select(valid,ray_tfar ,ssef(neg_inf));
      const ssef inf = ssef(pos_inf);
      Precalculations pre(valid,ray);

      /* compute near/far per ray */
      sse3i nearXYZ;
      nearXYZ.x = select(rdir.x >= 0.0f,ssei(0*(int)sizeof(ssef)),ssei(1*(int)sizeof(ssef)));
      nearXYZ.y = select(rdir.y >= 0.0f,ssei(2*(int)sizeof(ssef)),ssei(3*(int)sizeof(ssef)));
      nearXYZ.z = select(rdir.z >= 0.0f,ssei(4*(int)sizeof(ssef)),ssei(5*(int)sizeof(ssef)));

      /* allocate stack and push root node */
      ssef    stack_near[stackSizeChunk];
      NodeRef stack_node[stackSizeChunk];
      stack_node[0] = BVH4::invalidNode;
      stack_near[0] = inf;
      stack_node[1] = bvh->root;
      stack_near[1] = ray_tnear; 
      NodeRef* stackEnd = stack_node+stackSizeChunk;
      NodeRef* __restrict__ sptr_node = stack_node + 2;
      ssef*    __restrict__ sptr_near = stack_near + 2;
      
      while (1) pop:
      {
        /* pop next node from stack */
        assert(sptr_node > stack_node);
        sptr_node--;
        sptr_near--;
        NodeRef curNode = *sptr_node;
        if (unlikely(curNode == BVH4::invalidNode)) {
          assert(sptr_node == stack_node);
          break;
        }

        /* cull node if behind closest hit point */
        ssef curDist = *sptr_near;
        const sseb active = curDist < ray_tfar;
        if (unlikely(none(active))) 
          continue;
        
        /* switch to single ray traversal */
#if !defined(__WIN32__) || defined(__X86_64__)
        size_t bits = movemask(active);
        if (unlikely(__popcnt(bits) <= SWITCH_THRESHOLD)) {
          for (size_t i=__bsf(bits); bits!=0; bits=__btc(bits,i), i=__bsf(bits)) {
            if (BVH4Intersector4Single<types,PrimitiveIntersector4>::occluded1(bvh,curNode,i,pre,ray,ray_org,ray_dir,rdir,ray_tnear,ray_tfar,nearXYZ))
              terminated[i] = -1;
          }
          if (all(terminated)) break;
          ray_tfar = select(terminated,ssef(neg_inf),ray_tfar);
          continue;
        }
#endif
                
        while (1)
        {
	  /* process normal nodes */
          if (likely((types & 0x1) && curNode.isNode()))
          {
	    const sseb valid_node = ray_tfar > curDist;
	    STAT3(normal.trav_nodes,1,popcnt(valid_node),4);
	    const Node* __restrict__ const node = curNode.node();
	    
	    /* pop of next node */
	    assert(sptr_node > stack_node);
	    sptr_node--;
	    sptr_near--;
	    curNode = *sptr_node; 
	    curDist = *sptr_near;
	    
#pragma unroll(4)
	    for (unsigned i=0; i<BVH4::N; i++)
	    {
	      const NodeRef child = node->children[i];
	      if (unlikely(child == BVH4::emptyNode)) break;
	      
#if defined(__AVX2__)
	      const ssef lclipMinX = msub(node->lower_x[i],rdir.x,org_rdir.x);
	      const ssef lclipMinY = msub(node->lower_y[i],rdir.y,org_rdir.y);
	      const ssef lclipMinZ = msub(node->lower_z[i],rdir.z,org_rdir.z);
	      const ssef lclipMaxX = msub(node->upper_x[i],rdir.x,org_rdir.x);
	      const ssef lclipMaxY = msub(node->upper_y[i],rdir.y,org_rdir.y);
	      const ssef lclipMaxZ = msub(node->upper_z[i],rdir.z,org_rdir.z);
	      const ssef lnearP = maxi(maxi(mini(lclipMinX, lclipMaxX), mini(lclipMinY, lclipMaxY)), mini(lclipMinZ, lclipMaxZ));
	      const ssef lfarP  = mini(mini(maxi(lclipMinX, lclipMaxX), maxi(lclipMinY, lclipMaxY)), maxi(lclipMinZ, lclipMaxZ));
	      const sseb lhit   = maxi(lnearP,ray_tnear) <= mini(lfarP,ray_tfar);      
#else
	      const ssef lclipMinX = (node->lower_x[i] - org.x) * rdir.x;
	      const ssef lclipMinY = (node->lower_y[i] - org.y) * rdir.y;
	      const ssef lclipMinZ = (node->lower_z[i] - org.z) * rdir.z;
	      const ssef lclipMaxX = (node->upper_x[i] - org.x) * rdir.x;
	      const ssef lclipMaxY = (node->upper_y[i] - org.y) * rdir.y;
	      const ssef lclipMaxZ = (node->upper_z[i] - org.z) * rdir.z;
	      const ssef lnearP = max(max(min(lclipMinX, lclipMaxX), min(lclipMinY, lclipMaxY)), min(lclipMinZ, lclipMaxZ));
	      const ssef lfarP  = min(min(max(lclipMinX, lclipMaxX), max(lclipMinY, lclipMaxY)), max(lclipMinZ, lclipMaxZ));
	      const sseb lhit   = max(lnearP,ray_tnear) <= min(lfarP,ray_tfar);      
#endif
	      
	      /* if we hit the child we choose to continue with that child if it 
		 is closer than the current next child, or we push it onto the stack */
	      if (likely(any(lhit)))
	      {
		assert(sptr_node < stackEnd);
		assert(child != BVH4::emptyNode);
		const ssef childDist = select(lhit,lnearP,inf);
		sptr_node++;
		sptr_near++;
		
		/* push cur node onto stack and continue with hit child */
		if (any(childDist < curDist))
		{
		  *(sptr_node-1) = curNode;
		  *(sptr_near-1) = curDist; 
		  curDist = childDist;
		  curNode = child;
		}
		
		/* push hit child onto stack */
		else {
		  *(sptr_node-1) = child;
		  *(sptr_near-1) = childDist; 
		}
	      }     
	    }
#if SWITCH_DURING_DOWN_TRAVERSAL == 1
          // seems to be the best place for testing utilization
          if (unlikely(popcnt(ray_tfar > curDist) <= SWITCH_THRESHOLD))
            {
              *sptr_node++ = curNode;
              *sptr_near++ = curDist;
              goto pop;
            }
#endif
	  }
	  
	  /* process motion blur nodes */
          else if (likely((types & 0x10) && curNode.isNodeMB()))
	  {
	    const sseb valid_node = ray_tfar > curDist;
	    STAT3(normal.trav_nodes,1,popcnt(valid_node),4);
	    const BVH4::NodeMB* __restrict__ const node = curNode.nodeMB();
          
	    /* pop of next node */
	    assert(sptr_node > stack_node);
	    sptr_node--;
	    sptr_near--;
	    curNode = *sptr_node; 
	    curDist = *sptr_near;
	    
#pragma unroll(4)
	    for (unsigned i=0; i<BVH4::N; i++)
	    {
	      const NodeRef child = node->child(i);
	      if (unlikely(child == BVH4::emptyNode)) break;

	      ssef lnearP;
	      const sseb lhit = intersectBox(ray,ray_tfar,rdir,node,i,lnearP);
	      
	      /* if we hit the child we choose to continue with that child if it 
		 is closer than the current next child, or we push it onto the stack */
	      if (likely(any(lhit)))
	      {
		assert(sptr_node < stackEnd);
		assert(child != BVH4::emptyNode);
		const ssef childDist = select(lhit,lnearP,inf);
		sptr_node++;
		sptr_near++;
		
		/* push cur node onto stack and continue with hit child */
		if (any(childDist < curDist))
		{
		  *(sptr_node-1) = curNode;
		  *(sptr_near-1) = curDist; 
		  curDist = childDist;
		  curNode = child;
		}
		
		/* push hit child onto stack */
		else {
		  *(sptr_node-1) = child;
		  *(sptr_near-1) = childDist; 
		}
	      }	      
	    }
#if SWITCH_DURING_DOWN_TRAVERSAL == 1
          // seems to be the best place for testing utilization
          if (unlikely(popcnt(ray_tfar > curDist) <= SWITCH_THRESHOLD))
            {
              *sptr_node++ = curNode;
              *sptr_near++ = curDist;
              goto pop;
            }
#endif
	  }
	  else 
	    break;
	}
        
        /* return if stack is empty */
        if (unlikely(curNode == BVH4::invalidNode)) {
          assert(sptr_node == stack_node);
          break;
        }

        
        /* intersect leaf */
        const sseb valid_leaf = ray_tfar > curDist;

        STAT3(shadow.trav_leaves,1,popcnt(valid_leaf),4);
        size_t items; const Primitive* prim = (Primitive*) curNode.leaf(items);
        terminated |= PrimitiveIntersector4::occluded(!terminated,pre,ray,prim,items,bvh->geometry);
        if (all(terminated)) break;
        ray_tfar = select(terminated,ssef(neg_inf),ray_tfar);
      }
      store4i(valid & terminated,&ray.geomID,0);
      AVX_ZERO_UPPER();
    }

    DEFINE_INTERSECTOR4(BVH4Triangle4Intersector4HybridMoeller, BVH4Intersector4Hybrid<0x1 COMMA Triangle4Intersector4MoellerTrumbore<true> >);
    DEFINE_INTERSECTOR4(BVH4Triangle4Intersector4HybridMoellerNoFilter, BVH4Intersector4Hybrid<0x1 COMMA Triangle4Intersector4MoellerTrumbore<false> >);
#if defined (__AVX__)
    DEFINE_INTERSECTOR4(BVH4Triangle8Intersector4HybridMoeller, BVH4Intersector4Hybrid<0x1 COMMA Triangle8Intersector4MoellerTrumbore<true> >);
    DEFINE_INTERSECTOR4(BVH4Triangle8Intersector4HybridMoellerNoFilter, BVH4Intersector4Hybrid<0x1 COMMA Triangle8Intersector4MoellerTrumbore<false> >);
#endif
    DEFINE_INTERSECTOR4(BVH4Triangle4vIntersector4HybridPluecker, BVH4Intersector4Hybrid<0x1 COMMA Triangle4vIntersector4Pluecker>);
  }
}
