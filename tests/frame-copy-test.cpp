#include "media/video-frame-copy.h"
#include <iostream>
#include <stdexcept>
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
int main(){try{
    unsigned char y[]{1,2,3,99,4,5,6,99,7,8,9,99},u[]{10,11,99,12,13,99},v[]{14,15,99,16,17,99};
    DecodedVideoFrame input{};input.pixelFormat=VideoPixelFormat::Yuv420p;input.width=input.height=3;input.data=y;input.stride=4;input.planes={y,u,v};input.strides={4,3,3};input.ptsSeconds=1.25;input.colorRange=VideoColorRange::Full;
    QueuedVideoFrame copy;require(copyVideoFrame(input,&copy),"native YUV frame copy missing");
    require(copy.pixels==std::vector<unsigned char>({1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17}),"padding or chroma size wrong");y[0]=88;u[0]=77;
    DecodedVideoFrame view;decodedFrameView(copy,&view);require(view.pixelFormat==VideoPixelFormat::Yuv420p&&view.planes[0][0]==1&&view.planes[1][0]==10&&view.planes[2][3]==17,"published plane alias invalid");require(view.ptsSeconds==1.25&&view.colorRange==VideoColorRange::Full,"metadata lost");
    input.planes[0]=y+8;input.strides[0]=-4;require(copyVideoFrame(input,&copy),"negative stride rejected");require(copy.pixels[0]==7&&copy.pixels[6]==88,"negative stride orientation wrong");
    input.planes[1]=nullptr;require(!copyVideoFrame(input,&copy),"invalid plane accepted");std::cout<<"PASS owned planar copy, padded chroma, negative stride and metadata\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
