#include "opengl-renderer.h"
#include "app/playback-session.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include <GL/gl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

namespace {
// Renderer-owned loader; the official ImGui backend uses its separate embedded loader.
constexpr GLenum Texture0=0x84C0, ArrayBuffer=0x8892, StaticDraw=0x88E4;
constexpr GLenum VertexShader=0x8B31, FragmentShader=0x8B30, CompileStatus=0x8B81, LinkStatus=0x8B82;
constexpr GLenum Framebuffer=0x8D40, ColorAttachment0=0x8CE0, FramebufferComplete=0x8CD5;
constexpr GLenum R8=0x8229, Red=0x1903, Rgba8=0x8058, Bgra=0x80E1, ClampToEdge=0x812F;
constexpr GLenum UnpackRowLength=0x0CF2, PackRowLength=0x0D02;
constexpr GLenum PixelUnpackBuffer=0x88EC, PixelPackBuffer=0x88EB, FramebufferSrgb=0x8DB9;
constexpr GLenum RasterizerDiscard=0x8C89;

#define RENDER_GL_FUNCTIONS(X) \
    X(void,ActiveTexture,(GLenum)) \
    X(void,GenBuffers,(GLsizei,GLuint*)) \
    X(void,DeleteBuffers,(GLsizei,const GLuint*)) \
    X(void,BindBuffer,(GLenum,GLuint)) \
    X(void,BufferData,(GLenum,std::ptrdiff_t,const void*,GLenum)) \
    X(void,GenVertexArrays,(GLsizei,GLuint*)) \
    X(void,DeleteVertexArrays,(GLsizei,const GLuint*)) \
    X(void,BindVertexArray,(GLuint)) \
    X(void,EnableVertexAttribArray,(GLuint)) \
    X(void,VertexAttribPointer,(GLuint,GLint,GLenum,GLboolean,GLsizei,const void*)) \
    X(GLuint,CreateShader,(GLenum)) \
    X(void,ShaderSource,(GLuint,GLsizei,const char* const*,const GLint*)) \
    X(void,CompileShader,(GLuint)) \
    X(void,GetShaderiv,(GLuint,GLenum,GLint*)) \
    X(void,GetShaderInfoLog,(GLuint,GLsizei,GLsizei*,char*)) \
    X(void,DeleteShader,(GLuint)) \
    X(GLuint,CreateProgram,()) \
    X(void,AttachShader,(GLuint,GLuint)) \
    X(void,LinkProgram,(GLuint)) \
    X(void,GetProgramiv,(GLuint,GLenum,GLint*)) \
    X(void,GetProgramInfoLog,(GLuint,GLsizei,GLsizei*,char*)) \
    X(void,DeleteProgram,(GLuint)) \
    X(void,UseProgram,(GLuint)) \
    X(GLint,GetUniformLocation,(GLuint,const char*)) \
    X(void,Uniform1i,(GLint,GLint)) \
    X(void,Uniform1f,(GLint,GLfloat)) \
    X(void,Uniform3f,(GLint,GLfloat,GLfloat,GLfloat)) \
    X(void,UniformMatrix3fv,(GLint,GLsizei,GLboolean,const GLfloat*)) \
    X(void,GenFramebuffers,(GLsizei,GLuint*)) \
    X(void,DeleteFramebuffers,(GLsizei,const GLuint*)) \
    X(void,BindFramebuffer,(GLenum,GLuint)) \
    X(void,FramebufferTexture2D,(GLenum,GLenum,GLenum,GLuint,GLint)) \
    X(GLenum,CheckFramebufferStatus,(GLenum)) \
    X(void,BindSampler,(GLuint,GLuint))

PROC address(const char* name) {
    PROC proc=wglGetProcAddress(name);
    const auto value=reinterpret_cast<std::intptr_t>(proc);
    if(!proc || value==1 || value==2 || value==3 || value==-1)
        proc=GetProcAddress(GetModuleHandleW(L"opengl32.dll"),name);
    return proc;
}
struct GlFunctions {
#define DECLARE(ret,name,args) using name##Proc=ret (APIENTRY*) args; name##Proc name=nullptr;
    RENDER_GL_FUNCTIONS(DECLARE)
#undef DECLARE
    bool load() {
#define LOAD(ret,name,args) name=reinterpret_cast<name##Proc>(address("gl" #name)); if(!name) return false;
        RENDER_GL_FUNCTIONS(LOAD)
#undef LOAD
        return true;
    }
};
#undef RENDER_GL_FUNCTIONS

const char* vertexSource=R"glsl(#version 330 core
layout(location=0) in vec2 position;
layout(location=1) in vec2 texcoord;
out vec2 uv;
void main() { gl_Position=vec4(position,0.0,1.0); uv=texcoord; }
)glsl";
const char* fragmentSource=R"glsl(#version 330 core
in vec2 uv;
out vec4 color;
uniform sampler2D planeY;
uniform sampler2D planeU;
uniform sampler2D planeV;
uniform int planar;
uniform int rotation;
uniform int filterMode;
uniform float zoom;
uniform float brightness;
uniform mat3 conversion;
uniform vec3 offset;
void main() {
    vec2 p=(uv-0.5)/zoom+0.5;
    if(rotation==1) p=vec2(p.y,1.0-p.x);
    else if(rotation==2) p=1.0-p;
    else if(rotation==3) p=vec2(1.0-p.y,p.x);
    if(any(lessThan(p,vec2(0.0))) || any(greaterThan(p,vec2(1.0)))) {
        color=vec4(0.0,0.0,0.0,1.0); return;
    }
    vec3 rgb;
    if(planar!=0) {
        vec3 yuv=vec3(texture(planeY,p).r,texture(planeU,p).r,texture(planeV,p).r);
        rgb=clamp(conversion*(yuv-offset),0.0,1.0);
    } else rgb=texture(planeY,p).rgb;
    if(filterMode==1) rgb=vec3(dot(rgb,vec3(0.299,0.587,0.114)));
    else if(filterMode==2) rgb=vec3(1.0)-rgb;
    color=vec4(clamp(rgb*brightness,0.0,1.0),1.0);
}
)glsl";

bool validEffects(const VideoEffects& effects) {
    return std::isfinite(effects.brightness) && effects.brightness>=0 &&
        std::isfinite(effects.zoom) && effects.zoom>0 && effects.rotation%90==0 &&
        (effects.filter==VideoFilter::Normal || effects.filter==VideoFilter::Grayscale || effects.filter==VideoFilter::Invert);
}
bool planeFits(const PlaybackFrame& frame,int plane,int width,int height,int bytes,int stride) {
    if(width<=0 || height<=0 || stride<=0) return false;
    const size_t row=static_cast<size_t>(width)*bytes, start=frame.planeOffsets[plane];
    if(static_cast<size_t>(stride)<row || start>frame.pixels.size()) return false;
    const size_t available=frame.pixels.size()-start;
    if(available<row) return false;
    return static_cast<size_t>(height-1)<=(available-row)/static_cast<size_t>(stride);
}
bool setPixelFormat(HDC dc) {
    PIXELFORMATDESCRIPTOR desc{}; desc.nSize=sizeof(desc); desc.nVersion=1;
    desc.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;
    desc.iPixelType=PFD_TYPE_RGBA; desc.cColorBits=32; desc.cAlphaBits=8;
    desc.iLayerType=PFD_MAIN_PLANE;
    const int existing=GetPixelFormat(dc);
    if(existing) {
        PIXELFORMATDESCRIPTOR actual{};
        return DescribePixelFormat(dc,existing,sizeof(actual),&actual)!=0 &&
            (actual.dwFlags&desc.dwFlags)==desc.dwFlags && actual.iPixelType==PFD_TYPE_RGBA;
    }
    const int format=ChoosePixelFormat(dc,&desc);
    return format!=0 && SetPixelFormat(dc,format,&desc)!=FALSE;
}
HGLRC createContext(HDC target) {
    // WGL extension loading needs a temporary legacy context on a separate window.
    const wchar_t* className=L"NativePlayerWglBootstrap";
    WNDCLASSW wc{}; wc.style=CS_OWNDC; wc.lpfnWndProc=DefWindowProcW;
    wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=className;
    const ATOM registered=RegisterClassW(&wc);
    if(!registered && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    HWND window=CreateWindowW(className,L"",WS_POPUP,0,0,1,1,nullptr,nullptr,wc.hInstance,nullptr);
    if(!window) { if(registered) UnregisterClassW(className,wc.hInstance); return nullptr; }
    HDC dc=GetDC(window);
    HGLRC bootstrap=nullptr, context=nullptr;
    const HDC previousDc=wglGetCurrentDC(); const HGLRC previous=wglGetCurrentContext();
    if(dc && setPixelFormat(dc)) bootstrap=wglCreateContext(dc);
    if(bootstrap && wglMakeCurrent(dc,bootstrap)) {
        using CreateContext=HGLRC (WINAPI*)(HDC,HGLRC,const int*);
        auto create=reinterpret_cast<CreateContext>(address("wglCreateContextAttribsARB"));
        const int attributes[]={0x2091,3,0x2092,3,0x9126,0x00000001,0};
        if(create) context=create(target,nullptr,attributes);
    }
    wglMakeCurrent(previousDc,previous);
    if(bootstrap) wglDeleteContext(bootstrap);
    if(dc) ReleaseDC(window,dc);
    DestroyWindow(window); if(registered) UnregisterClassW(className,wc.hInstance);
    return context;
}
bool writePng(const wchar_t* path,UINT width,UINT height,const std::vector<unsigned char>& bgra) {
    using Microsoft::WRL::ComPtr;
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    ComPtr<IWICImagingFactory> factory; ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder; ComPtr<IWICBitmapFrameEncode> frame;
    bool ok=false;
    if(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))) &&
       SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path,GENERIC_WRITE)) &&
       SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder)) &&
       SUCCEEDED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache)) &&
       SUCCEEDED(encoder->CreateNewFrame(&frame,nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
       SUCCEEDED(frame->SetSize(width,height))) {
        WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
        ok=SUCCEEDED(frame->SetPixelFormat(&format)) && IsEqualGUID(format,GUID_WICPixelFormat32bppBGRA) &&
           SUCCEEDED(frame->WritePixels(height,width*4,static_cast<UINT>(bgra.size()),const_cast<BYTE*>(bgra.data()))) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }
    frame.Reset(); encoder.Reset(); stream.Reset(); factory.Reset();
    if(SUCCEEDED(com)) CoUninitialize();
    return ok;
}
}

struct OpenGLRenderer::Impl {
    HWND window=nullptr; HDC dc=nullptr; HGLRC context=nullptr; DWORD thread=0;
    ImGuiContext* imgui=nullptr; bool backend=false;
    GlFunctions gl;
    GLuint program=0, vao=0, vbo=0, fbo=0, output=0;
    std::array<GLuint,3> textures{};
    std::array<int,3> textureWidths{},textureHeights{};
    VideoPixelFormat textureFormat=VideoPixelFormat::Bgra;
    int outputWidth=0,outputHeight=0;
    unsigned int width=0,height=0;
    std::shared_ptr<const PlaybackFrame> uploaded;
    bool current() const {
        return context && thread==GetCurrentThreadId() &&
            (wglGetCurrentContext()==context || wglMakeCurrent(dc,context));
    }
    GLuint shader(GLenum type,const char* source) {
        GLuint id=gl.CreateShader(type); if(!id) return 0;
        gl.ShaderSource(id,1,&source,nullptr); gl.CompileShader(id);
        GLint ok=0; gl.GetShaderiv(id,CompileStatus,&ok);
        if(!ok) {
            char log[2048]{}; gl.GetShaderInfoLog(id,sizeof(log),nullptr,log);
            std::cerr<<"OpenGL video shader: "<<log<<'\n'; gl.DeleteShader(id); return 0;
        }
        return id;
    }
    bool setup() {
        if(!gl.load()) return false;
        GLuint vertex=shader(VertexShader,vertexSource),fragment=shader(FragmentShader,fragmentSource);
        if(!vertex || !fragment) {
            if(vertex) gl.DeleteShader(vertex); if(fragment) gl.DeleteShader(fragment); return false;
        }
        program=gl.CreateProgram(); gl.AttachShader(program,vertex); gl.AttachShader(program,fragment);
        gl.LinkProgram(program); gl.DeleteShader(vertex); gl.DeleteShader(fragment);
        GLint ok=0; gl.GetProgramiv(program,LinkStatus,&ok);
        if(!ok) {
            char log[2048]{}; gl.GetProgramInfoLog(program,sizeof(log),nullptr,log);
            std::cerr<<"OpenGL video program: "<<log<<'\n'; return false;
        }
        // Output texture's row 0 is the top video row for ImGui's default UV convention.
        const float vertices[]={-1,-1,0,0, 1,-1,1,0, 1,1,1,1, -1,-1,0,0, 1,1,1,1, -1,1,0,1};
        gl.GenVertexArrays(1,&vao); gl.GenBuffers(1,&vbo);
        gl.BindVertexArray(vao); gl.BindBuffer(ArrayBuffer,vbo);
        gl.BufferData(ArrayBuffer,sizeof(vertices),vertices,StaticDraw);
        gl.EnableVertexAttribArray(0); gl.VertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,4*sizeof(float),nullptr);
        gl.EnableVertexAttribArray(1); gl.VertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,4*sizeof(float),reinterpret_cast<void*>(2*sizeof(float)));
        gl.BindVertexArray(0); gl.BindBuffer(ArrayBuffer,0);
        gl.GenFramebuffers(1,&fbo); glGenTextures(3,textures.data());
        return program && vao && vbo && fbo && textures[0] && textures[1] && textures[2] && glGetError()==GL_NO_ERROR;
    }
    void clearVideo() {
        uploaded.reset(); if(output) glDeleteTextures(1,&output); output=0; outputWidth=outputHeight=0;
    }
    bool target(int w,int h) {
        if(output && outputWidth==w && outputHeight==h) return true;
        if(output) glDeleteTextures(1,&output); output=0;
        glGenTextures(1,&output); glBindTexture(GL_TEXTURE_2D,output);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,ClampToEdge);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,ClampToEdge);
        glTexImage2D(GL_TEXTURE_2D,0,Rgba8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        gl.BindFramebuffer(Framebuffer,fbo);
        gl.FramebufferTexture2D(Framebuffer,ColorAttachment0,GL_TEXTURE_2D,output,0);
        const bool ok=gl.CheckFramebufferStatus(Framebuffer)==FramebufferComplete && glGetError()==GL_NO_ERROR;
        gl.BindFramebuffer(Framebuffer,0);
        if(!ok) { glDeleteTextures(1,&output); output=0; return false; }
        outputWidth=w; outputHeight=h; return true;
    }
    bool upload(const PlaybackFrame& frame) {
        const bool planar=frame.pixelFormat==VideoPixelFormat::Yuv420p;
        const int count=planar?3:1;
        gl.BindBuffer(PixelUnpackBuffer,0); glPixelStorei(GL_UNPACK_ALIGNMENT,1);
        glPixelStorei(GL_UNPACK_SKIP_ROWS,0); glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
        for(int plane=0;plane<count;++plane) {
            const int w=plane?(frame.width/2+frame.width%2):frame.width;
            const int h=plane?(frame.height/2+frame.height%2):frame.height;
            const int bytes=planar?1:4;
            const int stride=frame.planeStrides[plane] ? frame.planeStrides[plane] : frame.stride;
            gl.ActiveTexture(Texture0+plane); gl.BindSampler(plane,0); glBindTexture(GL_TEXTURE_2D,textures[plane]);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,ClampToEdge);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,ClampToEdge);
            const unsigned char* data=frame.pixels.data()+frame.planeOffsets[plane];
            std::vector<unsigned char> packed;
            if(stride%bytes) {
                packed.resize(static_cast<size_t>(w)*h*bytes);
                for(int row=0;row<h;++row) std::memcpy(packed.data()+static_cast<size_t>(row)*w*bytes,data+static_cast<size_t>(row)*stride,static_cast<size_t>(w)*bytes);
                data=packed.data(); glPixelStorei(UnpackRowLength,0);
            } else glPixelStorei(UnpackRowLength,stride/bytes);
            if(textureWidths[plane]!=w || textureHeights[plane]!=h || textureFormat!=frame.pixelFormat) {
                glTexImage2D(GL_TEXTURE_2D,0,planar?R8:Rgba8,w,h,0,planar?Red:Bgra,GL_UNSIGNED_BYTE,data);
                textureWidths[plane]=w; textureHeights[plane]=h;
            } else glTexSubImage2D(GL_TEXTURE_2D,0,0,0,w,h,planar?Red:Bgra,GL_UNSIGNED_BYTE,data);
        }
        textureFormat=frame.pixelFormat;
        glPixelStorei(UnpackRowLength,0); glPixelStorei(GL_UNPACK_ALIGNMENT,4);
        gl.ActiveTexture(Texture0);
        return glGetError()==GL_NO_ERROR;
    }
    void conversion(const PlaybackFrame& frame) {
        double kr=.299,kb=.114;
        if(frame.colorSpace==VideoColorSpace::Bt709) { kr=.2126; kb=.0722; }
        else if(frame.colorSpace==VideoColorSpace::Bt2020) { kr=.2627; kb=.0593; }
        const double kg=1-kr-kb;
        const bool full=frame.colorRange==VideoColorRange::Full;
        const float y=full?1.f:255.f/219.f, c=full?1.f:255.f/224.f;
        const float matrix[]={y,y,y, 0,static_cast<float>(-2*kb*(1-kb)/kg)*c,static_cast<float>(2*(1-kb))*c,
            static_cast<float>(2*(1-kr))*c,static_cast<float>(-2*kr*(1-kr)/kg)*c,0};
        gl.UniformMatrix3fv(gl.GetUniformLocation(program,"conversion"),1,GL_FALSE,matrix);
        gl.Uniform3f(gl.GetUniformLocation(program,"offset"),full?0.f:16.f/255.f,128.f/255.f,128.f/255.f);
    }
    void render(const PlaybackFrame& frame,const VideoEffects& effects,int turns) {
        gl.BindFramebuffer(Framebuffer,fbo); glViewport(0,0,outputWidth,outputHeight);
        glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE); glDisable(GL_DITHER); glDisable(FramebufferSrgb); glDisable(RasterizerDiscard);
        glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE); glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
        gl.UseProgram(program); gl.BindVertexArray(vao);
        gl.Uniform1i(gl.GetUniformLocation(program,"planar"),frame.pixelFormat==VideoPixelFormat::Yuv420p);
        gl.Uniform1i(gl.GetUniformLocation(program,"rotation"),turns);
        gl.Uniform1i(gl.GetUniformLocation(program,"filterMode"),static_cast<int>(effects.filter));
        gl.Uniform1f(gl.GetUniformLocation(program,"brightness"),effects.brightness);
        gl.Uniform1f(gl.GetUniformLocation(program,"zoom"),effects.zoom);
        conversion(frame);
        const char* samplers[]={"planeY","planeU","planeV"};
        for(int plane=0;plane<3;++plane) {
            gl.ActiveTexture(Texture0+plane); gl.BindSampler(plane,0); glBindTexture(GL_TEXTURE_2D,textures[plane]);
            gl.Uniform1i(gl.GetUniformLocation(program,samplers[plane]),plane);
        }
        glDrawArrays(GL_TRIANGLES,0,6);
        gl.BindFramebuffer(Framebuffer,0); gl.UseProgram(0); gl.BindVertexArray(0);
        for(int plane=0;plane<3;++plane) { gl.ActiveTexture(Texture0+plane); glBindTexture(GL_TEXTURE_2D,0); }
        gl.ActiveTexture(Texture0);
    }
    void drawUi() {
        if(!backend || !imgui) return;
        ImGuiContext* previous=ImGui::GetCurrentContext(); ImGui::SetCurrentContext(imgui);
        if(ImDrawData* data=ImGui::GetDrawData()) ImGui_ImplOpenGL3_RenderDrawData(data);
        ImGui::SetCurrentContext(previous);
    }
};

OpenGLRenderer::OpenGLRenderer():impl_(std::make_unique<Impl>()) {}
OpenGLRenderer::~OpenGLRenderer() { shutdown(); }
bool OpenGLRenderer::initialize(HWND window) {
    shutdown();
    if(!window || !IsWindow(window) || !ImGui::GetCurrentContext() || ImGui::GetIO().BackendRendererUserData) return false;
    auto& p=*impl_; p.thread=GetCurrentThreadId(); p.window=window; p.dc=GetDC(window);
    if(!p.dc || !setPixelFormat(p.dc)) { shutdown(); return false; }
    p.context=createContext(p.dc);
    if(!p.current() || !p.setup()) { shutdown(); return false; }
    p.imgui=ImGui::GetCurrentContext(); p.backend=ImGui_ImplOpenGL3_Init("#version 330 core");
    if(!p.backend) { shutdown(); return false; }
    RECT rect{}; GetClientRect(window,&rect); resize(rect.right-rect.left,rect.bottom-rect.top);
    return true;
}
void OpenGLRenderer::resize(unsigned int width,unsigned int height) {
    if(!impl_->current() || width>static_cast<unsigned int>(std::numeric_limits<int>::max()) ||
       height>static_cast<unsigned int>(std::numeric_limits<int>::max())) return;
    impl_->width=width; impl_->height=height;
}
bool OpenGLRenderer::updateVideo(std::shared_ptr<const PlaybackFrame> frame,VideoEffects effects) {
    auto& p=*impl_;
    if(!p.current()) return false;
    if(!frame) { p.clearVideo(); return true; }
    if(!validEffects(effects) || frame->width<=0 || frame->height<=0 || frame->pixels.empty()) return false;
    GLint maximum=0; glGetIntegerv(GL_MAX_TEXTURE_SIZE,&maximum);
    if(frame->width>maximum || frame->height>maximum) return false;
    if(frame->pixelFormat!=VideoPixelFormat::Bgra && frame->pixelFormat!=VideoPixelFormat::Yuv420p) return false;
    const bool planar=frame->pixelFormat==VideoPixelFormat::Yuv420p;
    for(int plane=0;plane<(planar?3:1);++plane) {
        const int w=plane?(frame->width/2+frame->width%2):frame->width;
        const int h=plane?(frame->height/2+frame->height%2):frame->height;
        const int stride=frame->planeStrides[plane] ? frame->planeStrides[plane] : (plane?0:frame->stride);
        if(!planeFits(*frame,plane,w,h,planar?1:4,stride)) return false;
    }
    // Allocation and uploads use host pointers, independent of whatever ImGui left bound.
    p.gl.BindBuffer(PixelUnpackBuffer,0);
    const int turns=((effects.rotation/90)%4+4)%4;
    if(!p.target(turns%2?frame->height:frame->width,turns%2?frame->width:frame->height)) return false;
    // Keep ownership as well as identity; serial can be zero or restart after a new source.
    if(p.uploaded!=frame) {
        if(!p.upload(*frame)) return false;
        p.uploaded=frame;
    }
    p.render(*frame,effects,turns);
    return glGetError()==GL_NO_ERROR;
}
unsigned int OpenGLRenderer::frameTexture() const { return impl_->output; }
void OpenGLRenderer::begin() {
    auto& p=*impl_; if(!p.current() || !p.width || !p.height) return;
    p.gl.BindFramebuffer(Framebuffer,0); glDrawBuffer(GL_BACK);
    glViewport(0,0,p.width,p.height); glDisable(GL_SCISSOR_TEST); glDisable(FramebufferSrgb);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glClearColor(.043f,.051f,.071f,1.f); glClear(GL_COLOR_BUFFER_BIT);
}
void OpenGLRenderer::present() {
    auto& p=*impl_; if(!p.current() || !p.width || !p.height) return;
    p.gl.BindFramebuffer(Framebuffer,0); glDrawBuffer(GL_BACK); p.drawUi(); SwapBuffers(p.dc);
}
bool OpenGLRenderer::saveScreenshot(const wchar_t* path) {
    auto& p=*impl_;
    if(!path || !*path || !p.current() || !p.width || !p.height ||
       p.width>std::numeric_limits<UINT>::max()/4 ||
       p.height>std::numeric_limits<UINT>::max()/(p.width*4)) return false;
    begin(); p.drawUi();
    p.gl.BindBuffer(PixelPackBuffer,0); glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT,1); glPixelStorei(PackRowLength,0);
    glPixelStorei(GL_PACK_SKIP_ROWS,0); glPixelStorei(GL_PACK_SKIP_PIXELS,0);
    std::vector<unsigned char> rgba(static_cast<size_t>(p.width)*p.height*4);
    glReadPixels(0,0,p.width,p.height,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());
    glPixelStorei(GL_PACK_ALIGNMENT,4);
    if(glGetError()!=GL_NO_ERROR) return false;
    const size_t stride=static_cast<size_t>(p.width)*4;
    for(unsigned int row=0;row<p.height/2;++row)
        std::swap_ranges(rgba.begin()+row*stride,rgba.begin()+(row+1)*stride,rgba.begin()+(p.height-1-row)*stride);
    // Windows' PNG encoder accepts BGRA; convert readback channels before encoding.
    for(size_t pixel=0;pixel<rgba.size();pixel+=4) std::swap(rgba[pixel],rgba[pixel+2]);
    return writePng(path,p.width,p.height,rgba);
}
void OpenGLRenderer::shutdown() {
    auto& p=*impl_;
    if(p.context && p.thread!=GetCurrentThreadId()) return;
    const HGLRC previous=wglGetCurrentContext(); const HDC previousDc=wglGetCurrentDC();
    if(p.context && p.current()) {
        if(p.backend) {
            ImGuiContext* previousImgui=ImGui::GetCurrentContext(); ImGui::SetCurrentContext(p.imgui);
            ImGui_ImplOpenGL3_Shutdown(); ImGui::SetCurrentContext(previousImgui); p.backend=false;
        }
        p.clearVideo(); glDeleteTextures(3,p.textures.data());
        if(p.fbo && p.gl.DeleteFramebuffers) p.gl.DeleteFramebuffers(1,&p.fbo);
        if(p.vbo && p.gl.DeleteBuffers) p.gl.DeleteBuffers(1,&p.vbo);
        if(p.vao && p.gl.DeleteVertexArrays) p.gl.DeleteVertexArrays(1,&p.vao);
        if(p.program && p.gl.DeleteProgram) p.gl.DeleteProgram(p.program);
        if(previous && previous!=p.context) wglMakeCurrent(previousDc,previous); else wglMakeCurrent(nullptr,nullptr);
    }
    if(p.context) wglDeleteContext(p.context);
    if(p.dc) ReleaseDC(p.window,p.dc);
    p=Impl{};
}
