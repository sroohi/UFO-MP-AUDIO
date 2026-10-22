#include "GpuWaveform.h"
#include <cmath>
using namespace juce;
using namespace juce::gl;

GpuWaveform::GpuWaveform(Colour colour):tint(colour) {
    setOpaque(true);setInterceptsMouseClicks(false,false);
    context.setOpenGLVersionRequired(OpenGLContext::openGL3_2);
    context.setRenderer(this);context.setComponentPaintingEnabled(false);
    context.setContinuousRepainting(false);
    const bool disabled=SystemStats::getEnvironmentVariable("UFO_DISABLE_GPU",{}).getIntValue()!=0;
    if(disabled){wanted=false;state=cpu;}else context.attachTo(*this);
    startTimerHz(10);
}
GpuWaveform::~GpuWaveform() {stopTimer();context.detach();context.setRenderer(nullptr);}
void GpuWaveform::setData(const std::vector<float>& data,float color,float playhead,float modulation) {
    Snapshot update;update.hasWaveform=!data.empty();update.color=jlimit(0.f,1.f,color);update.playhead=jlimit(0.f,1.f,playhead);update.modulation=jlimit(-1.f,1.f,modulation);
    if(!data.empty())for(size_t i=0;i<update.waveform.size();++i) {
        const double p=double(i)*double(data.size()-1)/double(update.waveform.size()-1);
        const auto a=size_t(p),b=std::min(a+1,data.size()-1);
        const float value=data[a]+float(p-double(a))*(data[b]-data[a]);
        update.waveform[i]=std::isfinite(value)?jlimit(-1.f,1.f,value):0.f;
    }
    {std::lock_guard<std::mutex> lock(dataMutex);snapshot=update;}
    if(wanted.load())context.triggerRepaint();
    repaint();
}
GpuWaveform::Snapshot GpuWaveform::currentSnapshot() const {std::lock_guard<std::mutex> lock(dataMutex);return snapshot;}
void GpuWaveform::setGpuEnabled(bool enabled) {
    wanted=enabled;firstShowing=0;
    context.detach();linked=false;
    if(enabled) {
        {std::lock_guard<std::mutex> lock(diagnosticsMutex);lastError={};}
        state=waiting;readbackRequested=true;context.attachTo(*this);
    } else state=cpu;
    repaint();
}
GpuWaveform::Diagnostics GpuWaveform::diagnostics() const {
    Diagnostics d;d.active=state.load()==active;d.shaderLinked=linked.load();d.frames=frameCount.load();d.readbackHash=pixelHash.load();d.brightPixels=bright.load();d.readbackWidth=readbackWidth.load();d.readbackHeight=readbackHeight.load();
    std::lock_guard<std::mutex> lock(diagnosticsMutex);d.renderer=rendererName;d.version=glVersion;d.error=lastError;return d;
}
void GpuWaveform::resized(){logicalWidth=getWidth();logicalHeight=getHeight();context.triggerRepaint();}
juce::Image GpuWaveform::readbackImage() const {std::lock_guard<std::mutex> lock(diagnosticsMutex);return lastReadback;}
void GpuWaveform::requestReadback(){readbackRequested=true;context.triggerRepaint();}
void GpuWaveform::fail(const String& error) {linked=false;state=failed;std::lock_guard<std::mutex> lock(diagnosticsMutex);lastError=error;}
void GpuWaveform::newOpenGLContextCreated() {
    {
        std::lock_guard<std::mutex> lock(diagnosticsMutex);
        if(const auto* name=glGetString(GL_RENDERER))rendererName=String::fromUTF8(reinterpret_cast<const char*>(name));
        if(const auto* version=glGetString(GL_VERSION))glVersion=String::fromUTF8(reinterpret_cast<const char*>(version));
    }
    if(glGenVertexArrays==nullptr){fail("OpenGL 3.2 vertex arrays are unavailable");return;}
    constexpr const char* vertex=R"GLSL(#version 150
in vec2 position;
out vec2 uv;
void main(){uv=position*0.5+0.5;gl_Position=vec4(position,0.0,1.0);}
)GLSL";
    constexpr const char* fragment=R"GLSL(#version 150
in vec2 uv;
out vec4 fragmentColor;
uniform float waveform[256];
uniform vec2 resolution;
uniform vec3 tint;
uniform vec2 cursors;
uniform float modulation;
void main(){
    float point=clamp(uv.x,0.0,1.0)*255.0;
    int first=int(floor(point));int second=min(first+1,255);
    float amplitude=mix(waveform[first],waveform[second],fract(point));
    float y=0.5+amplitude*0.42;
    float pixelY=1.0/max(resolution.y,1.0),pixelX=1.0/max(resolution.x,1.0);
    float line=1.0-smoothstep(pixelY,2.5*pixelY,abs(uv.y-y));
    float center=1.0-smoothstep(0.0,pixelY,abs(uv.y-0.5));
    float gridX=1.0-smoothstep(0.0,pixelX*8.0,abs(fract(uv.x*8.0)-0.5));
    float fill=step(min(y,0.5),uv.y)*step(uv.y,max(y,0.5));
    float colorLine=1.0-smoothstep(pixelX,2.0*pixelX,abs(uv.x-cursors.x));
    float playhead=1.0-smoothstep(0.5*pixelX,1.5*pixelX,abs(uv.x-cursors.y));
    vec3 background=vec3(0.040,0.060,0.048)+tint*(center*0.06+gridX*0.018+fill*0.08);
    vec3 wave=tint*(line*(0.78+0.18*abs(modulation)));
    fragmentColor=vec4(background+wave+tint*colorLine*0.6+vec3(0.92,0.98,0.94)*playhead*0.72,1.0);
}
)GLSL";
    program=std::make_unique<OpenGLShaderProgram>(context);
    if(!program->addVertexShader(vertex) || !program->addFragmentShader(fragment) || !program->link()) {fail(program->getLastError());program.reset();return;}
    position=std::make_unique<OpenGLShaderProgram::Attribute>(*program,"position");
    waveformUniform=std::make_unique<OpenGLShaderProgram::Uniform>(*program,"waveform[0]");
    resolutionUniform=std::make_unique<OpenGLShaderProgram::Uniform>(*program,"resolution");
    tintUniform=std::make_unique<OpenGLShaderProgram::Uniform>(*program,"tint");
    cursorsUniform=std::make_unique<OpenGLShaderProgram::Uniform>(*program,"cursors");
    modulationUniform=std::make_unique<OpenGLShaderProgram::Uniform>(*program,"modulation");
    const float vertices[]={-1,-1,1,-1,-1,1,1,1};
    glGenVertexArrays(1,&vertexArray);glBindVertexArray(vertexArray);
    glGenBuffers(1,&vertexBuffer);glBindBuffer(GL_ARRAY_BUFFER,vertexBuffer);glBufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices,GL_STATIC_DRAW);
    glEnableVertexAttribArray(position->attributeID);glVertexAttribPointer(position->attributeID,2,GL_FLOAT,GL_FALSE,2*sizeof(float),nullptr);
    glBindVertexArray(0);glBindBuffer(GL_ARRAY_BUFFER,0);
    linked=true;state=active;readbackRequested=true;
}
void GpuWaveform::renderOpenGL() {
    OpenGLHelpers::clear(Colour(0xff101812));
    if(!program || !linked.load())return;
    const auto data=currentSnapshot();const auto scale=context.getRenderingScale();
    const int width=std::max(1,int(std::round(logicalWidth.load()*scale))),height=std::max(1,int(std::round(logicalHeight.load()*scale)));
    glViewport(0,0,width,height);glDisable(GL_DEPTH_TEST);glDisable(GL_BLEND);
    program->use();waveformUniform->set(data.waveform.data(),int(data.waveform.size()));resolutionUniform->set(float(width),float(height));
    tintUniform->set(tint.getFloatRed(),tint.getFloatGreen(),tint.getFloatBlue());cursorsUniform->set(data.color,data.playhead);modulationUniform->set(data.modulation);
    glBindVertexArray(vertexArray);glDrawArrays(GL_TRIANGLE_STRIP,0,4);glBindVertexArray(0);glUseProgram(0);
    const auto error=glGetError();if(error!=GL_NO_ERROR){fail("OpenGL render error "+String(int(error)));return;}
    ++frameCount;
    if(readbackRequested.exchange(false)) {
        readback.resize(size_t(width)*size_t(height)*4);glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,readback.data());
        uint64_t hash=1469598103934665603ULL;int count=0;
        for(size_t i=0;i<readback.size();i+=4) {
            for(size_t c=0;c<3;++c){hash^=readback[i+c];hash*=1099511628211ULL;}
            if(std::max({readback[i],readback[i+1],readback[i+2]})>100)++count;
        }
        Image captured(Image::ARGB,width,height,false);
        {
            Image::BitmapData pixels(captured,Image::BitmapData::writeOnly);
            for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
                const size_t offset=(size_t(height-1-y)*size_t(width)+size_t(x))*4;
                pixels.setPixelColour(x,y,Colour(readback[offset],readback[offset+1],readback[offset+2],readback[offset+3]));
            }
        }
        {std::lock_guard<std::mutex> lock(diagnosticsMutex);lastReadback=captured;}
        pixelHash=hash;bright=count;readbackWidth=width;readbackHeight=height;
    }
}
void GpuWaveform::openGLContextClosing() {
    linked=false;if(wanted.load() && state.load()!=failed)state=waiting;
    waveformUniform.reset();resolutionUniform.reset();tintUniform.reset();cursorsUniform.reset();modulationUniform.reset();position.reset();program.reset();
    if(vertexBuffer!=0){glDeleteBuffers(1,&vertexBuffer);vertexBuffer=0;}
    if(vertexArray!=0){glDeleteVertexArrays(1,&vertexArray);vertexArray=0;}
}
void GpuWaveform::timerCallback() {
    if(!wanted.load())return;
    if(state.load()==failed){context.detach();repaint();return;}
    if(!isShowing() || getPeer()==nullptr){firstShowing=0;return;}
    if(state.load()==active)return;
    const double now=Time::getMillisecondCounterHiRes();if(firstShowing<=0)firstShowing=now;
    if(now-firstShowing>2500){fail("GPU context unavailable; using CPU waveform rendering");context.detach();repaint();}
}
void GpuWaveform::paint(Graphics& g) {
    const auto data=currentSnapshot();g.fillAll(Colour(0xff101812));
    const float width=float(getWidth()),height=float(getHeight());
    g.setColour(tint.withAlpha(.12f));g.drawHorizontalLine(getHeight()/2,0,width);
    if(data.hasWaveform) {
        Path path;
        for(size_t i=0;i<data.waveform.size();++i){const float x=float(i)*width/float(data.waveform.size()-1),y=height*(.5f-.42f*data.waveform[i]);if(i==0)path.startNewSubPath(x,y);else path.lineTo(x,y);}
        g.setColour(tint.withAlpha(.85f));g.strokePath(path,PathStrokeType(1));
    } else {g.setColour(tint.withAlpha(.55f));g.setFont(FontOptions(10));g.drawText("SOURCE WAVEFORM",getLocalBounds(),Justification::centred);}
    g.setColour(tint.withAlpha(.65f));g.drawVerticalLine(int(data.color*width),0,height);
    g.setColour(Colours::white.withAlpha(.75f));g.drawVerticalLine(int(data.playhead*width),0,height);
}
