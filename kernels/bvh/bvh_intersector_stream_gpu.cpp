// ======================================================================== //
// Copyright 2009-2018 Intel Corporation                                    //
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

#include "node_intersector_packet_stream.h"
#include "node_intersector_frustum.h"
#include "bvh_traverser_stream.h"

#if defined(EMBREE_DPCPP_SUPPORT)
#include "../gpu/bvh.h"
#include "../gpu/ray.h"
#include "../gpu/geometry.h"

extern "C" uint intel_sub_group_ballot(bool valid);

#endif

#define STACK_ENTRIES 64

#if defined(ENABLE_RAY_STATS)
#define RAY_STATS(x) x
#else
#define RAY_STATS(x) 
#endif

#define STACK_CULLING 1



namespace embree
{
  namespace isa
  {

    /*! BVH ray stream GPU intersector */
    class BVHNGPUIntersectorStream
    {
      typedef BVHN<4> BVH;
      typedef typename BVH::NodeRef NodeRef;

    public:
      static void intersect(Accel::Intersectors* This, RayHitN** inputRays, size_t numRays, IntersectContext* context);
      static void occluded (Accel::Intersectors* This, RayN** inputRays, size_t numRays, IntersectContext* context);

    };

#if defined(EMBREE_DPCPP_SUPPORT)
    [[cl::intel_reqd_sub_group_size(BVH_NODE_N)]] inline void traceRayBVH16(const cl::sycl::intel::sub_group &sg, gpu::RTCRayHitGPU &rayhit, void *bvh_mem, const cl::sycl::stream &out)
    {
      unsigned int stack_offset[STACK_ENTRIES]; 
      float        stack_dist[STACK_ENTRIES];  
      
      const uint subgroupLocalID = sg.get_local_id()[0];
      const uint subgroupSize    = sg.get_local_range().size();

      const cl::sycl::float3 &org = rayhit.ray.org;
      const cl::sycl::float3 &dir = rayhit.ray.dir;
      const float tnear = rayhit.ray.tnear;
      float tfar        = rayhit.ray.tfar;
      
      const unsigned int maskX = cl::sycl::select(1,0,(uint)(dir.x() >= 0.0f));
      const unsigned int maskY = cl::sycl::select(1,0,(uint)(dir.y() >= 0.0f));
      const unsigned int maskZ = cl::sycl::select(1,0,(uint)(dir.z() >= 0.0f));

      const cl::sycl::float3 new_dir(cl::sycl::select(1E-18f,(float)dir.x(),(int)(dir.x() != 0.0f)),
				     cl::sycl::select(1E-18f,(float)dir.y(),(int)(dir.y() != 0.0f)),
				     cl::sycl::select(1E-18f,(float)dir.z(),(int)(dir.z() != 0.0f)));

      //const cl::sycl::float3 inv_dir(cl::sycl::recip(new_dir.x()),cl::sycl::recip(new_dir.y()),cl::sycl::recip(new_dir.z())); // FIXME
      
      const cl::sycl::float3 inv_dir( cl::sycl::native::recip((float)new_dir.x()),
				      cl::sycl::native::recip((float)new_dir.y()),
				      cl::sycl::native::recip((float)new_dir.z()));

      //const cl::sycl::float3 inv_dir_org = -inv_dir * org; // FIXME

      const cl::sycl::float3 inv_dir_org(-(float)inv_dir.x() * (float)org.x(),-(float)inv_dir.y() * (float)org.y(),-(float)inv_dir.z() * (float)org.z());
      
  
      const unsigned int max_uint  = 0xffffffff;  
      const unsigned int mask_uint = 0xfffffff0;

      const char *bvh_base = (char*)bvh_mem + sizeof(struct gpu::BVHBase);
      stack_offset[0] = max_uint; // sentinel
      stack_dist[0]   = -(float)INFINITY;

      stack_offset[1] = 0; //sizeof(struct gpu::BVHNodeN); // single node after bvh start //*(global uint*)(bvh); // root noderef stored at the beginning of the bvh
      stack_dist[1]   = -(float)INFINITY;

      if (0 == subgroupLocalID)
	{
	  out << "sizes " << sizeof(cl::sycl::float3) << " " << sizeof(gpu::AABB3f) << " " <<  sizeof(gpu::BVHBase) << cl::sycl::endl;
	  out << ((gpu::BVHNodeN *)bvh_base)[0] << cl::sycl::endl;
	}

      unsigned int sindex = 2; 

      while(1)
	{ 
	  sindex--;
       
	  unsigned int cur = stack_offset[sindex]; 

	  //while((cur & BVH_LEAF_MASK) == 0) 
	    {
	      const gpu::BVHNodeN &node = *(gpu::BVHNodeN*)(bvh_base + cur);
	      
	      const float _lower_x = node.lower_x[subgroupLocalID];
	      const float _lower_y = node.lower_y[subgroupLocalID];
	      const float _lower_z = node.lower_z[subgroupLocalID];
	      const float _upper_x = node.upper_x[subgroupLocalID];
	      const float _upper_y = node.upper_y[subgroupLocalID];
	      const float _upper_z = node.upper_z[subgroupLocalID];
	      const uint  offset   = node.offset[subgroupLocalID];

	      const float lower_x = cl::sycl::select(_lower_x,_upper_x,maskX);
	      const float upper_x = cl::sycl::select(_upper_x,_lower_x,maskX);
	      const float lower_y = cl::sycl::select(_lower_y,_upper_y,maskY);
	      const float upper_y = cl::sycl::select(_upper_y,_lower_y,maskY);
	      const float lower_z = cl::sycl::select(_lower_z,_upper_z,maskZ);
	      const float upper_z = cl::sycl::select(_upper_z,_lower_z,maskZ);
	      
	      const float lowerX = cl::sycl::fma((float)inv_dir.x(), lower_x, (float)inv_dir_org.x());
	      const float upperX = cl::sycl::fma((float)inv_dir.x(), upper_x, (float)inv_dir_org.x());
	      const float lowerY = cl::sycl::fma((float)inv_dir.y(), lower_y, (float)inv_dir_org.y());
	      const float upperY = cl::sycl::fma((float)inv_dir.y(), upper_y, (float)inv_dir_org.y());
	      const float lowerZ = cl::sycl::fma((float)inv_dir.z(), lower_z, (float)inv_dir_org.z());
	      const float upperZ = cl::sycl::fma((float)inv_dir.z(), upper_z, (float)inv_dir_org.z());

	      const float near = cl::sycl::fmax( cl::sycl::fmax(lowerX,lowerY), cl::sycl::fmax(lowerZ,tnear) );
	      const float far  = cl::sycl::fmin( cl::sycl::fmin(upperX,upperY), cl::sycl::fmin(upperZ,tfar) );
	      const uint valid = islessequal(near,far);	  // final valid mask
	      uint mask = intel_sub_group_ballot(subgroupLocalID < 8);  

	      if (0 == subgroupLocalID)
		{
		  out << mask << cl::sycl::endl;
		}

#if 0	      
	      if (mask == 0)
		{
		  sindex--;	   
		  cur = stack_offset[sindex];
		  continue;
		}
	    
	      offset += cur; /* relative encoding */
	      const uint popc = popcount(mask); 
	      cur = broadcast(offset, ctz(mask));
	      if (popc == 1) continue; // single hit only
	      int t = (as_int(near) & mask_uint) | slotID;  // make the integer distance unique by masking off the least significant bits and adding the slotID
	      t = valid ? t : max_uint;                     // invalid slots set to MIN_INT, as we sort descending;	

	      for (uint i=0;i<popc-1;i++)
		{
		  const int t_max = sub_group_reduce_max(t); // from larger to smaller distance
		  t = (t == t_max) ? max_uint : t;
		  stack_offset[sindex] = broadcast(offset,t_max & (~mask_uint));
		  stack_dist[sindex]   = broadcast(near  ,t_max & (~mask_uint));
		  sindex++;
		}
	      const int t_max = sub_group_reduce_max(t); // from larger to smaller distance
	      cur = broadcast(offset,t_max & (~mask_uint));
#endif	      
	    }
	  if (cur == max_uint) break; // sentinel reached -> exit
	}
      
      // for (uint i=0;i<subgroupSize;i++)
      // 	if (i == subgroupLocalID)
      // 	  out << "groupID " << groupID << " numGroups " << numGroups << cl::sycl::endl;
      
    }
#endif

    void BVHNGPUIntersectorStream::intersect(Accel::Intersectors* This, RayHitN** _inputRays, size_t numRays, IntersectContext* context)
    {
      BVH* __restrict__ bvh = (BVH*) This->ptr;
      
      if (bvh->root == BVH::emptyNode)
	{
	  PRINT(bvh);
	  PRINT(bvh->root);
	  PRINT("empty");
	  exit(0);
	  return;
	}

      
#if defined(EMBREE_DPCPP_SUPPORT)
      gpu::RTCRayHitGPU* inputRays = (gpu::RTCRayHitGPU*)_inputRays;
      void *bvh_mem = (void*)(size_t)(bvh->root);
      
      //for (size_t i=0;i<10;i++)
      // std::cout << i << " " << inputRays[i] << std::endl;


      numRays = 1;
      
      DeviceGPU* deviceGPU = (DeviceGPU*)bvh->device;
      cl::sycl::queue &gpu_queue = deviceGPU->getQueue();

      cl::sycl::event queue_event = gpu_queue.submit([&](cl::sycl::handler &cgh) {

	  cl::sycl::stream out(DBG_PRINT_BUFFER_SIZE, DBG_PRINT_LINE_SIZE, cgh);
	  const cl::sycl::nd_range<1> nd_range(numRays*cl::sycl::range<1>(BVH_NODE_N),cl::sycl::range<1>(BVH_NODE_N));		  
	  cgh.parallel_for<class trace_ray_stream>(nd_range,[=](cl::sycl::nd_item<1> item) {
	      const uint groupID   = item.get_group(0);
	      cl::sycl::intel::sub_group sg = item.get_sub_group();	      
	      traceRayBVH16(sg,inputRays[groupID],bvh_mem,out);	      
	    });		  
	});
      try {
	gpu_queue.wait_and_throw();
      } catch (cl::sycl::exception const& e) {
	std::cout << "Caught synchronous SYCL exception:\n"
		  << e.what() << std::endl;
      }

#endif      
    }

    void BVHNGPUIntersectorStream::occluded (Accel::Intersectors* This, RayN** inputRays, size_t numRays, IntersectContext* context)
    {
#if 0	
      const unsigned int slotID  = get_sub_group_local_id();

      const float   ray  = as_float((intel_sub_group_block_read((const __global uint*)(rh))));
      const float3 org   = (float3)(sub_group_broadcast(ray,0),sub_group_broadcast(ray,1),sub_group_broadcast(ray,2));
      const float tnear  = sub_group_broadcast(ray,3);
      const float3 dir   = (float3)(sub_group_broadcast(ray,4),sub_group_broadcast(ray,5),sub_group_broadcast(ray,6));
      float tfar         = sub_group_broadcast(ray,8);
    
      unsigned int stack_offset[STACK_ENTRIES]; 
      float        stack_dist[STACK_ENTRIES];  

      // hold local tfar and hit in registers
      float hit_tfar = tfar;  
      float8 local_hit;
      local_hit.s5 = as_float(-1);
      local_hit.s6 = as_float(-1);

      const unsigned int maskX = select(1,0,dir.x >= 0.0f);
      const unsigned int maskY = select(1,0,dir.y >= 0.0f);
      const unsigned int maskZ = select(1,0,dir.z >= 0.0f);
   
      const float3 inv_dir = native_recip(select(1E-18f,dir,dir != 0.0f));
      const float3 inv_dir_org = -inv_dir * org;
  
      const unsigned int max_uint  = 0xffffffff;  
      const unsigned int mask_uint = 0xfffffff0;

      const global char *bvh = _bvh + sizeof(struct BVHBase);
      stack_offset[0] = max_uint; // sentinel
      stack_dist[0]   = -(float)INFINITY;

      stack_offset[1] = sizeof(struct BVHNodeN); // single node after bvh start //*(global uint*)(bvh); // root noderef stored at the beginning of the bvh
      stack_dist[1]   = -(float)INFINITY;
  
      unsigned int sindex = 2; 
      while(1) { 
	sindex--;
    
#if STACK_CULLING  == 1    
	if (stack_dist[sindex] > tfar) continue;
#endif
    
	unsigned int cur = stack_offset[sindex]; 

	while((cur & BVH_LEAF_MASK) == 0) 
	  {	
	    RAY_STATS(travStats_inc_tsteps());

	    const struct NodeIntersection nsec = intersectNode(bvh, cur, maskX, maskY, maskZ, inv_dir, inv_dir_org, tnear, tfar);
	    unsigned int offset      = nsec.offset;
	    const float near         = nsec.near;
	    const unsigned int valid = nsec.valid;
	
	    uint mask = intel_sub_group_ballot(valid);  
	    if (mask == 0)
	      {
#if STACK_CULLING  == 1
		do { 
		  sindex--;
		} while (stack_dist[sindex] > tfar);
#else
		sindex--;	   
#endif	   
		cur = stack_offset[sindex];
		continue;
	      }
	    offset += cur; /* relative encoding */
	    const uint popc = popcount(mask); 
	    cur = broadcast(offset, ctz(mask));
	    if (popc == 1) continue; // single hit only
	    int t = (as_int(near) & mask_uint) | slotID;  // make the integer distance unique by masking off the least significant bits and adding the slotID
	    t = valid ? t : max_uint;                     // invalid slots set to MIN_INT, as we sort descending;	

	    for (uint i=0;i<popc-1;i++)
	      {
		const int t_max = sub_group_reduce_max(t); // from larger to smaller distance
		t = (t == t_max) ? max_uint : t;
		stack_offset[sindex] = broadcast(offset,t_max & (~mask_uint));
		stack_dist[sindex]   = broadcast(near  ,t_max & (~mask_uint));
		sindex++;
	      }
	    const int t_max = sub_group_reduce_max(t); // from larger to smaller distance
	    cur = broadcast(offset,t_max & (~mask_uint));	
	  }
	if (cur == max_uint) break; // sentinel reached -> exit

	const unsigned int numPrims = getNumLeafPrims(cur);
	const unsigned int leafOffset = getLeafOffset(cur);    
    
	RAY_STATS(travStats_inc_leaves());    
	RAY_STATS(travStats_inc_isteps(numPrims));

	global struct Quad1 *quads = (global struct Quad1 *)(bvh + leafOffset);
	hit_tfar = intersectQuad1(quads, numPrims, org, dir, tnear, hit_tfar, &local_hit, slotID);
    
	const float old_tfar = tfar;
	tfar = sub_group_reduce_min(hit_tfar);
#endif	
    }


    /*! BVH ray GPU intersectors */
    
    class BVHNGPUIntersector1
    {
    public:
      static void intersect (const Accel::Intersectors* This, RayHit& ray, IntersectContext* context);
      static void occluded  (const Accel::Intersectors* This, Ray& ray, IntersectContext* context);
      static bool pointQuery(const Accel::Intersectors* This, PointQuery* query, PointQueryContext* context);
    };
    
    void BVHNGPUIntersector1::intersect (const Accel::Intersectors* This, RayHit& ray, IntersectContext* context)

    {
      
    }

    void BVHNGPUIntersector1::occluded  (const Accel::Intersectors* This, Ray& ray, IntersectContext* context)

    {
      
    }
    
    bool BVHNGPUIntersector1::pointQuery(const Accel::Intersectors* This, PointQuery* query, PointQueryContext* context)
    {
      return false;
    }
    



    class BVHNGPUIntersector4
    {
    public:
      static void intersect(vint<4>* valid, Accel::Intersectors* This, RayHitK<4>& ray, IntersectContext* context);
      static void occluded (vint<4>* valid, Accel::Intersectors* This, RayK<4>& ray, IntersectContext* context);
    };

    void BVHNGPUIntersector4::intersect(vint<4>* valid, Accel::Intersectors* This, RayHitK<4>& ray, IntersectContext* context)
    {
      
    }
    
    void BVHNGPUIntersector4::occluded(vint<4>* valid, Accel::Intersectors* This, RayK<4>& ray, IntersectContext* context)
    {
      
    }
    
    ////////////////////////////////////////////////////////////////////////////////
    /// General BVHIntersectorStreamPacketFallback Intersector
    ////////////////////////////////////////////////////////////////////////////////

    DEFINE_INTERSECTORN(BVHGPUIntersectorStream,BVHNGPUIntersectorStream);
    DEFINE_INTERSECTOR1(BVHGPUIntersector1,BVHNGPUIntersector1);
    DEFINE_INTERSECTOR4(BVHGPUIntersector4,BVHNGPUIntersector4);    
    
  };
};