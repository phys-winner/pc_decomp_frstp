"""Execute candidate DXVK format tables and upload math, without GPU/device access."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import dxvk_astc_overlay as overlay
from leaf_probe import windows_toolchain


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(data, encoding='utf-8')


SHIM = r'''#pragma once
#include <cstdint>
#include <cstddef>
#include <array>
#include <initializer_list>
#include <algorithm>
#include <cstring>
#include <cassert>
#include <cstdio>
#include <vulkan/vulkan.h>
#include <dxgiformat.h>
#include "util_flags.h"
#ifndef likely
#define likely(x) (x)
#endif
using BOOL=int;
using UINT=unsigned;
#define TRUE 1
#define FALSE 0
namespace dxvk {
  template<typename T> struct Rc { T* ptr; T* operator->() const { return ptr; } };
  struct Logger { static void info(const char*) {} static void err(const char*) { assert(false); } };
}
'''
FAKE_DEVICE = r'''#pragma once
namespace dxvk {
  struct DxvkDevice {
    DxvkFormatFeatures getFormatFeatures(VkFormat) const {
      return { VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT, 0, 0 };
    }
  };
}
'''
FIXTURE = r'''
#include "src/dxgi/dxgi_format.h"
#include "upload_math.h"
using namespace dxvk;
int main() {
  DxvkDevice device; DXGIVkFormatTable table(Rc<DxvkDevice>{&device});
  unsigned formats=0, views=0, uploads=0;
  DXGI_FORMAT ids[] = {FP_DXGI_FORMAT_ASTC_4X4_TYPELESS,FP_DXGI_FORMAT_ASTC_4X4_UNORM,FP_DXGI_FORMAT_ASTC_4X4_SRGB,
                      FP_DXGI_FORMAT_ASTC_6X6_TYPELESS,FP_DXGI_FORMAT_ASTC_6X6_UNORM,FP_DXGI_FORMAT_ASTC_6X6_SRGB};
  VkFormat vk[] = {VK_FORMAT_ASTC_4x4_UNORM_BLOCK,VK_FORMAT_ASTC_4x4_UNORM_BLOCK,VK_FORMAT_ASTC_4x4_SRGB_BLOCK,
                   VK_FORMAT_ASTC_6x6_UNORM_BLOCK,VK_FORMAT_ASTC_6x6_UNORM_BLOCK,VK_FORMAT_ASTC_6x6_SRGB_BLOCK};
  for(unsigned i=0;i<6;i++) {
    assert(fpAstcFormatIndex(ids[i])==int(i));
    for(int mode=0;mode<4;mode++) {
      auto f=table.GetFormatInfo(ids[i],DXGI_VK_FORMAT_MODE(mode));
      auto p=table.GetPackedFormatInfo(ids[i],DXGI_VK_FORMAT_MODE(mode));
      assert(f.Format==p.Format);
      assert(f.Format==(mode<2?vk[i]:VK_FORMAT_UNDEFINED));
      if(mode<2) assert(f.Aspect==(i%3?VK_IMAGE_ASPECT_COLOR_BIT:0));
      assert(f.Swizzle.r==VK_COMPONENT_SWIZZLE_IDENTITY);
      formats++;
    }
    auto family=table.GetFormatFamily(ids[i],DXGI_VK_FORMAT_MODE_ANY);
    assert(family.FormatCount==(i%3?1u:2u));
    assert(family.Formats[0]==vk[i]);
    if(i%3==0)assert(family.Formats[1]==vk[i+2]);
    assert(table.GetFormatFamily(ids[i],DXGI_VK_FORMAT_MODE_DEPTH).FormatCount==0);
    auto info=lookupFormatInfo(vk[i]);
    assert(info&&info->elementSize==16&&info->blockSize.width==(i<3?4u:6u));
    assert(info->blockSize.height==info->blockSize.width&&info->blockSize.depth==1);
    assert(info->flags.test(DxvkFormatFlag::BlockCompressed));
    assert(info->flags.test(DxvkFormatFlag::ColorSpaceSrgb)==(i%3==2));
    for(unsigned j=0;j<6;j++) {
      bool expected=i/3==j/3&&j%3!=0&&(i%3==0||i==j);
      assert(fpAstcViewCompatible(ids[i],ids[j])==expected);views++;
    }
    assert(!fpAstcViewCompatible(ids[i],DXGI_FORMAT_BC3_UNORM));
    assert(!fpAstcViewCompatible(DXGI_FORMAT_BC3_UNORM,ids[i]));
    // Run upstream non-power-of-two block math and padded row copies for all
    // mip tails, 2D arrays, six cube faces and a two-cube array.
    for(unsigned layers:{1u,3u,6u,12u})for(unsigned w:{1u,2u,4u,5u,6u,7u,13u,125u,256u,513u}) {
      unsigned block=info->blockSize.width;
      unsigned h=w+1,row=((w+block-1)/block)*16,rows=(h+block-1)/block;
      auto count=util::computeBlockCount({w,h,1},info->blockSize);
      assert(count.width==row/16&&count.height==rows);
      uint32_t rp=0,bytes=0;
      assert(fpAstcMipLayout(ids[i],w,h,&rp,&bytes)&&rp==row&&bytes==row*rows);
      assert(util::computeImageDataSize(vk[i],{w,h,1})==uint64_t(row)*rows);
      assert(size_t(row+16)*rows*layers<8000000);
      static unsigned char src[8000000],dst[8000000];
      memset(src,0xcc,sizeof(src));memset(dst,0xa5,sizeof(dst));
      for(unsigned l=0;l<layers;l++)for(unsigned y=0;y<rows;y++)for(unsigned x=0;x<row;x++)
        src[(size_t(l)*rows+y)*(row+16)+x]=uint8_t(l*37+y*7+x);
      util::packImageData(dst,src,row+16,0,0,0,VK_IMAGE_TYPE_2D,{w,h,1},layers,info,VK_IMAGE_ASPECT_COLOR_BIT);
      for(unsigned l=0;l<layers;l++)for(unsigned y=0;y<rows;y++)for(unsigned x=0;x<row;x++)
        assert(dst[(size_t(l)*rows+y)*row+x]==uint8_t(l*37+y*7+x));
      assert(dst[size_t(layers)*rows*row]==0xa5);uploads++;
    }
  }
  DXGI_FORMAT mapped=DXGI_FORMAT_UNKNOWN;
  assert(fpAstcFromD3DFormat(FP_D3DFMT_ASTC_4X4,false,&mapped)&&mapped==ids[1]);
  assert(fpAstcFromD3DFormat(FP_D3DFMT_ASTC_4X4,true,&mapped)&&mapped==ids[2]);
  assert(fpAstcFromD3DFormat(FP_D3DFMT_ASTC_6X6,false,&mapped)&&mapped==ids[4]);
  assert(fpAstcFromD3DFormat(FP_D3DFMT_ASTC_6X6,true,&mapped)&&mapped==ids[5]);
  assert(!fpAstcFromD3DFormat(0x35545844,false,&mapped)&&mapped==ids[5]);
  assert(!fpAstcFromD3DFormat(FP_D3DFMT_ASTC_4X4,false,nullptr));
  uint32_t row=123,size=456;
  assert(!fpAstcMipLayout(ids[1],0,1,&row,&size)&&row==123&&size==456);
  assert(!fpAstcMipLayout(ids[1],1,0,&row,&size));
  assert(!fpAstcMipLayout(ids[1],0xffffffffu,0xffffffffu,&row,&size));
  assert(!fpAstcMipLayout(ids[1],1,1,nullptr,&size));
  assert(!fpAstcMipLayout(DXGI_FORMAT_BC3_UNORM,1,1,&row,&size));
  for(auto f:{DXGI_FORMAT(0x7f0043ff),DXGI_FORMAT(0x7f004403),DXGI_FORMAT(0x7f0065ff),DXGI_FORMAT(0x7f006603),DXGI_FORMAT(0xffffffff)})
    assert(table.GetFormatInfo(f,DXGI_VK_FORMAT_MODE_ANY).Format==VK_FORMAT_UNDEFINED);
  assert(table.GetFormatInfo(DXGI_FORMAT_BC3_UNORM,DXGI_VK_FORMAT_MODE_ANY).Format==VK_FORMAT_BC3_UNORM_BLOCK);
  assert(table.GetPackedFormatInfo(DXGI_FORMAT_BC3_UNORM_SRGB,DXGI_VK_FORMAT_MODE_ANY).Format==VK_FORMAT_BC3_SRGB_BLOCK);
  assert(lookupFormatInfo(VK_FORMAT_BC3_UNORM_BLOCK)->blockSize.width==4);
  assert(lookupFormatInfo(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)->elementSize==6);
  assert(lookupFormatInfo(VK_FORMAT_A8_UNORM_KHR)->elementSize==1);
  assert(lookupFormatInfo(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK)==nullptr);
  assert(lookupFormatInfo(VK_FORMAT_ASTC_5x4_UNORM_BLOCK)==nullptr);
  printf("ASTC_FORMAT formats=%u views=%u uploads=%u BC_unchanged=1 ETC_not_faked=1 PASS\n",formats,views,uploads);
}
'''


def extract_function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 1
    for end in range(brace + 1, len(text)):
        depth += (text[end] == '{') - (text[end] == '}')
        if depth == 0:
            return text[start:end + 1]
    raise AssertionError('Unclosed upstream function')


def main():
    out=ROOT/'build/dxvk-astc-candidate'
    before={name:hashlib.sha256((overlay.SOURCE/name).read_bytes()).hexdigest()for name in overlay.PREIMAGES}
    outputs=overlay.apply(out,overlay.SOURCE)
    negatives=0
    layout_cases=0
    for block in (4,6):
        for layers in (1,3,6,12):
            layouts=overlay.subresource_layout(256,256,9,layers,block,cube=layers in (6,12))
            assert len(layouts)==9*layers and [row['index']for row in layouts]==list(range(9*layers))
            assert layouts[-1]['bytes']==16 and layouts[-1]['offset']%16==0
            total=layouts[-1]['offset']+layouts[-1]['bytes']
            assert overlay.subresource_layout(256,256,9,layers,block,payload_bytes=total)==layouts
            for wrong in (total-1,total+1):
                try:overlay.subresource_layout(256,256,9,layers,block,payload_bytes=wrong)
                except ValueError:negatives+=1
                else:raise AssertionError('Must reject exact allocation length mismatch')
            layout_cases+=len(layouts)
    for values,kw in [((0,1,1,1,4),{}),((1,1,2,1,4),{}),((1,1,1,0,4),{}),
                      ((256,256,9,7,4),{'cube':True}),((256,128,9,6,4),{'cube':True}),
                      ((1,1,1,1,5),{}),((0xffffffff,0xffffffff,1,1,4),{}),((1,1,1,0xffffffff,4),{})]:
        try:overlay.subresource_layout(*values,**kw)
        except ValueError:negatives+=1
        else:raise AssertionError('Must reject malformed upload layout')
    with tempfile.TemporaryDirectory(prefix='fp-astc-preimage-')as folder:
        work=Path(folder)
        for name in overlay.PREIMAGES:
            target=work/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(overlay.SOURCE/name,target)
        for name in overlay.PREIMAGES:
            path=work/name;data=path.read_bytes();path.write_bytes(data+b'\n')
            try:overlay.apply(work)
            except ValueError:negatives+=1
            else:raise AssertionError('Must reject source drift')
            assert not(work/overlay.NEW_HEADER).exists(),'Partial output before all preimages checked'
            path.write_bytes(data)
        overlay.apply(work)
        try:overlay.apply(work)
        except ValueError:negatives+=1
        else:raise AssertionError('Double apply must fail')
    fixture=out/'host-fixture';fixture.mkdir(parents=True,exist_ok=True)
    for name in ('src/dxgi/dxgi_format.cpp','src/dxgi/dxgi_format.h','src/dxvk/dxvk_format.cpp','src/dxvk/dxvk_format.h',overlay.NEW_HEADER):
        text=(out/name).read_text()
        if name.endswith('dxgi_format.h'):
            text=text.replace('#include "dxgi_include.h"','#include "shim.h"').replace('#include "../dxvk/dxvk_device.h"','')
            text=text.replace('#include "../dxvk/dxvk_format.h"','#include "../dxvk/dxvk_format.h"\n#include "fake_device.h"')
        if name.endswith('dxvk_format.h'):text=text.replace('#include "dxvk_include.h"','#include "shim.h"')
        write(fixture/name,text)
    write(fixture/'shim.h',SHIM);write(fixture/'fake_device.h',FAKE_DEVICE)
    util_h=(overlay.SOURCE/'src/dxvk/dxvk_util.h').read_text();util_cpp=(overlay.SOURCE/'src/dxvk/dxvk_util.cpp').read_text()
    block_count=extract_function(util_h,'inline VkExtent3D computeBlockCount(')
    flatten=extract_function(util_h,'inline uint32_t flattenImageExtent(')
    first=util_cpp.index('  uint32_t computeMipLevelCount(');last=util_cpp.index('  static VkColorComponentFlags remapComponentFlag(')
    upload='''#pragma once
#include "src/dxvk/dxvk_format.h"
namespace dxvk::vk {
inline VkImageAspectFlags getNextAspect(VkImageAspectFlags& a) {auto x=a&-a;a-=x;return x;}
inline uint32_t getPlaneIndex(VkImageAspectFlags a) {return a==VK_IMAGE_ASPECT_PLANE_0_BIT?0:a==VK_IMAGE_ASPECT_PLANE_1_BIT?1:2;}
}
namespace dxvk::util {
VkDeviceSize computeImageDataSize(VkFormat,VkExtent3D,VkImageAspectFlags);
''' + block_count+'\n'+flatten+'\n'+util_cpp[first:last]+'\n}\n'
    write(fixture/'upload_math.h',upload);write(fixture/'fixture.cpp',FIXTURE)
    config=json.loads((ROOT/'port.json').read_text());binaries,env=windows_toolchain()
    clang=Path(config['android_sdk'])/'ndk'/config['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
    objects=[]
    includes=[fixture,overlay.SOURCE/'include/native/directx',overlay.SOURCE/'include/vulkan/include',overlay.SOURCE/'src/util']
    for name in ('src/dxgi/dxgi_format.cpp','src/dxvk/dxvk_format.cpp','fixture.cpp'):
        source=fixture/name;obj=fixture/(source.stem+'.obj');objects.append(obj)
        subprocess.run([str(binaries/'cl.exe'),'/nologo','/O2','/std:c++17','/DNOMINMAX','/DCOM_NO_WINDOWS_H',
                        *['/I'+str(p)for p in includes],'/c',str(source),'/Fo'+str(obj)],check=True,env=env)
        subprocess.run([str(clang),'--target=aarch64-linux-android30','-std=c++17','-DCOM_NO_WINDOWS_H',
                        *['-I'+str(p)for p in includes],'-fsyntax-only',str(source)],check=True,env=env)
    exe=fixture/'fixture.exe'
    subprocess.run([str(binaries/'link.exe'),'/nologo','/OUT:'+str(exe),*map(str,objects),
                    'libcmt.lib','oldnames.lib','kernel32.lib'],check=True,env=env)
    run=subprocess.run([str(exe)],check=True,capture_output=True,text=True,timeout=30)
    assert 'ASTC_FORMAT formats=24 views=36 uploads=240' in run.stdout,run.stdout
    assert before=={name:hashlib.sha256((overlay.SOURCE/name).read_bytes()).hexdigest()for name in overlay.PREIMAGES}
    report={'success':True,'output':run.stdout.strip(),'negative_cases':negatives,'mip_array_cube_layout_cases':layout_cases,'candidate_outputs':outputs,
            'upstream_tables_executed':True,'upstream_upload_math_executed':True,'actual_GPU_test':False,
            'actual_D3D11_CreateTexture2D_test':False,'game_loader_integrated':False,'ARM64_syntax_compile':True,
            'fixture_executable_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),
            'producer_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    write(out/'host-test.json',json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))


if __name__=='__main__':main()
