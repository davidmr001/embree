// ======================================================================== //
// Copyright 2009-2015 Intel Corporation                                    //
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

#pragma once

#include "bvh.h"
#include "../../common/ray.h"
#include "../../common/stack_item.h"

namespace embree
{
  namespace isa 
  {

    /*! An item on the stack holds the node ID and distance of that node. */
    struct __aligned(16) StackItemMask
    {
      union {
        size_t mask;
        size_t dist;
      };
      size_t ptr; 
    };

#if defined(__AVX__)
    template<int types, int N, int K>
      class BVHNNodeTraverserKHit
    {
      typedef BVHN<N> BVH;
      typedef typename BVH::NodeRef NodeRef;
      typedef typename BVH::BaseNode BaseNode;


    public:

      template<class T>
      static __forceinline void traverseClosestHit(NodeRef& cur,
                                                   size_t &m_trav_active,
                                                   const vbool<K> &vmask,
                                                   const vfloat<K>& tNear,
                                                   const T * const tMask,
                                                   StackItemMask*& stackPtr)
      {
        size_t mask = movemask(vmask);
        assert(mask != 0);
        const BaseNode* node = cur.baseNode(types);

        /*! one child is hit, continue with that child */
        const size_t r0 = __bscf(mask);          
        assert(r0 < 8);
        cur = node->child(r0);         
        cur.prefetch(types);
        m_trav_active = tMask[r0];        
        assert(cur != BVH::emptyNode);
        if (unlikely(mask == 0)) return;

        const unsigned int * const tNear_i = (unsigned int*)&tNear;

        /*! two children are hit, push far child, and continue with closer child */
        NodeRef c0 = cur; 
        unsigned int d0 = tNear_i[r0];
        const size_t r1 = __bscf(mask);
        assert(r1 < 8);
        NodeRef c1 = node->child(r1); 
        c1.prefetch(types); 
        unsigned int d1 = tNear_i[r1];

        assert(c0 != BVH::emptyNode);
        assert(c1 != BVH::emptyNode);
        if (likely(mask == 0)) {
          if (d0 < d1) { stackPtr->ptr = c1; stackPtr->mask = tMask[r1]; stackPtr++; cur = c0; m_trav_active = tMask[r0]; return; }
          else         { stackPtr->ptr = c0; stackPtr->mask = tMask[r0]; stackPtr++; cur = c1; m_trav_active = tMask[r1]; return; }
        }
        /*! slow path for more than two hits */
        const size_t hits = __popcnt(movemask(vmask));
        const vint<K> dist_i = select(vmask,(asInt(tNear) & 0xfffffff8) | vint<K>( step ),0x7fffffff);
#if defined(__AVX512F__)
        const vint8 tmp = _mm512_castsi512_si256(dist_i);
        const vint<K> dist_i_sorted = sortNetwork(tmp);
#else
        const vint<K> dist_i_sorted = sortNetwork(dist_i);
#endif
        const vint<K> sorted_index = dist_i_sorted & 7;

        size_t i = hits-1;
        for (;;)
        {
          const unsigned int index = sorted_index[i];
          assert(index < 8);
          cur = node->child(index);
          m_trav_active = tMask[index];
          assert(m_trav_active);
          cur.prefetch(types);
          if (unlikely(i==0)) break;
          i--;
          assert(cur != BVH::emptyNode);
          stackPtr->ptr = cur; 
          stackPtr->mask = m_trav_active;
          stackPtr++;
        }
      }

      template<class T, class M>
      static __forceinline void traverseAnyHit(NodeRef& cur,
                                               size_t &m_trav_active,
                                               const M &vmask,
                                               const T * const tMask,
                                               StackItemMask*& stackPtr)
      {
        size_t mask = movemask(vmask);
        assert(mask != 0);
        const BaseNode* node = cur.baseNode(types);

        /*! one child is hit, continue with that child */
        size_t r = __bscf(mask);
        cur = node->child(r);         
        cur.prefetch(types);
        m_trav_active = tMask[r];
        
        /* simple in order sequence */
        assert(cur != BVH::emptyNode);
        if (likely(mask == 0)) return;
        stackPtr->ptr  = cur;
        stackPtr->mask = m_trav_active;
        stackPtr++;

        for (; ;)
        {
          r = __bscf(mask);
          cur = node->child(r);          
          cur.prefetch(types);
          m_trav_active = tMask[r];
          assert(cur != BVH::emptyNode);
          if (likely(mask == 0)) return;
          stackPtr->ptr  = cur;
          stackPtr->mask = m_trav_active;
          stackPtr++;
        }
      }

    };
#endif


    /*! BVH ray stream intersector. */
    template<int N, int K, int types, bool robust, typename PrimitiveIntersector>
      class BVHNStreamIntersector
    {
      /* shortcuts for frequently used types */
      typedef typename PrimitiveIntersector::Precalculations Precalculations;
      typedef typename PrimitiveIntersector::Primitive Primitive;
      typedef BVHN<N> BVH;
      typedef typename BVH::NodeRef NodeRef;
      typedef typename BVH::BaseNode BaseNode;
      typedef typename BVH::Node Node;
      typedef typename BVH::NodeMB NodeMB;
      typedef Vec3<vfloat<K>> Vec3vfK;
      typedef Vec3<vint<K>> Vec3viK;

      struct __aligned(32) RayContext {
        Vec3fa rdir;      //     rdir.w = tnear;
        Vec3fa org_rdir;  // org_rdir.w = tfar;        
      };

      struct NearFarPreCompute
      {
#if defined(__AVX512F__)
        vint<K> permX,permY,permZ;
#else
        size_t nearX,nearY,nearZ;
        size_t farX,farY,farZ;
#endif
        __forceinline NearFarPreCompute(const Vec3fa &dir)
        {
#if defined(__AVX512F__)
        const vint<K> id( step );
        const vint<K> id2 = align_shift_right<K/2>(id,id);
        permX = select(vfloat<K>(dir.x) >= 0.0f,id,id2);
        permY = select(vfloat<K>(dir.y) >= 0.0f,id,id2);
        permZ = select(vfloat<K>(dir.z) >= 0.0f,id,id2);
#else
        nearX = (dir.x < 0.0f) ? 1*sizeof(vfloat<N>) : 0*sizeof(vfloat<N>);
        nearY = (dir.y < 0.0f) ? 3*sizeof(vfloat<N>) : 2*sizeof(vfloat<N>);
        nearZ = (dir.z < 0.0f) ? 5*sizeof(vfloat<N>) : 4*sizeof(vfloat<N>);
        farX  = nearX ^ sizeof(vfloat<N>);
        farY  = nearY ^ sizeof(vfloat<N>);
        farZ  = nearZ ^ sizeof(vfloat<N>);
#endif
        }
      };

      static __forceinline void initRayContext(RayContext * __restrict__ ray_ctx, 
                                               Ray ** __restrict__  rays, 
                                               const size_t numOctantRays)
      {
        for (size_t i=0;i<numOctantRays;i++)
        {
#if defined(__AVX512F__)
          vfloat<K> org(vfloat4(rays[i]->org));
          vfloat<K> dir(vfloat4(rays[i]->dir));
          vfloat<K> rdir       = select(0x7777,rcp_safe(dir),rays[i]->tnear);
          vfloat<K> org_rdir   = select(0x7777,org * rdir,rays[i]->tfar);
          vfloat<K> res = select(0xf,rdir,org_rdir);
          vfloat8 r = extractf256bit(res);
          *(vfloat8*)&ray_ctx[i] = r;          
#else
          Vec3fa &org = rays[i]->org;
          Vec3fa &dir = rays[i]->dir;
          ray_ctx[i].rdir       = rcp_safe(dir);
          ray_ctx[i].org_rdir   = org * ray_ctx[i].rdir;
          ray_ctx[i].rdir.w     = rays[i]->tnear;
          ray_ctx[i].org_rdir.w = rays[i]->tfar;
#endif
        }       
      }

      static const size_t stackSizeChunk  = N*BVH::maxDepth+1;
      static const size_t stackSizeSingle = 1+(N-1)*BVH::maxDepth;

    public:
      static void intersect(BVH* bvh, Ray **ray, size_t numRays, size_t flags);
      static void occluded (BVH* bvh, Ray **ray, size_t numRays, size_t flags);
    };

  }
}
