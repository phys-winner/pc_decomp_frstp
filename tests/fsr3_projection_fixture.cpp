#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
struct FpTemporalFrame{float nearPlane=0,farPlane=0,verticalFov=0;bool inverted=false;std::array<float,16> view{};};
struct DxvkFsr3{static bool projection(const float*,const float*,FpTemporalFrame&);};
#include "fsr3_projection_production.inc"
int main(){
    unsigned cases=0;
    for(bool transpose:{false,true})for(bool inverted:{false,true})for(float angle:{0.f,.27f,1.01f,2.3f})
    for(float nearPlane:{.1f,1.f,3.f})for(float farPlane:{100.f,1000.f,10000.f}){
        const float fov=1.13f;float view[16]={},projection[16]={},vp[16]={};
        float cosine=std::cos(angle),sine=std::sin(angle),f=1/std::tan(fov*.5f);
        view[0]=view[10]=cosine;view[2]=sine;view[8]=-sine;view[5]=view[15]=1;
        view[12]=41;view[13]=-3;view[14]=13;
        projection[0]=f/1.6f;projection[5]=f;projection[11]=1;
        projection[10]=inverted?nearPlane/(nearPlane-farPlane):farPlane/(farPlane-nearPlane);
        projection[14]=(inverted?1:-1)*farPlane*nearPlane/(farPlane-nearPlane);
        for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k)vp[i*4+j]+=view[i*4+k]*projection[k*4+j];
        if(transpose)for(unsigned i=0;i<4;++i)for(unsigned j=i+1;j<4;++j){std::swap(view[i*4+j],view[j*4+i]);std::swap(vp[i*4+j],vp[j*4+i]);}
        FpTemporalFrame frame;assert(DxvkFsr3::projection(vp,view,frame));
        if(std::fabs(frame.nearPlane-nearPlane)>nearPlane*.001f||std::fabs(frame.farPlane-farPlane)>farPlane*.015f||frame.inverted!=inverted){
            std::fprintf(stderr,"case transpose=%u reversed=%u near=%g far=%g got=%g,%g angle=%g\n",transpose,inverted,nearPlane,farPlane,frame.nearPlane,frame.farPlane,angle);return 1;
        }
        assert(std::fabs(frame.verticalFov-fov)<.0001f);++cases;
    }
    float zero[16]={},identity[16]={};for(unsigned i=0;i<4;++i)identity[i*4+i]=1;
    FpTemporalFrame result;assert(!DxvkFsr3::projection(zero,zero,result));assert(!DxvkFsr3::projection(identity,identity,result));
    identity[0]=std::numeric_limits<float>::quiet_NaN();assert(!DxvkFsr3::projection(identity,identity,result));
    std::printf("{\"success\":true,\"projection_cases\":%u,\"invalid_cases\":3}\n",cases);
}
