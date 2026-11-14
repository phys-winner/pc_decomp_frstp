#pragma once
#include "dxvk_image.h"
#include "dxvk_resource.h"
#include "dxvk_sampler.h"
#include "dxvk_shader.h"
#include "fsr3_api.h"
#include <array>

extern "C" void frostpunk_dxvk_upscaler_failed(uint32_t) __attribute__((weak));

namespace dxvk {
class DxvkDevice;
struct FpTemporalFrame {
    Rc<DxvkImageView> depth, opaque;
    float nearPlane=0, farPlane=0, verticalFov=0, jitterX=0, jitterY=0;
    std::array<float,16> view{};
    bool valid=false, inverted=false;
};

class DxvkFsr3 : public DxvkResource {
public:
    DxvkFsr3(const Rc<DxvkDevice>& device,VkExtent2D input,VkExtent2D output,bool inverted);
    ~DxvkFsr3();
    void retire();
    static const FpFsr3Api *load();
    static bool projection(const float *viewProjection,const float *view,FpTemporalFrame& frame);
    void nextJitter(uint32_t index,float& x,float& y) const;
    bool matches(VkExtent2D input,VkExtent2D output,bool inverted) const;
    FpFsr3Frame frame(const FpTemporalFrame& metadata,const Rc<DxvkImageView>& source) const;
    void fail(uint32_t error);
    const FpFsr3Api *api=nullptr;
    void *sdk=nullptr;
    VkExtent2D input{},output{};
    bool inverted=false, initialized=false, reset=true, failed=false;
    uint64_t lastNanoseconds=0;
    std::array<float,16> previousView{};
    Rc<DxvkShader> prepare;
    Rc<DxvkSampler> sampler;
    std::array<Rc<DxvkImageView>,9> images;
    enum {Depth,Motion,Reactive,Output,Flow,SceneChange,DilatedDepth,DilatedMotion,PreviousDepth};
private:
    Rc<DxvkDevice> m_device;
};
}
