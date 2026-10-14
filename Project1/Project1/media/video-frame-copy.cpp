#include "video-frame-copy.h"
#include <cstring>
#include <limits>
bool copyVideoFrame(const DecodedVideoFrame& source,QueuedVideoFrame* destination){
    if(!destination||source.width<=0||source.height<=0||source.width>32768||source.height>32768)return false;
    QueuedVideoFrame result;result.width=source.width;result.height=source.height;result.ptsSeconds=source.ptsSeconds;result.pixelFormat=source.pixelFormat;result.colorSpace=source.colorSpace;result.colorRange=source.colorRange;
    const int count=source.pixelFormat==VideoPixelFormat::Yuv420p?3:1;
    for(int p=0;p<count;++p){const int rows=p?(source.height+1)/2:source.height;const int bytes=p?(source.width+1)/2:source.pixelFormat==VideoPixelFormat::Bgra?source.width*4:source.width;
        const auto* data=source.planes[p]?source.planes[p]:p==0?source.data:nullptr;const int stride=source.strides[p]?source.strides[p]:p==0?source.stride:0;
        if(!data||stride==std::numeric_limits<int>::min()||std::abs(stride)<bytes)return false;
        result.planeOffsets[p]=result.pixels.size();result.planeStrides[p]=bytes;result.pixels.resize(result.pixels.size()+std::size_t(bytes)*rows);
        for(int y=0;y<rows;++y)std::memcpy(result.pixels.data()+result.planeOffsets[p]+std::size_t(bytes)*y,data+std::ptrdiff_t(stride)*y,bytes);
    }
    result.stride=result.planeStrides[0];*destination=std::move(result);return true;
}
void decodedFrameView(const QueuedVideoFrame& source,DecodedVideoFrame* destination){
    if(!destination)return;*destination={};destination->data=source.pixels.empty()?nullptr:source.pixels.data();destination->width=source.width;destination->height=source.height;destination->stride=source.stride;destination->ptsSeconds=source.ptsSeconds;destination->pixelFormat=source.pixelFormat;destination->colorSpace=source.colorSpace;destination->colorRange=source.colorRange;
    const int count=source.pixelFormat==VideoPixelFormat::Yuv420p?3:1;for(int p=0;p<count;++p){destination->planes[p]=source.pixels.empty()?nullptr:source.pixels.data()+source.planeOffsets[p];destination->strides[p]=source.planeStrides[p]?source.planeStrides[p]:source.stride;}
}
