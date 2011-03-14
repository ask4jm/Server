/*
* copyright (c) 2010 Sveriges Television AB <info@casparcg.com>
*
*  This file is part of CasparCG.
*
*    CasparCG is free software: you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    CasparCG is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.

*    You should have received a copy of the GNU General Public License
*    along with CasparCG.  If not, see <http://www.gnu.org/licenses/>.
*
*/
 
#ifndef CLEAR_H_
#define CLEAR_H_

#include "../CPUID.hpp"

namespace caspar{
namespace utils{
namespace image{
	
void Clear_SSE2	 (void* dest, size_t size);
void Clear_REF	 (void* dest, size_t size);
void ClearParallel_SSE2	 (void* dest, size_t size);
void ClearParallel_REF	 (void* dest, size_t size);

typedef void(*ClearFun)(void*, size_t);
ClearFun GetClearFun(SIMD simd = REF);


//void StraightTransform_SSE2(const void* source, void* dest, size_t size);
//void StraightTransform_REF(const void* source, void* dest, size_t size);
//
//typedef void(*StraightTransformFun)(const void*, void*, size_t);
//StraightTransformFun GetStraightTransformFun(SIMD simd = REF);

} // namespace image
} // namespace utils
} // namespace caspar

#endif

