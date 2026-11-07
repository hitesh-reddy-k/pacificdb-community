#pragma once
#include <cstddef>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif
namespace pacificdb::vector_kernels {
// Preserve double-precision accumulation, including extreme finite float inputs.
// SSE2 is baseline on x64; the scalar fallback covers other supported platforms.
inline double dot(const float* a,const float* b,size_t count) noexcept {
    double sum=0.;size_t i=0;
#if defined(__SSE2__) || defined(_M_X64)
    __m128d acc=_mm_setzero_pd();
    for(;i+4<=count;i+=4){
        const auto x=_mm_loadu_ps(a+i),y=_mm_loadu_ps(b+i);
        acc=_mm_add_pd(acc,_mm_mul_pd(_mm_cvtps_pd(x),_mm_cvtps_pd(y)));
        acc=_mm_add_pd(acc,_mm_mul_pd(_mm_cvtps_pd(_mm_movehl_ps(x,x)),_mm_cvtps_pd(_mm_movehl_ps(y,y))));
    }
    double lanes[2];_mm_storeu_pd(lanes,acc);sum=lanes[0]+lanes[1];
#endif
    for(;i<count;++i)sum+=double(a[i])*double(b[i]);
    return sum;
}
}
