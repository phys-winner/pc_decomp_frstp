/* Executes the candidate's unmodified metadata and descriptor-update C++ methods
 * against recording Vulkan objects. It never creates or fakes a Vulkan device. */
#include <vulkan/vulkan.h>
#include <array>
#include <bitset>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
#include <type_traits>

template<typename T> static T handle(uintptr_t x) {return reinterpret_cast<T>(x);}
constexpr uint32_t MaxNumResourceSlots=1216;
static bool unlikely(bool value){return value;}
struct DxvkDescriptorSets {static constexpr uint32_t SetCount=3;};
enum class DxvkAccess {None,Read,Write};
using DxvkError=std::runtime_error;
struct DxvkDescriptorInfo {VkDescriptorImageInfo image={};VkDescriptorBufferInfo buffer={};VkBufferView texelBuffer=VK_NULL_HANDLE;};
struct FakeBuffer {std::array<uint32_t,2432> data={};};
struct DxvkBufferSlice {
  std::shared_ptr<FakeBuffer> allocation;uint64_t bytes=0;
  uint64_t length()const{return bytes;}
  auto buffer()const{return allocation;}
  void *mapPtr(uint64_t)const{return allocation?allocation->data.data():nullptr;}
  DxvkDescriptorInfo getDescriptor()const {
    DxvkDescriptorInfo result;result.buffer={allocation?handle<VkBuffer>(0x1000):VK_NULL_HANDLE,0,bytes};return result;
  }
};
struct FakeView {
  struct Info {uint64_t rangeLength=0;} description;
  VkBufferView view=::handle<VkBufferView>(0x2000);unsigned updates=0;
  void updateView(){updates++;}
  auto handle()const{return view;}
  const Info &info()const{return description;}
  auto buffer()const{return 1;}
};
struct FakeImage {
  VkImageViewType supported=VK_IMAGE_VIEW_TYPE_2D;VkImageView view=::handle<VkImageView>(0x3000);
  struct Info {VkImageLayout layout=VK_IMAGE_LAYOUT_GENERAL;} description;
  auto handle(VkImageViewType type)const{return type==supported?view:VK_NULL_HANDLE;}
  const Info &imageInfo()const{return description;}
  auto image()const{return 1;}
};
struct FakeSampler {VkSampler handle()const{return ::handle<VkSampler>(0x4000);}};
struct Resource {
  std::shared_ptr<FakeView> bufferView;std::shared_ptr<FakeImage> imageView;
  std::shared_ptr<FakeSampler> sampler;DxvkBufferSlice bufferSlice;
};
struct DxvkBindingInfo {VkDescriptorType descriptorType;uint32_t resourceBinding;VkImageViewType viewType=VK_IMAGE_VIEW_TYPE_2D;};
struct DxvkBindingLayout {
  std::array<std::vector<DxvkBindingInfo>,3> sets;
  uint32_t getBindingCount(unsigned set)const{return uint32_t(sets[set].size());}
  const auto &getBinding(unsigned set,unsigned index)const{return sets[set][index];}
};
struct DxvkBindingLayoutObjects {
  DxvkBindingLayout bindings;
  const auto &layout()const{return bindings;}
  unsigned getBindingCount()const{return bindings.getBindingCount(0)+bindings.getBindingCount(1)+bindings.getBindingCount(2);}
  unsigned getSetMask()const {unsigned mask=0;for(unsigned i=0;i<3;i++)if(bindings.getBindingCount(i))mask|=1u<<i;return mask;}
  VkDescriptorUpdateTemplate getSetUpdateTemplate(unsigned)const{return VK_NULL_HANDLE;}
  VkPipelineLayout getPipelineLayout(bool)const{return VK_NULL_HANDLE;}
};
struct DxvkDevice {
  struct Properties {VkPhysicalDeviceProperties2 core={};} properties_;
  struct Features {VkPhysicalDeviceRobustness2FeaturesEXT extRobustness2={};} features_;
  DxvkDevice(){properties_.core.properties.vendorID=0x13b5;properties_.core.properties.limits.minStorageBufferOffsetAlignment=64;}
  const auto &properties()const{return properties_;}
  const auto &features()const{return features_;}
};
template<typename T> struct Pointer {T object;T *ptr(){return &object;}T *operator->(){return &object;}};
struct Staging {
  unsigned allocations=0;
  DxvkBufferSlice alloc(uint64_t alignment,uint64_t bytes){assert(alignment==64&&bytes==9728);allocations++;return {std::make_shared<FakeBuffer>(),bytes};}
};
struct RecordedWrite {unsigned binding;VkDescriptorType type;uint64_t range;};
struct Commands {
  std::vector<RecordedWrite> writes;std::vector<std::shared_ptr<FakeBuffer>> retained;
  unsigned updateCalls=0,bindCalls=0,templateCalls=0;
  template<DxvkAccess A,typename T>void trackResource(const T &object){
    if constexpr(std::is_same<T,std::shared_ptr<FakeBuffer>>::value)retained.push_back(object);
  }
  void updateDescriptorSets(unsigned count,const VkWriteDescriptorSet *items){
    updateCalls++;
    for(unsigned i=0;i<count;i++) {
      const auto &w=items[i];uint64_t range=0;
      switch(w.descriptorType) {
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
          assert(w.pImageInfo->imageView!=VK_NULL_HANDLE);break;
        case VK_DESCRIPTOR_TYPE_SAMPLER:assert(w.pImageInfo->sampler!=VK_NULL_HANDLE);break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
          assert(w.pBufferInfo->buffer!=VK_NULL_HANDLE);range=w.pBufferInfo->range;break;
        default:assert(*w.pTexelBufferView!=VK_NULL_HANDLE);break;
      }
      writes.push_back({w.dstBinding,w.descriptorType,range});
    }
  }
  void updateDescriptorSetWithTemplate(VkDescriptorSet,VkDescriptorUpdateTemplate,const void*){templateCalls++;}
  void cmdBindDescriptorSets(VkPipelineBindPoint,VkPipelineLayout,unsigned,unsigned,const VkDescriptorSet*,unsigned,const uint32_t*){bindCalls++;}
};
struct Pool {
  unsigned lastMask=0;
  void alloc(const DxvkBindingLayoutObjects*,unsigned mask,VkDescriptorSet *sets){lastMask=mask;for(unsigned i=0;i<3;i++)sets[i]=handle<VkDescriptorSet>(0x5000+i);}
};
struct State {unsigned dirty=7;unsigned getDirtyGraphicsSets(){return dirty;}unsigned getDirtyComputeSets(){return dirty;}};
struct Tracked {std::bitset<1216> bits;bool set(unsigned i){bool previous=bits[i];bits.set(i);return !previous;}};
enum class DxvkContextFlag {GpIndependentSets};
struct Flags {bool test(DxvkContextFlag)const{return false;}};
struct Common {
  struct Dummy {VkSampler samplerHandle()const{return handle<VkSampler>(0x6000);}} dummy;
  auto &dummyResources(){return dummy;}
};
namespace env {static bool is32BitHostPlatform(){return false;}}
namespace bit {
static std::vector<unsigned> BitMask(unsigned mask){std::vector<unsigned> result;for(unsigned i=0;i<32;i++)if(mask&(1u<<i))result.push_back(i);return result;}
static unsigned tzcnt(unsigned x){unsigned n=0;while(x&&!(x&1u)){n++;x>>=1;}return n;}
}
class DxvkContext {
public:
  Pointer<DxvkDevice> m_device;Staging m_staging;std::shared_ptr<Commands> m_cmd=std::make_shared<Commands>();
  std::array<Resource,1216> m_rc;Tracked m_rcTracked;State m_descriptorState;Flags m_flags;
  std::shared_ptr<Pool> m_descriptorPool=std::make_shared<Pool>();Pointer<Common> m_common;
  std::vector<DxvkDescriptorInfo> m_descriptors;std::vector<VkWriteDescriptorSet> m_descriptorWrites;
  DxvkBufferSlice createRobustnessMetadata(const DxvkBindingLayout&);
  template<VkPipelineBindPoint P>void updateResourceBindings(const DxvkBindingLayoutObjects*);
  void resizeDescriptorArrays(uint32_t count){
    m_descriptors.resize(count);m_descriptorWrites.resize(count);
    for(unsigned i=0;i<count;i++) {
      auto &w=m_descriptorWrites[i];w={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.descriptorCount=1;
      w.pImageInfo=&m_descriptors[i].image;w.pBufferInfo=&m_descriptors[i].buffer;w.pTexelBufferView=&m_descriptors[i].texelBuffer;
    }
  }
};
#define __ANDROID__ 1
#include "backend_methods.inc"
#undef __ANDROID__

int main() {
  DxvkContext c;DxvkBindingLayoutObjects layout;
  // FS views and constant buffers split across sets. Starting with only UBO dirty
  // must still allocate metadata's set, with all cross-set entries in one snapshot.
  layout.bindings.sets[0]={{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,3},{VK_DESCRIPTOR_TYPE_SAMPLER,4},
    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,5},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1216}};
  layout.bindings.sets[1]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,7}};
  layout.bindings.sets[2]={{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,9},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,10,VK_IMAGE_VIEW_TYPE_CUBE},
    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1216}};
  c.m_rc[5].bufferSlice={std::make_shared<FakeBuffer>(),4};
  c.m_rc[7].bufferSlice={std::make_shared<FakeBuffer>(),20};
  c.m_rc[9].bufferView=std::make_shared<FakeView>();c.m_rc[9].bufferView->description.rangeLength=12;
  c.m_rc[10].imageView=std::make_shared<FakeImage>(); // Incompatible 2D view for CUBE is unbound.
  c.m_descriptorState.dirty=2;
  c.updateResourceBindings<VK_PIPELINE_BIND_POINT_GRAPHICS>(&layout);
  assert(c.m_descriptorPool->lastMask==7&&c.m_staging.allocations==1);
  assert(c.m_cmd->templateCalls==0&&c.m_cmd->writes.size()==6);
  auto old=c.m_cmd->retained[0];auto &data=old->data;
  assert(data[3*2]==0&&data[4*2]==1&&data[5*2]==1&&data[5*2+1]==4);
  assert(data[7*2]==1&&data[7*2+1]==20&&data[9*2+1]==12&&data[10*2]==0);
  unsigned metadataWrites=0;for(const auto &w:c.m_cmd->writes)metadataWrites+=w.range==9728;
  assert(metadataWrites==2);
  c.m_rc[7].bufferSlice.bytes=16;c.m_rc[3].imageView=std::make_shared<FakeImage>();
  c.m_descriptorState.dirty=1;c.m_cmd->writes.clear();
  c.updateResourceBindings<VK_PIPELINE_BIND_POINT_GRAPHICS>(&layout);
  auto fresh=c.m_cmd->retained[3]; // snapshot, counter buffer, UBO are retained for each update.
  assert(fresh!=old&&old->data[14+1]==20&&old->data[6]==0);
  assert(fresh->data[15]==16&&fresh->data[6]==1&&c.m_cmd->writes.size()==7);
  c.m_descriptorState.dirty=0;unsigned allocations=c.m_staging.allocations;
  c.updateResourceBindings<VK_PIPELINE_BIND_POINT_GRAPHICS>(&layout);assert(c.m_staging.allocations==allocations);
  unsigned rejected=0;
  c.m_rc[7].bufferSlice.bytes=UINT64_C(0x100000000);
  try{c.createRobustnessMetadata(layout.bindings);}catch(const DxvkError&){rejected++;}
  c.m_rc[7].bufferSlice.bytes=16;
  layout.bindings.sets[0].push_back({VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1217});
  try{c.createRobustnessMetadata(layout.bindings);}catch(const DxvkError&){rejected++;}
  layout.bindings.sets[0].pop_back();
  layout.bindings.sets[0].push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3}); // Same logical ID with a contradictory descriptor.
  try{c.createRobustnessMetadata(layout.bindings);}catch(const DxvkError&){rejected++;}
  assert(rejected==3);
  std::puts("{\"success\":true,\"actual_backend_methods\":true,\"metadata_abi\":1,\"sets_refreshed\":3,\"immutable_snapshots\":2,\"negative_gates\":3,\"null_writes\":0}");
}
