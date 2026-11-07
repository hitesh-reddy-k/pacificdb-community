#pragma once
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
namespace pacificdb::vectors {
// Rare overflow path only: ordinary float64 kernels retain their arithmetic and
// ordering. Wider intermediates recover representable scores; unsupported ranges
// fail explicitly rather than silently turning a cosine into zero.
template<class Get>double wideDenseScore(const std::vector<double>& query,Get get,const std::string& metric){
    long double sum=0,qq=0,vv=0;
    const bool l2=metric=="l2"||metric=="euclidean",dot=metric=="dot"||metric=="dot_product";
    for(size_t i=0;i<query.size();++i){const long double a=query[i],b=get(i);
        if(!std::isfinite(a)||!std::isfinite(b))throw std::runtime_error("vector_score_out_of_range");
        if(l2){const auto d=a-b;sum+=d*d;}else{sum+=a*b;if(!dot){qq+=a*a;vv+=b*b;}}
    }
    long double score=sum;
    if(l2)score=-std::sqrt(sum);
    else if(!dot){const auto denominator=std::sqrt(qq)*std::sqrt(vv);
        if(!std::isfinite(denominator))throw std::runtime_error("vector_score_out_of_range");
        score=denominator<1e-12L?0.L:sum/denominator;
    }
    if(!std::isfinite(score)||std::abs(score)>std::numeric_limits<double>::max())throw std::runtime_error("vector_score_out_of_range");
    return static_cast<double>(score);
}
}
